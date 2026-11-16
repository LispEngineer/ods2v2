#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_escdot_check.sh - a file spelling with an escaped dot and no other dot ("a^.b") names the file "a.b" with the NULL
# type, stored "a.b." (VMS: DIR shows it as a^.b.); "a.b" is name a, type b, stored "a.b". The two are different files. The
# one-file commands (COPY, DELETE, TYPE, SET/EXPORT paths that look a file up, SYMLINK, LINK) completed the name from the
# unescaped literal "a.b", which holds a dot, so a^.b named the file a.b (review 26b N6). Called by `make test`; run from the
# project root.

TMP=samples/cli_escdot_tmp
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
echo "null type" > "$TMP/n.txt"
echo "name a type b" > "$TMP/t.txt"
echo "three dots" > "$TMP/c.txt"
img="$TMP/e.img"
$INIT "$img" 8192 --structure=5 --label=ESCDOT >/dev/null 2>&1

out=$($ODS2 "$img" "COPY $TMP/n.txt a^.b" 2>&1)
check "COPY n.txt a^.b: stored as a^.b. (null type)" "1" "$(echo "$out" | grep -c 'COPIED.* to a^\.b\.;1')"
out=$($ODS2 "$img" "COPY $TMP/t.txt a.b" 2>&1)
check "COPY t.txt a.b: a different file (name a, type b), no 'already exists'" "1 0" \
    "$(echo "$out" | grep -c 'COPIED.* to a\.b;1') $(echo "$out" | grep -c 'already exists')"
check "DIR shows both: a^.b.;1 and a.b;1" "1 1" \
    "$($ODS2 "$img" 'DIR' 2>&1 | grep -c '^a^\.b\.  *;1') $($ODS2 "$img" 'DIR' 2>&1 | grep -c '^a\.b  *;1')"
check "TYPE a^.b prints the null-type file" "null type" "$($ODS2 "$img" 'TYPE a^.b' 2>&1 | tail -1)"
check "TYPE a.b prints the other" "name a type b" "$($ODS2 "$img" 'TYPE a.b' 2>&1 | tail -1)"
check "TYPE a^.b. (the dot written) prints the null-type file" "null type" "$($ODS2 "$img" 'TYPE a^.b.' 2>&1 | tail -1)"

out=$($ODS2 "$img" "COPY $TMP/c.txt x^.y^.z" 2>&1)
check "COPY to x^.y^.z (two escaped dots): null type, stored x.y.z." "1" "$(echo "$out" | grep -c 'COPIED.* to x^\.y^\.z\.;1')"
check "TYPE x^.y^.z finds it" "three dots" "$($ODS2 "$img" 'TYPE x^.y^.z' 2>&1 | tail -1)"

# a second null-type name for the same content: LINK and SYMLINK name their entries the same way
out=$($ODS2 "$img" "SYMLINK l^.k ../elsewhere" 2>&1)
check "SYMLINK l^.k: stored as l^.k. (null type)" "1" "$(echo "$out" | grep -c 'SYMLINK, l^\.k\.;1')"

# DELETE of the spelling takes only the null-type file
out=$($ODS2 "$img" "DELETE a^.b" 2>&1)
check "DELETE a^.b: deletes the null-type file (the message shows the literal name, a.b.;1) and not a.b;1" "1 0" \
    "$(echo "$out" | grep -c 'deleted a\.b\.;1') $(echo "$out" | grep -c 'deleted a\.b;1')"
check "... a.b (name a, type b) is still there and holds its content" "name a type b" "$($ODS2 "$img" 'TYPE a.b' 2>&1 | tail -1)"
check "... a^.b. is gone" "1" "$($ODS2 "$img" 'TYPE a^.b' 2>&1 | grep -c 'not found')"
out=$($ODS2 "$img" "DELETE a.b" 2>&1)
check "DELETE a.b: deletes the name-a-type-b file" "1" "$(echo "$out" | grep -c 'deleted a\.b;1')"
check "ods5check: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"

# a name with no dot at all is unchanged: README -> README.
echo plain > "$TMP/p.txt"
$ODS2 "$img" "COPY $TMP/p.txt README" >/dev/null 2>&1
check "COPY p.txt README, TYPE README. and TYPE README agree" "plain plain" "$($ODS2 "$img" 'TYPE README' 2>&1 | tail -1) $($ODS2 "$img" 'TYPE README.' 2>&1 | tail -1)"

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_escdot_check: $fails FAILED"
    exit 1
fi
echo "cli_escdot_check: all checks passed"
