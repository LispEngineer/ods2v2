#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_mfd_check.sh - the header of the master file directory (file 4, [000000]000000.DIR) of a new volume, both levels (review 52 F1). INITIALIZE writes it as a reserved file:
# RECPROT 0xFE00 and revision 1 (not the RECPROT 0 and revision 0 of a directory made by CREATE/DIRECTORY, which the header builder now gives every directory).
# 1. the fields are written as above: always run;
# 2. they equal those of the VMS-made volumes (V8.4-2L2 and V7.3-2 of ods5-cli's corpus at both levels, VAX V7.3 and V8.4 of ods5-vms3's containers): fileprot, recprot,
#    revision, highwater, efblk, filechar, read by an independent reader (tests/dirhdr_fields.py); SKIP where an image is not found.
# Fails on ods5-dirhdr 35f59f8 (RECPROT 0, revision 0). Called by `make test`; run from the project root.
TMP=samples/cli_mfd_tmp
C=${VMS_CORPUS_DIR:-$HOME/tmp/ods5-cli}
D=${WO_VMS_DIR:-$HOME/tmp/ods5-vms3/wo}
fails=0
rm -rf "$TMP"; mkdir -p "$TMP"
want="fileprot=ba00 recprot=fe00 revision=1 highwater=2 efblk=2 filechar=00002080"
pick() { # pick <image>: the fields compared, from the MFD
    python3 -I tests/dirhdr_fields.py "$1" '[000000]000000.DIR' | tr ' ' '\n' | grep -E '^(fileprot|recprot|revision|highwater|efblk|filechar)=' | tr '\n' ' ' | sed 's/ $//'
}
for level in 2 5; do
    for cluster in 1 8; do
        img="$TMP/m$level-$cluster.img"
        ./ods2_init "$img" 20000 --structure=$level --label=MFD$level --cluster=$cluster >/dev/null 2>&1
        got=$(pick "$img")
        # a fixed order of fields for the comparison
        norm=$(echo "$got" | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')
        exp=$(echo "$want" | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')
        if [ "$norm" = "$exp" ]; then echo "PASS: ODS-$level cluster $cluster: file 4 has RECPROT 0xFE00 and revision 1 ($got)"
        else echo "FAIL: ODS-$level cluster $cluster: file 4"; echo "      expected: $exp"; echo "      got:      $norm"; fails=$((fails + 1)); fi
        ref="$norm"
    done
done
for v in "$C/asim12-ods2t-20261108.img" "$C/asim12-ods5t-20261108.img" "$C/asim6-ods2t6-20261108.img" "$C/asim6-ods5t6-20261108.img" "$D/img/phvs2-copy-2.img" "$D/a5/img/asim5-copy-2.img" "$D/a5/img/asim5-copy-5.img"; do
    if [ ! -f "$v" ]; then echo "SKIP: $v not found (VMS-made volume)"; continue; fi
    got=$(pick "$v" | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$got" = "$ref" ]; then echo "PASS: file 4 of $(basename "$v") equals that of the new volumes ($got)"
    else echo "FAIL: file 4 of $(basename "$v")"; echo "      new volume: $ref"; echo "      VMS:        $got"; fails=$((fails + 1)); fi
done
for img in "$TMP"/m2-1.img "$TMP"/m5-8.img; do
    r=$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)
    [ "$r" = "0 error(s), 0 warning(s)" ] && echo "PASS: ods5check clean ($(basename "$img"))" || { echo "FAIL: ods5check $img: $r"; fails=$((fails + 1)); }
done
rm -rf "$TMP"
if [ "$fails" -ne 0 ]; then echo "cli_mfd_check: $fails FAILED"; exit 1; fi
echo "cli_mfd_check: all checks passed"
