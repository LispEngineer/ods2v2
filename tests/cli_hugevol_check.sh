#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_hugevol_check.sh - volumes at the 2^31 and 2^32 block boundaries (1 TiB and 2 TiB, LARGE_VOLUMES.md 5): ods2_init makes
# them sparse (a few tens of MB on disk), and mounting, COPY, TYPE, EXPORT and DELETE work on them, through fseeko/off_t.
# The structures INITIALIZE places at N/2 lie at LBN 2^30 to 2^31 (Format 2 retrieval pointers, byte offsets above 2^39).
# Skipped when the file system cannot hold a sparse file of that size. Called by `make test`; run from the project root.

TMP=samples/cli_hugevol_tmp
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
head -c 300000 /dev/urandom > "$TMP/f.bin"

for blocks in 2147475456 2147483632 2147483664 4294967280; do
    img="$TMP/h$blocks.img"
    if ! $INIT "$img" $blocks --structure=5 --label=HUGE >/dev/null 2>"$TMP/err"; then
        echo "SKIP: $blocks blocks: the file system cannot hold a sparse image that large"
        rm -f "$img"
        continue
    fi
    # P1 2: VMS V7.3-2 mounts at most 2,147,475,456 blocks (CORPUS 35.3), V8.4 at most 4,261,348,350 (35.1): a bigger volume is
    # made, with a warning that says which one will not mount it
    case $blocks in
        2147475456) want="0 0" ;;
        4294967280) want="1 1" ;;
        *)          want="1 0" ;;
    esac
    check "$blocks blocks: the warnings of ods2_init (V7.3-2 limit, V8.4 limit)" "$want" \
        "$(grep -c 'V7.3-2.*2,147,475,456' "$TMP/err") $(grep -c 'V8.4 mounts at most 4,261,348,350' "$TMP/err")"
    check "$blocks blocks: SHOW VOLUME gives the size, the free blocks are below it" "yes" \
        "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | awk -v n=$blocks '/Total blocks:/ {t=$3} /Free blocks:/ {f=$3} END {print (t==n && f>0 && f<n) ? "yes" : "no " t " " f}')"
    out=$($ODS2 "$img" "COPY $TMP/f.bin [000000]F.BIN" 2>&1)
    check "$blocks blocks: COPY of 300,000 bytes" "1" "$(echo "$out" | grep -c COPIED)"
    check "... TYPE/RAW gives the same bytes" "same" "$($ODS2 "$img" 'TYPE/RAW F.BIN' 2>/dev/null | cmp - "$TMP/f.bin" >/dev/null 2>&1 && echo same || echo different)"
    rm -f "$TMP/out"
    $ODS2 "$img" "EXPORT [000000]F.BIN $TMP/out" >/dev/null 2>&1
    check "... EXPORT gives the same bytes" "same" "$(cmp "$TMP/out" "$TMP/f.bin" >/dev/null 2>&1 && echo same || echo different)"
    $ODS2 "$img" 'DELETE F.BIN' >/dev/null 2>&1
    check "... DELETE, and the file is gone" "1" "$($ODS2 "$img" 'TYPE F.BIN' 2>&1 | grep -c 'not found')"
    rm -f "$img"
done

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_hugevol_check: $fails FAILED"
    exit 1
fi
echo "cli_hugevol_check: all checks passed"
