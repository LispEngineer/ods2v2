#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_linkcount_check.sh - ods5check warns when a link count on a HARDLINKS volume is not the number of entries that name the file (review 67 F4). The checks are in tests/linkcount_check.py.
# Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/linkcount_check.py
