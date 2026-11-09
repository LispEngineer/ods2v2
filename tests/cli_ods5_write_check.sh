#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_write_check.sh - writing to an ODS-5 volume through the ods2
# command line: CREATE/DIRECTORY, COPY and IMPORT with UTF-8 names and ^-escapes,
# SYMLINK, LINK, DELETE, SET VOLUME/LABEL, then EXPORT back to Linux and compare,
# and tests/ods5check.py (an independent structure check: headers, bitmaps,
# directory order and name types, back links, link counts) must find no error.
# The volume is the empty ODS-2 starter volume (a real VMS INITIALIZE) with its
# home block copies patched to level 5. Called by `make test`; run from the project root.

GZ=samples/empty_ods2_volume.dsk.gz
DISK=samples/cli_ods5w_disk.img
ODS2=./ods2
TMP=samples/cli_ods5w_tmp
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

[ -f "$GZ" ] || { echo "FAIL: $GZ missing"; exit 1; }
rm -rf "$DISK" "$TMP"
mkdir -p "$TMP/tree/Sub Dir/Deeper" "$TMP/tree/nippon" "$TMP/back"
gunzip -c "$GZ" | dd of="$DISK" bs=1M conv=sparse status=none

# level 5, HARDLINKS | ACCESSTIMES; both home block checksums recomputed
python3 -I - "$DISK" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
# every copy of the home block of the starter volume: LBN 1-5 and 1032-1034
for lbn in (1, 2, 3, 4, 5, 1032, 1033, 1034):
    f.seek(lbn * 512); b = bytearray(f.read(512))
    assert struct.unpack_from('<I', b, 0)[0] == lbn and struct.unpack_from('<H', b, 58)[0] == csum(b, 29)
    struct.pack_into('<H', b, 12, 0x0501)
    struct.pack_into('<H', b, 42, struct.unpack_from('<H', b, 42)[0] | 0x60)
    struct.pack_into('<H', b, 58, csum(b, 29))
    struct.pack_into('<H', b, 510, csum(b, 255))
    f.seek(lbn * 512); f.write(b)
PY
# the fixture's own flaws (the checker's errors before anything is written) are not ours
python3 -I tests/ods5check.py "$DISK" | grep '^ERROR' > "$TMP/errors-before.txt"

T="$TMP/tree"
printf 'read me\n' > "$T/Read Me.txt"
printf 'in a sub directory\n' > "$T/Sub Dir/A.txt"
printf 'cafe\n' > "$T/Sub Dir/Deeper/caf$(printf '\303\251').TXT"
printf 'nihongo\n' > "$T/nippon/$(printf '\346\227\245\346\234\254\350\252\236').dat"
printf 'upper\n' > "$T/UPPER.TXT"
printf 'lower\n' > "$T/lower.txt"
printf 'slash\n' > "$T/a%2Fb.txt"
printf 'refused\n' > "$T/bad#name.txt"
head -c 70000 /dev/urandom > "$T/random.bin"
: > "$T/empty.dat"
ln -s 'Read Me.txt' "$T/link to read me.lnk"
ln -s 'Deeper/caf'"$(printf '\303\251')"'.TXT' "$T/Sub Dir/rel.lnk"

cat > "$TMP/script.cmd" <<CMD
CREATE/DIRECTORY [Made^_By^.Hand^.Dots]
COPY $T/UPPER.TXT [Made^_By^.Hand^.Dots]New^_Name.TXT
IMPORT $T [Imported]
IMPORT $T/lower.txt [Made^_By^.Hand^.Dots]
SYMLINK [Made^_By^.Hand^.Dots]sym .Target^_Text.txt
LINK [Made^_By^.Hand^.Dots]hard1.txt [Imported]UPPER.TXT
SET FILE/ENTER=[Made^_By^.Hand^.Dots]hard2.txt [Imported]UPPER.TXT
SET VOLUME/LABEL=ODS5W
DIR/PATHS [...]
CMD
$ODS2 $DISK 'CREATE/DIRECTORY [Imported]' >/dev/null 2>&1
out=$($ODS2 $DISK @"$TMP/script.cmd" 2>&1)
echo "$out" > "$TMP/out.txt"

check "IMPORT counts 9 files and 3 directories, skips none, refuses the name with #" \
    "1" "$(grep -c '%ODS2-I-IMPORTED, 9 files.*3 directories, 2 symbolic links.*0 skipped, 1 error' "$TMP/out.txt")"
check "the refused name is reported" 1 "$(grep -c 'VMS refuses the characters' "$TMP/out.txt")"
check "a directory made with ^_ and ^. in its name is listed with its literal name" 1 \
    "$(grep -c '^/Made By.Hand.Dots/New Name.TXT;1' "$TMP/out.txt")"
check "DIR/PATHS shows the UTF-8 names" 1 "$(grep -c '^/Imported/Sub Dir/Deeper/caf.*\.TXT;1' "$TMP/out.txt")"
check "the link count of the linked file is 3" "3" \
    "$($ODS2 $DISK 'DIR/FULL [Imported]UPPER.TXT' 2>&1 | sed -n 's/^  Link count: *//p')"
check "DELETE of the primary link of a linked file is refused" 1 \
    "$($ODS2 $DISK 'DELETE [Imported]UPPER.TXT' 2>&1 | grep -c 'primary link')"
check "DELETE of a link works" 1 \
    "$($ODS2 $DISK 'DELETE [Made^_By^.Hand^.Dots]hard1.txt' 2>&1 | grep -c '%ODS2-I-DELETED')"
check "the link count is 2" "2" \
    "$($ODS2 $DISK 'DIR/FULL [Imported]UPPER.TXT' 2>&1 | sed -n 's/^  Link count: *//p')"
check "SET VOLUME/LABEL on a level 5 volume" "ODS5W" \
    "$($ODS2 $DISK 'SHOW VOLUME' 2>&1 | sed -n 's/^  Volume label: *//p' | head -1)"

# export it all back and compare with the tree (the refused file is not there)
$ODS2 $DISK "EXPORT/LINKS [Imported...] $TMP/back" >/dev/null 2>&1
rm -f "$T/bad#name.txt"
check "EXPORT gives back the imported tree byte for byte, links included" "" \
    "$(cd "$TMP" && diff -r tree back 2>&1 | head -3)"

# delete everything created and check the structure
$ODS2 $DISK 'DELETE [Made^_By^.Hand^.Dots]hard2.txt' >/dev/null 2>&1
if command -v python3 >/dev/null 2>&1; then
    python3 -I tests/ods5check.py "$DISK" | grep '^ERROR' > "$TMP/errors-after.txt"
    check "the independent structure check finds no new error" "" \
        "$(diff "$TMP/errors-before.txt" "$TMP/errors-after.txt" | head -5)"
fi

[ -n "$KEEP" ] || rm -rf "$TMP" "$DISK"
[ "$fails" -eq 0 ] || { echo "$fails ODS-5 write check(s) failed"; exit 1; }
