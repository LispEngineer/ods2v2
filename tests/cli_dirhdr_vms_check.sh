#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_dirhdr_vms_check.sh - a directory header ods2v2 writes on ODS-2 against C.DIR of ods5-vms3's step 0 containers (follow-up c): tests/dirhdr_vms_compare.py. WO_VMS_DIR,
# default ~/tmp/ods5-vms3/wo, holds img/phvs2-copy-2.img (VAX V7.3) and a5/img/asim5-copy-2.img (V8.4, ODS-2). SKIP where they are not found. Called by `make test`.
D=${WO_VMS_DIR:-$HOME/tmp/ods5-vms3/wo}
fails=0
for img in "$D/img/phvs2-copy-2.img" "$D/a5/img/asim5-copy-2.img"; do
    if [ ! -f "$img" ]; then echo "SKIP: $img not found (the VMS containers of PLAN_WRITE_OPS step 0)"; continue; fi
    python3 -I tests/dirhdr_vms_compare.py "$img" || fails=$((fails + 1))
done
if [ $fails -ne 0 ]; then echo "cli_dirhdr_vms_check: $fails FAILED"; exit 1; fi
echo "cli_dirhdr_vms_check: done"
