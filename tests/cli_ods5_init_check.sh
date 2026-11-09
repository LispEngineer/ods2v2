#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_init_check.sh - ods2_init makes empty ODS-2 and ODS-5 volumes of several sizes and
# cluster factors, laid out as VMS V8.4 lays out INITIALIZE's volume: tests/ods5check.py (an
# independent structure check) finds nothing wrong, ods2 mounts them, SHOW VOLUME reports what
# was asked, a file written to each is read back, and an existing image is never overwritten.
# Called by `make test`; run from the project root.

TMP=samples/cli_init_tmp
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
printf 'hello, fresh volume\n' > "$TMP/hello.txt"

for spec in "8192 5" "10000 2" "65536 5" "300M 5" "100M 2" "1G 5 --cluster=3" "1G 2 --cluster=1" "2G 5"; do
    set -- $spec
    img="$TMP/v.img"
    rm -f "$img"
    $INIT "$img" "$1" --structure="$2" ${3:+"$3"} --label=FRESHVOL >/dev/null 2>&1
    check "init $spec: the independent structure check finds nothing" "0 error(s), 0 warning(s)" \
        "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    out=$($ODS2 "$img" 'SHOW VOLUME' 2>&1)
    check "init $spec: SHOW VOLUME names ODS-$2 and the label" "ODS-$2 FRESHVOL" \
        "ODS-$(echo "$out" | sed -n 's/^  Structure level: *ODS-\([0-9]\).*/\1/p') $(echo "$out" | sed -n 's/^  Volume label: *//p')"
    $ODS2 "$img" "COPY $TMP/hello.txt [000000]HELLO.TXT" >/dev/null 2>&1
    check "init $spec: a file written and read back" "hello, fresh volume" "$($ODS2 "$img" 'TYPE [000000]HELLO.TXT' 2>&1)"
    check "init $spec: still no error after the write" "0 error(s), 0 warning(s)" \
        "$(python3 -I tests/ods5check.py "$img" | tail -1)"
done

img="$TMP/v5.img"
$INIT "$img" 64M --structure=5 --hardlinks --access-dates >/dev/null 2>&1
check "init --hardlinks --access-dates sets the characteristics" "ACCESS_DATES, HARDLINKS" \
    "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | sed -n 's/^  Characteristics: *//p')"
check "ODS-2 refuses ODS-5 characteristics" 1 "$($INIT "$TMP/bad.img" 64M --structure=2 --hardlinks >/dev/null 2>&1; echo $?)"
check "ods2_init never overwrites an existing image" 1 "$($INIT "$img" 64M >/dev/null 2>&1; echo $?)"
check "the refused image is not created" "no" "$([ -e "$TMP/bad.img" ] && echo yes || echo no)"
check "too small a volume is refused" 1 "$($INIT "$TMP/small.img" 1000 >/dev/null 2>&1; echo $?)"

# a full volume: writes fail with a message, nothing is left half done, deleting gives the space back
img="$TMP/full.img"
$INIT "$img" 8192 --label=FULL >/dev/null 2>&1
head -c 1000000 /dev/zero > "$TMP/mb.bin"
for i in 1 2 3 4 5 6 7 8; do $ODS2 "$img" "COPY $TMP/mb.bin [000000]F$i.BIN" >/dev/null 2>&1; done
check "a full volume refuses a file with a message" 1 \
    "$($ODS2 "$img" "COPY $TMP/mb.bin [000000]ONEMORE.BIN" 2>&1 | grep -c 'not enough free space')"
check "...and is still consistent" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
for i in 1 2 3 4 5 6 7 8; do $ODS2 "$img" "DELETE [000000]F$i.BIN" >/dev/null 2>&1; done
check "after deleting everything the space is back" "Free blocks: 8166" \
    "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | sed -n 's/^  Free blocks: */Free blocks: /p')"

# writes to damaged volumes (see fuzz_ods5.py --write): errors, never crashes
mkdir -p "$TMP/fz/sub"
printf 'hello\n' > "$TMP/fz/hello.txt"; printf 'x\n' > "$TMP/fz/sub/a b.txt"; ln -s hello.txt "$TMP/fz/ln1"
$INIT "$TMP/fzbase.img" 64M --hardlinks >/dev/null 2>&1
$ODS2 "$TMP/fzbase.img" "IMPORT $TMP/fz [000000]" >/dev/null 2>&1
check "60 iterations of writing to a damaged ODS-5 volume: no crash, hang or sanitizer report" 0 \
    "$(python3 -I tests/fuzz_ods5.py $ODS2 "$TMP/fzbase.img" 60 7 "$TMP/fzwork" --write "$TMP/fz" >/dev/null 2>&1; echo $?)"

rm -rf "$TMP"
[ "$fails" -eq 0 ] || { echo "$fails INITIALIZE check(s) failed"; exit 1; }
