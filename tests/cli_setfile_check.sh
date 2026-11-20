#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_setfile_check.sh - SET FILE/PROTECTION and SET FILE/OWNER_UIC (PLAN_WRITE_OPS item 4), ODS-2 and ODS-5. The expectations are what VMS itself did
# (CORPUS 49.4 on VAX V7.3, 49.8 on V8.4, 49.7 for the process that is not the volume owner):
#   * /PROTECTION: a partial specification changes only the fields named; a field with no letters and no colon clears it ("O,G,W"); the long names SYSTEM:
#     and OWNER: are accepted; a repeated field takes the last; (O:X) is %SET-F-SYNTAX, error parsing 'X' and changes nothing; the value the file
#     already has succeeds and still counts as a revision;
#   * every success adds 1 to the revision number; the revision date moves in a level 2 header, the attribute change date (shown as "Revised:") in a level 5
#     header, whose data modification date and access date stay; a failed command changes nothing;
#   * /LOG: %SET-I-PROTECTED, <spec> file protection changed to S:RWED,O:R,G:R,W:R (the whole new protection); a wildcard sets each version; owner:
#     %SET-I-MODIFIED, <spec> modified. Without /LOG nothing is printed;
#   * a directory takes both; the reserved files (INDEXF.SYS, BITMAP.SYS, the master directory) take a protection change (revision +1); an owner change on
#     one is refused;
#   * /OWNER_UIC: [group,member] in octal, also the 11-digit form [00000000200,00000000003]; [128,3] is a syntax error (128 is not octal); [777,3] works;
#     [SYSTEM] is [1,4]; a name that needs the rights database ([USERS,USER1]) is refused; each success adds 1 to the revision, even for the same owner;
#   * ods2v2 acts as a privileged process: no NOPRIV refusals (VMS refuses an unprivileged process that is not the volume owner: CORPUS 49.7);
#   * only one of /PROTECTION and /OWNER_UIC per command (both at once was not measured).
# After every step the independent checker (ods5check.py) is clean, which includes the header checksum. Called by `make test`; run from the project root.

TMP=samples/cli_setfile_tmp
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
clean() { python3 -I tests/ods5check.py "$1" 2>&1 | tail -1; }
prot() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^  Protection: *//p' | head -1; }
owner() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^  Owner: *//p' | head -1; }
rev() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^  Revised:.*(\([0-9]*\))$/\1/p' | head -1; }
revdate() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^  Revised: *\(.*\) ([0-9]*)$/\1/p' | head -1; }
datamod() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^  Data modified: *//p' | head -1; }
setf() { $ODS2 "$1" "$2" 2>&1; }

rm -rf "$TMP"
mkdir -p "$TMP"
printf 'first file\n' > "$TMP/f.txt"

for level in 2 5; do
    img="$TMP/sf$level.img"
    $INIT "$img" 20000 --structure=$level --label=SF$level --cluster=1 >/dev/null 2>&1
    echo "== ODS-$level"
    cat > "$TMP/mk.cmd" <<CMD
CREATE/DIRECTORY [S]
COPY $TMP/f.txt [S]F1.TXT
COPY $TMP/f.txt [S]F2.TXT
COPY $TMP/f.txt [S]F2.TXT
COPY $TMP/f.txt [S]G.DAT
CMD
    $ODS2 "$img" "@$TMP/mk.cmd" >/dev/null 2>&1
    f1='[S]F1.TXT'
    check "ODS-$level start: a new file is (S:RWED, O:RWED, G:RE, W:), owner [1,4], revision 1" "(S:RWED, O:RWED, G:RE, W:) [1,4] 1" "$(prot "$img" $f1) $(owner "$img" $f1) $(rev "$img" $f1)"

    # --- S1: partial specifications change only the fields named, revision +1 each ---------------------------------------------------------------------
    setf "$img" "SET FILE/PROTECTION=(S:R) $f1" >/dev/null
    check "ODS-$level S1: (S:R) changes the system field only; revision 2" "(S:R, O:RWED, G:RE, W:) 2" "$(prot "$img" $f1) $(rev "$img" $f1)"
    setf "$img" "SET FILE/PROTECTION=(O:R) $f1" >/dev/null
    setf "$img" "SET FILE/PROTECTION=(G:RWED) $f1" >/dev/null
    setf "$img" "SET FILE/PROTECTION=(W:RE) $f1" >/dev/null
    check "ODS-$level S1: (O:R), (G:RWED), (W:RE) one field each: (S:R, O:R, G:RWED, W:RE); revision 5" "(S:R, O:R, G:RWED, W:RE) 5" "$(prot "$img" $f1) $(rev "$img" $f1)"
    setf "$img" "SET FILE/PROTECTION=(O,G,W) $f1" >/dev/null
    check "ODS-$level S1: (O,G,W) clears the three fields (S:R, O:, G:, W:); revision 6" "(S:R, O:, G:, W:) 6" "$(prot "$img" $f1) $(rev "$img" $f1)"
    setf "$img" "SET FILE/PROTECTION=(SYSTEM:RWED,OWNER:RWED,GROUP:RE,WORLD) $f1" >/dev/null
    check "ODS-$level S1: the long names SYSTEM: OWNER: GROUP: WORLD: are accepted; revision 7" "(S:RWED, O:RWED, G:RE, W:) 7" "$(prot "$img" $f1) $(rev "$img" $f1)"
    setf "$img" "SET FILE/PROTECTION=(O:RWED,O:R) $f1" >/dev/null
    check "ODS-$level S1: a repeated field takes the last (O:RWED,O:R gives O:R)" "(S:RWED, O:R, G:RE, W:) 8" "$(prot "$img" $f1) $(rev "$img" $f1)"
    o=$(setf "$img" "SET FILE/PROTECTION=(O:X) $f1")
    check "ODS-$level S1: (O:X) is %SET-F-SYNTAX, error parsing 'X'; nothing changes, revision stays 8" "%SET-F-SYNTAX, error parsing 'X'|(S:RWED, O:R, G:RE, W:) 8" "$o|$(prot "$img" $f1) $(rev "$img" $f1)"
    setf "$img" "SET FILE/PROTECTION=(O:R) $f1" >/dev/null
    check "ODS-$level S1: setting the value the file already has succeeds and still counts as a revision (9)" "(S:RWED, O:R, G:RE, W:) 9" "$(prot "$img" $f1) $(rev "$img" $f1)"
    check "ODS-$level S1: without /LOG nothing was printed" "" "$(setf "$img" "SET FILE/PROTECTION=(O:R) $f1")"
    check "ODS-$level S1: ods5check is clean (the header checksum was made again)" "0 error(s), 0 warning(s)" "$(clean "$img")"
    setf "$img" "SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) $f1" >/dev/null

    # --- S2: the date that moves, and the ones that stay -----------------------------------------------------------------------------------------------
    d0=$(revdate "$img" $f1); m0=$(datamod "$img" $f1)
    sleep 1
    setf "$img" "SET FILE/PROTECTION=(W:R) $f1" >/dev/null
    d1=$(revdate "$img" $f1); m1=$(datamod "$img" $f1)
    if [ $level = 2 ]; then
        check "ODS-$level S2: the revision date (what Revised: shows) is the time of the command" "moved" "$([ "$d0" != "$d1" ] && echo moved || echo "same: $d0")"
    else
        check "ODS-$level S2: the attribute change date (Revised:) moves, the data modification date stays" "moved stays" "$([ "$d0" != "$d1" ] && echo moved || echo "same: $d0") $([ "$m0" = "$m1" ] && echo stays || echo "changed: $m0 $m1")"
    fi

    # --- S3: /LOG and a wildcard over versions ---------------------------------------------------------------------------------------------------------
    o=$(setf "$img" "SET FILE/LOG/PROTECTION=(G:R,W:R) [S]F*.TXT;*")
    want="%SET-I-PROTECTED, [S]F1.TXT;1 file protection changed to S:RWED,O:RWED,G:R,W:R
%SET-I-PROTECTED, [S]F2.TXT;2 file protection changed to S:RWED,O:RWED,G:R,W:R
%SET-I-PROTECTED, [S]F2.TXT;1 file protection changed to S:RWED,O:RWED,G:R,W:R"
    check "ODS-$level S3: /LOG with a wildcard and ;*: the whole new protection for each version, in directory order" "$want" "$o"
    check "ODS-$level S3: without ;* only the highest version of each name is set (F2.TXT;2)" "(S:RWED, O:RWED, G:R, W:R) (S:RWED, O:RWED, G:R, W:R) (S:RWED, O:RWED, G:RE, W:)" "$(prot "$img" '[S]F2.TXT;1') $(prot "$img" '[S]F2.TXT;2') $(prot "$img" '[S]G.DAT')"
    setf "$img" "SET FILE/PROTECTION=(W) [S]F2.TXT" >/dev/null
    check "ODS-$level S3: SET FILE/PROTECTION=(W) [S]F2.TXT sets the highest version only" "(S:RWED, O:RWED, G:R, W:R) (S:RWED, O:RWED, G:R, W:)" "$(prot "$img" '[S]F2.TXT;1') $(prot "$img" '[S]F2.TXT;2')"
    o=$(setf "$img" "SET FILE/PROTECTION=(W:R) [S]NOSUCH.TXT")
    check "ODS-$level S3: a name that does not exist: %SET-W-SEARCHFAIL, FNF" "%SET-W-SEARCHFAIL, error searching for [S]NOSUCH.TXT;
-RMS-E-FNF, file not found" "$o"
    setf "$img" "SET FILE/PROTECTION=(G:RE, W:RE) [S]G.DAT" >/dev/null
    check "ODS-$level S3: a parenthesis list written with spaces is read as one list" "(S:RWED, O:RWED, G:RE, W:RE)" "$(prot "$img" '[S]G.DAT')"
    o=$(setf "$img" "SET FILE/PROTECTION=(G:) [S]G.DAT")
    check "ODS-$level S3: a field with a colon and no letters is %DCL-W-VALREQ (as on VMS), nothing changes" "%DCL-W-VALREQ, missing qualifier or keyword value|(S:RWED, O:RWED, G:RE, W:RE)" "$o|$(prot "$img" '[S]G.DAT')"
    setf "$img" "SET FILE/PROTECTION=(W) [S]G.DAT" >/dev/null

    # --- S4: a directory, and the reserved files -------------------------------------------------------------------------------------------------------
    r0=$(rev "$img" '[000000]S.DIR;1')
    setf "$img" "SET FILE/PROTECTION=(W:RE) [000000]S.DIR;1" >/dev/null
    check "ODS-$level S4: a directory takes a protection change (W:RE), revision +1" "W:RE $((r0 + 1))" "$(prot "$img" '[000000]S.DIR;1' | sed 's/.*W:/W:/;s/)//') $(rev "$img" '[000000]S.DIR;1')"
    for rf in INDEXF.SYS BITMAP.SYS 000000.DIR; do
        r0=$(rev "$img" "[000000]$rf;1")
        o=$(setf "$img" "SET FILE/LOG/PROTECTION=(W:R) [000000]$rf;1")
        check "ODS-$level S4: a protection change on $rf is accepted (measured on VMS), revision +1" "1 W:R $((r0 + 1))" "$(echo "$o" | grep -c 'file protection changed to') $(prot "$img" "[000000]$rf;1" | sed 's/.*W:/W:/;s/)//') $(rev "$img" "[000000]$rf;1")"
    done
    o=$(setf "$img" "SET FILE/OWNER_UIC=[200,3] [000000]BITMAP.SYS;1")
    check "ODS-$level S4: an owner change on a reserved file is refused, nothing changes" "1 [1,4]" "$(echo "$o" | grep -c 'reserved file') $(owner "$img" '[000000]BITMAP.SYS;1')"
    check "ODS-$level S4: ods5check is clean after the reserved files and the directory" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- S5: /OWNER_UIC -----------------------------------------------------------------------------------------------------------------------------------
    f2='[S]G.DAT'
    r0=$(rev "$img" $f2)
    o=$(setf "$img" "SET FILE/LOG/OWNER_UIC=[200,3] $f2")
    check "ODS-$level S5: /OWNER_UIC=[200,3] (octal) sets [200,3]; %SET-I-MODIFIED with /LOG; revision +1" "%SET-I-MODIFIED, [S]G.DAT;1 modified|[200,3] $((r0 + 1))" "$o|$(owner "$img" $f2) $(rev "$img" $f2)"
    setf "$img" "SET FILE/OWNER_UIC=[1,4] $f2" >/dev/null
    setf "$img" "SET FILE/OWNER_UIC=[00000000200,00000000003] $f2" >/dev/null
    check "ODS-$level S5: the 11-digit numeric form gives the same owner [200,3]" "[200,3]" "$(owner "$img" $f2)"
    r1=$(rev "$img" $f2)
    setf "$img" "SET FILE/OWNER_UIC=[200,3] $f2" >/dev/null
    check "ODS-$level S5: the same owner again still counts as a revision" "$((r1 + 1))" "$(rev "$img" $f2)"
    o=$(setf "$img" "SET FILE/OWNER_UIC=[128,3] $f2")
    check "ODS-$level S5: [128,3] is a syntax error (128 is not octal); the owner stays" "%SET-E-SYNTAX, error parsing '[128,3]'|[200,3]" "$o|$(owner "$img" $f2)"
    setf "$img" "SET FILE/OWNER_UIC=[777,3] $f2" >/dev/null
    check "ODS-$level S5: [777,3] (an owner with no identifier) is accepted" "[777,3]" "$(owner "$img" $f2)"
    setf "$img" "SET FILE/OWNER_UIC=[SYSTEM] $f2" >/dev/null
    check "ODS-$level S5: [SYSTEM] is [1,4]" "[1,4]" "$(owner "$img" $f2)"
    o=$(setf "$img" "SET FILE/OWNER_UIC=[USERS,USER1] $f2" | head -1)
    check "ODS-$level S5: a name that needs the rights database is refused, the owner stays" "%SET-E-SYNTAX, error parsing 'USERS'|[1,4]" "$o|$(owner "$img" $f2)"
    check "ODS-$level S5: ods5check is clean" "0 error(s), 0 warning(s)" "$(clean "$img")"

    # --- S6: privileges and the two qualifiers --------------------------------------------------------------------------------------------------------
    setf "$img" "SET FILE/PROTECTION=(S:RWED,O,G,W) $f2" >/dev/null
    setf "$img" "SET FILE/OWNER_UIC=[200,5] $f2" >/dev/null
    check "ODS-$level S6: no NOPRIV: a file nobody but the system may touch still takes an owner and a protection change (ods2v2 is privileged)" "[200,5] (S:RWED, O:, G:, W:)" "$(owner "$img" $f2) $(prot "$img" $f2)"
    o=$(setf "$img" "SET FILE/PROTECTION=(W:R)/OWNER_UIC=[1,4] $f2")
    check "ODS-$level S6: both qualifiers in one command are refused, nothing changes" "1 [200,5]" "$(echo "$o" | grep -c 'usage') $(owner "$img" $f2)"
    before=$(prot "$img" '[S]G.DAT')
    o=$(setf "$img" "SET FILE/PROTECTION=(W:R) [S]F1.TXT [S]G.DAT")
    check "ODS-$level S6: a second spec is refused (%SET-F-TOOMANY) and nothing is changed (review 49 N2: it was ignored in silence)" "%SET-F-TOOMANY, SET FILE takes one file specification, and \"[S]G.DAT\" follows \"[S]F1.TXT\": nothing is changed (run SET FILE once for each)|$before" "$o|$(prot "$img" '[S]G.DAT')"
    o=$(setf "$img" "SET FILE/PROTECTION=(W:R) [S]G.DAT /LOG")
    check "ODS-$level S6: a qualifier after the spec is refused with a hint where qualifiers go" "1" "$(echo "$o" | grep -c 'qualifiers go before the spec')"
    check "ODS-$level S6: ods5check is clean at the end" "0 error(s), 0 warning(s)" "$(clean "$img")"
done

if [ "$fails" -eq 0 ]; then echo "cli_setfile_check: all checks passed"; else echo "cli_setfile_check: $fails FAILED"; fi
exit $fails
