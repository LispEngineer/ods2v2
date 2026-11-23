#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_versorder_check.sh - versions of one name stored out of order (H1, CORPUS 66.4): reads follow the stored order as VMS does, commands that change something refuse the name unless a
# version is named, ods5check says BAD_VERSORDER. The checks are in tests/versorder_check.py. (tests/cli_verorder_check.sh is the older check of the checker's message on the dirsearch
# images.) Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/versorder_check.py
