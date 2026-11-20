#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_typewild_check.sh - TYPE with wildcards and comma lists (PLAN_WRITE_OPS item 3), ODS-2 and ODS-5. The expectations are what VMS itself did
# (CORPUS 49.3 on VAX V7.3, 49.8 on V8.4):
#   * for each file a wildcard finds: a line with one space, a line with the full name and version, a line with one space, then the records; the
#     device VMS prints in front of the name is not printed here ("[T]A1.TXT;1"). With no wildcard and no list there is no header;
#   * a comma list: the first file, when it is named without a wildcard, has no header; the others have one. An element with no directory takes the
#     previous element's;
#   * a wildcard without a version shows the highest version of each name, in name order; ;* every version, highest first; an empty file has its header and
#     no records; a 100-character and a 180-character record are written whole;
#   * nothing found: %TYPE-W-SEARCHFAIL, error searching for [T]*.NOSUCH; (the version ";" is added) and -RMS-E-FNF, file not found; a missing directory:
#     -RMS-E-DNF, directory not found and -SYSTEM-W-NOSUCHFILE, no such file; in a list the matches come first, the messages at the end;
#   * a wildcard in the directory part is still refused (%ODS2-E-WILDDIR): TYPE [T...] was measured on VMS but is not implemented.
# Called by `make test`; run from the project root.

TMP=samples/cli_typewild_tmp
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
mkdir -p "$TMP"
printf 'A1 line one\nA1 line two\n' > "$TMP/a1.txt"
printf 'A2 first version\n' > "$TMP/a2v1.txt"
printf 'A2 second version\nA2 second line\n' > "$TMP/a2v2.txt"
printf 'A3 only line\n' > "$TMP/a3.txt"
: > "$TMP/e0.txt"
awk 'BEGIN { s = ""; for (i = 0; i < 10; i++) s = s "0123456789"; print s; t = ""; for (i = 0; i < 18; i++) t = t "abcdefghij"; print t; print "short" }' > "$TMP/longrec.txt"
printf 'S1 in the subdirectory\n' > "$TMP/s1.txt"
SP=" "

for level in 2 5; do
    img="$TMP/tw$level.img"
    $INIT "$img" 20000 --structure=$level --label=TW$level >/dev/null 2>&1
    echo "== ODS-$level"
    cat > "$TMP/mk.cmd" <<CMD
CREATE/DIRECTORY [T]
CREATE/DIRECTORY [T.SUB]
COPY $TMP/a1.txt [T]A1.TXT
COPY $TMP/a2v1.txt [T]A2.TXT
COPY $TMP/a2v2.txt [T]A2.TXT
COPY $TMP/a3.txt [T]A3.TXT
COPY $TMP/e0.txt [T]E0.TXT
COPY $TMP/longrec.txt [T]LONGREC.TXT
COPY $TMP/s1.txt [T.SUB]S1.TXT
CMD
    $ODS2 "$img" "@$TMP/mk.cmd" >/dev/null 2>&1

    # --- T1: TYPE [T]*.TXT: every name, its highest version, a header for each ---------------------------------------------------------------------------
    want=$(printf ' \n[T]A1.TXT;1\n \nA1 line one\nA1 line two\n \n[T]A2.TXT;2\n \nA2 second version\nA2 second line\n \n[T]A3.TXT;1\n \nA3 only line\n \n[T]E0.TXT;1\n \n \n[T]LONGREC.TXT;1\n \n')
    got=$($ODS2 "$img" "TYPE [T]*.TXT" 2>&1)
    long=$(cat "$TMP/longrec.txt")
    check "ODS-$level T1: TYPE [T]*.TXT: a space line, the name, a space line, the records, per file in name order (highest version; the empty file has its header only)" "$want
$long" "$got"
    # --- T2: no wildcard: no header, no blank line ------------------------------------------------------------------------------------------------------
    check "ODS-$level T2: TYPE [T]A2.TXT;1 has no header" "A2 first version" "$($ODS2 "$img" 'TYPE [T]A2.TXT;1' 2>&1)"
    check "ODS-$level T2: TYPE [T]A1.TXT has no header" "A1 line one
A1 line two" "$($ODS2 "$img" 'TYPE [T]A1.TXT' 2>&1)"
    # --- T3: a comma list: the first file has no header, the second has one ------------------------------------------------------------------------------
    want=$(printf 'A1 line one\nA1 line two\n \n[T]A3.TXT;1\n \nA3 only line')
    check "ODS-$level T3: TYPE [T]A1.TXT,A3.TXT: no header for the first file, a header for the second (it is in the first element's directory)" "$want" "$($ODS2 "$img" 'TYPE [T]A1.TXT,A3.TXT' 2>&1)"
    # --- T4: versions: ;* shows every version highest first, with headers; % and * in the name; A*.* -----------------------------------------------------
    want=$(printf ' \n[T]A2.TXT;2\n \nA2 second version\nA2 second line\n \n[T]A2.TXT;1\n \nA2 first version')
    check "ODS-$level T4: TYPE [T]A2.TXT;* shows ;2 then ;1, each with a header" "$want" "$($ODS2 "$img" 'TYPE [T]A2.TXT;*' 2>&1)"
    got=$($ODS2 "$img" 'TYPE [T]A%.TXT' 2>&1 | grep -c '^\[T\]A.\.TXT;')
    check "ODS-$level T4: TYPE [T]A%.TXT finds A1, A2, A3 (the highest version of A2)" "3" "$got"
    check "ODS-$level T4: TYPE [T]A*.* names A1.TXT;1 A2.TXT;2 A3.TXT;1" "[T]A1.TXT;1 [T]A2.TXT;2 [T]A3.TXT;1" "$($ODS2 "$img" 'TYPE [T]A*.*' 2>&1 | grep '^\[T\]' | tr '\n' ' ' | sed 's/ $//')"
    # --- T5: records of 100 and 180 characters are written whole ---------------------------------------------------------------------------------------
    check "ODS-$level T5: the 100- and 180-character records come out whole" "100 180 5" "$($ODS2 "$img" 'TYPE [T]LONGREC.TXT' 2>&1 | awk '{ printf "%d ", length($0) }' | sed 's/ $//')"
    # --- T6: nothing found ------------------------------------------------------------------------------------------------------------------------------
    got=$($ODS2 "$img" 'TYPE [T]*.NOSUCH' 2>&1)
    check "ODS-$level T6: TYPE [T]*.NOSUCH: SEARCHFAIL with the version ; added, and FNF" "%TYPE-W-SEARCHFAIL, error searching for [T]*.NOSUCH;
-RMS-E-FNF, file not found" "$got"
    got=$($ODS2 "$img" 'TYPE [NODIR]*.TXT' 2>&1)
    check "ODS-$level T6: a missing directory: SEARCHFAIL, DNF, NOSUCHFILE" "%TYPE-W-SEARCHFAIL, error searching for [NODIR]*.TXT;
-RMS-E-DNF, directory not found
-SYSTEM-W-NOSUCHFILE, no such file" "$got"
    want=$(printf ' \n[T]A1.TXT;1\n \nA1 line one\nA1 line two\n \n[T]A2.TXT;2\n \nA2 second version\nA2 second line\n \n[T]A3.TXT;1\n \nA3 only line\n \n[T]E0.TXT;1\n \n \n[T]LONGREC.TXT;1\n \n')
    got=$($ODS2 "$img" 'TYPE [T]*.TXT,*.NOSUCH' 2>&1)
    check "ODS-$level T6: TYPE [T]*.TXT,*.NOSUCH: the matches first, the SEARCHFAIL at the end" "$want
$long
%TYPE-W-SEARCHFAIL, error searching for [T]*.NOSUCH;
-RMS-E-FNF, file not found" "$got"
    # --- T7: directory wildcards stay refused; /LINES and /RAW work with a wildcard --------------------------------------------------------------------
    check "ODS-$level T7: a wildcard in the directory is refused" "1" "$($ODS2 "$img" 'TYPE [T...]*.TXT' 2>&1 | grep -c 'WILDDIR')"
    check "ODS-$level T7: TYPE/LINES [T]A1*.TXT works with a header" " 
[T]A1.TXT;1
 
A1 line one
A1 line two" "$($ODS2 "$img" 'TYPE/LINES [T]A1*.TXT' 2>&1)"
    # --- T8: a single missing file without a wildcard keeps the message of the single-file TYPE -----------------------------------------------------------
    check "ODS-$level T8: TYPE [T]NOSUCH.TXT (no wildcard, no list) is still the one-file TYPE's %ODS2-E-FNF" "1" "$($ODS2 "$img" 'TYPE [T]NOSUCH.TXT' 2>&1 | grep -c 'ODS2-E-FNF')"
done

if [ "$fails" -eq 0 ]; then echo "cli_typewild_check: all checks passed"; else echo "cli_typewild_check: $fails FAILED"; fi
exit $fails
