#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_longargs_check.sh - the sanitizer gate for command parsing (review 48 F1, F2: two stack overflows from long arguments in one review). `ods2` is built with
# -fsanitize=address,undefined (see the Makefile), so any write past a buffer, or undefined behaviour, in the command line code stops the run with a
# sanitizer report. This test gives every command that takes text from the user an argument of every length near a buffer size (60 to 4,000 characters, and the
# exact inputs of review 48: a /PROTECTION list of 130 to 159 characters and a first word of 281 characters followed by " O:R) [D]X.TXT"), and fails when
# any run prints "Sanitizer", "runtime error" or dies of a signal. A refusal is the right answer for too long a value; only a crash fails.
# Called by `make test`, and by `make sancheck`, which also runs every other cli_*_check.sh under the same binary; run from the project root.

TMP=samples/cli_longargs_tmp
INIT=./ods2_init
ODS2=./ods2
fails=0
runs=0

# the gate means nothing if the binary is not sanitized: refuse to run then
if ! nm "$ODS2" 2>/dev/null | grep -q '__asan_init\|__ubsan'; then
    echo "FAIL: $ODS2 is not built with -fsanitize=address,undefined: this test would find nothing"
    echo "cli_longargs_check: 1 FAILED"
    exit 1
fi
rm -rf "$TMP"
mkdir -p "$TMP"
printf 'one\ntwo\n' > "$TMP/a.txt"
rep() { # rep <string> <length>: the string repeated, cut to exactly <length> characters
    awk -v s="$1" -v n="$2" 'BEGIN { r = ""; while (length(r) < n) r = r s; printf "%s", substr(r, 1, n) }'
}
try() { # try <img> <command>: run it, fail on a sanitizer report or a crash
    runs=$((runs + 1))
    out=$($ODS2 "$1" "$2" 2>&1); rc=$?
    case "$out" in
    *Sanitizer*|*"runtime error"*|*"stack-buffer"*) fails=$((fails + 1)); echo "FAIL: a sanitizer report for: $(echo "$2" | cut -c1-100)... (${#2} characters)"; echo "$out" | grep -m3 'Sanitizer\|runtime error\|#[0-4] ' | cut -c1-200 ;;
    esac
    if [ $rc -ge 128 ]; then fails=$((fails + 1)); echo "FAIL: signal $((rc - 128)) for: $(echo "$2" | cut -c1-100)... (${#2} characters)"; fi
}

for level in 2 5; do
    img="$TMP/la$level.img"
    $INIT "$img" 20000 --structure=$level --label=LA$level --cluster=1 >/dev/null 2>&1
    printf 'CREATE/DIRECTORY [D]\nCOPY %s [D]X.TXT\nCOPY %s [D]X.TXT\nCOPY %s [D]A.TXT\nCOPY %s [D]B.TXT\n' "$TMP/a.txt" "$TMP/a.txt" "$TMP/a.txt" "$TMP/a.txt" > "$TMP/mk.cmd"
    $ODS2 "$img" "@$TMP/mk.cmd" >/dev/null 2>&1
    echo "== ODS-$level"
    before=$fails
    # the exact inputs of review 48
    for n in 128 129 130 135 141 150 159 160 161; do
        try "$img" "SET FILE/PROTECTION=($(rep 'S:RWED,' $((n - 2)))) [D]X.TXT"       # a value of n characters with its parentheses
    done
    for n in 254 255 256 257 258 270 281 300 1000; do
        try "$img" "SET FILE/PROTECTION=($(rep 'S:R,' $((n - 19)))  O:R) [D]X.TXT"   # a first word of n characters, the list split at a space
        try "$img" "SET FILE/PROTECTION=(S:R,$(rep 'O:R,' $n) O:R) /LOG [D]X.TXT"
        try "$img" "SET FILE/LOG/OWNER_UIC=[1,$(rep 7 $n)] [D]X.TXT"
        try "$img" "SET FILE/LOG/PROTECTION=(W:R)$(rep '/LOG' $n) [D]X.TXT"
    done
    for n in 1 10 11 12 20 31 32 33 60 63 64 65 100 127 128 129 200 254 255 256 257 300 511 512 513 1000 2047 2048 2049 4000; do
        X=$(rep 'A' $n)
        try "$img" "DIR $X"
        try "$img" "DIR [$X]*.*"
        try "$img" "DIR/FULL [D]$X.TXT"
        try "$img" "DIR [D]$X*"
        try "$img" "TYPE $X"
        try "$img" "TYPE [D]$X.TXT"
        try "$img" "TYPE [D]A.TXT,$X"
        try "$img" "TYPE [D]A.TXT,[$X]B.TXT"
        try "$img" "TYPE [D]A.TXT,[D]$X.TXT,[D]B.TXT"
        try "$img" "TYPE [$X]*.TXT"
        try "$img" "TYPE $(rep '[D]A.TXT,' $n)"
        try "$img" "PURGE $X"
        try "$img" "PURGE [$X]*.*"
        try "$img" "PURGE [D]$X*.*"
        try "$img" "PURGE/KEEP=$X [D]X.TXT"
        try "$img" "PURGE/$X [D]X.TXT"
        try "$img" "PURGE/KEEP=2/LOG$(rep '/LOG' $n) [D]X.TXT"
        try "$img" "SET FILE/PROTECTION=($X) [D]X.TXT"
        try "$img" "SET FILE/PROTECTION=($X:R) [D]X.TXT"
        try "$img" "SET FILE/PROTECTION=(S:R,$X) [D]X.TXT"
        try "$img" "SET FILE/PROTECTION=(S:$X) [D]X.TXT"
        try "$img" "SET FILE/PROTECTION=(S:R) $X"
        try "$img" "SET FILE/PROTECTION=(S:R) [D]$X.TXT"
        try "$img" "SET FILE/OWNER_UIC=[$X,$X] [D]X.TXT"
        try "$img" "SET FILE/OWNER_UIC=[$X] [D]X.TXT"
        try "$img" "SET FILE/OWNER_UIC=$X [D]X.TXT"
        try "$img" "SET FILE/$X=(S:R) [D]X.TXT"
        try "$img" "SET FILE/VERSION_LIMIT=$X [D]X.TXT"
        try "$img" "SET FILE/ATTRIBUTES=($X) [D]X.TXT"
        try "$img" "SET DEFAULT [$X]"
        try "$img" "SET DEFAULT $X"
        try "$img" "SET NAMES $X"
        try "$img" "SET VOLUME/LABEL=$X"
        try "$img" "SHOW $X"
        try "$img" "CREATE/DIRECTORY [$X]"
        try "$img" "CREATE/DIRECTORY [D.$X]"
        try "$img" "DELETE [D]$X.TXT"
        try "$img" "DELETE [$X]*.*"
        try "$img" "COPY $TMP/a.txt [D]$X.TXT"
        try "$img" "COPY $TMP/a.txt [D]X.TXT;$X"
        try "$img" "COPY/ATTRIBUTES=($X) $TMP/a.txt [D]Y.TXT"
        try "$img" "COPY/$X $TMP/a.txt [D]Y.TXT"
        try "$img" "COPY $X [D]Y.TXT"
        try "$img" "LINK [D]$X.TXT [D]A.TXT"
        try "$img" "LINK [D]L.TXT [D]$X.TXT"
        try "$img" "SYMLINK [D]$X.TXT $X"
        try "$img" "EXPORT [D]*.* $TMP/$X"
        try "$img" "EXPORT/$X [D]*.* $TMP/x"
        try "$img" "IMPORT $TMP/$X [D]"
        try "$img" "$X"
        try "$img" "$X $X"
    done
    if [ $fails -eq $before ]; then echo "PASS: ODS-$level: no sanitizer report and no crash for any of the long arguments"; fi
done
echo "cli_longargs_check: $runs commands run"
if [ "$fails" -eq 0 ]; then echo "cli_longargs_check: all checks passed"; else echo "cli_longargs_check: $fails FAILED"; fi
exit $fails
