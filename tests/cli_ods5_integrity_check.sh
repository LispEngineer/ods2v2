#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_integrity_check.sh - the writer's data-integrity rules (review 6), through the CLI:
#   - the reserved files (INDEXF.SYS ... BADLOG.SYS, file numbers 1-9, and SECURITY.SYS, 10) cannot be deleted,
#     on ODS-2 and ODS-5, with and without a version or ;*: the command fails, nothing on the volume changes
# Called by `make test`; run from the project root.

TMP=samples/cli_integrity_tmp
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

# ---- reserved files ----
for level in 2 5; do
    img="$TMP/r$level.img"
    $INIT "$img" 16M --structure=$level --label=RESV >/dev/null 2>&1
    before=$(md5sum < "$img")
    listing=$($ODS2 "$img" 'DIR [000000]' 2>&1)
    bad=0
    for name in INDEXF.SYS BITMAP.SYS BADBLK.SYS 000000.DIR CORIMG.SYS VOLSET.SYS CONTIN.SYS BACKUP.SYS BADLOG.SYS SECURITY.SYS; do
        for ver in "" ";1" ";*"; do
            out=$($ODS2 "$img" "DELETE [000000]$name$ver" 2>&1)
            rc=$?
            case "$out" in *reserved*) msg=1 ;; *) msg=0 ;; esac
            if [ $rc -ne 1 ] || [ $msg -ne 1 ]; then
                echo "      DELETE $name$ver (ODS-$level): exit $rc: $out"
                bad=$((bad + 1))
            fi
        done
    done
    check "ODS-$level: DELETE of each reserved file (bare, ;1, ;*) fails with a message saying it is reserved" 0 "$bad"
    check "ODS-$level: the image is byte for byte unchanged after those refusals" "$before" "$(md5sum < "$img")"
    check "ODS-$level: DIR [000000] is unchanged" "$listing" "$($ODS2 "$img" 'DIR [000000]' 2>&1)"
    check "ODS-$level: the independent structure check finds nothing wrong" "0 error(s), 0 warning(s)" \
        "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    printf 'x\n' > "$TMP/u.txt"
    $ODS2 "$img" "COPY $TMP/u.txt [000000]USER.TXT" >/dev/null 2>&1
    check "ODS-$level: an ordinary file in [000000] still deletes" "%ODS2-I-DELETED, deleted USER.TXT;1" \
        "$($ODS2 "$img" 'DELETE [000000]USER.TXT' 2>&1)"
done

# ---- the checker does not hide a half-done delete: a header marked for delete (FID and checksum intact)
# that a directory still lists is an error; unlisted it is not (the VMS-made AXP1 system disk has 9 such headers)
for level in 2 5; do
    img="$TMP/m$level.img"
    $INIT "$img" 8192 --structure=$level --label=MARKED >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    $ODS2 "$img" "COPY $TMP/u.txt [000000]MARKME.TXT" >/dev/null 2>&1
    fnum=$($ODS2 "$img" 'DIR/FULL [000000]MARKME.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I - "$img" "$fnum" <<'PY'
import struct, sys
img, fnum = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]   # INDEXF header LBN + IBMAPSIZE
f.seek((base + fnum - 1) * 512); h = bytearray(f.read(512))
fc = struct.unpack_from('<I', h, 52)[0] | 0x8000
struct.pack_into('<I', h, 52, fc)
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek((base + fnum - 1) * 512); f.write(h)
PY
    out=$(python3 -I tests/ods5check.py "$img" | grep "^ERROR")
    case "$out" in *"marked for delete (a half-done delete)"*) r=reported ;; *) r="not reported: $out" ;; esac
    check "ODS-$level: a header marked for delete that a directory still lists is an error in the checker" reported "$r"
    $ODS2 "$img" 'DELETE [000000]MARKME.TXT' >/dev/null 2>&1
    check "ODS-$level: DELETE takes the entry of a marked file out (it reads as gone) and the checker is clean" \
        "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
done

# ---- a damaged directory block is not written to (review 6, finding 3): a name count that runs past the block
for level in 2 5; do
    img="$TMP/d$level.img"
    $INIT "$img" 8192 --structure=$level --label=DAMAGED >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/u.txt [D]ONE.TXT" >/dev/null 2>&1
    fnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I - "$img" "$fnum" <<'PY'
import struct, sys
img, fnum = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
f.seek((base + fnum - 1) * 512); h = f.read(512)
mp = h[1] * 2
w0, w1 = struct.unpack_from('<HH', h, mp)
lbn = (((w0 >> 8) & 0x3f) << 16) | w1
f.seek(lbn * 512); b = bytearray(f.read(512))
b[5] = 255            # the first record's name count: 255 bytes of name in a short record
f.seek(lbn * 512); f.write(b)
PY
    before=$(md5sum < "$img")
    out=$($ODS2 "$img" "COPY $TMP/u.txt [D]TWO.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: COPY into a directory with a damaged block fails (exit 1) and says so" "1 damaged" "$rc $msg"
    out=$($ODS2 "$img" "DELETE [D]ONE.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: DELETE in a directory with a damaged block fails (exit 1) and says so" "1 damaged" "$rc $msg"
    check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"
done

# ---- a damaged header is not rewritten (review 6, finding 4): IDOFFSET 250 puts the ident stores past the block
# (the checksum is refreshed, so only the offsets are wrong; the old end-of-file makes the directory be stored);
# a file header with a wrong checksum is not marked for delete with a fresh one
for level in 2 5; do
    img="$TMP/h$level.img"
    $INIT "$img" 8192 --structure=$level --label=HDRBAD >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/u.txt [D]ONE.TXT" >/dev/null 2>&1
    dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    fnum=$($ODS2 "$img" 'DIR/FULL [D]ONE.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    cat > "$TMP/patch.py" <<'PY'
import struct, sys
img, fnum, what = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
pos = (base + fnum - 1) * 512
f.seek(pos); h = bytearray(f.read(512))
if what == 'idoffset':
    h[0] = 250
    h[28:32] = bytes([0, 0, 1, 0]); h[32:34] = bytes([2, 0])      # EFBLK 1, FFBYTE 2: the old end-of-file
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
elif what == 'extension':
    struct.pack_into('<HH', h, 14, 20, 1)                            # EXT_FID (20,1): an extension header
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
elif what == 'maplbn':
    mp = h[1] * 2
    struct.pack_into('<H', h, mp + 2, 0xFFFF)                        # the directory's first extent: far beyond the volume
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
else:
    h[510] ^= 0x55                                                   # a wrong checksum
f.seek(pos); f.write(h)
PY
    python3 -I "$TMP/patch.py" "$img" "$dnum" idoffset
    before=$(md5sum < "$img")
    out=$($ODS2 "$img" "COPY $TMP/u.txt [D]TWO.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: COPY into a directory whose header has a bad IDOFFSET fails (exit 1) and says so" "1 damaged" "$rc $msg"
    out=$($ODS2 "$img" "DELETE [D]ONE.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: DELETE in it fails (exit 1) and says so" "1 damaged" "$rc $msg"
    out=$($ODS2 "$img" "CREATE/DIRECTORY [D.SUB]" 2>&1); rc=$?
    check "ODS-$level: CREATE/DIRECTORY in it fails (exit 1)" 1 "$rc"
    check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"

    # a directory with an extension header is not written to (review 6, finding 5)
    img="$TMP/e$level.img"
    $INIT "$img" 8192 --structure=$level --label=EXTHDR >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/u.txt [D]ONE.TXT" >/dev/null 2>&1
    dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I "$TMP/patch.py" "$img" "$dnum" extension
    before=$(md5sum < "$img")
    for cmd in "COPY $TMP/u.txt [D]TWO.TXT" "DELETE [D]ONE.TXT" "CREATE/DIRECTORY [D.SUB]"; do
        out=$($ODS2 "$img" "$cmd" 2>&1); rc=$?
        case "$out" in *"extension header"*) msg="refused, extension header" ;; *) msg="$out" ;; esac
        check "ODS-$level: $cmd, directory with an extension header: refused" "1 refused, extension header" "$rc $msg"
    done
    check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"

    # a directory that cannot be read is not "no such name" (review 6, finding 8): no duplicate entry is made
    img="$TMP/n$level.img"
    $INIT "$img" 8192 --structure=$level --label=NOREAD >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/u.txt [D]ONE.TXT" >/dev/null 2>&1
    dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I "$TMP/patch.py" "$img" "$dnum" maplbn
    before=$(md5sum < "$img")
    for cmd in "COPY $TMP/u.txt [D]ONE.TXT" "COPY $TMP/u.txt [D]TWO.TXT" "CREATE/DIRECTORY [D.SUB]"; do
        out=$($ODS2 "$img" "$cmd" 2>&1); rc=$?
        case "$out" in *"already exists"*) msg="said it exists: $out" ;; *) msg=refused ;; esac
        check "ODS-$level: $cmd, directory unreadable: exit 1, and not taken for a free name" "1 refused" "$rc $msg"
    done
    check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"

    img="$TMP/g$level.img"
    $INIT "$img" 8192 --structure=$level --label=HDRBAD >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/u.txt [000000]ONE.TXT" >/dev/null 2>&1
    fnum=$($ODS2 "$img" 'DIR/FULL [000000]ONE.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I "$TMP/patch.py" "$img" "$fnum" checksum
    before=$(md5sum < "$img")
    out=$($ODS2 "$img" "DELETE [000000]ONE.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: DELETE of a file whose header has a bad checksum fails (exit 1) and says so" "1 damaged" "$rc $msg"
    check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"
done

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_ods5_integrity_check: $fails FAILED"
    exit 1
fi
echo "cli_ods5_integrity_check: all checks passed"
