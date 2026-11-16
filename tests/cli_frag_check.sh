#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_frag_check.sh - a file in very many pieces (LARGE_VOLUMES.md 6 and 9). ods2v2 held a file's retrieval pointers in an
# array of 1,024 and its extension headers in 64: a file VMS stores in more pieces (a full volume of one-block holes) could
# not be read, deleted or exported, and one ods2v2 would have to write in more pieces was refused. The volume here is
# filled, every other one-block file is deleted (the holes), and a file as big as the holes is copied into them: one extent
# per hole. FRAG_FILES (default 6000: 3,000 extents) sets the number of small files; `make test-large` uses 20000 (10,000
# extents, more than 64 extension headers). Called by `make test`; run from the project root.

TMP=samples/cli_frag_tmp
INIT=./ods2_init
ODS2=./ods2
FILES=${FRAG_FILES:-6000}
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
mkdir -p "$TMP/tree/D"
python3 -I - "$TMP/tree/D" "$FILES" <<'PY'
import sys
d, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    open('%s/F%05d.TXT' % (d, i), 'w').write('x')
PY
holes=$((FILES / 2))
blocks=$((FILES * 3 + holes + 6000))
img="$TMP/f.img"
$INIT "$img" $blocks --structure=2 --cluster=1 --maxfiles=$((FILES + 2000)) --label=FRAG >/dev/null 2>&1
$ODS2 "$img" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
free=$(free_blocks "$img")
head -c $((free * 512)) /dev/zero > "$TMP/filler.bin"
out=$($ODS2 "$img" "COPY $TMP/filler.bin [000000]FILLER.BIN" 2>&1)
check "the volume is filled: free blocks 0" "0" "$(free_blocks "$img")"
for d in 1 3 5 7 9; do $ODS2 "$img" "DELETE [D]F*$d.TXT;*" >/dev/null 2>&1; done
check "every other small file deleted: $holes one-block holes" "$holes" "$(free_blocks "$img")"
head -c $((holes * 512)) /dev/urandom > "$TMP/big.bin"
out=$($ODS2 "$img" "COPY $TMP/big.bin [000000]BIG.BIN" 2>&1)
check "COPY of a $holes-block file into the holes" "1" "$(echo "$out" | grep -c 'COPIED')"
check "... the volume has no free block left" "0" "$(free_blocks "$img")"
check "ods5check: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
# how many extension headers the file has: follow FH2$W_EXT_FID from its primary header (ods2_dump_header prints ext_fid=(num,...))
n=$($ODS2 "$img" 'DIR/FULL BIG.BIN' 2>&1 | sed -n 's/.*File ID: *(\([0-9]*\),.*/\1/p' | head -1)
chain=0
while [ "$chain" -lt 70000 ]; do
    n=$(./ods2_dump_header "$img" "$n" 2>&1 | sed -n 's/.*ext_fid=(\([0-9]*\),.*/\1/p' | head -1)
    [ -n "$n" ] && [ "$n" != 0 ] || break
    chain=$((chain + 1))
done
echo "      ($chain extension headers)"
if [ "$FILES" -ge 12000 ]; then
    check "the file has more than 64 extension headers" "yes" "$([ "$chain" -gt 64 ] && echo yes || echo no)"
else
    check "the file has extension headers (more than the 1,024 extents of the old array need 8; chain $chain)" "yes" "$([ "$chain" -gt 8 ] && echo yes || echo no)"
fi
check "TYPE/RAW gives the same bytes" "same" "$($ODS2 "$img" 'TYPE/RAW BIG.BIN' 2>/dev/null | cmp - "$TMP/big.bin" >/dev/null 2>&1 && echo same || echo different)"
rm -f "$TMP/out.bin"
$ODS2 "$img" "EXPORT [000000]BIG.BIN $TMP/out.bin" >/dev/null 2>&1
check "EXPORT gives the same bytes" "same" "$(cmp "$TMP/out.bin" "$TMP/big.bin" >/dev/null 2>&1 && echo same || echo different)"
out=$($ODS2 "$img" 'DELETE BIG.BIN' 2>&1)
check "DELETE of the file: done, every block free again" "1 $holes" "$(echo "$out" | grep -c DELETED) $(free_blocks "$img")"
check "ods5check after the delete: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"

[ -n "$FRAG_KEEP" ] || rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_frag_check: $fails FAILED"
    exit 1
fi
echo "cli_frag_check: all checks passed"
