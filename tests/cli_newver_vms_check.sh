#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_newver_vms_check.sh - COPY to a new version against the directories VMS itself wrote (step 0 of PLAN_WRITE_OPS: CORPUS 49 on VAX V7.3,
# 49.8 on V8.4): tests/newver_vms_compare.py. The containers are ods5-vms3's (not part of this repository): NEWVER_VMS_DIR, default
# ~/tmp/ods5-vms3/wo, holding a5/img/asim5-copy-{2,5}.img and img/phvs2-copy-2.img. SKIP where they are not found. Called by `make test`.
D=${NEWVER_VMS_DIR:-$HOME/tmp/ods5-vms3/wo}
fails=0
run_one() { # run_one <level> <image>
    if [ ! -f "$2" ]; then echo "SKIP: $2 not found (the VMS containers of PLAN_WRITE_OPS step 0)"; return; fi
    python3 -I tests/newver_vms_compare.py "$1" "$2" || fails=$((fails + 1))
}
run_one 2 "$D/a5/img/asim5-copy-2.img"
run_one 5 "$D/a5/img/asim5-copy-5.img"
run_one 2 "$D/img/phvs2-copy-2.img"
rm -rf "${TMPDIR:-samples}/newver_vms_compare"
if [ $fails -ne 0 ]; then echo "cli_newver_vms_check: $fails FAILED"; exit 1; fi
echo "cli_newver_vms_check: done"
