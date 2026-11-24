#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_rename_dir_check.sh - RENAME of a DIRECTORY (step 3 of PLAN_WRITE_OPS item 5): within the parent, to another parent, the refusals (IDR, names, depth), the finish of a stopped move, the 9th name
# on ODS-2; ODS-2, ODS-5 and ODS-5 HARDLINKS. The checks are in tests/rename_dir_check.py; the crash order is the rename_dir_* operations of tests/fault_inject.py. Called by `make test`.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/rename_dir_check.py
