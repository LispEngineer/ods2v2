#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_volsize_zero_check.sh - a volume whose storage control block gives
# a volume size of 0 (SCB$L_VOLSIZE, spec 5.2.1), or whose SCB cannot be
# read, is never written: without the size, nothing bounds the allocator
# or the writes to the end of the volume (upstream issue #23, b167ac0,
# refuses allocation in that case). Reading still works. ODS-2 and ODS-5.
# Called by `make test`; run from the project root.

ODS2=./ods2
INIT=./ods2_init
TMP=samples/cli_volsize0_tmp
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

rm -rf "$TMP"; mkdir -p "$TMP"
printf 'hello\n' > "$TMP/hello.txt"

for level in 2 5; do
    DISK=$TMP/v$level.img
    "$INIT" "$DISK" 8192 --label=VS0TEST --structure=$level >/dev/null 2>&1
    check "ODS-$level: the volume is made" 0 $?
    "$ODS2" "$DISK" "COPY $TMP/hello.txt BEFORE.TXT" >/dev/null 2>&1
    check "ODS-$level: COPY works while the SCB is intact" 0 $?

    # Set SCB$L_VOLSIZE (bytes 4-7 of BITMAP.SYS VBN 1) to 0; the SCB checksum (last word) is recomputed.
    python3 -I - "$DISK" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); h = f.read(512)
ibmaplbn, = struct.unpack_from('<I', h, 24); ibmapsize, = struct.unpack_from('<H', h, 32)
f.seek((ibmaplbn + ibmapsize + 1) * 512); bh = f.read(512)        # BITMAP.SYS header (file 2)
mp = bh[1] * 2; w0, low = struct.unpack_from('<HH', bh, mp)
assert w0 >> 14 == 1
lbn = ((w0 >> 8) & 0x3f) << 16 | low
f.seek(lbn * 512); scb = bytearray(f.read(512))
struct.pack_into('<I', scb, 4, 0)
struct.pack_into('<H', scb, 510, sum(struct.unpack_from('<255H', scb, 0)) & 0xffff)
f.seek(lbn * 512); f.write(scb)
EOF
    SUM_BEFORE=$(sha256sum < "$DISK")

    out=$("$ODS2" "$DISK" "COPY $TMP/hello.txt HELLO.TXT" 2>&1)
    check "ODS-$level: COPY onto a volume whose SCB gives size 0 is refused" 1 $?
    case "$out" in *"volume size is 0 or unreadable"*) r=named ;; *) r="$out" ;; esac
    check "ODS-$level: the message names the volume size" named "$r"
    "$ODS2" "$DISK" "CREATE/DIRECTORY [NEWDIR]" >/dev/null 2>&1
    check "ODS-$level: CREATE/DIRECTORY is refused too" 1 $?
    check "ODS-$level: the image is unchanged" "$SUM_BEFORE" "$(sha256sum < "$DISK")"

    out=$("$ODS2" "$DISK" "DIR [000000]" 2>&1)
    case "$out" in *BEFORE.TXT*) r=listed ;; *) r="$out" ;; esac
    check "ODS-$level: the volume can still be read" listed "$r"
done

rm -rf "$TMP"
if [ "$fails" -ne 0 ]; then
    echo "cli_volsize_zero_check: $fails check(s) FAILED"
    exit 1
fi
echo "cli_volsize_zero_check: all checks passed"
