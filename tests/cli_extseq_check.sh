#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_extseq_check.sh - an extension header must name its primary header by file number and sequence number (review 54 item 1): see tests/extseq_check.py.
# Called by `make test`; run from the project root.
exec python3 -I tests/extseq_check.py
