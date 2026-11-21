#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_setfile_ext_check.sh - SET FILE/OWNER_UIC writes the owner into every extension header of a file, and the protection stays in the primary header (CORPUS 56.2 on V8.4).
# Called by `make test`; run from the project root. All the work is in tests/setfile_ext_check.py.
exec python3 -I tests/setfile_ext_check.py
