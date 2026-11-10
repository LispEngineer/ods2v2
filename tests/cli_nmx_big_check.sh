#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_nmx_big_check.sh - 75,000 real files on a fresh ODS-2 and ODS-5 volume (file numbers past 65535, so FIDs
# carry NMX = 1): IMPORT them, every file reads back through EXPORT and a sample through TYPE, the structure
# checker finds nothing wrong, then deletes and a second import round. About 3 minutes: `make test-large`,
# not part of `make test` (tests/cli_nmx_check.sh covers the same code with seconds of work).

TMP=samples/cli_nmx_big_tmp
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
mkdir -p "$TMP/tree" "$TMP/ex"
python3 -I - "$TMP/tree" <<'PY'
import os, sys
root = sys.argv[1]
for d in range(75):
    os.makedirs('%s/d%02d' % (root, d))
    for i in range(1000):
        open('%s/d%02d/f%04d.dat' % (root, d, i), 'wb').write(b'%d/%d\n' % (d, i))
PY

for level in 2 5; do
    img="$TMP/big.img"
    rm -f "$img"; rm -rf "$TMP/ex"; mkdir "$TMP/ex"
    $INIT "$img" 400000 --structure=$level --cluster=1 --maxfiles=100000 --label=BIGFILES >/dev/null 2>&1
    out=$($ODS2 "$img" "IMPORT $TMP/tree [000000]" 2>&1 | tail -1)
    check "ODS-$level: IMPORT of 75,000 files in 75 directories" "75000 files 75 directories 0 errors" \
        "$(echo "$out" | sed -n 's/.*IMPORTED, \([0-9]*\) files.* \([0-9]*\) directories.*; [0-9]* skipped, \([0-9]*\) errors.*/\1 files \2 directories \3 errors/p')"
    check "ODS-$level: the structure checker finds nothing wrong" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    check "ODS-$level: the last file's number is 75,000 or more (past 65,535)" "yes" \
        "$([ "$($ODS2 "$img" 'DIR/FULL [d74]f0999.dat' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')" -gt 65535 ] && echo yes)"
    check "ODS-$level: TYPE of the last file" "74/999" "$($ODS2 "$img" 'TYPE [d74]f0999.dat' 2>&1)"
    $ODS2 "$img" "EXPORT [...]*.*;* $TMP/ex" >/dev/null 2>&1
    check "ODS-$level: EXPORT gives every file back with its own bytes" "75000 bad 0" "$(python3 -I - "$TMP/ex" "$level" <<'PY'
import os, sys
ex, level = sys.argv[1], sys.argv[2]
n = bad = 0
for d in range(75):
    for i in range(1000):
        name = 'd%02d/f%04d.dat;1' % (d, i)
        p = os.path.join(ex, name) if level == '5' else os.path.join(ex, name.upper())
        try:
            data = open(p, 'rb').read()
        except OSError:
            bad += 1
            continue
        n += 1
        if data != b'%d/%d\n' % (d, i):
            bad += 1
print(n, 'bad', bad)
PY
)"
    # deletes of files above 65535 and the numbers coming back
    i=0; while [ $i -lt 200 ]; do echo "DELETE [d74]f0$((i+500)).dat"; i=$((i+1)); done > "$TMP/del.ods2"
    $ODS2 "$img" "@$TMP/del.ods2" >/dev/null 2>&1
    check "ODS-$level: after 200 deletes the checker finds nothing wrong" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    printf 'new\n' > "$TMP/new.txt"
    $ODS2 "$img" "COPY $TMP/new.txt [d74]NEW.TXT" >/dev/null 2>&1
    check "ODS-$level: a new file reuses a freed number above 65535" "yes" \
        "$([ "$($ODS2 "$img" 'DIR/FULL [d74]new.txt' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')" -gt 65535 ] && echo yes)"
    check "ODS-$level: and the checker is still clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
done

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_nmx_big_check: $fails FAILED"
    exit 1
fi
echo "cli_nmx_big_check: all checks passed"
