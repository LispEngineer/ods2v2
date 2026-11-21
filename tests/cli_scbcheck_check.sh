#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_scbcheck_check.sh - ods5check checks the storage control block against the home block (review 57 item 4): see tests/scbcheck_check.py.
# Called by `make test`; run from the project root.
exec python3 -I tests/scbcheck_check.py
