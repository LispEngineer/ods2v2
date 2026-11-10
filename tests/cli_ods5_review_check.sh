#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_review_check.sh - the fixes of the ods5-read review (ods5-rev1):
#  1 EXPORT never writes through a symbolic link below the destination (a directory and a symbolic
#    link of one Linux name; a link already lying in the destination), IMPORT does not follow
#    symbolic links of the tree it reads;
#  3 a malformed directory record is skipped, the others in its block are listed, and a warning says so;
#  4 control characters in names are shown and exported as %XX, and IMPORT reads them back;
#  5 the CLI's name comparison folds ISO Latin-1 and UCS-2 letters like the directory does.
# Called by `make test`; run from the project root.

TMP=samples/cli_review_tmp
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

rm -rf "$TMP"
mkdir -p "$TMP/out" "$TMP/outside"
IMG="$TMP/v.img"
$INIT "$IMG" 64M --structure=5 >/dev/null 2>&1

# --- 1: a directory A.B and a symbolic link A.B (the same Linux name) ---
printf 'inside\n' > "$TMP/f.txt"
cat > "$TMP/s1.cmd" <<CMD
CREATE/DIRECTORY [A^.B]
COPY $TMP/f.txt [A^.B]f.txt
SYMLINK [000000]A.B $PWD/$TMP/outside
CMD
$ODS2 "$IMG" @"$TMP/s1.cmd" >/dev/null 2>&1
err=$($ODS2 "$IMG" "EXPORT [000000...] $TMP/out" 2>&1)
check "EXPORT writes nothing outside the destination through a symbolic link" "" "$(ls "$TMP/outside")"
check "...and says why" 1 "$([ "$(echo "$err" | grep -c '%ODS2-E-MKDIR')" -ge 1 ] && echo 1)"
rm -rf "$TMP/out"; mkdir -p "$TMP/out"
ln -s "$PWD/$TMP/outside" "$TMP/out/A.B"
$ODS2 "$IMG" "EXPORT/OVERWRITE [A^.B] $TMP/out/sub" >/dev/null 2>&1
$ODS2 "$IMG" "EXPORT [000000...]*.DIR $TMP/out" >/dev/null 2>&1
check "a symbolic link already in the destination is not written through either" "" "$(ls "$TMP/outside")"
rm -rf "$TMP/out"; mkdir -p "$TMP/out"
printf 'precious\n' > "$TMP/outside/f.txt"
ln -s "$PWD/$TMP/outside/f.txt" "$TMP/out/f.txt"
$ODS2 "$IMG" "EXPORT [A^.B]f.txt $TMP/out" >/dev/null 2>&1
check "a symbolic link in place of the file is not followed (O_EXCL, O_NOFOLLOW)" "precious" "$(cat "$TMP/outside/f.txt")"
$ODS2 "$IMG" "EXPORT/OVERWRITE [A^.B]f.txt $TMP/out" >/dev/null 2>&1
check "EXPORT/OVERWRITE replaces the link, not the file it points at" "precious inside" \
    "$(tr -d '\n' < "$TMP/outside/f.txt") $(tr -d '\n' < "$TMP/out/f.txt")"

# IMPORT does not follow the symbolic links of the tree
rm -rf "$TMP/tree" "$TMP/out"; mkdir -p "$TMP/tree" "$TMP/out"
printf 'secret\n' > "$TMP/outside/secret.txt"
ln -s "$PWD/$TMP/outside" "$TMP/tree/dirlink"
ln -s "$PWD/$TMP/outside/secret.txt" "$TMP/tree/filelink"
$ODS2 "$IMG" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
check "IMPORT makes symbolic links of the tree's links and copies nothing from outside" "2 0" \
    "$($ODS2 "$IMG" 'DIR/PATHS [000000]*link*.*' 2>&1 | grep -c '^/') $($ODS2 "$IMG" 'DIR/PATHS [000000]secret*.*' 2>&1 | grep -c 'secret')"

# --- 4: control characters in names (a name with one cannot be made here, so the stored name is patched) ---
patch_root() { # patch_root <image> <python statements using blk, pos, name>
    python3 -I - "$1" "$2" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); home = f.read(512)
ibmaplbn, ibmapsize = struct.unpack_from('<I', home, 24)[0], struct.unpack_from('<H', home, 32)[0]
f.seek((ibmaplbn + ibmapsize + 3) * 512); hdr = f.read(512)         # header 4: the root directory
w = struct.unpack_from('<H', hdr, hdr[1] * 2)[0]
lbn = ((w >> 8) & 0x3f) << 16 | struct.unpack_from('<H', hdr, hdr[1] * 2 + 2)[0]
f.seek(lbn * 512); blk = bytearray(f.read(512)); pos = 0
while True:
    size = struct.unpack_from('<H', blk, pos)[0]
    if size == 0xffff: break
    name = bytes(blk[pos + 6:pos + 6 + blk[pos + 5]])
    exec(sys.argv[2])
    pos += size + 2
f.seek(lbn * 512); f.write(blk)
PY
}
IMG2="$TMP/v2.img"
$INIT "$IMG2" 64M --structure=5 >/dev/null 2>&1
for n in P1 P2 P3 P4; do printf '%s\n' $n > "$TMP/$n.txt"; $ODS2 "$IMG2" "COPY $TMP/$n.txt [000000]AX$n.TXT" >/dev/null 2>&1; done
patch_root "$IMG2" "
if name == b'AXP1.TXT': blk[pos + 7] = 0x0A                      # a newline in the name
if name == b'AXP2.TXT': blk[pos + 7] = 0x1B                      # an escape
if name == b'AXP3.TXT': blk[pos + 7] = 0x85; blk[pos + 4] = 0x08 # a C1 control (ISO Latin-1 0x85)
if name == b'AXP4.TXT': blk[pos + 7] = 0x2F                      # a slash
"
shown=$($ODS2 "$IMG2" 'DIR/UTF8 [000000]' 2>&1; $ODS2 "$IMG2" 'DIR/PATHS [000000]' 2>&1; $ODS2 "$IMG2" 'DIR/FULL/UTF8 [000000]' 2>&1)
check "no control character reaches the terminal in /UTF8, /PATHS and /FULL listings" 0 \
    "$(printf '%s' "$shown" | LC_ALL=C grep -c "$(printf '[\001-\011\013-\037\177]')")"
check "...they are shown as %XX" "A%0AP1.TXT A%1BP2.TXT A%2FP4.TXT A%85P3.TXT" \
    "$(printf '%s\n' "$shown" | grep -o 'A%[0-9A-F][0-9A-F]P[0-9]\.TXT' | sort -u | tr '\n' ' ' | sed 's/ $//')"
check "...also the DCL spelling (^0A)" 1 "$($ODS2 "$IMG2" 'DIR [000000]' 2>&1 | grep -c 'A^0AP1')"
rm -rf "$TMP/out"; mkdir -p "$TMP/out"
$ODS2 "$IMG2" "EXPORT [000000] $TMP/out" >/dev/null 2>&1
check "EXPORT names the Linux files with %XX, no newline or slash in a name" \
    "A%0AP1.TXT A%1BP2.TXT A%2FP4.TXT A%85P3.TXT" \
    "$(ls "$TMP/out" | LC_ALL=C grep '^A' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
check "...and the content is right" "P1" "$(cat "$TMP/out/A%0AP1.TXT" 2>/dev/null | tr -d '\n')"

# --- 3: a malformed record among good ones ---
IMG3="$TMP/v3.img"
$INIT "$IMG3" 64M --structure=5 >/dev/null 2>&1
for n in a b c; do printf '%s\n' $n > "$TMP/$n.txt"; $ODS2 "$IMG3" "COPY $TMP/$n.txt [000000]$n.txt" >/dev/null 2>&1; done
patch_root "$IMG3" "
if name == b'b.txt': blk[pos + 4] = 0x18; blk[pos + 5] = 5   # UCS-2 name type, 5 bytes: an odd length
"
out=$($ODS2 "$IMG3" 'DIR [000000]' 2>&1)
check "the records around a malformed one are listed" "a.txt c.txt" \
    "$(echo "$out" | grep -E '^[abc]\.txt' | sed 's/ .*//' | tr '\n' ' ' | sed 's/ $//')"
check "...and a warning says a record was skipped" 1 "$(echo "$out" | grep -c '%ODS2-W-DIRBLOCK, 1 directory record could not be read and was skipped')"

# --- 5: the CLI folds like the directory ---
IMG4="$TMP/v4.img"
$INIT "$IMG4" 64M --structure=5 >/dev/null 2>&1
printf 'e\n' > "$TMP/e.txt"
$ODS2 "$IMG4" "CREATE/DIRECTORY [caf^E9]" >/dev/null 2>&1
$ODS2 "$IMG4" "COPY $TMP/e.txt [caf^E9]e.txt" >/dev/null 2>&1
check "DIR of a directory typed with another case of an ISO Latin-1 letter shows the stored name" "1" \
    "$($ODS2 "$IMG4" 'DIR [CAF^C9]' 2>&1 | grep -c 'e.txt')"
check "...and the heading shows the stored name, not the typed one" "0" \
    "$($ODS2 "$IMG4" 'DIR [CAF^C9]' 2>&1 | grep -c 'Directory \[CAF')"

# --- writer review: round trip of dotless names and empty directories; COPY decodes %XX; a file X.DIR is not a directory ---
IMG5="$TMP/v5.img"
$INIT "$IMG5" 64M --structure=5 >/dev/null 2>&1
rm -rf "$TMP/tree" "$TMP/out"; mkdir -p "$TMP/tree/emptydir" "$TMP/tree/full" "$TMP/out"
printf 'r\n' > "$TMP/tree/README"; printf 'm\n' > "$TMP/tree/Makefile"; printf 'f\n' > "$TMP/tree/full/inside"
printf 'd\n' > "$TMP/tree/a.b.c"
ln -s README "$TMP/tree/lnk"
$ODS2 "$IMG5" "IMPORT $TMP/tree [000000]" >/dev/null 2>&1
$ODS2 "$IMG5" "EXPORT/LINKS [000000...] $TMP/out" >/dev/null 2>&1
rm -rf "$TMP/out/"[0-9A-Z]*.SYS "$TMP/out/000000"* 2>/dev/null
for f in BACKUP.SYS BADBLK.SYS BADLOG.SYS BITMAP.SYS CONTIN.SYS CORIMG.SYS INDEXF.SYS SECURITY.SYS VOLSET.SYS; do rm -f "$TMP/out/$f"; done
rm -rf "$TMP/out/000000"
check "IMPORT then EXPORT is the identity: dotless names stay dotless, empty directories come back" "" \
    "$(cd "$TMP" && diff -r tree out 2>&1 | head -3)"
printf 'p\n' > "$TMP/x%2Fy.txt"
$ODS2 "$IMG5" "COPY $TMP/x%2Fy.txt [000000]" >/dev/null 2>&1
check "COPY with no name decodes %2F like IMPORT (the name has a slash)" 1 "$($ODS2 "$IMG5" 'DIR/PATHS [000000]x*' 2>&1 | grep -c '^/x%2Fy.txt')"
printf 'not a dir\n' > "$TMP/file.txt"
$ODS2 "$IMG5" "COPY $TMP/file.txt [000000]NOTDIR.DIR" >/dev/null 2>&1
check "COPY into [NOTDIR] where NOTDIR.DIR is a plain file is refused" 1 \
    "$($ODS2 "$IMG5" "COPY $TMP/file.txt [NOTDIR]new.txt" 2>&1 | grep -c 'not a directory')"
check "CREATE/DIRECTORY [NOTDIR.SUB] is refused too" 1 "$($ODS2 "$IMG5" 'CREATE/DIRECTORY [NOTDIR.SUB]' 2>&1 | grep -c 'not a directory')"
check "the file's data is untouched" "not a dir" "$($ODS2 "$IMG5" 'TYPE [000000]NOTDIR.DIR' 2>&1)"
rm -rf "$TMP/tree"; mkdir -p "$TMP/tree/NOTDIR"; printf 'q\n' > "$TMP/tree/NOTDIR/q.txt"
check "IMPORT of a directory whose name is a plain file's X.DIR is refused" 1 \
    "$($ODS2 "$IMG5" "IMPORT $TMP/tree [000000]" 2>&1 | grep -c 'is not a directory')"

rm -rf "$TMP"
[ "$fails" -eq 0 ] || { echo "$fails review check(s) failed"; exit 1; }
