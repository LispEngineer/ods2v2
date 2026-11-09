#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_init_check.sh - ods2_init makes empty ODS-2 and ODS-5 volumes of several sizes and
# cluster factors, laid out as VMS V8.4 lays out INITIALIZE's volume: tests/ods5check.py (an
# independent structure check) finds nothing wrong, ods2 mounts them, SHOW VOLUME reports what
# was asked, a file written to each is read back, and an existing image is never overwritten.
# Called by `make test`; run from the project root.

TMP=samples/cli_init_tmp
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
mkdir -p "$TMP"
printf 'hello, fresh volume\n' > "$TMP/hello.txt"

for spec in "8192 5" "10000 2" "65536 5" "300M 5" "100M 2" "1G 5 --cluster=3" "1G 2 --cluster=1" "2G 5"; do
    set -- $spec
    img="$TMP/v.img"
    rm -f "$img"
    $INIT "$img" "$1" --structure="$2" ${3:+"$3"} --label=FRESHVOL >/dev/null 2>&1
    check "init $spec: the independent structure check finds nothing" "0 error(s), 0 warning(s)" \
        "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    out=$($ODS2 "$img" 'SHOW VOLUME' 2>&1)
    check "init $spec: SHOW VOLUME names ODS-$2 and the label" "ODS-$2 FRESHVOL" \
        "ODS-$(echo "$out" | sed -n 's/^  Structure level: *ODS-\([0-9]\).*/\1/p') $(echo "$out" | sed -n 's/^  Volume label: *//p')"
    $ODS2 "$img" "COPY $TMP/hello.txt [000000]HELLO.TXT" >/dev/null 2>&1
    check "init $spec: a file written and read back" "hello, fresh volume" "$($ODS2 "$img" 'TYPE [000000]HELLO.TXT' 2>&1)"
    check "init $spec: still no error after the write" "0 error(s), 0 warning(s)" \
        "$(python3 -I tests/ods5check.py "$img" | tail -1)"
done

# SECURITY.SYS: the profile record VMS INITIALIZE writes (label at +82), revision 2; --security=empty writes nothing
img="$TMP/sec.img"
$INIT "$img" 64M --label=SECLBL >/dev/null 2>&1
check "SECURITY.SYS block 1: record length 82+label, label last, checksum = XOR of the longwords from offset 8" "88 SECLBL ok" \
    "$(python3 -I - "$img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb'); f.seek(512); h = f.read(512)
base = struct.unpack_from('<I', h, 24)[0] + struct.unpack_from('<H', h, 32)[0]
f.seek((base + 9) * 512); hdr = f.read(512)
lbn = (((struct.unpack_from('<H', hdr, hdr[1] * 2)[0] >> 8) & 0x3f) << 16) | struct.unpack_from('<H', hdr, hdr[1] * 2 + 2)[0]
f.seek(lbn * 512); b = f.read(512)
n = struct.unpack_from('<I', b, 8)[0]
x = 0
for k in range(8, n // 4 * 4, 4): x ^= struct.unpack_from('<I', b, k)[0]
print('%d %s %s' % (n, b[82:n].decode(), 'ok' if x == struct.unpack_from('<I', b, 0)[0] and struct.unpack_from('<H', b, 80)[0] == n - 78 else 'BAD'))
PY
)"
rm -f "$img"
$INIT "$img" 64M --security=empty >/dev/null 2>&1
check "--security=empty leaves SECURITY.SYS with EFBLK 1" "1" \
    "$(./ods2_dump_header "$img" 10 2>/dev/null | sed -n 's/.*efblk(raw)=65536.*/1/p' | head -1)"
rm -f "$img"

img="$TMP/v5.img"
$INIT "$img" 64M --structure=5 --hardlinks --access-dates >/dev/null 2>&1
check "init --hardlinks --access-dates sets the characteristics" "ACCESS_DATES, HARDLINKS" \
    "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | sed -n 's/^  Characteristics: *//p')"
check "ODS-2 refuses ODS-5 characteristics" 1 "$($INIT "$TMP/bad.img" 64M --structure=2 --hardlinks >/dev/null 2>&1; echo $?)"
check "ods2_init never overwrites an existing image" 1 "$($INIT "$img" 64M >/dev/null 2>&1; echo $?)"
check "the refused image is not created" "no" "$([ -e "$TMP/bad.img" ] && echo yes || echo no)"
check "too small a volume is refused" 1 "$($INIT "$TMP/small.img" 1000 >/dev/null 2>&1; echo $?)"

# a full volume: writes fail with a message, nothing is left half done, deleting gives the space back
img="$TMP/full.img"
$INIT "$img" 8192 --label=FULL >/dev/null 2>&1
head -c 1000000 /dev/zero > "$TMP/mb.bin"
for i in 1 2 3 4 5 6 7 8; do $ODS2 "$img" "COPY $TMP/mb.bin [000000]F$i.BIN" >/dev/null 2>&1; done
check "a full volume refuses a file with a message" 1 \
    "$($ODS2 "$img" "COPY $TMP/mb.bin [000000]ONEMORE.BIN" 2>&1 | grep -c 'not enough free space')"
check "...and is still consistent" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
for i in 1 2 3 4 5 6 7 8; do $ODS2 "$img" "DELETE [000000]F$i.BIN" >/dev/null 2>&1; done
check "after deleting everything the space is back" "Free blocks: 8166" \
    "$($ODS2 "$img" 'SHOW VOLUME' 2>&1 | sed -n 's/^  Free blocks: */Free blocks: /p')"

# writes to damaged volumes (see fuzz_ods5.py --write): errors, never crashes
mkdir -p "$TMP/fz/sub"
printf 'hello\n' > "$TMP/fz/hello.txt"; printf 'x\n' > "$TMP/fz/sub/a b.txt"; ln -s hello.txt "$TMP/fz/ln1"
$INIT "$TMP/fzbase.img" 64M --hardlinks >/dev/null 2>&1
$ODS2 "$TMP/fzbase.img" "IMPORT $TMP/fz [000000]" >/dev/null 2>&1
check "60 iterations of writing to a damaged ODS-5 volume: no crash, hang or sanitizer report" 0 \
    "$(python3 -I tests/fuzz_ods5.py $ODS2 "$TMP/fzbase.img" 60 7 "$TMP/fzwork" --write "$TMP/fz" >/dev/null 2>&1; echo $?)"

# LINK without HARDLINKS makes a plain alias (as VMS SET FILE/ENTER); deleting the alias keeps the file
for st in 2 5; do
    $INIT "$TMP/al.img" 8M --structure=$st --label=ALIAS >/dev/null 2>&1
    printf 'alias test\n' > "$TMP/al.txt"
    $ODS2 "$TMP/al.img" "COPY $TMP/al.txt [000000]A.TXT" >/dev/null 2>&1
    check "structure $st, no HARDLINKS: LINK makes an alias" "yes" \
        "$($ODS2 "$TMP/al.img" 'LINK [000000]B.TXT [000000]A.TXT' 2>&1 | grep -q 'alias of' && echo yes)"
    check "structure $st: both names have one file ID" 1 \
        "$($ODS2 "$TMP/al.img" 'DIR/FULL [000000]*.TXT' 2>&1 | sed -n 's/^  File ID: *//p' | sort -u | wc -l | tr -d ' ')"
    $ODS2 "$TMP/al.img" 'DELETE [000000]B.TXT;1' >/dev/null 2>&1
    check "structure $st: deleting the alias leaves the file" "alias test" "$($ODS2 "$TMP/al.img" 'TYPE [000000]A.TXT' 2>&1)"
    check "structure $st: the checker is satisfied" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/al.img" 2>&1 | tail -1)"
    check "structure $st: DIR/FULL says link count 1 (as VMS does without HARDLINKS)" 1 "$($ODS2 "$TMP/al.img" 'DIR/FULL [000000]A.TXT' 2>&1 | sed -n 's/^  Link count: *//p')"
done
# DELETE says when it keeps the file because the entry is an alias (other name; same name in another directory)
$INIT "$TMP/dm.img" 8M --structure=5 --label=ALMSG >/dev/null 2>&1
$ODS2 "$TMP/dm.img" "COPY $TMP/al.txt [000000]A.TXT" >/dev/null 2>&1
$ODS2 "$TMP/dm.img" 'CREATE/DIRECTORY [SUB]' >/dev/null 2>&1
$ODS2 "$TMP/dm.img" 'LINK [000000]B.TXT [000000]A.TXT' >/dev/null 2>&1
$ODS2 "$TMP/dm.img" 'LINK [SUB]A.TXT [000000]A.TXT' >/dev/null 2>&1
check "DELETE of an alias with another name says the file stays and names the primary entry" "yes" \
    "$($ODS2 "$TMP/dm.img" 'DELETE [000000]B.TXT' 2>&1 | grep -q 'ALIASREMOVED.*file stays.*A.TXT;1 as the primary' && echo yes)"
check "DELETE of an alias with the same name in another directory says the back link names another directory" "yes" \
    "$($ODS2 "$TMP/dm.img" 'DELETE [SUB]A.TXT' 2>&1 | grep -q 'ALIASREMOVED.*file stays.*back link names another directory' && echo yes)"
check "... the file is still there" "alias test" "$($ODS2 "$TMP/dm.img" 'TYPE [000000]A.TXT' 2>&1)"
check "DELETE of the primary entry says plain DELETED" "yes" "$($ODS2 "$TMP/dm.img" 'DELETE [000000]A.TXT' 2>&1 | grep -q '^%ODS2-I-DELETED' && echo yes)"

# an alias left behind when its file is deleted through the other name dangles (VMS keeps the entry, "no such file");
# the file number may be reused, and the entry must not reach the new file (sequence number check)
$INIT "$TMP/dg.img" 8M --structure=5 --label=DANG >/dev/null 2>&1
$ODS2 "$TMP/dg.img" "COPY $TMP/al.txt [000000]A.TXT" >/dev/null 2>&1
$ODS2 "$TMP/dg.img" 'LINK [000000]B.TXT [000000]A.TXT' >/dev/null 2>&1
$ODS2 "$TMP/dg.img" 'DELETE [000000]A.TXT' >/dev/null 2>&1
$ODS2 "$TMP/dg.img" "COPY $TMP/al.txt [000000]NEW.TXT" >/dev/null 2>&1
check "a dangling alias: TYPE says not found (not the file that reused the slot)" "yes" \
    "$($ODS2 "$TMP/dg.img" 'TYPE [000000]B.TXT' 2>&1 | grep -q 'dangling' && echo yes)"
check "a dangling alias: DIR/FULL says no such file" "yes" "$($ODS2 "$TMP/dg.img" 'DIR/FULL [000000]B.TXT' 2>&1 | grep -q 'no such file' && echo yes)"
check "deleting the dangling alias says that it was dangling" "yes" \
    "$($ODS2 "$TMP/dg.img" 'DELETE [000000]B.TXT' 2>&1 | grep -q 'ODS2-I-DANGLING.*no longer exists' && echo yes)"
check "deleting the dangling alias takes only its entry" "alias test" "$($ODS2 "$TMP/dg.img" 'TYPE [000000]NEW.TXT' 2>&1)"
check "... and leaves a checker-clean volume" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/dg.img" 2>&1 | tail -1)"

rm -rf "$TMP"
[ "$fails" -eq 0 ] || { echo "$fails INITIALIZE check(s) failed"; exit 1; }
