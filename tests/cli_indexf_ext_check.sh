#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_indexf_ext_check.sh - INDEXF.SYS growing past its primary header's map (LARGE_VOLUMES.md 2): ods2v2 grew the index file in
# pieces mapped in the primary header only, about 73 of them, and then refused every new file ("would need an extension header
# - not yet implemented"). It now lengthens the last pointer when the new blocks follow it, and when a header's map is full
# the next pointer goes into an extension header of INDEXF.SYS (made in a free slot of the blocks just added), up to
# FH2$W_SEG_NUM's 65,535 headers; mount follows the whole chain. ODS2_TEST_INDEXF=step=2,nomerge (the ods2_fault build only)
# makes the index file grow two blocks at a time in separate pieces so that a few hundred files fill the map area.
# The pieces here are Format 1 pointers (4 bytes), which never leave a header with 4 bytes of map: Format 2 pieces, the
# interruption of a growth and a stuck index file are tests/cli_indexf_stall_check.sh and tests/fault_inject.py (index_*).
# Not measured on VMS: no VMS-made index file with an extension header was available. Called by `make test`; run from the
# project root.

TMP=samples/cli_indexf_ext_tmp
INIT=./ods2_init
ODS2=./ods2
FAULT=./ods2_fault
FILES=${INDEXF_FILES:-600}
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
mkdir -p "$TMP/tree"
i=1
while [ $i -le $FILES ]; do echo "file $i of the tree" > "$TMP/tree/F$i.TXT"; i=$((i + 1)); done

for level in 2 5; do
    img="$TMP/ix$level.img"
    $INIT "$img" 8192 --structure=$level --cluster=1 --maxfiles=$((FILES + 400)) --label=IX$level >/dev/null 2>&1
    out=$(ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$img" "IMPORT $TMP/tree [000000]" 2>&1)
    check "ODS-$level: IMPORT of $FILES files, INDEXF.SYS growing two blocks at a time in separate pieces" "1" "$(echo "$out" | grep -c "IMPORTED, $FILES files")"
    chain=$(chain_length "$img")
    check "ODS-$level: INDEXF.SYS has extension headers (more than 1; chain $chain)" "yes" "$([ "$chain" -gt 1 ] && echo yes || echo no)"
    check "ODS-$level: ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
    check "ODS-$level: the backup index file header equals the primary (byte for byte)" "same" "$(python3 -I - "$img" "$level" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb')
f.seek(512); home = f.read(512)
alt = struct.unpack_from('<I', home, 8)[0]
ib = struct.unpack_from('<I', home, 24)[0]; ibsz = struct.unpack_from('<H', home, 32)[0]
f.seek((ib + ibsz) * 512); primary = f.read(512)
f.seek(alt * 512); backup = f.read(512)
print('same' if primary == backup else 'different')
PY
)"
    rm -rf "$TMP/out$level"
    $ODS2 "$img" "EXPORT [000000]*.* $TMP/out$level" >/dev/null 2>&1
    same=$(for n in 1 2 77 300 $FILES; do cmp "$TMP/out$level/F$n.TXT" "$TMP/tree/F$n.TXT" >/dev/null 2>&1 && echo ok; done | wc -l)
    check "ODS-$level: a fresh mount (no test hook) reads the whole index file: 5 sampled files exported identical, $FILES files in all" "5 $FILES" \
        "$same $(ls "$TMP/out$level" | grep -c '^F[0-9]*\.TXT$')"
    out=$($ODS2 "$img" 'DELETE [000000]F*.TXT;*' 2>&1)
    check "ODS-$level: every file deleted" "1" "$(echo "$out" | grep -c "$FILES entries deleted")"
    check "ODS-$level: ods5check after the deletes: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
    out=$(ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$img" "IMPORT $TMP/tree [000000]" 2>&1)
    check "ODS-$level: the freed header slots are reused, and the index file can still grow: the tree imported again" "1" "$(echo "$out" | grep -c "IMPORTED, $FILES files")"
    check "ODS-$level: ods5check: clean again" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
done

# one block at a time (review 27 F3): a piece of one block has no slot for an extension header besides the new file's own;
# the header is made when a later piece has one, and the index file goes on growing
for level in 2 5; do
    img="$TMP/one$level.img"
    $INIT "$img" 8192 --structure=$level --cluster=1 --maxfiles=1500 --label=ONE$level >/dev/null 2>&1
    out=$(ODS2_TEST_INDEXF=step=1,nomerge $FAULT "$img" "IMPORT $TMP/tree [000000]" 2>&1)
    check "ODS-$level: IMPORT of $FILES files, INDEXF.SYS growing one block at a time" "1" "$(echo "$out" | grep -c "IMPORTED, $FILES files")"
    check "ODS-$level: ods5check is clean (one block at a time)" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
done

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_indexf_ext_check: $fails FAILED"
    exit 1
fi
echo "cli_indexf_ext_check: all checks passed"
