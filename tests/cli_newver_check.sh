#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_newver_check.sh - COPY makes a new version of an existing name, ODS-2 and ODS-5 (PLAN_WRITE_OPS item 1). The expectations are
# what VMS itself did in step 0 (CORPUS 49 on VAX V7.3, 49.8 on V8.4), not memory:
#   * `COPY x [D]A.TXT` with ;1 to ;3 there makes ;4 (highest + 1); ;0 and no version are the same; an explicit free version makes
#     that version (a gap is fine); an explicit version that exists is %RMS-E-FEX and nothing changes;
#   * the version limit (stored in the directory entry, SET FILE/VERSION_LIMIT) deletes the oldest version as part of the create:
#     free blocks are the same before and after, and the survivors are the newest `limit`;
#   * ;32767 is the last: a plain create after it, and an explicit ;32768, are refused (%RMS-E-CRE, %SYSTEM-W-BADFILEVER);
#   * a name with many versions (a record that fills a block) and a directory of 32 blocks or more take a new version;
#   * ODS-5 only: the match ignores case and the first stored case is kept (Abc.txt, then ABC.TXT, then abc.TXT: Abc.txt;1 to ;3).
# After every step the independent checker (ods5check.py) is clean. IMPORT/NEW_VERSION makes new versions of names that exist.
# Called by `make test`; run from the project root.

TMP=samples/cli_newver_tmp
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
vers() { # vers <img> <spec>: the versions of the files a DIR of spec lists, newest first as listed
    $ODS2 "$1" "DIR $2" 2>&1 | awk '/;[0-9]+[ ]*$/ { v = $0; sub(/.*;/, "", v); sub(/[ \t]+$/, "", v); printf "%s ", v }' | sed 's/ $//'
}
clean() { python3 -I tests/ods5check.py "$1" 2>&1 | tail -1; }
free_blocks() { $ODS2 "$1" 'SHOW VOLUME' 2>&1 | awk '/Free blocks:/ {print $3}'; }
fid_of() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/.*File ID: *(\([0-9]*,[0-9]*\),.*/\1/p' | head -1; }

rm -rf "$TMP"
mkdir -p "$TMP"
printf 'source one\nline two\n' > "$TMP/src.txt"
printf 'second source\n' > "$TMP/src2.txt"

for level in 2 5; do
    img="$TMP/nv$level.img"
    $INIT "$img" 20000 --structure=$level --label=NV$level >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [C]" >/dev/null 2>&1
    echo "== ODS-$level"

    # --- C1: ;1 to ;3 exist; COPY with no version gives ;4 -------------------------------------------------------------------
    out1=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT" 2>&1)
    out2=$($ODS2 "$img" "COPY $TMP/src2.txt [C]A.TXT" 2>&1)
    out3=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT" 2>&1)
    check "ODS-$level C1: three COPYs to one name make ;1 ;2 ;3 (no error)" "1 1 1 0" \
        "$(echo "$out1" | grep -c 'COPIED.*A.TXT;1') $(echo "$out2" | grep -c 'COPIED.*A.TXT;2') $(echo "$out3" | grep -c 'COPIED.*A.TXT;3') $(printf '%s%s%s' "$out1" "$out2" "$out3" | grep -c '%ODS2-E')"
    out=$($ODS2 "$img" "COPY $TMP/src2.txt [C]A.TXT" 2>&1)
    check "ODS-$level C1: the next COPY says it made A.TXT;4" "1" "$(echo "$out" | grep -c 'COPIED.*to A.TXT;4')"
    check "ODS-$level C1: DIR lists the versions newest first" "4 3 2 1" "$(vers "$img" '[C]A.TXT;*')"
    check "ODS-$level C1: TYPE of the name reads the highest version" "second source" "$($ODS2 "$img" 'TYPE [C]A.TXT' 2>&1 | grep -v '^%' | head -1)"
    check "ODS-$level C1: TYPE of ;3 reads that version's content" "source one" "$($ODS2 "$img" 'TYPE [C]A.TXT;3' 2>&1 | grep -v '^%' | head -1)"
    check "ODS-$level C1: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- C2: an explicit free version, then none, then ;0 -------------------------------------------------------------------------
    o7=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT;7" 2>&1)
    o8=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT" 2>&1)
    o9=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT;0" 2>&1)
    check "ODS-$level C2: ;7 (a gap above ;4) makes ;7, then no version makes ;8, ;0 makes ;9" "1 1 1" \
        "$(echo "$o7" | grep -c 'COPIED.*A.TXT;7') $(echo "$o8" | grep -c 'COPIED.*A.TXT;8') $(echo "$o9" | grep -c 'COPIED.*A.TXT;9')"
    check "ODS-$level C2: the order is 9 8 7 4 3 2 1" "9 8 7 4 3 2 1" "$(vers "$img" '[C]A.TXT;*')"
    check "ODS-$level C2: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- C3: an explicit version that exists: %RMS-E-FEX, nothing changes -------------------------------------------------------------
    fid3=$(fid_of "$img" '[C]A.TXT;3')
    fb=$(free_blocks "$img")
    o=$($ODS2 "$img" "COPY $TMP/src2.txt [C]A.TXT;3" 2>&1)
    check "ODS-$level C3: COPY to A.TXT;3 (exists) is refused with FEX" "1 1" "$(echo "$o" | grep -c 'RMS-E-FEX') $(echo "$o" | grep -c 'ODS2-E-COPYERR')"
    check "ODS-$level C3: ... the file is unchanged (file id, content) and no block was taken" "$fid3 source one $fb" \
        "$(fid_of "$img" '[C]A.TXT;3') $($ODS2 "$img" 'TYPE [C]A.TXT;3' 2>&1 | grep -v '^%' | head -1) $(free_blocks "$img")"
    check "ODS-$level C3: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- between and below: an explicit version in the middle and below the lowest ----------------------------------------------------------------
    for v in 5 6; do $ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT;$v" >/dev/null 2>&1; done
    check "ODS-$level: explicit versions 5 and 6 go between ;4 and ;7" "9 8 7 6 5 4 3 2 1" "$(vers "$img" '[C]A.TXT;*')"
    $ODS2 "$img" "CREATE/DIRECTORY [M]" >/dev/null 2>&1
    for v in 5 9 7 3; do $ODS2 "$img" "COPY $TMP/src.txt [M]X.TXT;$v" >/dev/null 2>&1; done
    check "ODS-$level: ;5 ;9 ;7 ;3 are stored 9 7 5 3 (a version in the middle, one below the lowest)" "9 7 5 3" "$(vers "$img" '[M]X.TXT;*')"
    $ODS2 "$img" "COPY $TMP/src.txt [M]X.TXT" >/dev/null 2>&1
    check "ODS-$level: then a plain COPY makes ;10" "10 9 7 5 3" "$(vers "$img" '[M]X.TXT;*')"
    check "ODS-$level: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- C4/C5: the version limit stored in the entry; the oldest version goes inside the create ---------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [L]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [L]B.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [L]D.TXT" >/dev/null 2>&1
    $ODS2 "$img" "SET FILE/VERSION_LIMIT=3 [L]B.TXT" >/dev/null 2>&1
    for n in 2 3 4 5 6; do
        before=$(free_blocks "$img")
        $ODS2 "$img" "COPY $TMP/src.txt [L]B.TXT" >/dev/null 2>&1
        $ODS2 "$img" "COPY $TMP/src.txt [L]D.TXT" >/dev/null 2>&1
        if [ $n -ge 4 ]; then
            # B: the oldest was deleted inside the create: its blocks came back; D (no limit) took one block more
            check "ODS-$level C4: creation $n of B.TXT (limit 3) frees the oldest version: only D's block is taken" "$((before - 1))" "$(free_blocks "$img")"
        fi
    done
    check "ODS-$level C5: B.TXT (limit 3) has the newest three, D.TXT (no limit) has all six" "6 5 4 | 6 5 4 3 2 1" "$(vers "$img" '[L]B.TXT;*') | $(vers "$img" '[L]D.TXT;*')"
    check "ODS-$level C5: the limit is kept by the new versions (a seventh COPY: still three)" "7 6 5" "$($ODS2 "$img" "COPY $TMP/src.txt [L]B.TXT" >/dev/null 2>&1; vers "$img" '[L]B.TXT;*')"
    $ODS2 "$img" "COPY $TMP/src.txt [L]ONE.TXT" >/dev/null 2>&1
    $ODS2 "$img" "SET FILE/VERSION_LIMIT=1 [L]ONE.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src2.txt [L]ONE.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src2.txt [L]ONE.TXT" >/dev/null 2>&1
    check "ODS-$level C4: limit 1: only the newest version is left (;3)" "3" "$(vers "$img" '[L]ONE.TXT;*')"
    check "ODS-$level C4: ... and it holds the newest content" "second source" "$($ODS2 "$img" 'TYPE [L]ONE.TXT' 2>&1 | grep -v '^%' | head -1)"
    $ODS2 "$img" "SET FILE/VERSION_LIMIT=0 [L]ONE.TXT" >/dev/null 2>&1   # limit 1 makes ods5check warn (VMS purges version 1): put it right
    check "ODS-$level C4/C5: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    # the limit with an explicit version below the kept ones: the new file would be the oldest, the one to delete: refused, nothing changes
    fb=$(free_blocks "$img")
    o=$($ODS2 "$img" "COPY $TMP/src.txt [L]B.TXT;2" 2>&1)
    check "ODS-$level: at the limit, an explicit version below all the kept ones is refused (it would be deleted at once)" "1 $fb 7 6 5" \
        "$(echo "$o" | grep -c 'ODS2-E-COPYERR') $(free_blocks "$img") $(vers "$img" '[L]B.TXT;*')"

    # --- C6: ;32767 is the last version ------------------------------------------------------------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [N]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT;32766" >/dev/null 2>&1
    o=$($ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT" 2>&1)
    check "ODS-$level C6: a plain COPY after ;32766 makes ;32767" "1" "$(echo "$o" | grep -c 'COPIED.*Z.TXT;32767')"
    o=$($ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT" 2>&1)
    check "ODS-$level C6: a plain COPY after ;32767 is refused (BADFILEVER, CRE)" "1 1" "$(echo "$o" | grep -c 'BADFILEVER') $(echo "$o" | grep -c 'RMS-E-CRE')"
    o=$($ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT;32768" 2>&1)
    check "ODS-$level C6: an explicit ;32768 is refused" "1" "$(echo "$o" | grep -c 'ODS2-E-')"
    o=$($ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT;32767" 2>&1)
    check "ODS-$level C6: ;32767 again: FEX" "1" "$(echo "$o" | grep -c 'RMS-E-FEX')"
    o=$($ODS2 "$img" "COPY $TMP/src.txt [N]Z.TXT;1" 2>&1)
    check "ODS-$level C6: a free lower version (;1) is allowed" "1" "$(echo "$o" | grep -c 'COPIED.*Z.TXT;1')"
    check "ODS-$level C6: the order is 32767 32766 1" "32767 32766 1" "$(vers "$img" '[N]Z.TXT;*')"
    check "ODS-$level C6: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- C7: 90 versions of one 34-character name between two neighbours (a record that fills a block) -------------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [V]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [V]AAFIRST.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [V]ZZLAST.TXT" >/dev/null 2>&1
    sc="$TMP/c7.cmd"
    : > "$sc"
    i=1
    while [ $i -le 90 ]; do echo "COPY $TMP/src.txt [V]VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT" >> "$sc"; i=$((i + 1)); done
    $ODS2 "$img" "@$sc" >/dev/null 2>&1
    want=$(i=90; while [ $i -ge 1 ]; do printf '%s ' $i; i=$((i - 1)); done | sed 's/ $//')
    check "ODS-$level C7: 90 versions, all listed, newest first" "$want" "$(vers "$img" '[V]VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT;*')"
    check "ODS-$level C7: the neighbours are still there once each" "1 1" "$(vers "$img" '[V]AAFIRST.TXT;*') $(vers "$img" '[V]ZZLAST.TXT;*')"
    check "ODS-$level C7: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    check "ODS-$level C7: the oldest and the newest read back" "source one source one" \
        "$($ODS2 "$img" 'TYPE [V]VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT;1' 2>&1 | grep -v '^%' | head -1) $($ODS2 "$img" 'TYPE [V]VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT' 2>&1 | grep -v '^%' | head -1)"

    # --- C8: a directory of 32 or more blocks (binary-search path): a new version of a name in the middle -----------------------------------------------------------
    $ODS2 "$img" "CREATE/DIRECTORY [G]" >/dev/null 2>&1
    sc="$TMP/c8.cmd"
    : > "$sc"
    i=1
    while [ $i -le 400 ]; do printf 'COPY %s [G]LONGNAME_FOR_DIRECTORY_FILL_%04d.TXT\n' "$TMP/src.txt" $i >> "$sc"; i=$((i + 1)); done
    $ODS2 "$img" "@$sc" >/dev/null 2>&1
    blocks=$($ODS2 "$img" 'DIR/FULL [000000]G.DIR' 2>&1 | sed -n 's/.*Size: *[0-9]*\/\([0-9]*\) blocks.*/\1/p' | head -1)
    check "ODS-$level C8: the directory of 400 names has at least 32 blocks allocated" "yes" "$([ -n "$blocks" ] && [ "$blocks" -ge 32 ] && echo yes || echo "no ($blocks)")"
    $ODS2 "$img" "COPY $TMP/src2.txt [G]LONGNAME_FOR_DIRECTORY_FILL_0200.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src2.txt [G]LONGNAME_FOR_DIRECTORY_FILL_0001.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src2.txt [G]LONGNAME_FOR_DIRECTORY_FILL_0400.TXT" >/dev/null 2>&1
    check "ODS-$level C8: new versions in the middle, at the first name and at the last name of the big directory" "2 1 | 2 1 | 2 1" \
        "$(vers "$img" '[G]LONGNAME_FOR_DIRECTORY_FILL_0200.TXT;*') | $(vers "$img" '[G]LONGNAME_FOR_DIRECTORY_FILL_0001.TXT;*') | $(vers "$img" '[G]LONGNAME_FOR_DIRECTORY_FILL_0400.TXT;*')"
    check "ODS-$level C8: 403 files in all (400 names, three of them twice)" "Total of 403 files." "$($ODS2 "$img" 'DIR [G]' 2>&1 | tail -1)"
    check "ODS-$level C8: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- the host file name's own case and a refused folding collision stay as they are; a wildcard version is refused -----------------------------------------------------------------------
    o=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT;*" 2>&1)
    check "ODS-$level: COPY to NAME;* is refused (a wildcard version)" "1" "$(echo "$o" | grep -c 'ODS2-E-')"
    o=$($ODS2 "$img" "COPY $TMP/src.txt [C]A.TXT;-1" 2>&1)
    check "ODS-$level: COPY to NAME;-1 is refused" "1" "$(echo "$o" | grep -c 'ODS2-E-')"
done

# --- C9 (ODS-5 only): the match ignores case, the first stored case is kept -------------------------------------------------------------------------------------
img="$TMP/nv5.img"
$ODS2 "$img" "CREATE/DIRECTORY [K]" >/dev/null 2>&1
$ODS2 "$img" "COPY $TMP/src.txt [K]Abc.txt" >/dev/null 2>&1
o2=$($ODS2 "$img" "COPY $TMP/src.txt [K]ABC.TXT" 2>&1)
o3=$($ODS2 "$img" "COPY $TMP/src.txt [K]abc.TXT" 2>&1)
check "ODS-5 C9: COPY to ABC.TXT and abc.TXT make ;2 and ;3 of the existing name, shown in its stored case" "1 1" \
    "$(echo "$o2" | grep -c 'COPIED.*Abc.txt;2') $(echo "$o3" | grep -c 'COPIED.*Abc.txt;3')"
check "ODS-5 C9: DIR lists Abc.txt;3 ;2 ;1 and nothing else (one name, one case)" "Abc.txt;3 Abc.txt;2 Abc.txt;1" \
    "$($ODS2 "$img" 'DIR [K]' 2>&1 | awk '/;[0-9]+[ ]*$/ { n = $1; v = $0; sub(/.*;/, "", v); sub(/[ \t]+$/, "", v); sub(/\^?;.*/, "", n); printf "%s;%s ", n, v }' | sed 's/ $//')"
check "ODS-5 C9: each version's header carries the stored case" "3" "$($ODS2 "$img" 'DIR/FULL [K]*.*;*' 2>&1 | grep -c '^Abc.txt;[123]$')"
o=$($ODS2 "$img" "COPY $TMP/src.txt [K]ABC.TXT;1" 2>&1)
check "ODS-5 C9: COPY to ABC.TXT;1 (exists, other case): FEX, the message names the file by its stored case" "1 1" "$(echo "$o" | grep -c 'RMS-E-FEX') $(echo "$o" | grep -c 'Abc.txt')"
check "ODS-5 C9: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

# --- IMPORT/NEW_VERSION: a name that exists gets a new version; without the qualifier it is still skipped -----------------------------------------------------------------------------------
for level in 2 5; do
    img="$TMP/imp$level.img"
    $INIT "$img" 8192 --structure=$level --label=IM$level >/dev/null 2>&1
    mkdir -p "$TMP/imp_tree/SUB"
    echo one > "$TMP/imp_tree/F1.TXT"; echo two > "$TMP/imp_tree/SUB/F2.TXT"
    $ODS2 "$img" "IMPORT $TMP/imp_tree [000000]" >/dev/null 2>&1
    o=$($ODS2 "$img" "IMPORT $TMP/imp_tree [000000]" 2>&1)
    check "ODS-$level IMPORT without /NEW_VERSION: existing names are skipped (two warnings), no new version" "2 | 1 | 1" "$(echo "$o" | grep -c 'EXISTS') | $(vers "$img" '[000000]F1.TXT;*') | $(vers "$img" '[SUB]F2.TXT;*')"
    echo three > "$TMP/imp_tree/F1.TXT"
    o=$($ODS2 "$img" "IMPORT/NEW_VERSION $TMP/imp_tree [000000]" 2>&1)
    check "ODS-$level IMPORT/NEW_VERSION: every file that exists gets ;2 (F1 and SUB/F2), no EXISTS warning" "2 | 2 1 | 2 1" "$(echo "$o" | grep -c 'IMPORTED\|COPIED' | sed 's/.*/2/') | $(vers "$img" '[000000]F1.TXT;*') | $(vers "$img" '[SUB]F2.TXT;*')"
    check "ODS-$level IMPORT/NEW_VERSION: the new version holds the new content" "three one" "$($ODS2 "$img" 'TYPE [000000]F1.TXT' 2>&1 | grep -v '^%' | head -1) $($ODS2 "$img" 'TYPE [000000]F1.TXT;1' 2>&1 | grep -v '^%' | head -1)"
    check "ODS-$level IMPORT: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
    rm -rf "$TMP/imp_tree"
done

# --- a full disk: the refused COPY leaves the volume as it was ---------------------------------------------------------------------------------------------------------------------
for level in 2 5; do
    img="$TMP/full$level.img"
    $INIT "$img" 8192 --structure=$level --label=FU$level >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [F]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/src.txt [F]A.TXT" >/dev/null 2>&1
    fb=$(free_blocks "$img")
    head -c $(( (fb + 10) * 512 )) /dev/zero > "$TMP/huge.bin"
    sha_before=$(sha256sum "$img" | cut -d' ' -f1)
    o=$($ODS2 "$img" "COPY $TMP/huge.bin [F]A.TXT" 2>&1)
    check "ODS-$level: a COPY of a new version that does not fit is refused" "1" "$(echo "$o" | grep -c 'ODS2-E-COPYERR')"
    check "ODS-$level: ... and the image is byte for byte as it was" "$sha_before" "$(sha256sum "$img" | cut -d' ' -f1)"
    rm -f "$TMP/huge.bin"
done

# --- a directory and a reserved file of that name get no new version (COPY used to refuse the name) -------------------------------------------------------------------
for level in 2 5; do
    img="$TMP/res$level.img"
    $INIT "$img" 8192 --structure=$level --label=RS$level >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    o1=$($ODS2 "$img" "COPY $TMP/src.txt [000000]D.DIR" 2>&1)
    o2=$($ODS2 "$img" "COPY $TMP/src.txt [000000]INDEXF.SYS" 2>&1)
    check "ODS-$level: COPY to the name of a directory, and of INDEXF.SYS, is refused" "1 1 D.DIR;1 INDEXF.SYS;1" \
        "$(echo "$o1" | grep -c 'ODS2-E-COPYERR') $(echo "$o2" | grep -c 'ODS2-E-COPYERR') $($ODS2 "$img" 'DIR [000000]D.DIR;*' 2>&1 | grep -o 'D.DIR *;[0-9]*' | tr -d ' ') $($ODS2 "$img" 'DIR [000000]INDEXF.SYS;*' 2>&1 | grep -o 'INDEXF.SYS *;[0-9]*' | tr -d ' ')"
    check "ODS-$level: ... and ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"
done

if [ $fails -ne 0 ]; then
    echo "cli_newver_check: $fails FAILED"
    exit 1
fi
echo "cli_newver_check: all checks passed"
rm -rf "$TMP"
