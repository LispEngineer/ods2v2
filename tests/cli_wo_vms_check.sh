#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_wo_vms_check.sh - TYPE with wildcards, PURGE and SET FILE/PROTECTION, /OWNER_UIC (PLAN_WRITE_OPS items 2 to 4) against what VMS itself did in step 0
# (CORPUS 49 on VAX V7.3, 49.8 on V8.4): tests/wo_vms_compare.py. The logs and containers are ods5-vms3's, not part of this repository: WO_VMS_LOGS, default
# ~/src/ods5-cli/corpus/manifests (write-ops-step0-phvs2, write-ops-step0-asim5), and WO_VMS_DIR, default ~/tmp/ods5-vms3/wo (img/phvs2-*.img, a5/img/asim5-*.img).
# SKIP where they are not found. Called by `make test`; run from the project root.
LG=${WO_VMS_LOGS:-$HOME/src/ods5-cli/corpus/manifests}
D=${WO_VMS_DIR:-$HOME/tmp/ods5-vms3/wo}
fails=0
one() { # one <kind> <level> <log> <container>
    if [ ! -f "$3" ] || [ ! -f "$4" ]; then echo "SKIP: $3 or $4 not found (the VMS logs and containers of PLAN_WRITE_OPS step 0)"; return; fi
    python3 -I tests/wo_vms_compare.py "$1" "$2" "$3" "$4" || fails=$((fails + 1))
}
for k in type purge setf; do
    K=$(echo $k | tr a-z A-Z)
    one $k 2 "$LG/write-ops-step0-phvs2/WO_${K}_2.LOG" "$D/img/phvs2-$k-2.img"
    one $k 2 "$LG/write-ops-step0-asim5/WO_${K}_2.LOG" "$D/a5/img/asim5-$k-2.img"
    one $k 5 "$LG/write-ops-step0-asim5/WO_${K}_5.LOG" "$D/a5/img/asim5-$k-5.img"
done
rm -rf "${TMPDIR:-samples}/wo_vms_compare"
if [ $fails -ne 0 ]; then echo "cli_wo_vms_check: $fails FAILED"; exit 1; fi
echo "cli_wo_vms_check: done"
