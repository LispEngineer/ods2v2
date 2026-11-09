#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer)
#
# cli_records_check.sh - TYPE and EXPORT/RECORDS give RMS sequential files as text, one line per
# record (VAR, VFC, FIX, STM, STMLF, STMCR, UDF); TYPE/RAW and EXPORT give the stored bytes.
# The files come from tests/mkrms_image.py (raw bytes copied in, record attributes patched
# into the headers). Expected text is written by that script from the record layouts.
ODS2=${ODS2:-./ods2}
INIT=${INIT:-./ods2_init}
TMP=${TMPDIR:-/tmp}/rec_check.$$
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1 (expected [$2] got [$3])"; fail=$((fail+1)); fi
}
python3 -I tests/mkrms_image.py "$ODS2" "$INIT" "$TMP/rms.img" "$TMP/src" >/dev/null || { echo "FAIL: building the test image"; exit 1; }
python3 -I tests/ods5check.py "$TMP/rms.img" | tail -1 | grep -q '^0 error(s), 0 warning(s)' && echo "PASS: record test image passes ods5check.py" || { echo "FAIL: record test image"; fail=$((fail+1)); }
for f in "$TMP"/src/expect/*; do
    n=$(basename "$f")
    $ODS2 "$TMP/rms.img" "TYPE [000000]$n" > "$TMP/out" 2>/dev/null
    if cmp -s "$f" "$TMP/out"; then echo "PASS: TYPE $n as text"; else echo "FAIL: TYPE $n as text"; fail=$((fail+1)); fi
done
mkdir "$TMP/exp"
$ODS2 "$TMP/rms.img" "EXPORT/RECORDS [000000]*.* $TMP/exp" >/dev/null 2>&1
for f in "$TMP"/src/expect/*; do
    n=$(basename "$f")
    if cmp -s "$f" "$TMP/exp/$n"; then echo "PASS: EXPORT/RECORDS $n"; else echo "FAIL: EXPORT/RECORDS $n"; fail=$((fail+1)); fi
done
mkdir "$TMP/raw"
$ODS2 "$TMP/rms.img" "EXPORT [000000]*.* $TMP/raw" >/dev/null 2>&1
for f in "$TMP"/src/*; do
    [ -f "$f" ] || continue
    n=$(basename "$f")
    [ "$n" = MANIFEST.txt ] && continue
    if cmp -s "$f" "$TMP/raw/$n"; then echo "PASS: EXPORT $n keeps the stored bytes"; else echo "FAIL: EXPORT $n keeps the stored bytes"; fail=$((fail+1)); fi
    $ODS2 "$TMP/rms.img" "TYPE/RAW [000000]$n" > "$TMP/out" 2>/dev/null
    cmp -s "$f" "$TMP/out" && echo "PASS: TYPE/RAW $n" || { echo "FAIL: TYPE/RAW $n"; fail=$((fail+1)); }
done
# /LINES: one line per record whatever the carriage control
check "TYPE/LINES: FIX_NONE (records that otherwise run together) as 8 lines" 8 "$($ODS2 "$TMP/rms.img" 'TYPE/LINES [000000]FIX_NONE.TXT' 2>/dev/null | wc -l | tr -d ' ')"
check "TYPE FIX_NONE: the records run together (no line ends), as VMS TYPE shows them" 0 "$($ODS2 "$TMP/rms.img" 'TYPE [000000]FIX_NONE.TXT' 2>/dev/null | wc -l | tr -d ' ')"
# DIR/FULL words the record format and attributes as VMS does
full=$($ODS2 "$TMP/rms.img" 'DIR/FULL [000000]VFC_PRN.TXT' 2>&1)
check "DIR/FULL: VFC format line" "Record format:      VFC, 2 byte header, maximum 0 bytes, longest 20 bytes" "$(echo "$full" | sed -n 's/^  Record format: */Record format:      /p')"
check "DIR/FULL: print file attribute" "Record attributes:  Print file carriage control" "$(echo "$full" | sed -n 's/^  Record attributes: */Record attributes:  /p')"
full=$($ODS2 "$TMP/rms.img" 'DIR/FULL [000000]FIX_CR.TXT' 2>&1)
check "DIR/FULL: fixed length line" "Record format:      Fixed length 20 byte records" "$(echo "$full" | sed -n 's/^  Record format: */Record format:      /p')"
check "DIR/FULL: CR attribute" "Record attributes:  Carriage return carriage control" "$(echo "$full" | sed -n 's/^  Record attributes: */Record attributes:  /p')"
full=$($ODS2 "$TMP/rms.img" 'DIR/FULL [000000]VAR_BLK.TXT' 2>&1)
check "DIR/FULL: CR and non-spanned" "Record attributes:  Carriage return carriage control, Non-spanned" "$(echo "$full" | sed -n 's/^  Record attributes: */Record attributes:  /p')"
# files VMS TYPE refuses (m2b): a damaged VAR file (RMS-F-IRC) and carriage control that cannot apply (RMS-F-RAT)
for n in BAD_VAR.TXT VAR_PRN.TXT FIX_PRN.TXT FIX_CRFTN.TXT; do
    $ODS2 "$TMP/rms.img" "TYPE [000000]$n" > "$TMP/out" 2> "$TMP/err"; st=$?
    check "TYPE $n fails as VMS TYPE does" "1 0" "$st $(wc -c < "$TMP/out" | tr -d ' ')"
    grep -q "READERR" "$TMP/err" && echo "PASS: ... and says so" || { echo "FAIL: $n: no message"; fail=$((fail+1)); }
    $ODS2 "$TMP/rms.img" "TYPE/RAW [000000]$n" > "$TMP/out" 2>/dev/null
    cmp -s "$TMP/src/$n" "$TMP/out" && echo "PASS: TYPE/RAW $n still gives the bytes" || { echo "FAIL: TYPE/RAW $n"; fail=$((fail+1)); }
    $ODS2 "$TMP/rms.img" "TYPE/LINES [000000]$n" > "$TMP/out" 2>/dev/null
done
if [ $fail -ne 0 ]; then echo "$fail records check(s) failed"; exit 1; fi
echo "all records checks passed"
