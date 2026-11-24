#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_rename_wild_check.sh - RENAME with wildcards (step 4; CORPUS 65.4 e1 and e2, reviews 75): each matched file its own transaction, the list made first, what is not measured refused. The checks are
# in tests/rename_wild_check.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/rename_wild_check.py
