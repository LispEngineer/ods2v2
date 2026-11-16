#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_hostfull_check.sh - the host file system cannot hold what ods2v2 writes (LARGE_VOLUMES.md 11). `ulimit -f` makes writes past a
# file size fail with EFBIG, as a full disk does with ENOSPC: a COPY whose blocks lie beyond the limit is refused, the message
# says why (strerror), the volume is still sound (ods5check clean, the file not there, the free blocks as before). And an
# image shorter than the volume it holds (a truncated copy) mounts read-only with a warning: ods2_mount_write refuses it.
# The starter volume's placeholder storage control block (no size) stays writable. Called by `make test`; run from the project root.

TMP=samples/cli_hostfull_tmp
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
free_blocks() { $ODS2 "$1" 'SHOW VOLUME' 2>&1 | awk '/Free blocks:/ {print $3}'; }

rm -rf "$TMP"
mkdir -p "$TMP"
img="$TMP/h.img"
$INIT "$img" 20000 --structure=2 --cluster=1 --label=HOSTFULL >/dev/null 2>&1
head -c $((14000 * 512)) /dev/zero > "$TMP/big.bin"
before=$(free_blocks "$img")
# the volume's structures are at its middle (block 10,000): a limit of 5 MiB (10,240 blocks) lets those be written and stops the file's blocks
(
    trap '' XFSZ
    ulimit -f 5100 2>/dev/null || exit 3
    $ODS2 "$img" "COPY $TMP/big.bin [000000]BIG.BIN" > "$TMP/out.txt" 2>&1
    echo $? > "$TMP/rc.txt"
)
if [ "$(cat "$TMP/rc.txt" 2>/dev/null)" = "3" ] || [ ! -s "$TMP/out.txt" ]; then
    echo "SKIP: ulimit -f is not available here"
else
    out=$(cat "$TMP/out.txt")
    check "COPY past the host's file size limit is refused with the reason" "1 1" "$(echo "$out" | grep -c 'COPYERR') $(echo "$out" | grep -ci 'file too large')"
    check "... the file is not there and the free blocks are as before" "1 $before" "$($ODS2 "$img" 'TYPE BIG.BIN' 2>&1 | grep -c 'not found') $(free_blocks "$img")"
    check "... ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
fi

# an image shorter than its volume
cp "$img" "$TMP/short.img"
truncate -s $((15000 * 512)) "$TMP/short.img"
out=$($ODS2 "$TMP/short.img" 'SHOW VOLUME' 2>&1)
check "an image of 15,000 blocks for a volume of 20,000: a warning that says so" "1" "$(echo "$out" | grep -c 'IMAGESHORT.*15000 blocks.*20000')"
out=$($ODS2 "$TMP/short.img" "COPY $TMP/big.bin [000000]X.BIN" 2>&1)
check "... and nothing is written to it: read-only" "1" "$(echo "$out" | grep -c -i 'read-only\|not written\|READONLY')"
check "... its size is unchanged" "$((15000 * 512))" "$(stat -c %s "$TMP/short.img")"
check "an image of the whole volume gives no such warning" "0" "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | grep -c IMAGESHORT)"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_hostfull_check: $fails FAILED"
    exit 1
fi
echo "cli_hostfull_check: all checks passed"
