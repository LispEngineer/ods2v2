#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_filehdr_check.sh - the header of a user file as VMS writes it (follow-up b; the fields already measured, CORPUS 49, 52 and the VMS-made ODS-2 images of ods5-cli's corpus):
#   * RECPROT 0 (VMS: all 2,644 user files of the VMS-made ODS-2 images on VAX V7.3, V7.3-2 and V8.4; only the reserved files carry 0xFE00);
#   * no CONTIG (FILECHAR 0x80) on a file (none of those files has it, 225 of them in one piece; a directory is 0x2080 and keeps CONTIG);
#   * "Highest block written" = HIBLK + 1, the whole allocation, which ods2v2 writes or zero-fills (DCL-written files; ODS-5: ods2v2 already does it).
# ods2v2 wrote RECPROT 0xFE00, CONTIG for a one-piece file and HIGHWATER 0 on ODS-2; ODS-5 already had the VMS values (the control, it passes). Written before the change
# (tests first): it fails on ods5-dirhdr 35f59f8. The header is read by an independent reader (tests/dirhdr_fields.py). The reserved files are NOT changed (the guard here).
# Called by `make test`; run from the project root.

TMP=samples/cli_filehdr_tmp
INIT=./ods2_init
ODS2=./ods2
fails=0

check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        echo "      expected: $2"
        echo "      got:      $3"
        fails=$((fails + 1))
    fi
}
fld() { # fld <img> <spec> <field>
    python3 -I tests/dirhdr_fields.py "$1" "$2" | tr ' ' '\n' | sed -n "s/^$3=//p"
}
clean() { python3 -I tests/ods5check.py "$1" 2>&1 | tail -1; }

rm -rf "$TMP"
mkdir -p "$TMP/imp"
printf 'one block of text\n' > "$TMP/small.txt"
awk 'BEGIN { for (i = 0; i < 40; i++) printf "line %03d of a file that takes several blocks, padded with some more text to fill them up\n", i }' > "$TMP/multi.txt"
head -c 20000 /dev/urandom > "$TMP/bin.dat"
cp "$TMP/small.txt" "$TMP/imp/I1.TXT"; cp "$TMP/multi.txt" "$TMP/imp/I2.TXT"

for level in 2 5; do
    for cluster in 1 8; do
        img="$TMP/fh$level-$cluster.img"
        $INIT "$img" 20000 --structure=$level --label=FH$level --cluster=$cluster >/dev/null 2>&1
        echo "== ODS-$level, cluster $cluster"
        cat > "$TMP/mk.cmd" <<CMD
CREATE/DIRECTORY [D]
COPY $TMP/small.txt [D]SMALL.TXT
COPY $TMP/multi.txt [D]MULTI.TXT
COPY $TMP/bin.dat [D]BIN.DAT
COPY $TMP/small.txt [D]SMALL.TXT
IMPORT $TMP/imp [D]
CMD
        $ODS2 "$img" "@$TMP/mk.cmd" >/dev/null 2>&1
        for f in 'SMALL.TXT;1' 'SMALL.TXT;2' 'MULTI.TXT' 'BIN.DAT' 'I1.TXT' 'I2.TXT'; do
            spec="[D]$f"
            check "ODS-$level/$cluster $f: RECPROT 0, no CONTIG, HIGHWATER = HIBLK + 1" "0000 0 $(( $(fld "$img" "$spec" hiblk) + 1 ))" \
                "$(fld "$img" "$spec" recprot) $(( 0x$(fld "$img" "$spec" filechar) & 0x80 )) $(fld "$img" "$spec" highwater)"
        done
        check "ODS-$level/$cluster: a directory keeps CONTIG (FILECHAR 0x2080) and its header" "00002080 ba88" "$(fld "$img" '[000000]D.DIR' filechar) $(fld "$img" '[000000]D.DIR' fileprot)"
        check "ODS-$level/$cluster: the reserved files keep RECPROT 0xFE00 (INDEXF.SYS, BITMAP.SYS, the master directory)" "fe00 fe00 fe00" \
            "$(fld "$img" '[000000]INDEXF.SYS' recprot) $(fld "$img" '[000000]BITMAP.SYS' recprot) $(fld "$img" '[000000]000000.DIR' recprot)"
        check "ODS-$level/$cluster: a file's protection and revision are as before (0xFA00, 1)" "fa00 1" "$(fld "$img" '[D]MULTI.TXT' fileprot) $(fld "$img" '[D]MULTI.TXT' revision)"
        check "ODS-$level/$cluster: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    done
done

# a file written into a fragmented volume (several pieces) and a symbolic link (ODS-5) have the same fields
img="$TMP/frag.img"
$INIT "$img" 20000 --structure=2 --label=FHF --cluster=1 >/dev/null 2>&1
awk 'BEGIN { for (i = 0; i < 300; i++) printf "COPY samples/cli_filehdr_tmp/small.txt [D]X%04d.TXT\n", i }' > "$TMP/frag.cmd"
( echo 'CREATE/DIRECTORY [D]'; cat "$TMP/frag.cmd"; k=0; while [ $k -lt 300 ]; do printf 'DELETE [D]X%04d.TXT\n' $k; k=$((k + 2)); done ) > "$TMP/frag2.cmd"
$ODS2 "$img" "@$TMP/frag2.cmd" >/dev/null 2>&1
head -c 150000 /dev/urandom > "$TMP/big.dat"
$ODS2 "$img" "COPY $TMP/big.dat [D]BIGPIECES.DAT" >/dev/null 2>&1
check "ODS-2: a file in several pieces: RECPROT 0, no CONTIG, HIGHWATER = HIBLK + 1" "0000 0 $(( $(fld "$img" '[D]BIGPIECES.DAT' hiblk) + 1 ))" "$(fld "$img" '[D]BIGPIECES.DAT' recprot) $(( 0x$(fld "$img" '[D]BIGPIECES.DAT' filechar) & 0x80 )) $(fld "$img" '[D]BIGPIECES.DAT' highwater)"
check "ODS-2: ods5check is clean (fragmented volume)" "0 error(s), 0 warning(s)" "$(clean "$img")"

if [ "$fails" -eq 0 ]; then echo "cli_filehdr_check: all checks passed"; else echo "cli_filehdr_check: $fails FAILED"; fi
exit $fails
