#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_maxfiles_check.sh - ods2_init clips --maxfiles the way VMS INITIALIZE clips /MAXIMUM_FILES (review 61 F1): to blocks / (cluster + 1) (CORPUS 38: "INITIALIZE clips
# /MAXIMUM_FILES to blocks/2: 2,048 for 4,096 blocks, 4,096 for 8,192" at cluster 1; the same family as the default, blocks / (2 * cluster + 2)), with a note, so that a volume of
# ods2v2's own making never has more files than blocks (ods5check refuses an SCB volume size smaller than the home block's maximum file count: review 59 S0). --maxfiles-over-limit
# keeps the number asked for. Called by `make test`; run from the project root.
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
    check "ODS-$level 8192 blocks --maxfiles=8193: clipped to 4096 (blocks / (cluster + 1))" "4096" "$(maxfiles "$img")"
    check "ODS-$level: the note says it clipped" "1" "$(echo "$out" | grep -c 'maxfiles.*clipped\|clipped.*maxfiles\|MAXFILES')"
    check "ODS-$level: ods5check is clean (the SCB volume size is not smaller than the file count)" "0 error(s), 0 warning(s)" "$(clean "$img")"
    img="$TMP/b$level.img"; $INIT "$img" 8192 --structure=$level --maxfiles=4096 >/dev/null 2>&1
    check "ODS-$level --maxfiles=4096 on 8192 blocks: kept (the bound itself)" "4096" "$(maxfiles "$img")"
    img="$TMP/c$level.img"; out=$($INIT "$img" 16384 --structure=$level --cluster=8 --maxfiles=3000 2>&1)
    check "ODS-$level 16384 blocks, cluster 8, --maxfiles=3000: clipped to 1820 (16384 / 9)" "1820" "$(maxfiles "$img")"
    check "ODS-$level: and ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    img="$TMP/d$level.img"; $INIT "$img" 8192 --structure=$level --maxfiles=5000 --maxfiles-over-limit >/dev/null 2>&1
    check "ODS-$level --maxfiles-over-limit keeps 5000 on 8192 blocks" "5000" "$(maxfiles "$img")"
    img="$TMP/e$level.img"; $INIT "$img" 8192 --structure=$level >/dev/null 2>&1
    check "ODS-$level the default (blocks / (2 * cluster + 2)) is not clipped: 2048" "2048" "$(maxfiles "$img")"
done
rm -rf "$TMP"
if [ "$fails" -eq 0 ]; then echo "cli_maxfiles_check: all checks passed"; else echo "cli_maxfiles_check: $fails FAILED"; fi
exit $fails
