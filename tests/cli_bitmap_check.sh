#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_bitmap_check.sh - the storage bitmap of a write mount is read once and kept (LARGE_VOLUMES.md 3). Every allocation used
# to read all of it: on a 2 TiB volume (4,294,901,760 blocks: the largest with 16-block clusters, ods2_init's minimum for more is 17) with 16-block clusters (a 32 MiB bitmap) each file created read 32 MiB, and 300 files
# took 18 seconds (0.2 s now). The speed is judged against the same import into a small volume made in the same run (a
# busy host slows both: review 27 T3), not by the clock alone. The placement of the blocks is checked too: a script of
# 90 COPY and DELETE commands in one mount, and the same commands one process each, must leave every header with the same
# retrieval pointers and the storage bitmap the same. Called by `make test`; run from the project root.

TMP=samples/cli_bitmap_tmp
INIT=./ods2_init
ODS2=./ods2
fails=0

check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        echo "      expected: $2"
        echo "      got:      $3"
        fails=$((fails + 1))
    fi
}
free_blocks() { $ODS2 "$1" 'SHOW VOLUME' 2>&1 | awk '/Free blocks:/ {print $3}'; }

rm -rf "$TMP"
mkdir -p "$TMP/tree"
python3 -I - "$TMP/tree" <<'PY'
import sys
for i in range(300):
    open('%s/F%d.TXT' % (sys.argv[1], i), 'w').write('file %d\n' % i * 20)
PY
img="$TMP/h.img"
if ! $INIT "$img" 4294901760 --structure=2 --cluster=16 --label=HUGE >/dev/null 2>&1; then
    echo "SKIP: the file system cannot hold a sparse 2 TiB image"
    rm -rf "$TMP"
    exit 0
fi
before=$(free_blocks "$img")
ms() { python3 -I -c 'import time; print(int(time.time() * 1000))'; }
small="$TMP/s.img"
$INIT "$small" 65536 --structure=2 --cluster=16 --label=SMALL >/dev/null 2>&1
s0=$(ms)
$ODS2 "$small" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
s1=$(ms)
t0=$(ms)
out=$($ODS2 "$img" "IMPORT $TMP/tree [000000]" 2>&1)
t1=$(ms)
check "IMPORT of 300 files into a 2 TiB volume (32 MiB bitmap)" "1" "$(echo "$out" | grep -c 'IMPORTED, 300 files')"
elapsed=$((t1 - t0))
base=$((s1 - s0))
[ $base -lt 300 ] && base=300
check "... in at most 25 times what 300 files take on a small volume (took $elapsed ms, small $((s1 - s0)) ms; the old code: 100 times and more)" "yes" "$([ $elapsed -lt $((base * 25)) ] && echo yes || echo no)"
rm -rf "$TMP/out"
$ODS2 "$img" "EXPORT [000000]*.* $TMP/out" >/dev/null 2>&1
same=$(for n in 0 1 150 299; do cmp "$TMP/out/F$n.TXT" "$TMP/tree/F$n.TXT" >/dev/null 2>&1 && echo ok; done | wc -l)
check "the files read back identical (4 sampled)" "4" "$same"
mid=$(free_blocks "$img")
used=$((before - mid))
check "the blocks taken are whole clusters of 16 (clusters for files and the directory: $used blocks)" "0" "$((used % 16))"
$ODS2 "$img" 'DELETE [000000]F*.TXT;*' >/dev/null 2>&1
after=$(free_blocks "$img")
check "deleting the 300 one-cluster files frees exactly 300 x 16 blocks (the rest of what was taken is INDEXF.SYS and the directory)" "4800" "$((after - mid))"
# placement: one mount against one mount per command
python3 -I - "$TMP" <<'PY'
import os, random, sys
tmp = sys.argv[1]
rnd = random.Random(5)
cmds = []
for i in range(90):
    n = rnd.choice([1, 100, 600, 5000, 9000, 14000])
    open('%s/p%d.dat' % (tmp, i), 'wb').write(bytes(n))
    cmds.append('COPY %s/p%d.dat [000000]P%d.DAT' % (tmp, i, i))
    if i % 3 == 2:
        cmds.append('DELETE [000000]P%d.DAT' % rnd.randrange(0, i))
open(tmp + '/place.ods2', 'w').write('\n'.join(cmds) + '\n')
open(tmp + '/place.cmds', 'w').write('\n'.join(cmds) + '\n')
PY
$INIT "$TMP/p1.img" 20000 --structure=2 --cluster=4 --label=PLACE >/dev/null 2>&1
cp "$TMP/p1.img" "$TMP/p2.img"
$ODS2 "$TMP/p1.img" "@$TMP/place.ods2" >/dev/null 2>&1
while IFS= read -r c; do $ODS2 "$TMP/p2.img" "$c" >/dev/null 2>&1; done < "$TMP/place.cmds"
cat > "$TMP/placement.py" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb')
def blk(n, c=1):
    f.seek(n * 512); return f.read(512 * c)
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
home = blk(1)
cl, ib, ibsz, maxf = u16(home, 14), u32(home, 24), u16(home, 32), u32(home, 28)
def ptrs(h):
    o = h[1] * 2; end = o + h[58] * 2; out = []
    while o < end:
        w = u16(h, o); fmt = w >> 14
        if fmt == 1: out.append((((w >> 8) & 0x3F) << 16 | u16(h, o + 2), (w & 0xFF) + 1)); o += 4
        elif fmt == 2: out.append((u32(h, o + 2), (w & 0x3FFF) + 1)); o += 6
        elif fmt == 3: out.append((u32(h, o + 4), (((w & 0x3FFF) << 16) | u16(h, o + 2)) + 1)); o += 8
        else: o += 2
    return out
h1 = blk(ib + ibsz)
idx = ptrs(h1)
def lbn_of(vbn):
    v = 1
    for l, c in idx:
        if vbn < v + c: return l + vbn - v
        v += c
efblk = (u16(h1, 28) << 16) | u16(h1, 30)
for n in range(1, efblk - 4 * cl - ibsz):
    h = blk(lbn_of(4 * cl + ibsz + n))
    if u16(h, 8) == 0 and u16(h, 12) == 0: continue
    print(n, ptrs(h))
bmh = blk(lbn_of(4 * cl + ibsz + 2))
for l, c in ptrs(bmh):
    sys.stdout.write('bitmap %d %d %s\n' % (l, c, blk(l, c).hex()))
PY
python3 -I "$TMP/placement.py" "$TMP/p1.img" > "$TMP/p1.txt"
python3 -I "$TMP/placement.py" "$TMP/p2.img" > "$TMP/p2.txt"
check "placement: 90 COPY/DELETE commands in one mount and one process each leave the same pointers and bitmap ($(grep -vc '^bitmap' "$TMP/p1.txt") headers)" "same" "$(cmp -s "$TMP/p1.txt" "$TMP/p2.txt" && echo same || echo different)"
check "placement: the dump has headers beyond the reserved files" "yes" "$([ "$(grep -vc '^bitmap' "$TMP/p1.txt")" -gt 40 ] && echo yes || echo no)"
check "placement: ods5check clean after the script" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/p1.img" 2>&1 | tail -1)"

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_bitmap_check: $fails FAILED"
    exit 1
fi
echo "cli_bitmap_check: all checks passed"
