#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_dirsearch_check.sh - a directory of 32 blocks or more is searched by halving blocks (LARGE_VOLUMES.md 8): a name lookup
# reads about log2(blocks) blocks instead of all, and a lookup that finds no such name stops at the first name past it. The
# directory here has 3,000 names (about 190 blocks), one version each (the names with many versions,
# which run across records and blocks, are tests/ods2_dirsearch_selftest.c). Every find and every miss must give the same
# answer as the whole-directory walk did: first, last, a name between two others, a name before the first and after the last. Called by `make test`; run from the
# project root.

TMP=samples/cli_dirsearch_tmp
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

rm -rf "$TMP"
mkdir -p "$TMP/tree/D"
python3 -I - "$TMP/tree/D" <<'PY'
import sys
for i in range(0, 3000):
    open('%s/N%05d.TXT' % (sys.argv[1], i * 2), 'w').write('even %d\n' % (i * 2))     # even numbers only: the odd ones are misses between
PY
img="$TMP/d.img"
$INIT "$img" 30000 --structure=2 --cluster=1 --maxfiles=4000 --label=DSEARCH >/dev/null 2>&1
$ODS2 "$img" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
blocks=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' 2>&1 | sed -n 's/.*Size: *\([0-9]*\)\/.*/\1/p' | head -1)
check "the directory is 32 blocks or more (it has $blocks)" "yes" "$([ "${blocks:-0}" -ge 32 ] && echo yes || echo no)"
t() { $ODS2 "$img" "TYPE [D]$1" 2>&1 | tail -1; }
check "TYPE of the first name" "even 0" "$(t N00000.TXT)"
check "TYPE of the last name" "even 5998" "$(t N05998.TXT)"
check "TYPE of one in the middle" "even 3000" "$(t N03000.TXT)"
n=1; bad=0
while [ $n -le 5997 ]; do
    odd=$(printf 'N%05d.TXT' $n)
    case "$(t $odd)" in *"not found"*) ;; *) bad=$((bad + 1)) ;; esac
    n=$((n + 600))
done
check "names that sort between two existing ones (sampled odd numbers) are not found" "0" "$bad"
check "a name before the first and one after the last are not found" "1 1" "$(t A.TXT | grep -c "not found") $(t Z.TXT | grep -c "not found")"
# COPY onto an existing name makes a new version (PLAN_WRITE_OPS item 1: ;2, as VMS does; the highest version is the one TYPE reads)
for n in 0 1500 5998; do
    nm=$(printf 'N%05d.TXT' $n)
    echo "version two of $n" > "$TMP/v.txt"
    $ODS2 "$img" "COPY $TMP/v.txt [D]$nm" >/dev/null 2>&1
done
check "COPY of an existing name makes version 2: TYPE gives the new content, ;1 keeps the old" "version two of 1500 even 1500" "$(t 'N01500.TXT') $(t 'N01500.TXT;1')"
check "DIR of one name, every version: two files, in the order 2 1" "2 1" "$($ODS2 "$img" 'DIR [D]N01500.TXT;*' 2>&1 | awk '/^N01500/ { v = $0; sub(/.*;/, "", v); printf "%s ", v }' | sed 's/ $//')"
# delete one, then look for it and its neighbours
$ODS2 "$img" 'DELETE [D]N02000.TXT;*' >/dev/null 2>&1
check "after DELETE the name is not found and its neighbours are" "1 even 1998 even 2002" "$(t N02000.TXT | grep -c 'not found') $(t N01998.TXT) $(t N02002.TXT)"
# create into the middle of the sorted directory and find it
echo "new middle" > "$TMP/m.txt"
$ODS2 "$img" "COPY $TMP/m.txt [D]N02001.TXT" >/dev/null 2>&1
check "a new name between two others is found, in order: its neighbours are unchanged" "new middle even 1998 even 2002" "$(t N02001.TXT) $(t N01998.TXT) $(t N02002.TXT)"
check "ods5check: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_dirsearch_check: $fails FAILED"
    exit 1
fi
echo "cli_dirsearch_check: all checks passed"
