#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_deep_check.sh - trees deeper than the 255 levels ods2v2 creates. A VMS volume can hold more (OpenVMS V7.3-2
# made 400 levels; its own wildcard walk stops at 256), so DIR [...], EXPORT and IMPORT must not lose the part of a
# tree below some fixed depth, and must say so when something is not copied (host path limit).
# The trees are made by ods2_deep_make (the library, with ODS2_TEST_MAX_DIR_DEPTH raising the creation limit in that
# test build only: paths this deep cannot be typed into the CLI). Called by `make test`; run from the project root.

TMP=samples/cli_deep_tmp
INIT=./ods2_init
ODS2=./ods2
MAKE=./ods2_deep_make
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
mkdir -p "$TMP"

# ---- creation stays capped at 255 levels (the tool without the knob) ----
$INIT "$TMP/cap.img" 65536 --structure=5 --label=CAP >/dev/null 2>&1
out=$($MAKE "$TMP/cap.img" 256 2>&1); rc=$?
case "$out" in *"level 256"*"at most 255 levels"*) msg=refused ;; *) msg="$out" ;; esac
check "creating level 256 is refused (the cap is 255 by default)" "refused 1" "$msg $rc"

# ---- 300 levels, files at 50, 100, ... 300 ----
$INIT "$TMP/d300.img" 65536 --structure=5 --label=D300 >/dev/null 2>&1
out=$(ODS2_TEST_MAX_DIR_DEPTH=1000 $MAKE "$TMP/d300.img" 300 50 100 150 200 250 300 2>&1); rc=$?
check "300 levels made with the test knob" "made 300 levels 0" "$out $rc"
check "ods5check: a 300-level tree is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/d300.img" 2>&1 | tail -1)"
out=$($ODS2 "$TMP/d300.img" 'DIR [L1...]F*.TXT' 2>&1); rc=$?
check "DIR [L1...]F*.TXT lists all six files, in six directories, below level 255 too" \
    "6 files; Grand total of 6 directories, 6 files. 0" \
    "$(echo "$out" | grep -c '^F[0-9]*\.TXT') files; $(echo "$out" | grep 'Grand total') $rc"
check "... including the one at level 300" 1 "$(echo "$out" | grep -c '^Directory \[L1\.L2\..*\.L299\.L300\]')"
out=$($ODS2 "$TMP/d300.img" 'DIR [000000...]*.*' 2>&1); rc=$?
check "DIR [000000...]*.* walks every level: 301 directories shown, no TOODEEP, exit 0" "301 0 0" \
    "$(echo "$out" | grep -c '^Directory \[') $(echo "$out" | grep -c TOODEEP) $rc"
rm -rf "$TMP/out300"
out=$($ODS2 "$TMP/d300.img" "EXPORT [L1...]*.* $TMP/out300" 2>&1); rc=$?
check "EXPORT of the tree: exit 0, no error" "0 0" "$rc $(echo "$out" | grep -c 'ODS2-E')"
check "... six files, 299 directories below [L1] (300 with the destination)" "6 300" \
    "$(find "$TMP/out300" -type f | wc -l) $(find "$TMP/out300" -type d | wc -l)"
deep=$(find "$TMP/out300" -type f -name F300.TXT)
check "... the file at level 300 is where its path says and holds its content" "deep 300 300" \
    "$(cat "$deep" | tr '\n' ' ' | sed 's/ $//') $(echo "$deep" | tr -cd '/' | awk '{print length($0) - '$(echo "$TMP/out300" | tr -cd '/' | awk '{print length($0)}')'}')"
rm -rf "$TMP/out300"

# ---- 1,200 levels: the Linux path limit (4,096) is reached; nothing is cut short, every loss is reported ----
$INIT "$TMP/d1200.img" 131072 --structure=5 --label=D1200 >/dev/null 2>&1
out=$(ODS2_TEST_MAX_DIR_DEPTH=2000 $MAKE "$TMP/d1200.img" 1200 400 700 1200 2>&1); rc=$?
check "1,200 levels made" "made 1200 levels 0" "$out $rc"
check "ods5check: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/d1200.img" 2>&1 | tail -1)"
out=$($ODS2 "$TMP/d1200.img" 'DIR [L1...]F*.TXT' 2>&1); rc=$?
check "DIR [L1...]F*.TXT: all three files, at levels 400, 700 and 1200 (a 6,000-character path), exit 0" \
    "3 files; Grand total of 3 directories, 3 files. 0" \
    "$(echo "$out" | grep -c '^F[0-9]*\.TXT') files; $(echo "$out" | grep 'Grand total') $rc"
rm -rf "$TMP/out1200"
out=$($ODS2 "$TMP/d1200.img" "EXPORT [L1...]*.* $TMP/out1200" 2>&1); rc=$?
check "EXPORT: exit status 1 (something was not copied)" 1 "$rc"
check "... F400.TXT and F700.TXT are exported" "2" "$(find "$TMP/out1200" -type f | wc -l)"
case "$out" in *"PATHTOOLONG, [L1."*"F1200.TXT: the Linux path would be too long"*) msg=named ;; *) msg="$out" ;; esac
check "... the file that could not be copied is named (F1200.TXT), with its VMS directory" named "$msg"
check "... the directories that could not be made are named once and counted" 1 \
    "$(echo "$out" | grep -c 'directories in all (from \[L1\.')"
check "... the summary line counts the errors" 1 "$(echo "$out" | grep -c 'EXPORTED.*errors: not everything was copied')"
longest=$(find "$TMP/out1200" | awk '{ if (length($0) > m) m = length($0) } END { print m }')
check "... no Linux path is longer than 4,000 characters and none was cut (the files hold their content)" "ok" \
    "$([ "$longest" -lt 4000 ] && [ "$(cat $(find "$TMP/out1200" -name F700.TXT))" = 'deep 700' ] && echo ok)"
rm -rf "$TMP/out1200"

# ---- the bound of the walk (2,048 levels): a tree one level deeper than it is reported, not crashed on or cut silently ----
$INIT "$TMP/d2100.img" 131072 --structure=5 --label=D2100 >/dev/null 2>&1
out=$(ODS2_TEST_MAX_DIR_DEPTH=3000 $MAKE "$TMP/d2100.img" 2100 2000 2100 2>&1); rc=$?
check "2,100 levels made" "made 2100 levels 0" "$out $rc"
for bin in ./ods2 ./ods2_fault; do
    out=$($bin "$TMP/d2100.img" 'DIR [000000...]F*.TXT' 2>&1); rc=$?
    case "$out" in *"E-TOODEEP, [L1.L2."*"L2048] is more than 2048 levels deep: this directory ("*" entries) and everything below it are skipped"*) msg=named ;; *) msg="$(echo "$out" | tail -3)" ;; esac
    check "$bin: DIR [000000...] on a 2,100-level tree: one TOODEEP naming the directory and its entry count, exit 1, no crash" "named 1 1" \
        "$msg $rc $(echo "$out" | grep -c TOODEEP)"
    check "$bin: ... the file at level 2000, above the bound, is listed" 1 "$(echo "$out" | grep -c '^F2000\.TXT')"
done

# ---- IMPORT into a deep tree and of a deep tree ----
# a Linux tree 300 levels deep is imported up to the creation cap (255): the directories below it are reported
mkdir -p "$TMP/imp"
p="$TMP/imp"; i=1
while [ $i -le 260 ]; do p="$p/d$i"; i=$((i + 1)); done
mkdir -p "$p"; echo deep > "$p/f.txt"
$INIT "$TMP/imp.img" 65536 --structure=5 --label=IMP >/dev/null 2>&1
out=$($ODS2 "$TMP/imp.img" "IMPORT $TMP/imp [000000]" 2>&1); rc=$?
case "$out" in *"CREATEERR"*"at most 255 levels"*) msg=reported ;; *) msg="$out" ;; esac
check "IMPORT of a 260-level tree: stops at the 255-level cap, says so, exit 1" "reported 1" "$msg $rc"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_deep_check: $fails FAILED"
    exit 1
fi
echo "cli_deep_check: all checks passed"
