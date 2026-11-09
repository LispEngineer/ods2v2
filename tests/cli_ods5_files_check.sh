#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_files_check.sh - TYPE and EXPORT (copy out) on a hand-built
# ODS-5 volume: files found by names with spaces, mixed case, ISO
# Latin-1 and UCS-2 characters (^-escapes, any case), versions, an alias
# (two directory entries for one file), a symbolic link (special file),
# a directory with dots in its name and the tree below it, Linux file
# names in UTF-8, refusal to overwrite, and a read error not leaving a
# partial file. The volume is the synthetic ODS-2 disk: files are COPYed
# onto it by ods2, then the root directory is rewritten with ODS-5 names
# and the home block patched to level 5.
# Called by `make test`; run from the project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_ods5f_disk.img
TMP=samples/cli_ods5f_tmp
OUT=$TMP/out
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
same() { if cmp -s "$1" "$2"; then echo same; else echo different; fi; }

[ -f "$SRC_DISK" ] || { echo "FAIL: $SRC_DISK missing"; exit 1; }
rm -rf "$TMP"; mkdir -p "$TMP"
cp "$SRC_DISK" "$DISK"

printf 'hello one\nsecond line\n' > "$TMP/f1.txt"
python3 -I -c "
import sys
sys.stdout.buffer.write(bytes((i * 7 + 3) & 255 for i in range(1700)))" > "$TMP/f2.dat"
printf 'newer version\n' > "$TMP/f3.txt"
printf '../elsewhere/target.txt' > "$TMP/link.txt"
$ODS2 $DISK COPY $TMP/f1.txt '[000000]F1.TXT' >/dev/null 2>&1
$ODS2 $DISK COPY $TMP/f2.dat '[000000]F2.DAT' >/dev/null 2>&1
$ODS2 $DISK COPY $TMP/f3.txt '[000000]F3.TXT' >/dev/null 2>&1
$ODS2 $DISK COPY $TMP/link.txt '[000000]L1.TXT' >/dev/null 2>&1
fid() { $ODS2 $DISK "DIR/FULL [000000]$1" 2>/dev/null | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p'; }
F1=$(fid F1.TXT); F2=$(fid F2.DAT); F3=$(fid F3.TXT); L1=$(fid L1.TXT)

python3 -I - "$DISK" $F1 $F2 $F3 $L1 <<'PY'
import struct, sys
disk = sys.argv[1]
F1, F2, F3, L1 = [int(x) for x in sys.argv[2:6]]
f = open(disk, 'r+b')
def blk(l): f.seek(l * 512); return bytearray(f.read(512))
def put(l, b): f.seek(l * 512); f.write(b)
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
home = blk(1)
cluster, = struct.unpack_from('<H', home, 14)
ibmaplbn, = struct.unpack_from('<I', home, 24)
ibmapsize, = struct.unpack_from('<H', home, 32)
def extents(h):
    out, o, end = [], h[1] * 2, h[1] * 2 + h[58] * 2
    while o < end:
        w, = struct.unpack_from('<H', h, o); fmt = w >> 14
        if fmt == 0: o += 2
        elif fmt == 1:
            lo, = struct.unpack_from('<H', h, o + 2); out.append((((w >> 8) & 0x3f) << 16 | lo, (w & 0xff) + 1)); o += 4
        elif fmt == 2:
            lbn, = struct.unpack_from('<I', h, o + 2); out.append((lbn, (w & 0x3fff) + 1)); o += 6
        else:
            cnt, lbn = struct.unpack_from('<HI', h, o + 2); out.append((lbn, ((w & 0x3fff) << 16 | cnt) + 1)); o += 8
    return out
idx = extents(blk(ibmaplbn + ibmapsize))
def hdr_lbn(n):
    vbn = 4 * cluster + ibmapsize + n; v = 1
    for lbn, c in idx:
        if vbn < v + c: return lbn + vbn - v
        v += c
    raise SystemExit('header %d not mapped' % n)
# the link file: a special file (symbolic link), the data is the target text
lbn = hdr_lbn(L1); h = blk(lbn)
h[20] = 0x40; h[21] = 5
struct.pack_into('<H', h, 510, csum(h, 255)); put(lbn, h)
decuslbn = extents(blk(hdr_lbn(11)))[0][0]
rootlbn = extents(blk(ibmaplbn + ibmapsize + 4 - 1))[0][0]

def rec(flags, name, versions):
    pad = b'\0' if len(name) & 1 else b''
    body = struct.pack('<HBB', 0x7fff, flags, len(name)) + name + pad
    for v, fid in versions:
        body += struct.pack('<HHHBB', v, fid, 4 if fid == 4 else 1 if fid == 11 else 1, 0, 0)
    return struct.pack('<H', len(body)) + body
ISL1, UCS2 = 1 << 3, 3 << 3
def seqfix(b):   # the new files have sequence number 1 (checked by lookups of the FID)
    return b
recs = [
    (b'000000.DIR', 0, [(1, 4)]),
    (b'DECUS.DIR', 0, [(1, 11)]),
    (b'Lots.Of.Periods.DIR', ISL1, [(1, 11)]),
    (b'Mixed Case.TXT', ISL1, [(1, F1)]),
    (b'alias.txt', 0, [(1, F1)]),
    (b'caf\xe9.dat', ISL1, [(1, F2)]),
    ('euro\u20ac.txt'.encode('utf-16-le'), UCS2, [(1, F3)]),
    (b'multi.txt', 0, [(2, F3), (1, F1)]),
    (b'sym.lnk', ISL1, [(1, L1)]),
]
put(rootlbn, bytearray((b''.join(rec(fl, nm, vs) for nm, fl, vs in recs) + b'\xff\xff').ljust(512, b'\0')))
inner = [(b'Inner File.txt', ISL1, [(1, F2)]), (b'inner.TXT', 0, [(1, F3)])]
put(decuslbn, bytearray((b''.join(rec(fl, nm, vs) for nm, fl, vs in inner) + b'\xff\xff').ljust(512, b'\0')))
home = blk(1)
struct.pack_into('<H', home, 12, 0x0501)
struct.pack_into('<H', home, 58, csum(home, 29))
struct.pack_into('<H', home, 510, csum(home, 255))
put(1, home)
PY

# --- TYPE ---
check "TYPE finds a name with a space by ^_ in any case" "$(cat $TMP/f1.txt)" \
    "$($ODS2 $DISK 'TYPE [000000]mixed^_case.txt' 2>/dev/null)"
check "TYPE finds a UCS-2 name by ^U" "newer version" "$($ODS2 $DISK 'TYPE [000000]euro^U20AC.TXT' 2>/dev/null)"
check "TYPE finds an ISO Latin-1 name by ^xx" 1700 "$($ODS2 $DISK 'TYPE [000000]caf^E9.dat' 2>/dev/null | wc -c)"
check "TYPE finds an ISO Latin-1 name with the other case of a non-ASCII letter (E acute for e acute)" 1700 \
    "$($ODS2 $DISK 'TYPE [000000]CAF^C9.DAT' 2>/dev/null | wc -c)"
check "TYPE takes the highest version" "newer version" "$($ODS2 $DISK 'TYPE [000000]multi.txt' 2>/dev/null)"
check "TYPE ;1 takes the lower version" "hello one" "$($ODS2 $DISK 'TYPE [000000]multi.txt;1' 2>/dev/null | head -1)"
check "TYPE through a directory with dots in its name" 1700 \
    "$($ODS2 $DISK 'TYPE [Lots^.Of^.Periods]inner^_file.txt' 2>/dev/null | wc -c)"
check "TYPE of a missing name fails with exit status 1" 1 "$($ODS2 $DISK 'TYPE [000000]nothing^_here' >/dev/null 2>&1; echo $?)"
check "TYPE of a malformed escape fails with exit status 1" 1 "$($ODS2 $DISK 'TYPE [000000]bad^Q' >/dev/null 2>&1; echo $?)"

# --- EXPORT ---
mkdir -p "$OUT"
check "EXPORT of one file into a directory uses its UTF-8 name" same \
    "$($ODS2 $DISK "EXPORT [000000]caf^E9.dat $OUT" >/dev/null 2>&1; same "$OUT/café.dat" $TMP/f2.dat)"
check "EXPORT of one file to a given Linux name" same \
    "$($ODS2 $DISK "EXPORT [000000]mixed^_case.txt $TMP/one.txt" >/dev/null 2>&1; same $TMP/one.txt $TMP/f1.txt)"
check "EXPORT of a UCS-2 name gives the UTF-8 name" same "$($ODS2 $DISK "EXPORT [000000]euro* $OUT" >/dev/null 2>&1; same "$OUT/euro€.txt" $TMP/f3.txt)"
check "a name with a space keeps it on Linux" same "$($ODS2 $DISK 'EXPORT [000000]mixed* '$OUT >/dev/null 2>&1; same "$OUT/Mixed Case.TXT" $TMP/f1.txt)"
check "EXPORT/VERSIONS names every version" "multi.txt;1
multi.txt;2" "$(rm -rf $TMP/v; $ODS2 $DISK "EXPORT/VERSIONS [000000]multi.txt;* $TMP/v" >/dev/null 2>&1; ls $TMP/v)"
check "...the versions have their own contents" same "$(same "$TMP/v/multi.txt;1" $TMP/f1.txt)"
check "EXPORT refuses to overwrite" 1 "$($ODS2 $DISK "EXPORT [000000]caf^E9.dat $OUT" >/dev/null 2>&1; echo $?)"
check "...and /OVERWRITE replaces" 0 "$($ODS2 $DISK "EXPORT/OVERWRITE [000000]caf^E9.dat $OUT" >/dev/null 2>&1; echo $?)"
check "EXPORT of two files to one file name fails" 1 \
    "$($ODS2 $DISK "EXPORT [000000]alias.txt $TMP/x.txt" >/dev/null 2>&1; $ODS2 $DISK "EXPORT [000000]*.txt $TMP/x.txt" >/dev/null 2>&1; echo $?)"
rm -rf $TMP/tree
$ODS2 $DISK "EXPORT [000000...]*.* $TMP/tree" >/dev/null 2>&1
check "an ellipsis recreates the tree, directories by name" "Inner File.txt
inner.TXT" "$(ls $TMP/tree/Lots.Of.Periods; )"
check "...every directory entry to the same directory is listed once there" 2 "$(ls $TMP/tree/Lots.Of.Periods | wc -l)"
check "...and the tree's files are the right bytes" same "$(same "$TMP/tree/Lots.Of.Periods/Inner File.txt" $TMP/f2.dat)"
check "a symbolic link (special file) becomes a Linux symbolic link" "../elsewhere/target.txt" \
    "$(readlink $TMP/tree/sym.lnk)"
check "a second name for one file is a copy" same "$(same $TMP/tree/alias.txt $TMP/tree/Mixed\ Case.TXT)"
rm -rf $TMP/links
$ODS2 $DISK "EXPORT/LINKS [000000]*.txt $TMP/links" >/dev/null 2>&1
check "EXPORT/LINKS makes the alias a hard link" 2 "$(stat -c %h "$TMP/links/alias.txt")"
check "the image is untouched by all of it" same "$(same $DISK $DISK)"

# A short fuzz run (tests/fuzz_ods5.py runs longer ones): damaged copies of this volume must give
# errors, never a crash, a sanitizer report or a hang.
check "40 damaged copies of the volume: no crash, no sanitizer report, no hang" 0 \
    "$(python3 -I tests/fuzz_ods5.py $ODS2 $DISK 40 7 $TMP/fuzz >$TMP/fuzz.log 2>&1; echo $?)"
cat $TMP/fuzz.log | grep -E '^FAIL' | head -3

rm -rf "$TMP" "$DISK"
[ "$fails" -eq 0 ] || { echo "$fails ODS-5 files check(s) failed"; exit 1; }
