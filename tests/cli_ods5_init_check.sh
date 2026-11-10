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

# ---- the storage control block (R1, physical VAX96: %ANALDISK-W-CHKSCB). VMS writes the geometry of the device,
# cylinders = ceil(blocks / (sectors * tracks)), BLKSIZE 1; VAX V7.3 ANALYZE takes an SCB as invalid when
# |VOLSIZE - sectors*tracks*cylinders/BLKSIZE| > sectors*tracks and divides by BLKSIZE (0: INTDIV).
cat > "$TMP/scb.py" <<'PY'
import struct, subprocess, sys, os
ods2, img, mode = sys.argv[1], sys.argv[2], sys.argv[3]
if mode == 'show':
    out = img + '.bm'
    if os.path.exists(out): os.remove(out)
    subprocess.run([ods2, img, 'EXPORT [000000]BITMAP.SYS;1 ' + out], capture_output=True)
    s = open(out, 'rb').read(512)
    lev, cl, vs, bs, sec, trk, cyl = struct.unpack_from('<HHIIIII', s, 0)
    print(lev, vs, bs, sec, trk, cyl)
else:
    # patch the SCB in place: mode = 'blksize0' or 'wrap'
    f = open(img, 'r+b'); f.seek(512); home = f.read(512)
    base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
    f.seek((base + 1) * 512); h = f.read(512)
    w0, w1 = struct.unpack_from('<HH', h, h[1] * 2)
    lbn = (((w0 >> 8) & 0x3f) << 16) | w1
    f.seek(lbn * 512); scb = bytearray(f.read(512))
    if mode == 'blksize0': struct.pack_into('<I', scb, 8, 0)
    elif mode == 'wrap': struct.pack_into('<I', scb, 20, struct.unpack_from('<I', scb, 20)[0] // 2)
    struct.pack_into('<H', scb, 510, sum(struct.unpack_from('<255H', scb, 0)) & 0xFFFF)
    f.seek(lbn * 512); f.write(scb)
PY
for spec in "2 8192" "5 8192" "5 16G" "2 20G"; do
    set -- $spec
    $INIT "$TMP/g.img" $2 --structure=$1 --label=GEO >/dev/null 2>&1
    got=$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/g.img" show)
    set -- $got
    # level volsize blksize sectors tracks cylinders
    ok=no
    [ "$3" = 1 ] && [ $(( ($2 + $4 * $5 - 1) / ($4 * $5) )) = "$6" ] && [ "$6" -le 65535 ] && ok=yes
    check "ODS-$1 SCB of a $2-block volume: BLKSIZE 1, cylinders = ceil(blocks/(sectors*tracks)) <= 65535 (got $4 x $5 x $6)" yes "$ok"
    warn=$($ODS2 "$TMP/g.img" 'DIR [000000]' 2>&1 >/dev/null | grep -c CHKSCB)
    check "ODS-$1 $2: a fresh volume draws no CHKSCB warning" 0 "$warn"
    rm -f "$TMP/g.img" "$TMP/g.img.bm"
done
$INIT "$TMP/g.img" 1G --structure=2 --label=GEO --geometry=38,38 >/dev/null 2>&1
check "--geometry=38,38 on 1 GiB: SCB 38 x 38 x 1453 (the LD driver's), BLKSIZE 1" "513 2097152 1 38 38 1453" \
    "$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/g.img" show | sed 's/^513 /513 /')"
check "... no CHKSCB warning" 0 "$($ODS2 "$TMP/g.img" 'DIR [000000]' 2>&1 >/dev/null | grep -c CHKSCB)"
$INIT "$TMP/g.img.2" 1G --structure=2 --label=GEO --geometry=0,5 >/dev/null 2>&1; check "--geometry=0,5 is refused" 2 $?
rm -f "$TMP/g.img.2" "$TMP/g.img.bm"
python3 -I "$TMP/scb.py" $ODS2 "$TMP/g.img" blksize0
out=$($ODS2 "$TMP/g.img" 'DIR [000000]' 2>&1 >/dev/null | grep CHKSCB)
case "$out" in *"blocking factor is 0"*"INTDIV"*) msg=ok ;; *) msg="$out" ;; esac
check "an SCB with BLKSIZE 0 (a wrapped SIMH RQ geometry): a read-only warning naming INTDIV; the volume is still read" ok "$msg"
check "... DIR still lists the volume" 1 "$($ODS2 "$TMP/g.img" 'DIR [000000]' 2>/dev/null | grep -c BITMAP.SYS)"
$INIT "$TMP/g2.img" 1G --structure=2 --label=GEO --geometry=32,16 >/dev/null 2>&1
python3 -I "$TMP/scb.py" $ODS2 "$TMP/g2.img" wrap
out=$($ODS2 "$TMP/g2.img" 'DIR [000000]' 2>&1 >/dev/null | grep CHKSCB)
case "$out" in *"more than one cylinder from the volume size"*) msg=ok ;; *) msg="$out" ;; esac
check "an SCB whose geometry is more than a cylinder from the volume size: warns" ok "$msg"

# ---- a SIMH container footer (simh/simh appends 512 bytes, "simh" signature, to a raw container it opens for
# write; on an image of 4 GiB or more a 32-bit overflow can put it into the last data block). After the volume: noted,
# never read or written. Inside the volume: warned, mounted read-only.
cat > "$TMP/foot.py" <<'PY'
import struct, sys, zlib
mode, img, blocks = sys.argv[1], sys.argv[2], int(sys.argv[3])
f = bytearray(512)
f[0:4] = b'simh'; f[4:13] = b'Open SIMH'; f[68:72] = b'RA81'
struct.pack_into('>III', f, 84, 512, blocks, 2)
f[96:120] = b'Mon Nov  9 12:00:00 2026'
f[124] = 0; f[125] = 2
struct.pack_into('>I', f, 508, zlib.crc32(bytes(f[:508])) & 0xffffffff)
fh = open(img, 'r+b')
fh.seek(blocks * 512 if mode == 'append' else (blocks - 1) * 512); fh.write(bytes(f))
PY
$INIT "$TMP/s0.img" 8192 --structure=2 --label=SIMHFT >/dev/null 2>&1
echo hi > "$TMP/hi.txt"
cp "$TMP/s0.img" "$TMP/s1.img"; python3 -I "$TMP/foot.py" append "$TMP/s1.img" 8192
foot1=$(tail -c 512 "$TMP/s1.img" | md5sum)
out=$($ODS2 "$TMP/s1.img" "COPY $TMP/hi.txt [000000]HI.TXT" 2>&1); rc=$?
case "$out" in *"I-SIMHFOOTER"*"not volume data (the volume is 8192 blocks)"*"%ODS2-I-COPIED"*) msg=ok ;; *) msg="$out" ;; esac
check "footer after the volume: noted, and the volume is written normally" "ok 0" "$msg $rc"
check "... the footer is untouched and the image is no longer" "$foot1 8193" "$(tail -c 512 "$TMP/s1.img" | md5sum) $(( $(stat -c %s "$TMP/s1.img") / 512 ))"
check "... ods5check (the volume, not the footer): clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/s1.img" 2>&1 | tail -1)"
cp "$TMP/s0.img" "$TMP/s2.img"; python3 -I "$TMP/foot.py" inside "$TMP/s2.img" 8192
before=$(md5sum < "$TMP/s2.img")
out=$($ODS2 "$TMP/s2.img" "COPY $TMP/hi.txt [000000]HI.TXT" 2>&1); rc=$?
case "$out" in *"W-SIMHFOOTER"*"inside the volume (8192 blocks)"*"last block of the volume may be lost"*"read-only"*) msg=warned ;; *) msg="$out" ;; esac
check "footer inside the volume (the last block): warned, and the write is refused (exit 1)" "warned 1" "$msg $rc"
check "... the image is unchanged" "$before" "$(md5sum < "$TMP/s2.img")"
check "... it can still be read" 1 "$($ODS2 "$TMP/s2.img" 'DIR [000000]' 2>/dev/null | grep -c BITMAP.SYS)"
# SET VOLUME/GEOMETRY: the geometry of the device the image is to be used on (OpenVMS VAX V7.3 compares it with the device's)
$INIT "$TMP/gs.img" 1G --structure=2 --label=GEOSET >/dev/null 2>&1
out=$($ODS2 "$TMP/gs.img" "SET VOLUME/GEOMETRY=57,15" 2>&1); rc=$?
check "SET VOLUME/GEOMETRY=57,15: done" "ok 0" "$(echo "$out" | grep -q 'I-GEOMETRY.*57 sectors x 15 tracks' && echo ok) $rc"
check "... the SCB has 57 x 15 x 2453, BLKSIZE 1, as VMS made on the RA82 (scb1 SCB_C)" "513 2097152 1 57 15 2453" "$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/gs.img" show)"
check "... ods5check is clean and the label is untouched" "0 error(s), 0 warning(s) GEOSET" \
    "$(python3 -I tests/ods5check.py "$TMP/gs.img" 2>&1 | tail -1) $($ODS2 "$TMP/gs.img" 'SHOW VOLUME' 2>&1 | grep -o 'GEOSET' | head -1)"
$ODS2 "$TMP/gs.img" "SET VOLUME/GEOMETRY=0,15" >/dev/null 2>&1; check "SET VOLUME/GEOMETRY=0,15 is refused (exit 1)" 1 $?
$ODS2 "$TMP/gs.img" "SET VOLUME/GEOMETRY=38,38/LABEL=BOTH" >/dev/null 2>&1
check "geometry and label together: both set" "513 2097152 1 38 38 1453 BOTH" "$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/gs.img" show) $($ODS2 "$TMP/gs.img" 'SHOW VOLUME' 2>&1 | grep -o 'BOTH' | head -1)"

# device types: the geometry each reports (measured by ods5-vms2 on VAX V7.3: LD 38 x 38 at 1 GiB, RA82 57 x 15, RA92 73 x 13)
$INIT "$TMP/tg.img" 1G --structure=2 --label=TGT --target=ld >/dev/null 2>&1
check "ods2_init --target=ld on 1 GiB: 38 x 38 x 1453 (what LDA5: reports)" "513 2097152 1 38 38 1453" "$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/tg.img" show)"
out=$($ODS2 "$TMP/tg.img" "SET VOLUME/GEOMETRY=ra" 2>&1)
check "SET VOLUME/GEOMETRY=ra on 1 GiB: 57 x 15 x 2453 (the RA82), no prediction warning (a measured size)" "513 2097152 1 57 15 2453 0" \
    "$(python3 -I "$TMP/scb.py" $ODS2 "$TMP/tg.img" show) $(echo "$out" | grep -c PREDICTED)"
$ODS2 "$TMP/tg.img" "SET VOLUME/GEOMETRY=nonesuch" >/dev/null 2>&1; check "an unknown device type is refused" 1 $?
$INIT "$TMP/tg2.img" 3000000 --target=ld >/dev/null 2>"$TMP/tg2.err"
check "an unmeasured size says it is a prediction" 1 "$(grep -c prediction "$TMP/tg2.err")"

# ods2_init says which geometry it wrote, and how to fit a VAX
out=$($INIT "$TMP/pg.img" 1G --structure=2 2>&1)
check "ods2_init (default): prints the geometry and the VAX advice" "1 1 1" \
    "$(echo "$out" | grep -c 'I-GEOMETRY.*32 sectors x 16 tracks x 4096 cylinders (the default)') $(echo "$out" | grep -c -- '--target=ld') $(echo "$out" | grep -c 'SET VOLUME/GEOMETRY')"
out=$($INIT "$TMP/pg2.img" 1G --structure=2 --target=ld 2>&1)
check "ods2_init --target=ld: prints 38 x 38 x 1453 (ld), no advice line" "1 0" \
    "$(echo "$out" | grep -c 'I-GEOMETRY.*38 sectors x 38 tracks x 1453 cylinders (ld)') $(echo "$out" | grep -c -- '-ODS2-I-VAX')"
# R14: "measured" per size and per type, and the 16-bit cylinder count
$INIT "$TMP/r1.img" 100G --structure=5 --target=rz >/dev/null 2>"$TMP/r1.err"
check "R14-1: --target=rz at 100 GiB (not a measured size): says it is a prediction, and warns that 819,200 cylinders wrap" "1 1" \
    "$(grep -c prediction "$TMP/r1.err") $(grep -c 'more than 16 bits' "$TMP/r1.err")"
$INIT "$TMP/r2.img" 1G --structure=5 --target=rz >/dev/null 2>"$TMP/r2.err"
check "R14-1/3: --target=rz at 1 GiB (measured): neither" "0 0" "$(grep -c prediction "$TMP/r2.err") $(grep -c 'more than 16 bits' "$TMP/r2.err")"
$INIT "$TMP/r3.img" 4G --structure=5 --target=ra92 >/dev/null 2>"$TMP/r3.err"
check "R14-2: --target=ra92 at 4 GiB (8,388,608 blocks is measured as RA92)" "0" "$(grep -c prediction "$TMP/r3.err")"
$INIT "$TMP/r4.img" 4G --structure=5 --target=ra82 >/dev/null 2>"$TMP/r4.err"
check "R14-2: --target=ra82 at 4 GiB (measured only as RA92): a prediction" "1" "$(grep -c prediction "$TMP/r4.err")"
$INIT "$TMP/r5.img" 1G --structure=5 --target=ra92 >/dev/null 2>"$TMP/r5.err"
check "R14-2: --target=ra92 at 1 GiB (measured only as RA82): a prediction" "1" "$(grep -c prediction "$TMP/r5.err")"
$INIT "$TMP/r6.img" 20G --structure=5 >/dev/null 2>"$TMP/r6.err"
check "the default geometry never wraps (20 GiB: 63 x 255), no warning" "0" "$(grep -c 'more than 16 bits' "$TMP/r6.err")"
rm -f "$TMP"/r?.img
out=$($ODS2 "$TMP/tg.img" "SET VOLUME/GEOMETRY=1,1" 2>&1)
check "SET VOLUME/GEOMETRY with more than 65,535 cylinders warns (CYLWRAP)" 1 "$(echo "$out" | grep -c CYLWRAP)"

# ---- file dates are the local time of the VMS system (FUTCREDAT / FUTREVDAT on VMS when they are not): the writer stamps
# the zone of TZ (ODS2_TZ names the VMS machine's zone when it is another), never UTC by default; ods2_init says which zone,
# and ods5check warns of dates ahead of the host that runs it. (A batch builder that sets TZ=UTC for reproducibility
# writes dates 5 hours ahead of an EST VMS machine.)
echo hi > "$TMP/tz.txt"
TZ=Pacific/Honolulu $INIT "$TMP/tzh.img" 8192 --structure=2 --label=TZH >"$TMP/tzh.out" 2>&1
TZ=Pacific/Honolulu $ODS2 "$TMP/tzh.img" "COPY $TMP/tz.txt [000000]X.TXT" >/dev/null 2>&1
TZ=UTC $INIT "$TMP/tzu.img" 8192 --structure=2 --label=TZU >"$TMP/tzu.out" 2>&1
TZ=UTC $ODS2 "$TMP/tzu.img" "COPY $TMP/tz.txt [000000]X.TXT" >/dev/null 2>&1
ODS2_TZ=Pacific/Honolulu TZ=UTC $INIT "$TMP/tzo.img" 8192 --structure=2 --label=TZO >"$TMP/tzo.out" 2>&1
ODS2_TZ=Pacific/Honolulu TZ=UTC $ODS2 "$TMP/tzo.img" "COPY $TMP/tz.txt [000000]X.TXT" >/dev/null 2>&1
check "ods2_init says the zone its dates are in (TZ=Pacific/Honolulu: HST, -1000)" "1 1" \
    "$(grep -c 'I-TIME' "$TMP/tzh.out") $(grep -c -- '-1000' "$TMP/tzh.out")"
check "... and that ODS2_TZ overrides TZ (it names the VMS machine's zone)" "1" "$(grep -c 'ODS2_TZ=Pacific/Honolulu' "$TMP/tzo.out")"
check "ods5check (run in Honolulu time) on a volume written in Honolulu time: no future date" "0 error(s), 0 warning(s)" \
    "$(TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzh.img" 2>&1 | tail -1)"
check "ods5check on a volume written with TZ=UTC (10 hours ahead of Honolulu): warns, names ODS2_TZ" "yes" \
    "$(TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzu.img" 2>&1 | grep -q 'hours in the future.*ODS2_TZ' && echo yes)"
check "ODS2_TZ=Pacific/Honolulu over TZ=UTC writes Honolulu dates: no future date" "0 error(s), 0 warning(s)" \
    "$(TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzo.img" 2>&1 | tail -1)"
# ---- the trailing partial cluster (physical/package B on V8.4: ALLOCCLR "blocks incorrectly marked allocated" for the
# blocks past the last whole cluster). VMS gives it to BADBLK.SYS: one extent at floor(VOLSIZE/cluster) * cluster, a whole
# cluster (what lies past the volume's end is unreadable), HIBLK = cluster, EFBLK = cluster + 1, fixed 512-byte records,
# HIGHWATER = cluster + 1; its bit in the bitmap is clear (V7.3 INITIALIZE, VAX and Alpha blank-volume library, an Alpha
# V8.4 system disk, and V8.4-2L2 INITIALIZE of 20,000 blocks /CLUSTER_SIZE=3: BADBLK.SYS = (19998, 3)). When the size is a
# multiple of the cluster BADBLK.SYS is empty.
cat > "$TMP/badblk.py" <<'PY'
import struct, subprocess, sys, os
ods2, img = sys.argv[1], sys.argv[2]
f = open(img, 'rb'); f.seek(512); home = f.read(512)
cl = struct.unpack_from('<H', home, 14)[0]; ibl = struct.unpack_from('<I', home, 24)[0]; ibs = struct.unpack_from('<H', home, 32)[0]
f.seek((ibl + ibs + 2) * 512); h = f.read(512)
def sw(o): a, b = struct.unpack_from('<HH', h, o); return (a << 16) | b
mp = h[1] * 2; inuse = h[58]; p = mp; q = 0; ex = []
while q < inuse:
    w0 = struct.unpack_from('<H', h, p)[0]; fmt = (w0 >> 14) & 3
    if fmt == 1: cnt = (w0 & 0xff) + 1; lbn = (((w0 >> 8) & 0x3f) << 16) | struct.unpack_from('<H', h, p + 2)[0]; p += 4; q += 2
    elif fmt == 2: cnt = (w0 & 0x3fff) + 1; lbn = struct.unpack_from('<I', h, p + 2)[0]; p += 6; q += 3
    else: break
    ex.append((lbn, cnt))
print(cl, ex, sw(24), sw(28), struct.unpack_from('<I', h, 76)[0], h[20], struct.unpack_from('<H', h, 22)[0])
PY
for spec in "2 20000 3" "5 20000 3" "2 20000 4" "5 100000 3"; do
    set -- $spec
    $INIT "$TMP/pc.img" $2 --structure=$1 --cluster=$3 --label=PARTC >/dev/null 2>&1
    rem=$(( $2 % $3 )); whole=$(( $2 / $3 * $3 ))
    if [ $rem -ne 0 ]; then want="$3 [($whole, $3)] $3 $(( $3 + 1 )) $(( $3 + 1 )) 1 512"; else want="$3 [] 0 1 1 1 512"; fi
    check "ODS-$1, $2 blocks, cluster $3 (remainder $rem): BADBLK.SYS maps the partial cluster as VMS does" "$want" \
        "$(python3 -I "$TMP/badblk.py" $ODS2 "$TMP/pc.img" | sed 's/(\([0-9]*\), \([0-9]*\))/(\1, \2)/')"
    check "... ods5check: clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/pc.img" 2>&1 | tail -1)"
    rm -f "$TMP/pc.img"
done
# an old volume (BADBLK.SYS empty, the partial cluster allocated to nobody): ods5check warns, the rest is as before
$INIT "$TMP/pc.img" 20000 --structure=2 --cluster=3 --label=PARTC >/dev/null 2>&1
python3 -I - "$TMP/pc.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); home = f.read(512)
ibl = struct.unpack_from('<I', home, 24)[0]; ibs = struct.unpack_from('<H', home, 32)[0]
f.seek((ibl + ibs + 2) * 512); h = bytearray(f.read(512))
struct.pack_into('<HH', h, 24, 0, 0); struct.pack_into('<HH', h, 28, 0, 1)   # HIBLK 0, EFBLK 1
h[58] = 0                                                                     # no retrieval pointers
struct.pack_into('<I', h, 76, 1)                                              # HIGHWATER 1
for i in range(h[1] * 2, 510): h[i] = 0
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek((ibl + ibs + 2) * 512); f.write(h)
PY
out=$(python3 -I tests/ods5check.py "$TMP/pc.img" 2>&1)
check "an old volume (BADBLK.SYS empty): ods5check warns that the partial cluster is owned by nobody" "0 error(s), 1 warning(s) yes" \
    "$(echo "$out" | tail -1) $(echo "$out" | grep -q 'partial cluster.*BADBLK.SYS' && echo yes)"
out=$($ODS2 "$TMP/pc.img" "SET VOLUME/PARTIAL_CLUSTER" 2>&1); rc=$?
case "$out" in *"now belong to BADBLK.SYS"*) msg=assigned ;; *) msg="$out" ;; esac
check "SET VOLUME/PARTIAL_CLUSTER on the old volume: BADBLK.SYS gets the partial cluster" "assigned 0" "$msg $rc"
check "... the same header as ods2_init makes now, and ods5check is clean" "3 [(19998, 3)] 3 4 4 1 512 0 error(s), 0 warning(s)" \
    "$(python3 -I "$TMP/badblk.py" $ODS2 "$TMP/pc.img") $(python3 -I tests/ods5check.py "$TMP/pc.img" 2>&1 | tail -1)"
out=$($ODS2 "$TMP/pc.img" "SET VOLUME/PARTIAL_CLUSTER" 2>&1)
check "... and a second run has nothing to do" yes "$(echo "$out" | grep -q 'nothing to do' && echo yes)"
rm -f "$TMP/pc.img"

# R17-1: SET VOLUME/PARTIAL_CLUSTER refuses an SCB that is not one (the starter volume's all-ones placeholder, a damaged
# one) and a partial cluster outside the image: nothing is written
$INIT "$TMP/pcs.img" 20000 --structure=2 --cluster=3 --label=PCSCB >/dev/null 2>&1
python3 -I - "$TMP/pcs.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); home = f.read(512)
ibl = struct.unpack_from('<I', home, 24)[0]; ibs = struct.unpack_from('<H', home, 32)[0]
# BADBLK.SYS emptied, as in an old volume
f.seek((ibl + ibs + 2) * 512); h = bytearray(f.read(512))
struct.pack_into('<HH', h, 24, 0, 0); struct.pack_into('<HH', h, 28, 0, 1); h[58] = 0; struct.pack_into('<I', h, 76, 1)
for i in range(h[1] * 2, 510): h[i] = 0
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek((ibl + ibs + 2) * 512); f.write(h)
# the SCB overwritten with the placeholder (all ones / 0xAAAA), as the starter volume has
f.seek((ibl + ibs + 1) * 512); h2 = f.read(512)
mp = h2[1] * 2; w0, w1 = struct.unpack_from('<HH', h2, mp); lbn = (((w0 >> 8) & 0x3f) << 16) | w1
f.seek(lbn * 512); f.write(b'\xff' * 512)
PY
before=$(md5sum < "$TMP/pcs.img")
out=$($ODS2 "$TMP/pcs.img" "SET VOLUME/PARTIAL_CLUSTER" 2>&1); rc=$?
case "$out" in *"E-PARTIAL"*"storage control block"*) msg=refused ;; *) msg="$out" ;; esac
check "SET VOLUME/PARTIAL_CLUSTER on a placeholder SCB: refused (exit 1), nothing written" "refused 1 $before" "$msg $rc $(md5sum < "$TMP/pcs.img")"
rm -f "$TMP/pcs.img"
if [ -f samples/empty_ods2_volume.dsk.gz ]; then
    zcat samples/empty_ods2_volume.dsk.gz > "$TMP/starter.img" 2>/dev/null
    before=$(md5sum < "$TMP/starter.img")
    out=$($ODS2 "$TMP/starter.img" "SET VOLUME/PARTIAL_CLUSTER" 2>&1); rc=$?
    case "$out" in *"nothing to do"*|*"E-PARTIAL"*) msg=ok ;; *) msg="$out" ;; esac
    check "SET VOLUME/PARTIAL_CLUSTER on the starter volume (VMS-made, size a multiple of nothing or owned): no change" "ok $before" "$msg $(md5sum < "$TMP/starter.img")"
    rm -f "$TMP/starter.img"
fi

# R17-4: the checker's future-date warning follows ODS2_TZ (the VMS system's zone) and names the clock as the other cause
TZ=UTC $INIT "$TMP/tzf.img" 8192 --structure=2 --label=TZF >/dev/null 2>&1
check "ods5check: dates written in UTC, host in Honolulu: warns; with ODS2_TZ=UTC (the VMS system's zone) it does not" "yes 0" \
    "$(TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzf.img" 2>&1 | grep -q 'hours in the future' && echo yes) $(ODS2_TZ=UTC TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzf.img" 2>&1 | grep -c 'hours in the future')"
check "... the warning names both causes: the zone and the VMS machine's clock" yes \
    "$(TZ=Pacific/Honolulu python3 -I tests/ods5check.py "$TMP/tzf.img" 2>&1 | grep 'hours in the future' | head -1 | grep -q "ODS2_TZ.*clock" && echo yes)"
rm -f "$TMP/tzf.img"

# the rule, pinned: with TZ set, a new file's Created date is that zone's wall-clock time, and it reads back as the same
# time (DIR/FULL prints the stored local time unchanged)
for zone in Pacific/Honolulu America/New_York Asia/Kolkata UTC; do
    TZ=$zone $INIT "$TMP/tzr.img" 8192 --structure=2 --label=TZR >/dev/null 2>&1
    want_before=$(TZ=$zone date +%d-%b-%Y' '%H:%M | tr a-z A-Z)
    TZ=$zone $ODS2 "$TMP/tzr.img" "COPY $TMP/tz.txt [000000]X.TXT" >/dev/null 2>&1
    want_after=$(TZ=$zone date +%d-%b-%Y' '%H:%M | tr a-z A-Z)
    got=$(TZ=$zone $ODS2 "$TMP/tzr.img" "DIR/FULL [000000]X.TXT" 2>&1 | sed -n 's/^ *Created: *\([0-9]*-[A-Z]*-[0-9]*\) \([0-9]*:[0-9]*\):.*/\1 \2/p')
    case "$got" in "$want_before"|"$want_after") res=local ;; *) res="$got (wanted $want_before)" ;; esac
    check "TZ=$zone: the Created date stored and read back is that zone's local time" local "$res"
    rm -f "$TMP/tzr.img"
done
# every date the tools write is local: the home block, the reserved files, a new file and a new directory
for zone in America/New_York Asia/Kolkata; do
    TZ=$zone $INIT "$TMP/tzk.img" 8192 --structure=5 --label=TZK >/dev/null 2>&1
    TZ=$zone $ODS2 "$TMP/tzk.img" "COPY $TMP/tz.txt [000000]X.TXT" >/dev/null 2>&1
    TZ=$zone $ODS2 "$TMP/tzk.img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    check "everything written under TZ=$zone is not ahead of $zone (reserved files, file, directory, ODS-5 dates)" "0 error(s), 0 warning(s)" \
        "$(TZ=$zone python3 -I tests/ods5check.py "$TMP/tzk.img" 2>&1 | tail -1)"
    rm -f "$TMP/tzk.img"
done

# a block that only starts like a footer (the signature and the two bytes, but a wrong CRC) is not one: no message, a normal write mount
cp "$TMP/s0.img" "$TMP/s4.img"; python3 -I "$TMP/foot.py" inside "$TMP/s4.img" 8192
python3 -I - "$TMP/s4.img" <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(8192 * 512 - 1); b = f.read(1); f.seek(8192 * 512 - 1); f.write(bytes([b[0] ^ 0xff]))
PY
out=$($ODS2 "$TMP/s4.img" "COPY $TMP/hi.txt [000000]HI.TXT" 2>&1); rc=$?
case "$out" in *SIMHFOOTER*) msg="$out" ;; *"I-COPIED"*) msg=written ;; *) msg="$out" ;; esac
check "a last block with the signature but a wrong CRC is no footer: no message, the volume is written" "written 0" "$msg $rc"
check "no footer: no SIMHFOOTER message" 0 "$($ODS2 "$TMP/s0.img" 'DIR [000000]' 2>&1 | grep -c SIMHFOOTER)"
# nothing is written at or past the end of the volume (the storage control block's VOLSIZE), even if the bitmap
# offers it: an SCB that says 2000 blocks on an 8192-block image whose first 2000 blocks the bitmap calls used
cat > "$TMP/bound.py" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
f.seek((base + 1) * 512); h = f.read(512)
w0, w1 = struct.unpack_from('<HH', h, h[1] * 2); lbn = (((w0 >> 8) & 0x3f) << 16) | w1
cl = struct.unpack_from('<H', home, 14)[0]
f.seek(lbn * 512); scb = bytearray(f.read(512))
struct.pack_into('<I', scb, 4, int(sys.argv[2]))
struct.pack_into('<H', scb, 510, sum(struct.unpack_from('<255H', scb, 0)) & 0xFFFF)
f.seek(lbn * 512); f.write(scb)
nbits = int(sys.argv[2]) // cl
nblk = (nbits + 4095) // 4096
f.seek((lbn + 1) * 512); bm = bytearray(f.read(512 * nblk))
for c in range(nbits): bm[c // 8] &= ~(1 << (c % 8))
f.seek((lbn + 1) * 512); f.write(bm)
PY
for vs in 2000 4200; do
    cp "$TMP/s0.img" "$TMP/s3.img"; python3 -I "$TMP/bound.py" "$TMP/s3.img" $vs; before=$(md5sum < "$TMP/s3.img")
    printf 'CREATE/DIRECTORY [X]\nDIR [000000]\n' > "$TMP/s3.scr"
    out=$($ODS2 "$TMP/s3.img" "@$TMP/s3.scr" 2>&1); rc=$?
    case "$out" in *"CREATEERR"*"BITMAP.SYS"*"WRITEFAIL"*) msg="DEAD: $out" ;; *"E-CREATEERR"*) msg=refused ;; *) msg="$out" ;; esac
    check "SCB VOLSIZE $vs: CREATE/DIRECTORY is refused, exit 1" "refused 1" "$msg $rc"
    check "... the script went on (no write_failed: nothing was half written) and listed the volume" 1 "$(echo "$out" | grep -c 'BITMAP.SYS')"
    check "... nothing is written" "$before" "$(md5sum < "$TMP/s3.img")"
    out=$($ODS2 "$TMP/s3.img" "COPY $TMP/hi.txt [000000]HI.TXT" 2>&1); rc=$?
    case "$out" in *"E-COPYERR"*) msg=refused ;; *) msg="$out" ;; esac
    check "SCB VOLSIZE $vs: COPY is refused, exit 1, nothing written" "refused 1 $before" "$msg $rc $(md5sum < "$TMP/s3.img")"
done

rm -rf "$TMP"
[ "$fails" -eq 0 ] || { echo "$fails INITIALIZE check(s) failed"; exit 1; }
