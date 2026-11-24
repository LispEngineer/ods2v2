#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_rename_check.sh - RENAME within one directory, ODS-2, ODS-5 and ODS-5 HARDLINKS, against what VMS does (CORPUS 65). The checks are in tests/rename_check.py; the crash
# order of the same operations (at least one name at every interruption) is the rename_* operations of tests/fault_inject.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/rename_check.py
