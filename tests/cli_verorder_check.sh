#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_verorder_check.sh - the versions of one name are stored newest first (review 28 b): a directory with two versions the
# other way round makes the lookup of the highest version read the wrong file, and no check noticed. ods5check.py now says
# "versions not descending" for it, across the records of a name as well as inside one. The two images are left by
# build/ods2_dirsearch_selftest (40 names with 70 versions each, as the library builds them, and the same with two versions
# of one name swapped); ods2_insert_into_directory's refusal of a version that would be stored out of order is that
# selftest's. Called by `make test` after the selftests; run from the project root.

OK=samples/dirsearch_order_ok.img
BAD=samples/dirsearch_order_bad.img
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

if [ ! -f "$OK" ] || [ ! -f "$BAD" ]; then
    echo "FAIL: the images of ods2_dirsearch_selftest are missing"
    exit 1
fi
check "a directory with every name's versions newest first: no 'versions not descending'" "0" "$(python3 -I tests/ods5check.py "$OK" 2>&1 | grep -c 'versions not descending')"
check "the same with two versions of one name swapped: reported" "1" "$(python3 -I tests/ods5check.py "$BAD" 2>&1 | grep -c 'versions not descending')"
rm -f "$OK" "$BAD"
if [ $fails -ne 0 ]; then
    echo "cli_verorder_check: $fails FAILED"
    exit 1
fi
echo "cli_verorder_check: all passed"
