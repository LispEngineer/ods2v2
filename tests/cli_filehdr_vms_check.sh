#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_filehdr_vms_check.sh - the rule of follow-up b (RECPROT 0, no CONTIG, HIGHWATER = HIBLK + 1 for a user file) holds in the VMS-made ODS-2 images of ods5-cli's corpus and ods5-vms3's
# step 0 containers: tests/filehdr_vms_rule.py. VMS_CORPUS_DIR (default ~/tmp/ods5-cli) holds asim12-ods2t-20261108.img (V8.4-2L2) and asim6-ods2t6-20261108.img (V7.3-2); WO_VMS_DIR
# (default ~/tmp/ods5-vms3/wo) holds img/phvs2-copy-2.img (VAX V7.3) and a5/img/asim5-copy-2.img (V8.4). SKIP where not found. Called by `make test`.
C=${VMS_CORPUS_DIR:-$HOME/tmp/ods5-cli}
D=${WO_VMS_DIR:-$HOME/tmp/ods5-vms3/wo}
fails=0
for img in "$C/asim12-ods2t-20261108.img" "$C/asim6-ods2t6-20261108.img" "$D/img/phvs2-copy-2.img" "$D/a5/img/asim5-copy-2.img"; do
    if [ ! -f "$img" ]; then echo "SKIP: $img not found (VMS-made ODS-2 images)"; continue; fi
    python3 -I tests/filehdr_vms_rule.py "$img" || fails=$((fails + 1))
done
if [ $fails -ne 0 ]; then echo "cli_filehdr_vms_check: $fails FAILED"; exit 1; fi
echo "cli_filehdr_vms_check: done"
