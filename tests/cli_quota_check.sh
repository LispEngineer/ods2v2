#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_quota_check.sh - a volume with active disk quotas ([000000]QUOTA.SYS exists, CORPUS 71/72) is not written unless --ignore-quota is given; QUOTA.SYS itself is protected. The checks are in
# tests/quota_check.py. Called by `make test`; run from the project root.
TMPDIR="${TMPDIR:-samples}"
export TMPDIR
python3 -I tests/quota_check.py
