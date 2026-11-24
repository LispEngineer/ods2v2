#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_mpoffset_check.sh - ods5check's map offset rule: at or above what the name needs, the slack zero (CORPUS 67.5). The checks are in tests/mpoffset_check.py. Called by `make test`.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/mpoffset_check.py
