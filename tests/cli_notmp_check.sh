#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_notmp_check.sh - no C source (library, tools, tests) names /tmp: scratch files go to $TMPDIR (a mkstemp file there) or the build directory, never to a fixed /tmp path
# (a RAM disk on many hosts: review 61 F2; ods2_read_file_selftest.c wrote /tmp/ods2_read_file_test.img in every make test). Called by `make test`; run from the project root.
hits=$(grep -n '"/tmp\|/tmp/' tests/*.c tests/*.h src/*.c tools/*.c include/*.h 2>/dev/null | grep -v '^[^:]*:[0-9]*:[[:space:]]*\(/\*\|\*\|//\)')
if [ -n "$hits" ]; then
    echo "FAIL: a C file names /tmp:"; echo "$hits" | head -5
    exit 1
fi
echo "PASS: no C file names /tmp"
