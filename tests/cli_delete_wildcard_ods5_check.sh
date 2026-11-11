#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_delete_wildcard_ods5_check.sh - DELETE with wildcards (upstream #24,
# be4e467) on what this branch adds to ods2v2: ODS-5 names (case-blind
# matching, ^-escapes literal, a name with no type means any type, as in
# DIR), hard links and aliases (an entry that is not the file's last one
# is removed and the file stays), directories (an empty one is deleted, a
# non-empty one is reported and the rest are still deleted), and the
# volume staying consistent (tests/ods5check.py: 0 errors). Called by
# `make test`; run from the project root.

ODS2=./ods2
INIT=./ods2_init
TMP=samples/cli_delwild5_tmp
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
names() { # the entries DIR lists, reserved files and 000000.DIR left out
    "$ODS2" "$1" "DIR $2" 2>/dev/null | grep ';' | grep -v -e '\.SYS *;' -e '^000000\.DIR' | sed 's/ *;/;/' |
        tr '\n' ' ' | sed 's/ $//'
}
errors() { python3 -I tests/ods5check.py "$1" 2>&1 | sed -n 's/^\([0-9][0-9]*\) error.*/\1/p' | tail -1; }

rm -rf "$TMP"; mkdir -p "$TMP"
printf 'one\n' > "$TMP/a.txt"

# --- ODS-5 names: case-blind, escapes, no type = any type
D=$TMP/n.img
"$INIT" "$D" 8192 --structure=5 --label=DELW5 >/dev/null 2>&1
check "ODS-5 volume made" 0 $?
for n in Foo.txt foo2.TXT FOO3.Txt Bar.dat "my^_file.txt" "my^_other.dat" "keep.me"; do
    "$ODS2" "$D" "COPY $TMP/a.txt [000000]$n" >/dev/null 2>&1
done

out=$("$ODS2" "$D" "DELETE [000000]f*.txt" 2>&1)
check "f*.txt deletes Foo.txt, foo2.TXT and FOO3.Txt (case-blind)" 3 \
    "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"
check "...and nothing else" "Bar.dat;1 keep.me;1 my^_file.txt;1 my^_other.dat;1" "$(names "$D" '[000000]*.*;*')"

out=$("$ODS2" "$D" "DELETE [000000]MY^_*" 2>&1)
check "MY^_* (an escaped space, no type = any type) deletes both my^_ names" 2 \
    "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"

"$ODS2" "$D" "DELETE [000000]Z*.*" >/dev/null 2>&1
check "a wildcard that matches nothing fails (exit 1)" 1 $?
check "ods5check: 0 errors after the deletes" 0 "$(errors "$D")"

# --- hard links (HARDLINKS: every entry is a link, the file goes with the last one) and plain aliases (no
#     HARDLINKS: removing an alias keeps the file; the primary entry is the file's own, as VMS has it)
for hl in --hardlinks ""; do
    D=$TMP/l$hl.img
    "$INIT" "$D" 8192 --structure=5 --label=DELWL $hl >/dev/null 2>&1
    "$ODS2" "$D" "CREATE/DIRECTORY [A]" >/dev/null 2>&1
    "$ODS2" "$D" "CREATE/DIRECTORY [B]" >/dev/null 2>&1
    "$ODS2" "$D" "COPY $TMP/a.txt [A]X1.TXT" >/dev/null 2>&1
    "$ODS2" "$D" "COPY $TMP/a.txt [A]X2.TXT" >/dev/null 2>&1
    "$ODS2" "$D" "LINK [B]Y1.TXT [A]X1.TXT" >/dev/null 2>&1
    check "LINK${hl:+ $hl} made" 0 $?
    if [ -n "$hl" ]; then gone=A; stays=B; file=Y1.TXT; n=2; else gone=B; stays=A; file=X1.TXT; n=1; fi
    out=$("$ODS2" "$D" "DELETE [$gone]*.*;*" 2>&1)
    check "${hl:-alias}: DELETE [$gone]*.*;* removes its $n entries" $n \
        "$(echo "$out" | grep -cE '^%ODS2-I-(DELETED|ALIASREMOVED|DANGLING),')"
    [ -z "$hl" ] && check "alias: ...said to be an alias" 1 "$(echo "$out" | grep -c '^%ODS2-I-ALIASREMOVED,')"
    check "${hl:-alias}: [$gone] is empty" "" "$(names "$D" "[$gone]*.*;*")"
    check "${hl:-alias}: the file is still there as [$stays]$file" "one" \
        "$("$ODS2" "$D" "TYPE [$stays]$file" 2>/dev/null)"
    check "${hl:-alias}: ods5check: 0 errors" 0 "$(errors "$D")"
done

# --- directories: an empty one goes, a non-empty one is reported, the rest still go
D=$TMP/d.img
"$INIT" "$D" 8192 --structure=5 --label=DELWD >/dev/null 2>&1
"$ODS2" "$D" "CREATE/DIRECTORY [EMPTY]" >/dev/null 2>&1
"$ODS2" "$D" "CREATE/DIRECTORY [FULL]" >/dev/null 2>&1
"$ODS2" "$D" "COPY $TMP/a.txt [FULL]F.TXT" >/dev/null 2>&1
"$ODS2" "$D" "COPY $TMP/a.txt [000000]E1.TXT" >/dev/null 2>&1
out=$("$ODS2" "$D" "DELETE [000000]E*.*;*" 2>&1)
check "E*.*;* deletes EMPTY.DIR and E1.TXT" 2 "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"
out=$("$ODS2" "$D" "DELETE [000000]FULL.DIR;*" 2>&1)
check "a non-empty directory is refused" 0 "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"
"$ODS2" "$D" "COPY $TMP/a.txt [000000]F2.TXT" >/dev/null 2>&1
out=$("$ODS2" "$D" "DELETE [000000]F*.*;*" 2>&1)
st=$?
check "F*.*;*: F2.TXT is deleted although FULL.DIR is refused" 1 "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"
check "...FULL.DIR is reported" 1 "$(echo "$out" | grep -c 'DELETEERR, FULL.DIR')"
check "...exit status 1" 1 $st
check "...[FULL]F.TXT is untouched" "one" "$("$ODS2" "$D" 'TYPE [FULL]F.TXT' 2>/dev/null)"
check "ods5check: 0 errors" 0 "$(errors "$D")"

rm -rf "$TMP"
if [ "$fails" -ne 0 ]; then
    echo "cli_delete_wildcard_ods5_check: $fails check(s) FAILED"
    exit 1
fi
echo "cli_delete_wildcard_ods5_check: all checks passed"
