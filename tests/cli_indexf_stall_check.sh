#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_indexf_stall_check.sh - INDEXF.SYS must always be able to grow (review 27 F1). A Format 2 retrieval pointer takes 6
# bytes (a piece of more than 256 blocks, or above LBN 4,194,304); the map area of an ODS-2 header is 310 bytes = 51 x 6 + 4.
# A header filled with them ended with 4 bytes left, the pre-check let the growth through, the pointer did not fit, and no
# extension header was made after a failed append: every later create failed for good, with free space and free file
# numbers left. Now the last header always keeps room for one more pointer of any size (an extension header is made when
# less than 8 bytes remain), and a header that cannot take the pointer wanted gets an extension header that carries it.
# ODS2_TEST_INDEXF=step=258,nomerge (the ods2_fault build only) grows the index file 258 blocks at a time in separate pieces,
# all Format 2; `noreserve` brings back the old rule (an extension header only below 4 bytes) to make the stuck state.
# Called by `make test`; run from the project root.

TMP=samples/cli_indexf_stall_tmp
INIT=./ods2_init
FAULT=./ods2_fault
DIRS=${STALL_DIRS:-200}
PER=${STALL_PER:-140}
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
chain_length() { # chain_length <image>: INDEXF.SYS extension headers
    n=1; c=0
    while [ $c -lt 70000 ]; do
        n=$(./ods2_dump_header "$1" "$n" 2>&1 | sed -n 's/.*ext_fid=(\([0-9]*\),.*/\1/p' | head -1)
        [ -n "$n" ] && [ "$n" != 0 ] || break
        c=$((c + 1))
    done
    echo $c
}

rm -rf "$TMP"
mkdir -p "$TMP"
: > "$TMP/empty.txt"
python3 -I - "$TMP" "$DIRS" "$PER" <<'PY'
import sys
tmp, dirs, per = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(tmp + '/make.ods2', 'w') as f:
    for d in range(dirs):
        f.write('CREATE/DIRECTORY [D%03d]\n' % d)
        for i in range(per):
            f.write('COPY %s/empty.txt [D%03d]F%03d.TXT\n' % (tmp, d, i))
PY
TOTAL=$((DIRS * PER))

for level in 2 5; do
    img="$TMP/st$level.img"
    $INIT "$img" 120000 --structure=$level --cluster=1 --maxfiles=$((TOTAL + 2000)) --label=ST$level >/dev/null 2>&1
    out=$(ODS2_TEST_INDEXF=step=258,nomerge $FAULT "$img" "@$TMP/make.ods2" 2>&1)
    check "ODS-$level: $TOTAL files created while INDEXF.SYS grows 258 blocks at a time (Format 2 pointers): none refused" "0" "$(echo "$out" | grep -c '%ODS2-E-')"
    check "ODS-$level: INDEXF.SYS has extension headers (chain $(chain_length "$img"))" "yes" "$([ "$(chain_length "$img")" -gt 0 ] && echo yes || echo no)"
    check "ODS-$level: ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
done

# the stuck state made by the old rule, and the way out
img="$TMP/stuck.img"
$INIT "$img" 120000 --structure=2 --cluster=1 --maxfiles=$((TOTAL + 8000)) --label=STUCK >/dev/null 2>&1
out=$(ODS2_TEST_INDEXF=step=258,nomerge,noreserve $FAULT "$img" "@$TMP/make.ods2" 2>&1)
check "old rule (noreserve): the index file gets stuck (creates refused: no room for another extent)" "yes" "$(echo "$out" | grep -q 'cannot grow\|no room for another extent' && echo yes || echo no)"
cp "$img" "$TMP/stuck0.img"
out=$(ODS2_TEST_INDEXF=step=258,nomerge $FAULT "$img" "COPY $TMP/empty.txt [000000]AFTER1.TXT" 2>&1)
check "a stuck index file grows again (the extension header goes into the new blocks)" "1" "$(echo "$out" | grep -c 'COPIED')"
python3 -I - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
with open(tmp + '/more.ods2', 'w') as f:
    for d in range(40):
        f.write('CREATE/DIRECTORY [M%03d]\n' % d)
        for i in range(140):
            f.write('COPY %s/empty.txt [M%03d]F%03d.TXT\n' % (tmp, d, i))
PY
out=$(ODS2_TEST_INDEXF=step=258,nomerge $FAULT "$img" "@$TMP/more.ods2" 2>&1)
check "and keeps growing: 5,600 more files, none refused" "0" "$(echo "$out" | grep -c '%ODS2-E-')"
check "stuck volume after the way out: ods5check clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"

# a slot beyond EFBLK that holds INDEXF.SYS's own extension header is never given to a new file, even when its index
# bitmap bit is clear (review 27 F2 c: the second COPY after an interrupted growth wrote its header over it)
img="$TMP/guard.img"
$INIT "$img" 8192 --structure=2 --cluster=1 --maxfiles=1500 --label=GUARD >/dev/null 2>&1
i=0
while [ "$(chain_length "$img")" -lt 1 ]; do
    i=$((i + 1))
    ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$img" "COPY $TMP/empty.txt [000000]G$i.TXT" >/dev/null 2>&1
    [ $i -lt 400 ] || break
done
E=$(./ods2_dump_header "$img" 1 | sed -n 's/.*ext_fid=(\([0-9]*\),.*/\1/p' | head -1)
python3 -I - "$img" "$E" <<'PY'
import struct, sys
img, e = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b')
f.seek(512); home = f.read(512)
alt, ib, ibsz, cl = struct.unpack_from('<I', home, 8)[0], struct.unpack_from('<I', home, 24)[0], struct.unpack_from('<H', home, 32)[0], struct.unpack_from('<H', home, 14)[0]
# the extension header's bit is cleared, and the primary's EFBLK is put back to just before its slot (the new file's header is the last one inside)
f.seek(ib * 512); bm = bytearray(f.read(512)); bm[(e - 1) // 8] &= ~(1 << ((e - 1) % 8)); f.seek(ib * 512); f.write(bm)
f.seek((ib + ibsz) * 512); p = bytearray(f.read(512))
vbn = cl * 4 + ibsz + e
sw = lambda v: struct.pack('<HH', v >> 16, v & 0xFFFF)
p[28:32] = sw(vbn); p[32:34] = b'\0\0'
sum_ = sum(struct.unpack_from('<255H', p, 0)) & 0xFFFF
struct.pack_into('<H', p, 510, sum_)
f.seek((ib + ibsz) * 512); f.write(p); f.seek(alt * 512); f.write(p)
PY
for k in 1 2 3 4 5 6; do ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$img" "COPY $TMP/empty.txt [000000]Z$k.TXT" >/dev/null 2>&1; done
check "ODS-2: the extension header's slot is not given out: six more files, nothing wrong but the bit cleared on purpose" "0" \
    "$(python3 -I tests/ods5check.py "$img" 2>&1 | grep '^ERROR' | grep -vc 'valid but its index bitmap bit is clear')"
check "ODS-2: the extension header is still there (segment 1)" "1" "$(./ods2_dump_header "$img" "$E" 2>&1 | grep -c 'seg_num=1')"

# a volume whose index file chain cannot be followed to its end is not written (review 27 N4), and still read
i=0
$INIT "$TMP/broken.img" 8192 --structure=2 --cluster=1 --maxfiles=1500 --label=BROKEN >/dev/null 2>&1
while [ "$(chain_length "$TMP/broken.img")" -lt 1 ]; do
    i=$((i + 1))
    ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$TMP/broken.img" "COPY $TMP/empty.txt [000000]G$i.TXT" >/dev/null 2>&1
    [ $i -lt 400 ] || break
done
E=$(./ods2_dump_header "$TMP/broken.img" 1 | sed -n 's/.*ext_fid=(\([0-9]*\),.*/\1/p' | head -1)
python3 -I - "$TMP/broken.img" "$E" <<'PY'
import struct, sys
img, e = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b')
f.seek(512); home = f.read(512)
ib, ibsz, cl = struct.unpack_from('<I', home, 24)[0], struct.unpack_from('<H', home, 32)[0], struct.unpack_from('<H', home, 14)[0]
f.seek((ib + ibsz) * 512); h = f.read(512)
o = h[1] * 2; end = o + h[58] * 2; idx = []
while o < end:
    w = struct.unpack_from('<H', h, o)[0]; fmt = w >> 14
    if fmt == 1: idx.append((((w >> 8) & 0x3F) << 16 | struct.unpack_from('<H', h, o + 2)[0], (w & 0xFF) + 1)); o += 4
    elif fmt == 2: idx.append((struct.unpack_from('<I', h, o + 2)[0], (w & 0x3FFF) + 1)); o += 6
    else: o += 2
vbn, v, lbn = 4 * cl + ibsz + e, 1, None
for l, c in idx:
    if vbn < v + c: lbn = l + vbn - v; break
    v += c
f.seek(lbn * 512); f.write(bytes(512))                 # the extension header is gone
PY
check "the volume with a lost extension header is mounted read-only (a COPY is refused)" "2 1" "$($FAULT "$TMP/broken.img" "COPY $TMP/empty.txt [000000]NOPE.TXT" 2>&1 | grep -c 'mounted read-only\|COPYERR' | tr '\n' ' ')$($FAULT "$TMP/broken.img" "COPY $TMP/empty.txt [000000]NOPE.TXT" 2>&1 | grep -c 'COPYERR')"
check "... and it is still read" "1" "$(./ods2 "$TMP/broken.img" 'DIR [000000]G1.TXT' 2>&1 | grep -c 'G1.TXT')"

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_indexf_stall_check: $fails FAILED"
    exit 1
fi
echo "cli_indexf_stall_check: all passed"
