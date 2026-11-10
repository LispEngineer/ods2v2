#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_dirname_check.sh - an ODS-2 directory name cannot start with "-": DCL reads a "-" that starts a directory component as
# "the parent directory" ([-], [A.-.B]) and ODS-2 has no "^" escape, so VMS could not name any file inside such a directory
# (%CLI-W-PARSEFAIL, %RMS-F-SYN: 15 and 28 files in the OpenVMS VAX V7.3 after-images of the stress harness). CREATE/DIRECTORY
# and IMPORT refuse it, with a message and nothing made (no renaming); ods5check warns of one that exists (made by an older
# ods2v2). On ODS-5 the name is fine (VMS reaches it as ^-). Called by `make test`; run from the project root.

TMP=samples/cli_dirname_tmp
INIT=./ods2_init
ODS2=./ods2
FAULT=./ods2_fault
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
mkdir -p "$TMP/tree/-dash/sub" "$TMP/tree/ok" "$TMP/tree/a-b" "$TMP/tree/c-"
echo inside > "$TMP/tree/-dash/sub/f.txt"; echo ok > "$TMP/tree/ok/g.txt"; echo ab > "$TMP/tree/a-b/h.txt"; echo c > "$TMP/tree/c-/i.txt"

# ---- ODS-2
img="$TMP/two.img"
$INIT "$img" 8192 --structure=2 --label=DASH2 >/dev/null 2>&1
out=$($ODS2 "$img" "IMPORT $TMP/tree [000000]" 2>&1); rc=$?
case "$out" in *"-dash: an ODS-2 directory name cannot start with '-'"*"%RMS-F-SYN"*) msg=refused ;; *) msg="$out" ;; esac
check "ODS-2: IMPORT of a host directory named -dash: refused with the reason" refused "$msg"
check "... exit status 1, one error counted, the other directories imported (3 files)" "1 1 3" \
    "$rc $(echo "$out" | grep -c ', 1 error') $(echo "$out" | sed -n 's/.*IMPORTED, \([0-9]*\) files.*/\1/p')"
check "... nothing was made for -dash (no directory, no file below it), and nothing was renamed" "none" \
    "$($ODS2 "$img" 'DIR [000000...]*.*' 2>&1 | grep -qi 'dash\|sub\|F\.TXT' && echo found || echo none)"
check "... directories with a dash elsewhere in the name are fine (A-B, C-)" "yes" \
    "$($ODS2 "$img" 'DIR [A-B]H.TXT' 2>&1 | grep -q '^H.TXT' && $ODS2 "$img" 'DIR [C-]I.TXT' 2>&1 | grep -q '^I.TXT' && echo yes)"
check "ODS-2: ods5check clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
# the library refuses it too (CREATE/DIRECTORY cannot even type the name: DCL reads [-A] as the parent)
# ---- a directory made the old way: the checker warns
img="$TMP/old.img"
$INIT "$img" 8192 --structure=2 --label=DASHOLD >/dev/null 2>&1
ODS2_TEST_DASH_DIR=1 $FAULT "$img" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
out=$(python3 -I tests/ods5check.py "$img")
check "ODS-2 volume with a -DASH directory (older ods2v2): ods5check warns, once, and has no error" "1 0" \
    "$(echo "$out" | grep -c "^WARN.*directory '-DASH.DIR' starts with \"-\"") $(echo "$out" | grep -c '^ERROR')"
# ---- ODS-5: allowed
img="$TMP/five.img"
$INIT "$img" 8192 --structure=5 --label=DASH5 >/dev/null 2>&1
out=$($ODS2 "$img" "IMPORT $TMP/tree [000000]" 2>&1); rc=$?
check "ODS-5: IMPORT of -dash works (VMS names it ^-dash), exit 0, 4 files" "0 4" \
    "$rc $(echo "$out" | sed -n 's/.*IMPORTED, \([0-9]*\) files.*/\1/p')"
check "ODS-5: ods5check clean (no warning: VMS can reach it with ^-)" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
# LINK / SET FILE/ENTER cannot make a "-" directory name either (a link to a directory is refused outright; the dash rule stands
# in case that ever changes) - and a link to a plain file may start with "-" (files named -X are fine on VMS)
img="$TMP/lnk.img"
$INIT "$img" 8192 --structure=2 --label=DASHLNK >/dev/null 2>&1
$ODS2 "$img" "CREATE/DIRECTORY [REAL]" >/dev/null 2>&1
echo hi > "$TMP/f.txt"; $ODS2 "$img" "COPY $TMP/f.txt [000000]PLAIN.TXT" >/dev/null 2>&1
check "ODS-2: LINK [000000]-ALIAS.DIR to a directory is refused (exit 1), SET FILE/ENTER too, nothing is listed" "1 1 0" \
    "$($ODS2 "$img" 'LINK [000000]-ALIAS.DIR [000000]REAL.DIR' >/dev/null 2>&1; echo $?) $($ODS2 "$img" 'SET FILE/ENTER=[000000]-ALIAS.DIR [000000]REAL.DIR' >/dev/null 2>&1; echo $?) $($ODS2 "$img" 'DIR [000000]-ALIAS.DIR' 2>&1 | grep -c '^-ALIAS')"
check "ODS-2: a link named -X.TXT to a plain file is fine" "0" "$($ODS2 "$img" 'LINK [000000]-X.TXT [000000]PLAIN.TXT' >/dev/null 2>&1; echo $?)"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_dirname_check: $fails FAILED"
    exit 1
fi
echo "cli_dirname_check: all checks passed"
