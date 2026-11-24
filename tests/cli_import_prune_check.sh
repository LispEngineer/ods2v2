#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_import_prune_check.sh - IMPORT/NEW_VERSION never deletes a file the same command imported (review 77 O1): the prune of such a file is refused for that file and reported. The checks are in
# tests/import_prune_check.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/import_prune_check.py
