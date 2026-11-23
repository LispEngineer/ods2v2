#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_commaesc_check.sh - an escaped comma (^,) in an ODS-5 file name is a character, not a list separator, in TYPE lists and every other command that takes a specification
# (review 66: TYPE [D]sp^,c.txt typed [D]c.txt). The checks are in tests/commaesc_check.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/commaesc_check.py
