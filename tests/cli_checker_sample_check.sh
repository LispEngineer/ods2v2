#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_checker_sample_check.sh - tests/ods5check.py on a volume with a very long index file (LARGE_VOLUMES.md 12). The checker
# kept every header and ident in memory (about 2 KB each: 16 million headers would need 30 GB) and looked a header's block up
# by walking INDEXF.SYS's extent list (a list that grows with the volume). The lookup is a binary search now, and --sample=K
# reads only headers 1-16 and every Kth, checking each one's own structure and the index file and its bitmap in full, for a
# volume too big for the whole check (the output says what was not done). The volume here has an index file of many pieces and
# extension headers (ODS2_TEST_INDEXF in the ods2_fault build). Called by `make test`; run from the project root.

TMP=samples/cli_checker_sample_tmp
INIT=./ods2_init
FAULT=./ods2_fault
CHK="python3 -I tests/ods5check.py"
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

rm -rf "$TMP"
mkdir -p "$TMP/tree"
i=1
while [ $i -le 700 ]; do echo "f$i" > "$TMP/tree/F$i.TXT"; i=$((i + 1)); done
img="$TMP/s.img"
$INIT "$img" 20000 --structure=2 --cluster=1 --maxfiles=1500 --label=SAMPLE >/dev/null 2>&1
ODS2_TEST_INDEXF=step=2,nomerge $FAULT "$img" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
check "the whole check of a volume with a long index file (extension headers): clean" "0 error(s), 0 warning(s)" "$($CHK "$img" 2>&1 | tail -1)"
out=$($CHK --sample=7 "$img" 2>&1)
check "--sample=7: clean, and says what it did not do" "0 error(s), 0 warning(s) 1" "$(echo "$out" | tail -1) $(echo "$out" | grep -c '^sampled: .* NOT done')"
check "--sample=7 reads headers 1-16 and every 7th (fewer than all of them)" "yes" "$(echo "$out" | sed -n 's/^sampled: \([0-9]*\) of \([0-9]*\) headers.*/\1 \2/p' | awk '{print ($1 > 16 && $1 < $2 / 3) ? "yes" : "no " $0}')"
check "--sample=1 is refused (usage), exit status 2" "2" "$($CHK --sample=1 "$img" >/dev/null 2>&1; echo $?)"
# a damaged header among the first 16: the sampled run finds it (header 3: BADBLK.SYS, checksum word)
python3 -I - "$img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); home = f.read(512)
ib = struct.unpack_from('<I', home, 24)[0]; sz = struct.unpack_from('<H', home, 32)[0]
f.seek((ib + sz + 2) * 512 + 510); f.write(b'\x01\x02')      # header 3's checksum
PY
check "a damaged reserved header is found by the sampled run too: exit status 1" "1" "$($CHK --sample=7 "$img" >/dev/null 2>&1; echo $?)"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_checker_sample_check: $fails FAILED"
    exit 1
fi
echo "cli_checker_sample_check: all checks passed"
