#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_dir_delete_check.sh - the DELETE rule for a directory file (never the last entry of a directory that is not empty; the other entry goes alone) and ods5check's rules for a directory entered more
# than once (a WARN) and a cycle (an ERROR), on the stopped states of a directory move made by hand (review 71 C2). The checks are in tests/dir_delete_check.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/dir_delete_check.py
