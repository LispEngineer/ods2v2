#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_exthdr_attr_check.sh - the record-attribute fields of the extension headers ods2v2 makes (follow-up d item 6, CORPUS 61): see tests/exthdr_attr_check.py.
# Called by `make test`; run from the project root.
exec python3 -I tests/exthdr_attr_check.py
