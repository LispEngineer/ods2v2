#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_spool_check.sh - COPY from a pipe (no length to go by) spools the input to a temporary file first. That file was made by
# tmpfile(), which ignores TMPDIR and uses /tmp (a RAM disk on many hosts: a piped multi-GiB input filled it). It is now made
# in $TMPDIR, else in the image's own directory, never /tmp, is anonymous (unlinked at once: nothing is left behind), and when
# neither can hold it the COPY fails with a message naming both and writes nothing. Called by `make test`; run from the project
# root.

TMP=samples/cli_spool_tmp
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
mkdir -p "$TMP/img" "$TMP/spool"
echo "piped content" > "$TMP/in.txt"
$INIT "$TMP/img/s.img" 8192 --structure=2 --label=SPOOL >/dev/null 2>&1

# 1. TMPDIR names a directory that does not exist: the image's directory is used
out=$(TMPDIR="$TMP/nonexistent" sh -c "cat $TMP/in.txt | $ODS2 $TMP/img/s.img 'COPY /dev/stdin A.TXT' 2>&1")
check "COPY from a pipe with TMPDIR unusable: falls back to the image's directory and copies" "1 piped content" \
    "$(echo "$out" | grep -c 'COPIED') $($ODS2 $TMP/img/s.img 'TYPE A.TXT' 2>&1 | tail -1)"
check "... nothing is left in the image's directory" "s.img" "$(ls $TMP/img)"

# 2. TMPDIR good: used, nothing left behind there
out=$(TMPDIR="$TMP/spool" sh -c "cat $TMP/in.txt | $ODS2 $TMP/img/s.img 'COPY /dev/stdin B.TXT' 2>&1")
check "COPY from a pipe with TMPDIR set: copies, and no spool file is left in TMPDIR" "1 0" \
    "$(echo "$out" | grep -c 'COPIED') $(ls $TMP/spool | wc -l)"

# 3. neither usable (TMPDIR missing, the image's directory read-only): refused with a message, nothing written
chmod 555 "$TMP/img"
before=$(md5sum < "$TMP/img/s.img")
out=$(TMPDIR="$TMP/nonexistent" sh -c "cat $TMP/in.txt | $ODS2 $TMP/img/s.img 'COPY /dev/stdin C.TXT' 2>&1")
rc=$?
case "$(id -u)" in 0) echo "SKIP: the read-only directory check needs a non-root user" ;; *)
    check "neither TMPDIR nor the image's directory usable: COPY fails and names both" "1 1 1" \
        "$(echo "$out" | grep -c 'could not create a temporary file') $(echo "$out" | grep -c 'nonexistent') $(echo "$out" | grep -c 'img')"
    check "... nothing was written to the image" "$before" "$(md5sum < "$TMP/img/s.img")" ;;
esac
chmod 755 "$TMP/img"

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_spool_check: $fails FAILED"
    exit 1
fi
echo "cli_spool_check: all checks passed"
