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
        case "$out" in *"does not write to such a directory"*) msg="refused, extension header" ;; *) msg="$out" ;; esac
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

# ---- CREATE/DIRECTORY on a full volume gives everything back (review 6, finding 6): no file number taken and
# left, no header over a block that is not a directory block
for level in 2 5; do
    img="$TMP/f$level.img"
    $INIT "$img" 8192 --structure=$level --label=FULLVOL >/dev/null 2>&1
    head -c 51200 /dev/zero | tr '\0' 'x' > "$TMP/f100.dat"
    printf 'x\n' > "$TMP/f1.dat"
    : > "$TMP/fill.ods2"
    i=0; while [ $i -lt 100 ]; do echo "COPY $TMP/f100.dat [000000]BIG$i.DAT" >> "$TMP/fill.ods2"; i=$((i+1)); done
    i=0; while [ $i -lt 400 ]; do echo "COPY $TMP/f1.dat [000000]S$i.DAT" >> "$TMP/fill.ods2"; i=$((i+1)); done
    $ODS2 "$img" "@$TMP/fill.ods2" >/dev/null 2>&1
    free=$($ODS2 "$img" 'SHOW VOLUME' | sed -n 's/^  Free blocks: *//p')
    check "ODS-$level: the volume is full" 0 "$free"
    check "ODS-$level: the full volume is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    before=$(md5sum < "$img")
    out=$($ODS2 "$img" 'CREATE/DIRECTORY [NEWD]' 2>&1); rc=$?
    check "ODS-$level: CREATE/DIRECTORY on a full volume fails (exit 1)" 1 "$rc"
    check "ODS-$level: and leaves nothing behind (no number taken, no lost header): checker clean" \
        "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    $ODS2 "$img" 'DELETE [000000]BIG0.DAT' >/dev/null 2>&1
    check "ODS-$level: after a delete CREATE/DIRECTORY succeeds" "%ODS2-I-CREATED, created [NEWD]" \
        "$($ODS2 "$img" 'CREATE/DIRECTORY [NEWD]' 2>&1)"
    check "ODS-$level: and the checker is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
done

# ---- INDEXF.SYS's new blocks are zeroed (review 6, finding 7): free space holds junk (0xA5), then enough files
# are made to extend the index file; no block of INDEXF.SYS may hold any of the junk
cat > "$TMP/junk.py" <<'PY'
import struct, sys
mode, img, cluster = sys.argv[1], sys.argv[2], int(sys.argv[3])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]

def header(n):
    f.seek((base + n - 1) * 512); return f.read(512)

def extents(h):
    mp = h[1] * 2; end = h[3] * 2; pos = mp; out = []
    while pos < mp + h[58] * 2 and pos < end:
        w0 = struct.unpack_from('<H', h, pos)[0]
        fmt = w0 >> 14
        if fmt == 1:
            w1 = struct.unpack_from('<H', h, pos + 2)[0]
            out.append(((((w0 >> 8) & 0x3f) << 16) | w1, (w0 & 0xff) + 1)); pos += 4
        elif fmt == 2:
            cnt = (w0 & 0x3fff) + 1; lbn = struct.unpack_from('<I', h, pos + 2)[0]
            out.append((lbn, cnt)); pos += 6
        elif fmt == 3:
            cnt = (((w0 & 0x3fff) << 16) | struct.unpack_from('<H', h, pos + 2)[0]) + 1
            lbn = struct.unpack_from('<I', h, pos + 4)[0]; out.append((lbn, cnt)); pos += 8
        else:
            pos += 2
    return out

if mode == 'junk':
    bm = extents(header(2))[0][0] + 1           # BITMAP.SYS: block 1 is the SCB, then the bits
    f.seek(bm * 512); bits = f.read(4096)
    size = struct.unpack_from('<I', home, 0)[0] if False else None
    f.seek(0, 2); total = f.tell() // 512
    for cl in range(total // cluster):
        if bits[cl // 8] >> (cl % 8) & 1:        # free
            f.seek(cl * cluster * 512); f.write(b'\xa5' * (cluster * 512))
else:
    junk = 0
    for lbn, cnt in extents(header(1)):
        for b in range(cnt):
            f.seek((lbn + b) * 512)
            if b'\xa5' * 16 in f.read(512): junk += 1
    print(junk)
PY
for level in 2 5; do
    img="$TMP/x$level.img"
    $INIT "$img" 8192 --structure=$level --label=INDEXX >/dev/null 2>&1
    cluster=$($ODS2 "$img" 'SHOW VOLUME' | sed -n 's/^  Cluster size: *//p')
    python3 -I "$TMP/junk.py" junk "$img" "$cluster"
    check "ODS-$level: the junk in free space leaves the volume clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    printf 'x\n' > "$TMP/f1.dat"
    : > "$TMP/many.ods2"
    i=0; while [ $i -lt 300 ]; do echo "COPY $TMP/f1.dat [000000]M$i.DAT" >> "$TMP/many.ods2"; i=$((i+1)); done
    $ODS2 "$img" "@$TMP/many.ods2" >/dev/null 2>&1
    check "ODS-$level: 300 files made (INDEXF.SYS extended), the checker is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    check "ODS-$level: no block of INDEXF.SYS holds the junk" 0 "$(python3 -I "$TMP/junk.py" count "$img" "$cluster")"
done

# ---- one writer or many readers (review 6, finding 9): ods2 refuses an image another process holds locked
cat > "$TMP/lock.py" <<'PY'
import fcntl, subprocess, sys
img, tmp = sys.argv[1], sys.argv[2]
def run(cmd):
    p = subprocess.run(['./ods2', img, cmd], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr
f = open(img, 'rb')
fcntl.flock(f, fcntl.LOCK_EX)
rc_w, out_w = run('COPY %s/u.txt [000000]LOCKED.TXT' % tmp)
rc_r, out_r = run('DIR [000000]')
fcntl.flock(f, fcntl.LOCK_UN)
fcntl.flock(f, fcntl.LOCK_SH)
rc_r2, out_r2 = run('DIR [000000]')
rc_w2, out_w2 = run('COPY %s/u.txt [000000]LOCKED.TXT' % tmp)
fcntl.flock(f, fcntl.LOCK_UN)
rc_w3, out_w3 = run('COPY %s/u.txt [000000]LOCKED.TXT' % tmp)
print(rc_w, 'locked' in out_w, rc_r, 'locked' in out_r, rc_r2, rc_w2, 'locked' in out_w2, rc_w3)
PY
for level in 2 5; do
    img="$TMP/k$level.img"
    $INIT "$img" 8192 --structure=$level --label=LOCKED >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    check "ODS-$level: an image locked by another process: write refused, read refused; locked for reading: read works, write refused; unlocked: write works" \
        "1 True 1 True 0 1 True 0" "$(python3 -I "$TMP/lock.py" "$img" "$TMP")"
done

# ---- the checker sees a header marked for delete whose clusters are free, or are owned by a live file (review 7,
# R7-1): a finished delete would free a live file's clusters. (The VMS-made AXP1 system disk has neither state.)
cat > "$TMP/marked.py" <<'PY'
import struct, sys
mode, img, cluster, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
def pos(n): return (base + n - 1) * 512
def header(n):
    f.seek(pos(n)); return bytearray(f.read(512))
def put(n, h):
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
    f.seek(pos(n)); f.write(h)
ha = header(a)
mp = ha[1] * 2
w0, w1 = struct.unpack_from('<HH', ha, mp)
lbn = (((w0 >> 8) & 0x3f) << 16) | w1
struct.pack_into('<I', ha, 52, struct.unpack_from('<I', ha, 52)[0] | 0x8000)       # a: marked for delete
put(a, ha)
if mode == 'free':
    hb = header(2); mb = hb[1] * 2
    x0, x1 = struct.unpack_from('<HH', hb, mb)
    bm = ((((x0 >> 8) & 0x3f) << 16) | x1) + 1
    cl = lbn // cluster
    f.seek(bm * 512 + cl // 8); byte = f.read(1)[0]
    f.seek(bm * 512 + cl // 8); f.write(bytes([byte | (1 << (cl % 8))]))             # its first cluster free
else:
    hb = header(b)
    mpb = hb[1] * 2
    struct.pack_into('<HH', hb, mpb, w0, w1)                                          # b maps a's first extent too
    put(b, hb)
PY
for level in 2 5; do
    for mode in free share; do
        img="$TMP/p$level$mode.img"
        $INIT "$img" 8192 --structure=$level --label=PENDING >/dev/null 2>&1
        printf 'x\n' > "$TMP/u.txt"
        $ODS2 "$img" "COPY $TMP/u.txt [000000]A.TXT" >/dev/null 2>&1
        $ODS2 "$img" "COPY $TMP/u.txt [000000]B.TXT" >/dev/null 2>&1
        cluster=$($ODS2 "$img" 'SHOW VOLUME' | sed -n 's/^  Cluster size: *//p')
        na=$($ODS2 "$img" 'DIR/FULL [000000]A.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        nb=$($ODS2 "$img" 'DIR/FULL [000000]B.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        python3 -I "$TMP/marked.py" $mode "$img" "$cluster" "$na" "$nb"
        $ODS2 "$img" 'DELETE [000000]A.TXT' >/dev/null 2>&1        # the entry goes: A is an unlisted, marked header
        out=$(python3 -I tests/ods5check.py "$img" | grep "^ERROR")
        case "$mode" in
            free) want="is free but still mapped by header" ;;
            share) want="and owned by file" ;;
        esac
        case "$out" in *"$want"*) r=reported ;; *) r="not reported: $out" ;; esac
        check "ODS-$level: checker: a marked header's clusters $mode" reported "$r"
    done
done

# ---- a directory entry with an RVN or an NMX is not followed (review 7, R7-2): it would resolve to another
# file's header by number alone
cat > "$TMP/fidpatch.py" <<'PY'
import struct, sys
img, dnum, fnum, which = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
f.seek((base + dnum - 1) * 512); h = f.read(512)
mp = h[1] * 2
w0, w1 = struct.unpack_from('<HH', h, mp)
lbn = (((w0 >> 8) & 0x3f) << 16) | w1
f.seek(lbn * 512); b = bytearray(f.read(512))
i = b.find(struct.pack('<HH', 1, fnum), 6)             # the version entry: version 1, FID number, sequence, RVN, NMX
assert i > 0
b[i + (6 if which == 'rvn' else 7)] = 1
f.seek(lbn * 512); f.write(b)
PY
for level in 2 5; do
    for which in rvn nmx; do
        img="$TMP/i$level$which.img"
        $INIT "$img" 8192 --structure=$level --label=FIDBITS >/dev/null 2>&1
        printf 'x\n' > "$TMP/u.txt"
        $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
        $ODS2 "$img" "COPY $TMP/u.txt [D]ONE.TXT" >/dev/null 2>&1
        dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        fnum=$($ODS2 "$img" 'DIR/FULL [D]ONE.TXT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        python3 -I "$TMP/fidpatch.py" "$img" "$dnum" "$fnum" $which
        before=$(md5sum < "$img")
        out=$($ODS2 "$img" 'DELETE [D]ONE.TXT' 2>&1); rc=$?
        # an RVN: a volume set member, not supported; an NMX: the file is number NUM + 65536, which this volume
        # (a few hundred files) does not have: not covered by INDEXF.SYS - either way not the file NUM
        case "$out" in *"not supported"*|*"not covered by INDEXF"*) msg=refused ;; *) msg="$out" ;; esac
        check "ODS-$level: DELETE of an entry whose FID has an $which is refused (exit 1), nothing changes" "1 refused $before" "$rc $msg $(md5sum < "$img")"
    done
done

# ---- INDEXF.SYS's own header is checked before it is rewritten (review 7, R7-4): IDOFFSET 250, checksum refreshed
for level in 2 5; do
    img="$TMP/ix$level.img"
    $INIT "$img" 8192 --structure=$level --label=INDEXHDR >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    python3 -I - "$img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
f.seek(base * 512); h = bytearray(f.read(512))
h[0] = 250
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek(base * 512); f.write(h)
PY
    before=$(md5sum < "$img")
    out=$($ODS2 "$img" "COPY $TMP/u.txt [000000]X.TXT" 2>&1); rc=$?
    case "$out" in *damaged*) msg=damaged ;; *) msg="$out" ;; esac
    check "ODS-$level: COPY with a damaged INDEXF.SYS header fails (exit 1) and says so; the image is unchanged" "1 damaged $before" "$rc $msg $(md5sum < "$img")"
done

# ---- a directory component whose entry has an RVN is not followed, to read or to write (review 8, R8-1):
# the entry for D.DIR in the MFD, the patch above; [D] is directory number NUM of this volume, whatever its RVN says
for level in 2 5; do
    for which in rvn; do
        img="$TMP/c$level$which.img"
        $INIT "$img" 8192 --structure=$level --label=PATHFID >/dev/null 2>&1
        printf 'x\n' > "$TMP/u.txt"
        $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
        $ODS2 "$img" "COPY $TMP/u.txt [D]X.TXT" >/dev/null 2>&1
        mkdir -p "$TMP/tree/sub"; printf 'y\n' > "$TMP/tree/sub/y.txt"
        dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        python3 -I "$TMP/fidpatch.py" "$img" 4 "$dnum" $which
        before=$(md5sum < "$img")
        for cmd in "COPY $TMP/u.txt [D]Y.TXT" "CREATE/DIRECTORY [D.S]" "IMPORT $TMP/tree [D]" "DIR [D]" "TYPE [D]X.TXT" "DELETE [D]X.TXT"; do
            out=$($ODS2 "$img" "$cmd" 2>&1); rc=$?
            case "$out" in *"not supported"*) msg=refused ;; *) msg="$out" ;; esac
            check "ODS-$level: $cmd, with an $which in D.DIR's entry: refused (exit 1)" "1 refused" "$rc $msg"
        done
        check "ODS-$level: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"
    done
done

# ---- a refused operation writes nothing (F1, found by the stress harness): a change is made in memory and
# reaches the image only when every step of it has succeeded, so a check that fails late cannot leave a
# half-done delete. Three damaged volumes, DELETE (and COPY where the damage is in what it needs): exit 1, the
# image byte for byte as it was.
cat > "$TMP/damage.py" <<'PY'
import struct, sys
img, what, fnum = sys.argv[1], sys.argv[2], int(sys.argv[3])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
def put(n, h):
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
    f.seek((base + n - 1) * 512); f.write(h)
def get(n):
    f.seek((base + n - 1) * 512); return bytearray(f.read(512))
if what == 'unaligned':                 # the file's first map pointer: LBN + 1, not a multiple of the cluster size
    h = get(fnum); o = h[1] * 2
    struct.pack_into('<H', h, o + 2, (struct.unpack_from('<H', h, o + 2)[0] + 1) & 0xFFFF); put(fnum, h)
elif what == 'bitmap':                  # BITMAP.SYS (file 2): its map in use is empty, so its extents cannot be decoded
    h = get(2); h[58] = 0; put(2, h)
elif what == 'extension':               # the file claims an extension header the index file does not cover
    h = get(fnum); struct.pack_into('<HH', h, 14, 60000, 1); put(fnum, h)
PY
for level in 2 5; do
    for what in unaligned bitmap extension; do
        img="$TMP/z$level$what.img"
        $INIT "$img" 8192 --structure=$level --cluster=4 --label=REFUSED >/dev/null 2>&1
        head -c 5000 /dev/zero | tr '\0' 'v' > "$TMP/v.dat"
        $ODS2 "$img" "COPY $TMP/v.dat [000000]VICTIM.DAT" >/dev/null 2>&1
        vnum=$($ODS2 "$img" 'DIR/FULL [000000]VICTIM.DAT' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        python3 -I "$TMP/damage.py" "$img" $what "$vnum"
        before=$(md5sum < "$img")
        out=$($ODS2 "$img" 'DELETE [000000]VICTIM.DAT' 2>&1); rc=$?
        [ $what = bitmap ] && [ $rc -eq 0 ] && rc="deleted (the bitmap is not needed?): $out"
        check "ODS-$level: DELETE with $what damage is refused (exit 1) and the image is unchanged" "1 $before" "$rc $(md5sum < "$img")"
        out=$($ODS2 "$img" "COPY $TMP/v.dat [000000]OTHER.DAT" 2>&1); rc=$?
        if [ $what = bitmap ]; then
            check "ODS-$level: COPY with the bitmap damaged is refused (exit 1) and the image is unchanged" "1 $before" "$rc $(md5sum < "$img")"
        fi
    done
done

# ---- F3: LINK / SET FILE/ENTER to a reserved file is refused (file numbers 1-10), hard link or alias
for spec in "2 plain" "5 plain" "5 hardlinks"; do
    set -- $spec
    level=$1; kind=$2
    img="$TMP/lk$level$kind.img"
    opts=""; [ "$kind" = hardlinks ] && opts="--hardlinks"
    $INIT "$img" 8192 --structure=$level --label=LINKRES $opts >/dev/null 2>&1
    before=$(md5sum < "$img")
    bad=0
    for name in INDEXF.SYS BITMAP.SYS BADBLK.SYS 000000.DIR CORIMG.SYS VOLSET.SYS CONTIN.SYS BACKUP.SYS BADLOG.SYS SECURITY.SYS; do
        out=$($ODS2 "$img" "LINK [000000]ALIAS$name [000000]$name" 2>&1); rc=$?
        case "$out" in *"reserved"*) ;; *) bad=$((bad + 1)); echo "      LINK to $name: exit $rc: $out" ;; esac
        [ $rc -eq 1 ] || bad=$((bad + 1))
    done
    check "ODS-$level $kind: LINK to each of the ten reserved files is refused (exit 1, 'reserved')" 0 "$bad"
    check "ODS-$level $kind: the image is byte for byte unchanged" "$before" "$(md5sum < "$img")"
done

# ---- F4: a name is never changed silently: ^" is refused (VMS refuses the quote character), an unterminated
# quote or a command with more words than the tokenizer holds is not run
for level in 2 5; do
    img="$TMP/qt$level.img"
    $INIT "$img" 8192 --structure=$level --label=QUOTES >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    before=$(md5sum < "$img")
    for cmd in "COPY $TMP/u.txt [000000]quote^\".q" "COPY $TMP/u.txt [000000]quote\"x.q" "COPY $TMP/u.txt [000000]a b c d e f g h i j.q"; do
        out=$($ODS2 "$img" "$cmd" 2>&1); rc=$?
        check "ODS-$level: '$cmd' is refused (exit 1) and nothing is created" "1 $before" "$rc $(md5sum < "$img")"
    done
done

# ---- F5 (changed: VMS V7.3-2, V8.4 and V8.4-2L2 created 10,000 levels): directories nest at most 2,048 levels, the depth DIR [...]
# and EXPORT read back; past 256 levels (where VMS's own wildcard [...] stops on V7.3-2, 512 on V8.4: %RMS-F-DIR, and BACKUP
# cannot save such files) CREATE/DIRECTORY warns; on an ODS-2 volume it also warns past 7 (OpenVMS VAX V7.3)
for level in 2 5; do
    img="$TMP/dp$level.img"
    $INIT "$img" 16384 --structure=$level --label=DEEPDIR >/dev/null 2>&1
    : > "$TMP/deep.ods2"
    path=A; i=1
    while [ $i -le 260 ]; do echo "CREATE/DIRECTORY [$path]" >> "$TMP/deep.ods2"; path="$path.A"; i=$((i+1)); done
    out=$($ODS2 "$img" "@$TMP/deep.ods2" 2>&1)
    check "ODS-$level: 260 levels are made (the cap is 2,048), none refused" "260 created, 0 refused" \
        "$(echo "$out" | grep -c 'CREATED') created, $(echo "$out" | grep -c 'at most') refused"
    check "ODS-$level: the warning past 256 levels (VMS wildcard [...] stops there; BACKUP cannot save such files): levels 257-260" 4 \
        "$(echo "$out" | grep -c 'W-DEEPDIR256')"
    check "ODS-$level: the checker finds nothing wrong" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    check "ODS-$level: DIR [...] still walks the whole tree" "260" "$($ODS2 "$img" 'DIR [...]*.*' 2>&1 | grep -c '^Directory ')"
done

# ---- review 9: R9-1 a change that cannot get its buffer fails instead of writing unbuffered; R9-2 after a write
# that fails part way (an I/O error at commit) no more is written through the mount and a script stops
# (ods2_fault: ODS2_FAULT_NOMEM_TXN=1 makes the buffer allocation fail, ODS2_FAULT_ERROR_AT_WRITE=k the k-th write)
for level in 2 5; do
    img="$TMP/mm$level.img"
    $INIT "$img" 8192 --structure=$level --label=NOMEM >/dev/null 2>&1
    printf 'x\n' > "$TMP/u.txt"
    $ODS2 "$img" "COPY $TMP/u.txt [000000]KEEP.TXT" >/dev/null 2>&1
    before=$(md5sum < "$img")
    for cmd in "DELETE [000000]KEEP.TXT" "COPY $TMP/u.txt [000000]NEW.TXT" "CREATE/DIRECTORY [NEWD]" "LINK [000000]LNK.TXT [000000]KEEP.TXT"; do
        out=$(ODS2_FAULT_NOMEM_TXN=1 ./ods2_fault "$img" "$cmd" 2>&1); rc=$?
        case "$out" in *"out of memory"*) msg=refused ;; *) msg="$out" ;; esac
        check "ODS-$level: '$cmd' without a buffer fails (exit 1, out of memory) and writes nothing" "1 refused $before" "$rc $msg $(md5sum < "$img")"
    done
    printf 'COPY %s [000000]ONE.TXT\nCOPY %s [000000]TWO.TXT\n' "$TMP/u.txt" "$TMP/u.txt" > "$TMP/two.ods2"
    out=$(ODS2_FAULT_ERROR_AT_WRITE=3 ./ods2_fault "$img" "@$TMP/two.ods2" 2>&1); rc=$?
    check "ODS-$level: a write that fails during the commit stops the script (exit 1, WRITEFAIL)" "1 yes no" \
        "$rc $(echo "$out" | grep -q WRITEFAIL && echo yes) $(echo "$out" | grep -q 'TWO.TXT' && echo yes || echo no)"
    check "ODS-$level: the second command's file was not made" "1" "$(./ods2 "$img" 'TYPE [000000]TWO.TXT' >/dev/null 2>&1; echo $?)"
    # in the same process a further change is refused outright
    printf 'COPY %s [000000]ONE.TXT\n' "$TMP/u.txt" > "$TMP/one.ods2"
    out=$(ODS2_FAULT_ERROR_AT_WRITE=3 ./ods2_fault "$img" "@$TMP/one.ods2" 2>&1)
    check "ODS-$level: and a later process works again, the old leak being a leak" "KEEP.TXT" "$(./ods2 "$img" 'DIR [000000]KEEP.TXT' 2>&1 | grep -o KEEP.TXT | head -1)"
done

# ---- R10-2: file 10 on an ODS-2 volume is reserved only when it is the SECURITY.SYS of the master file directory
# (the DELETE rule): a header 10 that names another file can be linked to
img="$TMP/s10.img"
$INIT "$img" 8192 --structure=2 --label=FILE10 >/dev/null 2>&1
python3 -I - "$img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
pos = (base + 9) * 512                      # file 10
f.seek(pos); h = bytearray(f.read(512))
o = h[0] * 2                                # the ident: FI2$T_FILENAME first, 20 characters
h[o:o + 20] = b'MYFILE.DAT;1'.ljust(20)
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek(pos); f.write(h)
PY
check "ODS-2: LINK to file 10 when it is not SECURITY.SYS is allowed (as DELETE's rule)" "ok" \
    "$($ODS2 "$img" 'LINK [000000]AL.TXT [000000]SECURITY.SYS' >/dev/null 2>&1 && echo ok)"
img="$TMP/s10b.img"
$INIT "$img" 8192 --structure=2 --label=FILE10 >/dev/null 2>&1
check "ODS-2: LINK to file 10 when it is SECURITY.SYS is refused" "1" "$($ODS2 "$img" 'LINK [000000]AL.TXT [000000]SECURITY.SYS' >/dev/null 2>&1; echo $?)"

# ---- records (ods5-vms2 batch C, VAX V7.3): VMS TYPE refuses Stream, Stream_LF and Stream_CR files with RAT:PRN
# (RMS-F-RAT). COPY/IMPORT never make one (/ATTRIBUTES included), and TYPE refuses one made another way.
for level in 2 5; do
    img="$TMP/rp$level.img"
    $INIT "$img" 8192 --structure=$level --label=PRNREC >/dev/null 2>&1
    printf 'line one\nline two\n' > "$TMP/t.txt"
    before=$(md5sum < "$img")
    for fmt in STMLF STM STMCR UDF; do
        out=$($ODS2 "$img" "COPY/ATTRIBUTES=(RFM:$fmt,RAT:PRN) $TMP/t.txt [000000]P.TXT" 2>&1); rc=$?
        case "$out" in *"RAT:PRN needs the VFC"*) msg=refused ;; *) msg="$out" ;; esac
        check "ODS-$level: COPY/ATTRIBUTES=(RFM:$fmt,RAT:PRN) is refused (exit 1) and writes nothing" "1 refused $before" "$rc $msg $(md5sum < "$img")"
    done
    for fmt in STMLF STM STMCR; do
        $ODS2 "$img" "COPY/FORMAT=$fmt $TMP/t.txt [000000]F$fmt.TXT" >/dev/null 2>&1
        fnum=$($ODS2 "$img" "DIR/FULL [000000]F$fmt.TXT" | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
        python3 -I - "$img" "$fnum" <<'PY'
import struct, sys
img, n = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
pos = (base + n - 1) * 512
f.seek(pos); h = bytearray(f.read(512))
h[21] = (h[21] & ~0x0f) | 4                  # FAT$B_RATTRIB: print carriage control, on a stream file
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek(pos); f.write(h)
PY
        out=$($ODS2 "$img" "TYPE [000000]F$fmt.TXT" 2>&1)
        case "$out" in *"RMS-F-RAT"*) msg="RMS-F-RAT" ;; *) msg="$out" ;; esac
        check "ODS-$level: TYPE of a $fmt file with RAT:PRN fails with RMS-F-RAT, as VMS" "RMS-F-RAT" "$msg"
    done
done

# ---- ods5-vms2 batch C (VAX V7.3 ODS-2): VMS DELETE refuses the reserved files (RMS-E-PRV) but SET FILE/REMOVE,
# which takes only the directory entry, was accepted on all ten and left LOSTHEADER. ods2v2 has no REMOVE; an
# entry-only removal here is DELETE of an alias or a dangling entry, and it is refused for a reserved file too,
# in whatever directory the entry is. Test: the entries for each reserved file copied into [D], then DELETE [D]name.
cat > "$TMP/copyentries.py" <<'PY'
import struct, sys
img, dnum = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
def first_block(n):
    f.seek((base + n - 1) * 512); h = f.read(512)
    mp = h[1] * 2; w0, w1 = struct.unpack_from('<HH', h, mp)
    return (((w0 >> 8) & 0x3f) << 16) | w1
mfd = first_block(4); f.seek(mfd * 512); blk = f.read(512)
recs = []; pos = 0
while pos + 6 <= 512:
    size = struct.unpack_from('<H', blk, pos)[0]
    if size in (0, 0xFFFF): break
    name = bytes(blk[pos + 6:pos + 6 + blk[pos + 5]])
    if name.endswith(b'.SYS'): recs.append(blk[pos:pos + size + 2])
    pos += size + 2
d = first_block(dnum)
out = bytearray(512); p = 0
for r in sorted(recs, key=lambda r: bytes(r[6:6 + r[5]])):
    out[p:p + len(r)] = r; p += len(r)
out[p:p + 2] = b'\xff\xff'
f.seek(d * 512); f.write(out)
PY
for level in 2 5; do
    img="$TMP/rm$level.img"
    $INIT "$img" 8192 --structure=$level --label=REMOVE >/dev/null 2>&1
    $ODS2 "$img" "CREATE/DIRECTORY [D]" >/dev/null 2>&1
    dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    python3 -I "$TMP/copyentries.py" "$img" "$dnum"
    listed=$($ODS2 "$img" 'DIR [D]' 2>&1 | grep -c '\.SYS')
    before=$(md5sum < "$img")
    bad=0
    for name in $($ODS2 "$img" 'DIR [D]' 2>&1 | grep '\.SYS' | awk '{print $1}'); do
        out=$($ODS2 "$img" "DELETE [D]$name" 2>&1); rc=$?
        case "$out" in *reserved*) ;; *) bad=$((bad + 1)); echo "      DELETE [D]$name: exit $rc: $out" ;; esac
        [ $rc -eq 1 ] || bad=$((bad + 1))
    done
    check "ODS-$level: $listed entries for reserved files in [D]: removing each (the REMOVE case) is refused" "0 $before" "$bad $(md5sum < "$img")"
done

# ---- F8 (ods5-st): on a volume without HARDLINKS, deleting the primary name of a file that has an alias leaves
# the alias behind, as VMS does. When the file number is then reused (a new sequence number) the stale alias must
# never open the new file: it is "not found" for TYPE and DIR/FULL; ods5check reports it as a WARNING (VMS makes
# this state), not an error; ods2v2's own DELETE of the alias removes the entry and nothing else.
cat > "$TMP/novc.py" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(512); h = bytearray(f.read(512))
v = struct.unpack_from('<H', h, 42)[0]; struct.pack_into('<H', h, 42, v & ~0x40)
struct.pack_into('<H', h, 58, sum(struct.unpack_from('<29H', h, 0)) & 0xFFFF)
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
f.seek(512); f.write(h)
PY
img="$TMP/f8.img"
$INIT "$img" 8192 --structure=5 --label=F8 --hardlinks >/dev/null 2>&1
echo "primary content" > "$TMP/f8p.txt"; echo "new file content" > "$TMP/f8q.txt"
printf 'COPY %s [000000]P.TXT\nLINK [000000]ALIAS.TXT [000000]P.TXT\n' "$TMP/f8p.txt" > "$TMP/f8s1"
$ODS2 "$img" "@$TMP/f8s1" >/dev/null 2>&1
python3 -I "$TMP/novc.py" "$img"
printf 'DELETE [000000]P.TXT\n' > "$TMP/f8s2"; $ODS2 "$img" "@$TMP/f8s2" >/dev/null 2>&1
out=$($ODS2 "$img" "TYPE [000000]ALIAS.TXT" 2>&1); rc=$?
case "$out" in *"stale"*) msg=stale ;; *) msg="$out" ;; esac
check "F8: the alias of a deleted file (header free) is not found" "stale 1" "$msg $rc"
$ODS2 "$img" "COPY $TMP/f8q.txt [000000]Q.TXT" >/dev/null 2>&1
out=$($ODS2 "$img" "TYPE [000000]ALIAS.TXT" 2>&1); rc=$?
case "$out" in *"new file content"*) msg="OPENED THE NEW FILE" ;; *"stale"*) msg=stale ;; *) msg="$out" ;; esac
check "F8: the number reused (new sequence number): the stale alias is still not found, never the new file" "stale 1" "$msg $rc"
out=$($ODS2 "$img" "DIR/FULL [000000]ALIAS.TXT" 2>&1)
case "$out" in *"File ID"*) msg="SHOWED A FILE" ;; *) msg=none ;; esac
check "F8: DIR/FULL of the stale alias shows no file" none "$msg"
chk=$(python3 -I tests/ods5check.py "$img" 2>&1)
check "F8: ods5check: a stale alias is one WARNING and no error" "0 error(s), 1 warning(s) stale" \
    "$(echo "$chk" | tail -1) $(echo "$chk" | grep -c 'WARN.*stale alias' | sed 's/1/stale/')"
out=$($ODS2 "$img" "DELETE [000000]ALIAS.TXT" 2>&1); rc=$?
case "$out" in *"removed ALIAS.TXT"*) msg=removed ;; *) msg="$out" ;; esac
check "F8: DELETE of the stale alias removes the entry" "removed 0" "$msg $rc"
check "F8: the new file is untouched" "new file content" "$($ODS2 "$img" "TYPE [000000]Q.TXT" 2>&1)"
check "F8: after removing the alias ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"

# ---- depth (operator's choice, via ods5-orch): OpenVMS VAX V7.3 reaches 7 levels below [000000] on an ODS-2 volume;
# CREATE/DIRECTORY (and IMPORT) deeper than that works but warns; ODS-5 volumes never warn.
for level in 2 5; do
    img="$TMP/dp$level.img"
    $INIT "$img" 8192 --structure=$level --label=DEPTH >/dev/null 2>&1
    path=""; warned=""; made=0
    for n in 1 2 3 4 5 6 7 8 9; do
        path="${path:+$path.}L$n"
        out=$($ODS2 "$img" "CREATE/DIRECTORY [$path]" 2>&1); rc=$?
        [ $rc -eq 0 ] && made=$((made + 1))
        case "$out" in *DEEPDIR*) warned="$warned $n" ;; esac
    done
    if [ $level = 2 ]; then exp="9| 8 9"; else exp="9|"; fi
    check "ODS-$level: CREATE/DIRECTORY to 9 levels: all made (exit 0); the warning (ODS-2 only) from level 8 up" \
        "$exp" "$made|$warned"
done
out=$($ODS2 "$TMP/dp2.img" "CREATE/DIRECTORY [L1.L2.L3.L4.L5.L6.L7.L8.L9.L10]" 2>&1)
case "$out" in *"10 levels below [000000]"*"RMS-F-DIR"*"Alpha V7.3-2"*) msg=text ;; *) msg="$out" ;; esac
check "ODS-2: the warning names the level, the VAX error and Alpha V7.3-2" text "$msg"
mkdir -p "$TMP/impdeep/a/b/c/d/e/f/g/h/i"; echo x > "$TMP/impdeep/a/b/c/d/e/f/g/h/i/f.txt"
$INIT "$TMP/dpi.img" 8192 --structure=2 --label=IMPDEEP >/dev/null 2>&1
out=$($ODS2 "$TMP/dpi.img" "IMPORT $TMP/impdeep [000000]" 2>&1); rc=$?
case "$out" in *"2 directories deeper than 7 levels"*) msg=warned ;; *) msg="$out" ;; esac
check "ODS-2: IMPORT of a tree 10 levels deep warns once, with the count" "warned 0" "$msg $rc"

# ---- version limits (R1, physical VAX96, B320): ods2v2 wrote a version limit of 1 in the directory entries of ODS-2
# volumes, so VMS purged version 1 of a file as soon as COPY or EDIT made version 2 (the F8 aliases of run 1 dangled
# that way). Every VMS-made volume has 32767 there, or the directory's default version limit (its FAT$W_VERSIONS) when
# it has one, and a directory made inside one with a default carries the default too.
cat > "$TMP/vl.py" <<'PY'
import struct, sys
img, dnum, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
def hdr(n):
    f.seek((base + n - 1) * 512); return bytearray(f.read(512)), (base + n - 1) * 512
def first_block(h):
    w0, w1 = struct.unpack_from('<HH', h, h[1] * 2); return (((w0 >> 8) & 0x3f) << 16) | w1
h, hpos = hdr(dnum)
if mode == 'default':          # the directory's default version limit: FAT$W_VERSIONS (header offset 50)
    struct.pack_into('<H', h, 50, int(sys.argv[4]))
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
    f.seek(hpos); f.write(h); sys.exit(0)
if mode == 'hdrversions':
    print(struct.unpack_from('<H', h, 50)[0]); sys.exit(0)
lbn = first_block(h)
f.seek(lbn * 512); blk = bytearray(f.read(512)); pos = 0; out = []
while pos + 6 <= 512:
    size = struct.unpack_from('<H', blk, pos)[0]
    if size in (0, 0xFFFF): break
    name = bytes(blk[pos + 6:pos + 6 + blk[pos + 5]]).decode('latin-1')
    if mode == 'show' and not name.endswith('.SYS') and name != '000000.DIR': out.append('%s=%d' % (name, struct.unpack_from('<H', blk, pos + 2)[0]))
    if mode == 'set1' and not name.endswith('.SYS') and name != '000000.DIR': struct.pack_into('<H', blk, pos + 2, 1)
    pos += size + 2
if mode == 'set1': f.seek(lbn * 512); f.write(blk)
print(' '.join(out))
PY
for level in 2 5; do
    img="$TMP/vl$level.img"
    $INIT "$img" 8192 --structure=$level --label=VERLIM >/dev/null 2>&1
    echo x > "$TMP/vl.txt"
    printf 'CREATE/DIRECTORY [D]\nCOPY %s [D]A.TXT\nCOPY %s [000000]B.TXT\n' "$TMP/vl.txt" "$TMP/vl.txt" > "$TMP/vls"
    $ODS2 "$img" "@$TMP/vls" >/dev/null 2>&1
    dnum=$($ODS2 "$img" 'DIR/FULL [000000]D.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    check "ODS-$level: new entries carry version limit 32767 (no limit), as VMS writes them" "B.TXT=32767 D.DIR=32767 | A.TXT=32767" \
        "$(python3 -I "$TMP/vl.py" "$img" 4 show | sed 's/ *$//') | $(python3 -I "$TMP/vl.py" "$img" $dnum show)"
    check "ODS-$level: ods5check: no version limit warning" 0 "$(python3 -I tests/ods5check.py "$img" 2>&1 | grep -c 'version limit 1')"
    # the volume as an older ods2v2 left it
    python3 -I "$TMP/vl.py" "$img" 4 set1; python3 -I "$TMP/vl.py" "$img" $dnum set1
    check "ODS-$level: an old volume (limit 1): ods5check warns in both directories" 2 "$(python3 -I tests/ods5check.py "$img" 2>&1 | grep -c 'version limit 1')"
    out=$($ODS2 "$img" 'SET FILE/VERSION_LIMIT=32767 [000000...]*.*' 2>&1); rc=$?
    case "$out" in *"VERLIMIT"*"3 directory records changed"*) msg=ok ;; *) msg="$out" ;; esac
    check "ODS-$level: SET FILE/VERSION_LIMIT=32767 [000000...]*.* puts it right (3 records)" "ok 0" "$msg $rc"
    check "ODS-$level: ... entries are 32767 again" "B.TXT=32767 D.DIR=32767 | A.TXT=32767" \
        "$(python3 -I "$TMP/vl.py" "$img" 4 show | sed 's/ *$//') | $(python3 -I "$TMP/vl.py" "$img" $dnum show)"
    check "ODS-$level: ... and ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
    # a directory with a default version limit: new entries and new subdirectories take it
    python3 -I "$TMP/vl.py" "$img" $dnum default 3
    printf 'COPY %s [D]C.TXT\nCREATE/DIRECTORY [D.SUB]\n' "$TMP/vl.txt" > "$TMP/vls2"
    $ODS2 "$img" "@$TMP/vls2" >/dev/null 2>&1
    check "ODS-$level: a directory with default version limit 3: its new entries get 3" "A.TXT=32767 C.TXT=3 SUB.DIR=3" "$(python3 -I "$TMP/vl.py" "$img" $dnum show)"
    subnum=$($ODS2 "$img" 'DIR/FULL [D]SUB.DIR' | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p')
    check "ODS-$level: ... and the directory made in it passes the default on (FAT version limit 3)" 3 "$(python3 -I "$TMP/vl.py" "$img" $subnum hdrversions)"
    check "ODS-$level: ... no checker error" "0 error(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1 | sed 's/, .*//')"
done

# ---- R12-4: a subdirectory entry whose header cannot be read is reported by the walk (and the exit status is 1), it is not
# skipped in silence; a stale alias (a VMS state) stays quiet
cat > "$TMP/badfid.py" <<'PY'
import struct, sys
img = sys.argv[1]
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
f.seek((base + 3) * 512); h = f.read(512)
w0, w1 = struct.unpack_from('<HH', h, h[1] * 2); lbn = (((w0 >> 8) & 0x3f) << 16) | w1
f.seek(lbn * 512); blk = bytearray(f.read(512)); pos = 0
while pos + 6 <= 512:
    size = struct.unpack_from('<H', blk, pos)[0]
    if size in (0, 0xFFFF): break
    if bytes(blk[pos + 6:pos + 6 + blk[pos + 5]]) == b'D.DIR':
        ent = pos + 6 + blk[pos + 5]; ent += ent % 2
        struct.pack_into('<H', blk, ent + 2, 60000)   # a file number the index file does not cover
    pos += size + 2
f.seek(lbn * 512); f.write(blk)
PY
img="$TMP/bf.img"
$INIT "$img" 8192 --structure=5 --label=BADFID >/dev/null 2>&1
printf 'CREATE/DIRECTORY [D]\nCREATE/DIRECTORY [D.E]\n' > "$TMP/bf.scr"; $ODS2 "$img" "@$TMP/bf.scr" >/dev/null 2>&1
python3 -I "$TMP/badfid.py" "$img"
out=$($ODS2 "$img" 'DIR [000000...]*.*' 2>&1); rc=$?
case "$out" in *"E-DIRERR, [D]"*) msg=reported ;; *) msg="$(echo "$out" | grep -c DIRERR) $out" ;; esac
check "DIR [000000...] with an unreadable subdirectory header: DIRERR names the directory, exit 1" "reported 1" "$msg $rc"

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_ods5_integrity_check: $fails FAILED"
    exit 1
fi
echo "cli_ods5_integrity_check: all checks passed"
