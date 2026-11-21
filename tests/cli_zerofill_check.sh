#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_zerofill_check.sh - a new directory or file never shows the old contents of the free blocks it was given (review 56 F1): see tests/zerofill_check.py.
# Called by `make test`; run from the project root.
exec python3 -I tests/zerofill_check.py
