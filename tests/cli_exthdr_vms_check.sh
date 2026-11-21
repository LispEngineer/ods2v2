#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_exthdr_vms_check.sh - the extension headers ods2v2 makes have the layout of the VMS-made ones (follow-up d): see tests/exthdr_vms_compare.py.
# Called by `make test`; run from the project root.
exec python3 -I tests/exthdr_vms_compare.py
