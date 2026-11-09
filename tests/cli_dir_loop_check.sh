#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_dir_loop_check.sh - DIR [...] on a volume with a directory loop
# (an entry pointing at an ancestor): the walk must end with one
# %ODS2-W-DIRLOOP warning naming both paths, not run to the depth
# limit; an alias that is not an ancestor is still followed.
# Called by `make test`; run from the project root.

DISK=samples/cli_loop_disk.img
ODS2=./ods2
MAKE_LOOP=build/ods2_make_loop
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

dirs() { $ODS2 $DISK "$@" 2>/dev/null | sed -n 's/^Directory \[\(.*\)\]$/\1/p' | tr '\n' ' ' | sed 's/ $//'; }

cp samples/synthetic_disk.img $DISK || exit 1
printf 'hello\n' > samples/cli_loop_tmp.txt
for d in A A.B A.B.C OTHER; do $ODS2 $DISK "CREATE/DIRECTORY [$d]" >/dev/null; done
for f in A]F1.TXT A.B]F2.TXT A.B.C]F3.TXT OTHER]F4.TXT; do
    $ODS2 $DISK "COPY samples/cli_loop_tmp.txt [$f" >/dev/null
done

check "without a loop DIR [A...] lists the three directories" \
    "A A.B A.B.C" "$(dirs 'DIR [A...]')"

# [A.B.C] gets an entry BACK.DIR that is [A], and [A.B] one that is [A.B].
$MAKE_LOOP $DISK A.B.C BACK A || exit 1
$MAKE_LOOP $DISK A.B SELF2 A.B || exit 1
check "DIR [A...] with loops lists each directory once" \
    "A A.B A.B.C" "$(dirs 'DIR [A...]')"
check "the loop to an ancestor is reported with both paths" \
    "%ODS2-W-DIRLOOP, [A.B.C.BACK] is the directory [A] again; not descended into" \
    "$($ODS2 $DISK 'DIR [A...]' 2>&1 >/dev/null | grep 'BACK')"
check "an entry naming its own directory is skipped silently (as 000000.DIR is)" \
    "0" "$($ODS2 $DISK 'DIR [A...]' 2>&1 >/dev/null | grep -c 'SELF2')"
check "a loop warning is not an error: exit status 0" \
    "0" "$($ODS2 $DISK 'DIR [A...]' >/dev/null 2>&1; echo $?)"

# From the root, a loop to the root is named [000000].
$MAKE_LOOP $DISK OTHER TOROOT "" || exit 1
check "a loop to the root names it [000000]" \
    "%ODS2-W-DIRLOOP, [OTHER.TOROOT] is the directory [000000] again; not descended into" \
    "$($ODS2 $DISK 'DIR [000000...]' 2>&1 >/dev/null | grep 'TOROOT')"

# An alias that is not an ancestor is still followed (as on VMS).
$MAKE_LOOP $DISK OTHER ALIASA A || exit 1
check "an alias of a non-ancestor directory is listed under both names" \
    "A A.B A.B.C OTHER OTHER.ALIASA OTHER.ALIASA.B OTHER.ALIASA.B.C" \
    "$(dirs 'DIR [*...]')"

rm -f samples/cli_loop_tmp.txt "$DISK"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails CLI directory loop check(s) failed"
    exit 1
fi
echo "PASS: all CLI directory loop checks"
