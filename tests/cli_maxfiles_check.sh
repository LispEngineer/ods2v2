#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_maxfiles_check.sh - ods2_init clips --maxfiles the way VMS INITIALIZE clips /MAXIMUM_FILES (review 61 F1): to blocks / (cluster + 1) (CORPUS 38: "INITIALIZE clips
# /MAXIMUM_FILES to blocks/2: 2,048 for 4,096 blocks, 4,096 for 8,192" at cluster 1; the same family as the default, blocks / (2 * cluster + 2)), with a note, so that a volume of
# ods2v2's own making never has more files than blocks (ods5check refuses an SCB volume size smaller than the home block's maximum file count: review 59 S0). Only the blocks bound is
# enforced (review 61 recheck: VMS ran the battery's a6-cluBIG, cluster 2048, with 2,000 files on five versions, though INITIALIZE itself clips to blocks / (cluster + 1): CORPUS 62); between blocks / (cluster + 1) and blocks the count is kept and a
# note says what VMS INITIALIZE would allow; the floor of 64 is noted with the number written. --maxfiles-over-limit keeps even a count above the blocks. Called by `make test`; run from the project root.
INIT=./ods2_init
TMP=${TMPDIR:-samples}/maxfiles_check.$$
fails=0
mkdir -p "$TMP"
check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1"; echo "      expected: $2"; echo "      got:      $3"; fails=$((fails + 1)); fi
}
maxfiles() { python3 -I -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(512); print(struct.unpack_from('<I', f.read(512), 28)[0])" "$1"; }
clean() { python3 -I tests/ods5check.py "$1" 2>&1 | tail -1; }
for level in 2 5; do
    img="$TMP/a$level.img"; out=$($INIT "$img" 8192 --structure=$level --maxfiles=8193 2>&1)
    check "ODS-$level 8192 blocks --maxfiles=8193: clipped to 8192 (a volume cannot have more files than blocks)" "8192" "$(maxfiles "$img")"
    check "ODS-$level: the note says it clipped, to which number" "1" "$(echo "$out" | grep -c 'clipped to 8192')"
    check "ODS-$level: ods5check is clean (the SCB volume size is not smaller than the file count)" "0 error(s), 0 warning(s)" "$(clean "$img")"
    img="$TMP/b$level.img"; out=$($INIT "$img" 8192 --structure=$level --maxfiles=4096 2>&1)
    check "ODS-$level --maxfiles=4096 on 8192 blocks: kept, no note" "4096 0" "$(maxfiles "$img") $(echo "$out" | grep -c 'ods2_init:')"
    img="$TMP/c$level.img"; out=$($INIT "$img" 8192 --structure=$level --maxfiles=5000 2>&1)
    check "ODS-$level --maxfiles=5000 on 8192 blocks: KEPT (between blocks / (cluster + 1) and blocks), with a note that VMS INITIALIZE would allow at most 4096" "5000 1" "$(maxfiles "$img") $(echo "$out" | grep -c 'VMS INITIALIZE would allow at most 4096')"
    img="$TMP/d$level.img"; out=$($INIT "$img" 16384 --structure=$level --cluster=8 --maxfiles=3000 2>&1)
    check "ODS-$level 16384 blocks, cluster 8, --maxfiles=3000: kept; the note names 1820 (16384 / 9)" "3000 1" "$(maxfiles "$img") $(echo "$out" | grep -c 'at most 1820')"
    img="$TMP/e$level.img"; out=$($INIT "$img" 262144 --structure=$level --cluster=2048 --maxfiles=2000 2>&1)
    check "ODS-$level 262144 blocks, cluster 2048, --maxfiles=2000 (the battery's a6-cluBIG; VMS ran it with 2,000 files on five versions): kept" "2000" "$(maxfiles "$img")"
    check "ODS-$level: and ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    img="$TMP/f$level.img"; out=$($INIT "$img" 8192 --structure=$level --maxfiles=20000 --maxfiles-over-limit 2>&1)
    check "ODS-$level --maxfiles-over-limit keeps 20000 on 8192 blocks (and ods5check says so: an error)" "20000 1" "$(maxfiles "$img") $(clean "$img" | grep -c '^1 error')"
    img="$TMP/g$level.img"; $INIT "$img" 8192 --structure=$level >/dev/null 2>&1
    check "ODS-$level the default (blocks / (2 * cluster + 2)) is not touched: 2048" "2048" "$(maxfiles "$img")"
    img="$TMP/h$level.img"; out=$($INIT "$img" 8192 --structure=$level --maxfiles=10 2>&1)
    check "ODS-$level --maxfiles=10: raised to the floor, 64, and the note names the number written" "64 1" "$(maxfiles "$img") $(echo "$out" | grep -c 'raised to 64')"
    img="$TMP/i$level.img"; out=$($INIT "$img" 8192 --structure=$level --cluster=512 --maxfiles=100 2>&1)
    check "ODS-$level 8192 blocks, cluster 512, --maxfiles=100: kept, the note says VMS would allow at most 15 (it is below the floor of 64)" "100 1" "$(maxfiles "$img") $(echo "$out" | grep -c 'at most 15')"
done
rm -rf "$TMP"
if [ "$fails" -eq 0 ]; then echo "cli_maxfiles_check: all checks passed"; else echo "cli_maxfiles_check: $fails FAILED"; fi
exit $fails
