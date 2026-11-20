#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_dirhdr_check.sh - the header of a new directory as VMS's CREATE/DIRECTORY makes it (follow-up c; CORPUS 49 and 53: the directories of the step 0 containers of VAX
# V7.3 and V8.4, ODS-2 and ODS-5): protection 0xBA88 (S:RWE,O:RWE,G:RE,W:E), record protection 0, revision number 0, and "Highest block written" = HIBLK + 1 (the whole
# allocation is written). ods2v2 wrote 0xFA00, 0xFE00, 1 and 0 for a directory on ODS-2; ODS-5 already had the VMS values (the control here). A directory that grows takes 2 to
# its revision for each block it gains (measured, ODS5_FORMAT_NOTES 15.4) from that 0, and keeps its highest block written at or above its end of file. The header is read by an
# independent reader (tests/dirhdr_fields.py). Called by `make test`; run from the project root.

TMP=samples/cli_dirhdr_tmp
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
mkdir -p "$TMP"
printf 'one\n' > "$TMP/f.txt"

for level in 2 5; do
    img="$TMP/dh$level.img"
    $INIT "$img" 20000 --structure=$level --label=DH$level --cluster=1 >/dev/null 2>&1
    echo "== ODS-$level"
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    d='[000000]D.DIR'
    check "ODS-$level: a new directory has FILEPROT 0xBA88 (S:RWE,O:RWE,G:RE,W:E)" "ba88" "$(fld "$img" $d fileprot)"
    check "ODS-$level: ... RECPROT 0" "0000" "$(fld "$img" $d recprot)"
    check "ODS-$level: ... revision number 0 (a file's is 1)" "0" "$(fld "$img" $d revision)"
    check "ODS-$level: ... Highest block written = HIBLK + 1" "$(( $(fld "$img" $d hiblk) + 1 ))" "$(fld "$img" $d highwater)"
    $ODS2 "$img" "COPY $TMP/f.txt [D]F.TXT" >/dev/null 2>&1
    check "ODS-$level: a file in it has the file values (FILEPROT 0xFA00, revision 1)" "fa00 1" "$(fld "$img" '[D]F.TXT' fileprot) $(fld "$img" '[D]F.TXT' revision)"
    check "ODS-$level: a sub-directory too" "ba88 0" "$($ODS2 "$img" 'CREATE/DIRECTORY [D.SUB]' >/dev/null 2>&1; fld "$img" '[D]SUB.DIR' fileprot) $(fld "$img" '[D]SUB.DIR' revision)"

    # --- growth: 400 names of 32 characters make the directory grow ----------------------------------------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [G]" >/dev/null 2>&1
    k=0; : > "$TMP/g.cmd"
    while [ $k -lt 400 ]; do printf 'COPY %s [G]DIRFILE_%04d_PADDING_TO_32CH.TXT\n' "$TMP/f.txt" $k >> "$TMP/g.cmd"; k=$((k + 1)); done
    $ODS2 "$img" "@$TMP/g.cmd" >/dev/null 2>&1
    g='[000000]G.DIR'
    ef=$(fld "$img" $g efblk); rv=$(fld "$img" $g revision); hw=$(fld "$img" $g highwater)
    check "ODS-$level: a grown directory (400 names) has revision 2 x (blocks it gained): 2 x (EFBLK - 2)" "$((2 * (ef - 2)))" "$rv"
    check "ODS-$level: ... and its Highest block written is at or past its end of file" "ok" "$([ "$hw" -ge "$ef" ] && echo ok || echo "highwater $hw, efblk $ef")"
    check "ODS-$level: ... FILEPROT is still 0xBA88" "ba88" "$(fld "$img" $g fileprot)"
    check "ODS-$level: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
done

if [ "$fails" -eq 0 ]; then echo "cli_dirhdr_check: all checks passed"; else echo "cli_dirhdr_check: $fails FAILED"; fi
exit $fails
