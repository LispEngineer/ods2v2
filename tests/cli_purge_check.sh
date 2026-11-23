#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_purge_check.sh - PURGE (PLAN_WRITE_OPS item 2), ODS-2 and ODS-5. The expectations are what VMS itself did (CORPUS 49.2 on VAX V7.3, 49.8 on V8.4,
# 49.7 for the process that is not the volume owner), not memory:
#   * PURGE [dir]NAME.TYP keeps the highest version (default /KEEP=1) and deletes the others, highest of the deleted first; /LOG prints
#     "%PURGE-I-FILPURG, <spec> deleted (n blocks)" for each and, when more than one went, "%PURGE-I-TOTAL, n files deleted (b blocks)";
#   * /KEEP=n keeps the n highest; /KEEP=0 is %PURGE-F-BADVALUE and nothing changes; nothing to purge, or a missing name: %PURGE-I-NOFILPURG (after
#     %PURGE-W-SEARCHFAIL with -RMS-E-FNF for a missing name); wildcards in the name; no spec purges the default directory;
#   * on a read-only volume each version to delete is %PURGE-W-FILNOTPUR with -RMS-E-MKD and -SYSTEM-F-WRITLCK, and nothing changes;
#   * a version whose protection forbids deleting is purged too: ods2v2 is a privileged process (VMS refuses it to an unprivileged process that is not the
#     volume owner: -SYSTEM-F-NOPRIV, CORPUS 49.7);
#   * an alias or a hard link is reported as DELETE reports it. The device VMS prints in front of the specification is not printed ("[P]X.TXT;5").
# After every step the independent checker (ods5check.py) is clean. Called by `make test`; run from the project root.

TMP=samples/cli_purge_tmp
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
vers() { # vers <img> <spec>: the versions a DIR of spec lists, as "name;ver ..." in order
    $ODS2 "$1" "DIR $2" 2>&1 | awk '/;[0-9]+[ ]*$/ { v = $0; sub(/[ \t]+/, "", v); gsub(/[ \t]+/, "", v); printf "%s ", v }' | sed 's/ $//'
}
clean() { python3 -I tests/ods5check.py "$1" 2>&1 | tail -1; }
free_blocks() { $ODS2 "$1" 'SHOW VOLUME' 2>&1 | awk '/Free blocks:/ {print $3}'; }
mk() { # mk <img> <dir> <name> <count>: <count> versions of dir name, two blocks each
    n=1
    while [ $n -le "$4" ]; do $ODS2 "$1" "COPY $TMP/two.txt [$2]$3" >/dev/null 2>&1; n=$((n + 1)); done
}

rm -rf "$TMP"
mkdir -p "$TMP"
awk 'BEGIN { for (i = 0; i < 12; i++) printf "line %02d of a two block file, padded with some more text to fill the blocks well\n", i }' > "$TMP/two.txt"

for level in 2 5; do
    img="$TMP/pg$level.img"
    $INIT "$img" 20000 --structure=$level --label=PG$level --cluster=1 >/dev/null 2>&1
    echo "== ODS-$level"
    $ODS2 "$img" "CREATE/DIRECTORY [P]" >/dev/null 2>&1

    # --- P1: X.TXT;1 to ;6 (2 blocks each), Y.TXT;1 ;2, Z.DAT;1 to ;4 ------------------------------------------------------------------------------
    mk "$img" P X.TXT 6; mk "$img" P Y.TXT 2; mk "$img" P Z.DAT 4
    fb=$(free_blocks "$img")
    o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1)
    want="%PURGE-I-FILPURG, [P]X.TXT;5 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;4 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;3 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;2 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;1 deleted (2 blocks)
%PURGE-I-TOTAL, 5 files deleted (10 blocks)"
    check "ODS-$level P1: PURGE/LOG of a name with six versions says what VMS says, highest of the deleted first, and a TOTAL" "$want" "$(echo "$o" | sed 's/ blocks)/ blocks)/')"
    check "ODS-$level P1: only ;6 is left, and ten blocks came back" "X.TXT;6 $((fb + 10))" "$(vers "$img" '[P]X.TXT;*') $(free_blocks "$img")"
    check "ODS-$level P1: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- P2: /KEEP=3 on four versions: one is deleted, no TOTAL for one file --------------------------------------------------------------------------
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=3 [P]Z.DAT" 2>&1)
    check "ODS-$level P2: /KEEP=3 of four versions deletes ;1 only (one FILPURG line, no TOTAL)" "%PURGE-I-FILPURG, [P]Z.DAT;1 deleted (2 blocks)|Z.DAT;4 Z.DAT;3 Z.DAT;2" "$(echo "$o" | tr '\n' '|' | sed 's/|$//')|$(vers "$img" '[P]Z.DAT;*')"

    # --- P3: keep more than exist, a single version, a name that does not exist ---------------------------------------------------------------------
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=10 [P]Y.TXT" 2>&1)
    check "ODS-$level P3: /KEEP=10 of two versions purges nothing: %PURGE-I-NOFILPURG" "%PURGE-I-NOFILPURG, no files purged" "$o"
    o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1)
    check "ODS-$level P3: PURGE of a name with one version: %PURGE-I-NOFILPURG" "%PURGE-I-NOFILPURG, no files purged" "$o"
    o=$($ODS2 "$img" "PURGE/LOG [P]NOSUCH.TXT" 2>&1)
    check "ODS-$level P3: a name that does not exist: SEARCHFAIL, FNF, then NOFILPURG" "%PURGE-W-SEARCHFAIL, error searching for [P]NOSUCH.TXT;*|-RMS-E-FNF, file not found|%PURGE-I-NOFILPURG, no files purged" "$(echo "$o" | tr '\n' '|' | sed 's/|$//')"
    o=$($ODS2 "$img" "PURGE/LOG [NODIR]X.TXT" 2>&1)
    check "ODS-$level P3: a directory that does not exist: SEARCHFAIL, DNF, NOSUCHFILE, NOFILPURG" "%PURGE-W-SEARCHFAIL, error searching for [NODIR]X.TXT;*|-RMS-E-DNF, directory not found|-SYSTEM-W-NOSUCHFILE, no such file|%PURGE-I-NOFILPURG, no files purged" "$(echo "$o" | tr '\n' '|' | sed 's/|$//')"

    # --- P4: /KEEP=2 with a wildcard: X.TXT (7 versions ;6 to ;12), Y.TXT (4), Z.DAT (7); Z.DAT is not touched; then PURGE with no spec ------------------------
    $ODS2 "$img" "DELETE [P]*.*;*" >/dev/null 2>&1
    mk "$img" P X.TXT 12; $ODS2 "$img" "DELETE [P]X.TXT;1" >/dev/null 2>&1; $ODS2 "$img" "DELETE [P]X.TXT;2" >/dev/null 2>&1; $ODS2 "$img" "DELETE [P]X.TXT;3" >/dev/null 2>&1
    $ODS2 "$img" "DELETE [P]X.TXT;4" >/dev/null 2>&1; $ODS2 "$img" "DELETE [P]X.TXT;5" >/dev/null 2>&1
    mk "$img" P Y.TXT 4; mk "$img" P Z.DAT 7
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=2 [P]*.TXT" 2>&1)
    want="%PURGE-I-FILPURG, [P]X.TXT;10 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;9 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;8 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;7 deleted (2 blocks)
%PURGE-I-FILPURG, [P]X.TXT;6 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Y.TXT;2 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Y.TXT;1 deleted (2 blocks)
%PURGE-I-TOTAL, 7 files deleted (14 blocks)"
    check "ODS-$level P4: /KEEP=2 [P]*.TXT keeps the two highest of each .TXT name, in name order, and does not touch Z.DAT" "$want" "$o"
    check "ODS-$level P4: left: X.TXT;12 ;11, Y.TXT;4 ;3, Z.DAT with its 7 versions" "X.TXT;12 X.TXT;11 Y.TXT;4 Y.TXT;3 Z.DAT;7 Z.DAT;6 Z.DAT;5 Z.DAT;4 Z.DAT;3 Z.DAT;2 Z.DAT;1" "$(vers "$img" '[P]*.*;*')"
    o=$(printf 'SET DEFAULT [P]\nPURGE/LOG\n' > "$TMP/dflt.cmd"; $ODS2 "$img" "@$TMP/dflt.cmd" 2>&1 | grep -v '^ODS2>')
    want="%PURGE-I-FILPURG, [P]X.TXT;11 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Y.TXT;3 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;6 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;5 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;4 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;3 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;2 deleted (2 blocks)
%PURGE-I-FILPURG, [P]Z.DAT;1 deleted (2 blocks)
%PURGE-I-TOTAL, 8 files deleted (16 blocks)"
    check "ODS-$level P4: PURGE with no spec takes every name of the default directory down to its highest version" "$want" "$(echo "$o" | grep -v '^%ODS2-I-DEFSET')"
    check "ODS-$level P4: the highest of each is left" "X.TXT;12 Y.TXT;4 Z.DAT;7" "$(vers "$img" '[P]*.*;*')"

    # --- P5: a version that its owner may not delete is purged too (ods2v2 is privileged: VMS refuses an unprivileged process that is not the volume owner) ----
    $ODS2 "$img" "DELETE [P]*.*;*" >/dev/null 2>&1; mk "$img" P Q.TXT 3
    printf 'SET FILE/PROTECTION=(S:RWED,O:R,G:R,W:R) [P]Q.TXT;2\n' > "$TMP/prot.cmd"; $ODS2 "$img" "@$TMP/prot.cmd" >/dev/null 2>&1
    o=$($ODS2 "$img" "PURGE/LOG [P]Q.TXT" 2>&1)
    check "ODS-$level P5: a version with the owner field R only is purged (no NOPRIV refusal)" "2 | Q.TXT;3" "$(echo "$o" | grep -c FILPURG) | $(vers "$img" '[P]Q.TXT;*')"

    # --- P5b: /KEEP=0 and other bad values ------------------------------------------------------------------------------------------------------------
    mk "$img" P Q.TXT 2
    before=$(vers "$img" '[P]Q.TXT;*')
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=0 [P]Q.TXT" 2>&1)
    check "ODS-$level P5b: /KEEP=0 is %PURGE-F-BADVALUE and nothing changes" "%PURGE-F-BADVALUE, '0' is an invalid keyword value|$before" "$(echo "$o" | tr '\n' '|' | sed 's/|$//')|$(vers "$img" '[P]Q.TXT;*')"
    o=$($ODS2 "$img" "PURGE/KEEP=x [P]Q.TXT" 2>&1)
    check "ODS-$level P5b: /KEEP=x is BADVALUE too" "%PURGE-F-BADVALUE, 'x' is an invalid keyword value" "$o"
    check "ODS-$level P5b: without /LOG nothing is printed for a purge that works" "" "$($ODS2 "$img" "PURGE [P]Q.TXT" 2>&1)"
    check "ODS-$level P5b: ... and the highest is left" "Q.TXT;5" "$(vers "$img" '[P]Q.TXT;*')"

    # --- P6: a read-only volume: each version to delete is FILNOTPUR with MKD and WRITLCK; nothing changes -----------------------------------------------
    $ODS2 "$img" "DELETE [P]*.*;*" >/dev/null 2>&1; mk "$img" P Z.DAT 5; mk "$img" P X.TXT 1
    chmod 444 "$img"
    o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1 | grep -v '^%ODS2-W-READONLY')
    check "ODS-$level P6: read-only mount, one version: %PURGE-I-NOFILPURG with no error" "%PURGE-I-NOFILPURG, no files purged" "$o"
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=3 [P]Z.DAT" 2>&1 | grep -v '^%ODS2-W-READONLY')
    want="%PURGE-W-FILNOTPUR, error deleting [P]Z.DAT;2
-RMS-E-MKD, ACP could not mark file for deletion
-SYSTEM-F-WRITLCK, write lock error
%PURGE-W-FILNOTPUR, error deleting [P]Z.DAT;1
-RMS-E-MKD, ACP could not mark file for deletion
-SYSTEM-F-WRITLCK, write lock error
%PURGE-I-NOFILPURG, no files purged"
    check "ODS-$level P6: read-only mount, five versions /KEEP=3: FILNOTPUR + MKD + WRITLCK for each version to delete, then NOFILPURG" "$want" "$o"
    check "ODS-$level P6: nothing changed" "Z.DAT;5 Z.DAT;4 Z.DAT;3 Z.DAT;2 Z.DAT;1" "$(vers "$img" '[P]Z.DAT;*')"
    chmod 644 "$img"
    check "ODS-$level P6: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- P7: PURGE deletes through the DELETE path: the entry of a file that another name also lists (an alias, no HARDLINKS) goes with its file, and the alias is left
    # dangling, which is what DELETE of that version does (ods5check says so in one warning) ---------------------------------------------------------------------
    $ODS2 "$img" "DELETE [P]*.*;*" >/dev/null 2>&1; mk "$img" P W.TXT 3
    $ODS2 "$img" "LINK [P]WALIAS.TXT [P]W.TXT;1" >/dev/null 2>&1
    o=$($ODS2 "$img" "PURGE/LOG [P]W.TXT" 2>&1)
    check "ODS-$level P7: PURGE of W.TXT, whose ;1 is also listed as WALIAS.TXT, leaves ;3 and the alias entry" "W.TXT;3 WALIAS.TXT;1" "$(vers "$img" '[P]*.*;*')"
    check "ODS-$level P7: ods5check says only that the alias is dangling" "0 error(s), 1 warning(s)" "$(clean "$img")"

    # --- P8: a name whose versions are stored out of order (review 48 F3, then H1 / CORPUS 66.4): VMS keeps the FIRST STORED entry (PURGE X.TXT leaves ;1 of ;1 ;2 ;3), the highest
    # number is not what it follows; ods2v2 used to keep the highest number, and both can lose the file the user wanted on a damaged directory. PURGE now REFUSES the name (BAD_VERSORDER)
    # and every entry stays as stored (a deliberate divergence from VMS, in the README) -------------------------------------------------------------------------------
    $ODS2 "$img" "DELETE [P]*.*;*" >/dev/null 2>&1; mk "$img" P X.TXT 3
    dn=$($ODS2 "$img" 'DIR/FULL [000000]P.DIR' 2>&1 | sed -n 's/.*File ID: *(\([0-9]*\),.*/\1/p' | head -1)
    python3 -I tests/swap_versions.py "$img" "$dn" X.TXT >/dev/null 2>&1
    o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1)
    check "ODS-$level P8: versions stored as ;1 ;2 ;3: PURGE refuses the name (BAD_VERSORDER, nothing deleted)" "1|X.TXT;1 X.TXT;2 X.TXT;3" "$(echo "$o" | grep -c 'BAD_VERSORDER')|$(vers "$img" '[P]X.TXT;*')"

    # --- P11: version numbers that are not usable (review 49 S1): the name is refused with FILNOTPUR and the reason, nothing of it is deleted, the other names go on ----------
    for case in "0:2:0:a version number is 0" "1:1:3:two entries have the same version number" "2:1:40000:a version number is over 32767"; do
        tag=${case%%:*}; rest=${case#*:}; idx=${rest%%:*}; rest=${rest#*:}; val=${rest%%:*}; why=${rest#*:}
        d=R$tag
        $ODS2 "$img" "CREATE/DIRECTORY [$d]" >/dev/null 2>&1; mk "$img" $d X.TXT 3; mk "$img" $d Y.TXT 3
        dn=$($ODS2 "$img" "DIR/FULL [000000]$d.DIR" 2>&1 | sed -n 's/.*File ID: *(\([0-9]*\),.*/\1/p' | head -1)
        python3 -I tests/swap_versions.py "$img" "$dn" X.TXT setver "$idx" "$val" >/dev/null 2>&1
        before=$(vers "$img" "[$d]X.TXT;*")
        o=$($ODS2 "$img" "PURGE/LOG [$d]*.TXT" 2>&1)
        check "ODS-$level P11.$tag: X.TXT with $why is refused (FILNOTPUR and the reason), its entries are as they were; Y.TXT is purged" "%PURGE-W-FILNOTPUR, error deleting [$d]X.TXT: $why (the directory entry is damaged): the name is not purged|%PURGE-I-FILPURG, [$d]Y.TXT;2 deleted (2 blocks)|%PURGE-I-FILPURG, [$d]Y.TXT;1 deleted (2 blocks)|%PURGE-I-TOTAL, 2 files deleted (4 blocks)|$before|Y.TXT;3" \
            "$(echo "$o" | tr '\n' '|')$(vers "$img" "[$d]X.TXT;*")|$(vers "$img" "[$d]Y.TXT;*")"
    done

    # --- P13: a name held in two records with another name between them (review 50 F1): every group of the name is refused, once, and no file is lost; other names go on ---
    d=R7
    $ODS2 "$img" "CREATE/DIRECTORY [$d]" >/dev/null 2>&1; mk "$img" $d A.TXT 2; mk "$img" $d B.TXT 1; mk "$img" $d C.TXT 3; mk "$img" $d D.TXT 3
    dn=$($ODS2 "$img" "DIR/FULL [000000]$d.DIR" 2>&1 | sed -n 's/.*File ID: *(\([0-9]*\),.*/\1/p' | head -1)
    python3 -I tests/swap_versions.py "$img" "$dn" C.TXT rename A.TXT >/dev/null 2>&1
    before=$(vers "$img" "[$d]*.TXT;*")
    o=$($ODS2 "$img" "PURGE/LOG [$d]*.TXT" 2>&1)
    check "ODS-$level P13: A.TXT in two records (B.TXT between): refused once with FILNOTPUR, nothing of it deleted; D.TXT is purged in the same command" "%PURGE-W-FILNOTPUR, error deleting [$d]A.TXT: the name is listed in more than one place in the directory (the directory entry is damaged): the name is not purged|%PURGE-I-FILPURG, [$d]D.TXT;2 deleted (2 blocks)|%PURGE-I-FILPURG, [$d]D.TXT;1 deleted (2 blocks)|%PURGE-I-TOTAL, 2 files deleted (4 blocks)|" \
        "$(echo "$o" | tr '\n' '|')"
    after=$(vers "$img" "[$d]*.TXT;*")
    check "ODS-$level P13: the entries of A.TXT and B.TXT are as they were (no version of A, no file lost)" "$(echo "$before" | sed 's/ D.TXT;[0-9]*//g')" "$(echo "$after" | sed 's/ D.TXT;[0-9]*//g')"
    o=$($ODS2 "$img" "PURGE/LOG [$d]A*.TXT" 2>&1)
    check "ODS-$level P13: a wildcard that matches only A.TXT (the two records are then neighbours, one group with the numbers twice): refused too" "%PURGE-W-FILNOTPUR, error deleting [$d]A.TXT: two entries have the same version number (the directory entry is damaged): the name is not purged|%PURGE-I-NOFILPURG, no files purged|" "$(echo "$o" | tr '\n' '|')"

    # --- P12: /KEEP= values and qualifiers that do not fit are refused, never cut (review 49 N1) ---------------------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [R9]" >/dev/null 2>&1; mk "$img" R9 X.TXT 4
    z57=$(awk 'BEGIN { for (i = 0; i < 57; i++) printf "0" }')
    o=$($ODS2 "$img" "PURGE/LOG/KEEP=${z57}25 [R9]X.TXT" 2>&1)
    check "ODS-$level P12: /KEEP= with 59 characters is refused (it used to be cut to 58 and read as 2), nothing purged" "1 4" "$(echo "$o" | grep -c 'IVQUAL') $(vers "$img" '[R9]X.TXT;*' | wc -w)"
done

# --- P10: two specs, or a comma list, are refused with a message and purge nothing (review 49, N7: the second used to be ignored in silence) ---------------------
img="$TMP/pg2s.img"
$INIT "$img" 20000 --structure=5 --label=PG2S --cluster=1 >/dev/null 2>&1
$ODS2 "$img" "CREATE/DIRECTORY [P]" >/dev/null 2>&1; mk "$img" P X.TXT 3; mk "$img" P Y.TXT 3
o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT [P]Y.TXT" 2>&1)
check "P10: PURGE [P]X.TXT [P]Y.TXT is refused and says so; neither name is purged" "%PURGE-F-TOOMANY, PURGE takes one file specification, and \"[P]Y.TXT\" follows \"[P]X.TXT\": nothing is purged (run PURGE once for each)|X.TXT;3 X.TXT;2 X.TXT;1 Y.TXT;3 Y.TXT;2 Y.TXT;1" "$(echo "$o" | tr '\n' '|')$(vers "$img" '[P]*.*;*')"
o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT,Y.TXT" 2>&1)
check "P10: a comma list is refused the same way, nothing is purged" "%PURGE-F-TOOMANY, PURGE takes one file specification, not a list (\"[P]X.TXT,Y.TXT\"): nothing is purged (run PURGE once for each)|X.TXT;3 X.TXT;2 X.TXT;1 Y.TXT;3 Y.TXT;2 Y.TXT;1" "$(echo "$o" | tr '\n' '|')$(vers "$img" '[P]*.*;*')"
o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1 | grep -c FILPURG)
check "P10: one spec still purges (2 versions of X.TXT)" "2" "$o"

# --- P9: a HARDLINKS volume (ODS-5): purging a version that has another link takes the entry and one off the link count; the file stays and nothing is freed, and
# the line says what DELETE says (review 48 N3) ------------------------------------------------------------------------------------------------------------
img="$TMP/pghl.img"
$INIT "$img" 20000 --structure=5 --hardlinks --label=PGHL --cluster=1 >/dev/null 2>&1
$ODS2 "$img" "CREATE/DIRECTORY [P]" >/dev/null 2>&1; $ODS2 "$img" "CREATE/DIRECTORY [Q]" >/dev/null 2>&1
mk "$img" P X.TXT 3
$ODS2 "$img" "LINK [Q]XL.TXT [P]X.TXT;1" >/dev/null 2>&1
fb=$(free_blocks "$img")
o=$($ODS2 "$img" "PURGE/LOG [P]X.TXT" 2>&1)
check "ODS-5 P9: a hard-linked ;1 is said as DELETE says it (not 'deleted (n blocks)'), the total counts only what was freed" "%PURGE-I-FILPURG, [P]X.TXT;2 deleted (2 blocks)|%ODS2-I-DELETED, deleted X.TXT;1|%PURGE-I-TOTAL, 2 files deleted (2 blocks)" "$(echo "$o" | tr '\n' '|' | sed 's/|$//')"
check "ODS-5 P9: the file stays for [Q]XL.TXT and its content reads, free blocks +2 only" "$((fb + 2)) 1" "$(free_blocks "$img") $($ODS2 "$img" 'TYPE [Q]XL.TXT' 2>&1 | grep -c 'line 00 of')"
check "ODS-5 P9: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

if [ "$fails" -eq 0 ]; then echo "cli_purge_check: all checks passed"; else echo "cli_purge_check: $fails FAILED"; fi
exit $fails
