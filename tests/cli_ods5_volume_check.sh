#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_volume_check.sh - an ODS-5 (structure level 5) volume mounts
# read-only: SHOW VOLUME names the structure level and the volume
# characteristics, reads work, every command that would write fails
# with exit status 1 and the image stays byte for byte as it was. The
# volume is the synthetic ODS-2 disk with its home block patched to
# level 5, so this needs no big image. Called by `make test`; run from
# the project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_ods5_disk.img
ORIG=samples/cli_ods5_orig.img
ODS2=./ods2
TMP=samples/cli_ods5_tmp
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

status() { "$@" >/dev/null 2>&1; echo $?; }
same() { if cmp -s "$1" "$2"; then echo unchanged; else echo changed; fi; }

[ -f "$SRC_DISK" ] || { echo "FAIL: $SRC_DISK missing"; exit 1; }
rm -f "$DISK" "$ORIG"
mkdir -p "$TMP"
cp "$SRC_DISK" "$DISK"

# Patch the home block (LBN 1): level 5 version 1, volume characteristics
# NOHIGHWATER | ACCESSTIMES | HARDLINKS, both checksums recomputed.
python3 -I - "$DISK" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); b = bytearray(f.read(512))
struct.pack_into('<H', b, 12, 0x0501)
struct.pack_into('<H', b, 42, 0x0068)
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
struct.pack_into('<H', b, 58, csum(b, 29))
struct.pack_into('<H', b, 510, csum(b, 255))
f.seek(512); f.write(b)
PY
cp "$DISK" "$ORIG"
printf 'ods5 test\n' > "$TMP/t.txt"

out=$($ODS2 $DISK 'SHOW VOLUME' 2>/dev/null)
check "SHOW VOLUME names the structure level" "ODS-5, version 1" \
    "$(echo "$out" | sed -n 's/^  Structure level: *//p')"
check "SHOW VOLUME lists the volume characteristics" \
    "NOHIGHWATER, ACCESS_DATES, HARDLINKS" \
    "$(echo "$out" | sed -n 's/^  Characteristics: *//p')"
check "mounting an ODS-5 volume warns on stderr" 1 \
    "$($ODS2 $DISK 'SHOW VOLUME' 2>&1 >/dev/null | grep -c '%ODS2-W-ODS5RO')"
check "DIR works on an ODS-5 volume and exits 0" 0 "$(status $ODS2 $DISK 'DIR [000000]')"

check "COPY onto an ODS-5 volume exits 1" 1 "$(status $ODS2 $DISK COPY $TMP/t.txt '[000000]T.TXT')"
check "...with a message naming ODS-5" "ODS-5" \
    "$($ODS2 $DISK COPY $TMP/t.txt '[000000]T.TXT' 2>&1 | grep -o 'ODS-5' | head -1)"
check "CREATE/DIRECTORY on an ODS-5 volume exits 1" 1 \
    "$(status $ODS2 $DISK 'CREATE/DIRECTORY [NEWDIR]')"
check "DELETE on an ODS-5 volume exits 1" 1 "$(status $ODS2 $DISK 'DELETE [000000]NOPE.TXT')"
check "SET VOLUME/LABEL on an ODS-5 volume exits 1" 1 \
    "$(status $ODS2 $DISK 'SET VOLUME/LABEL=NEWLBL')"
check "the ODS-5 image is unchanged by all of it" unchanged "$(same $DISK $ORIG)"

# The same disk as ODS-2 shows no ODS-5 characteristics (bits 5-7 mean nothing at level 2).
python3 -I - "$DISK" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); b = bytearray(f.read(512))
struct.pack_into('<H', b, 12, 0x0201)
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
struct.pack_into('<H', b, 58, csum(b, 29))
struct.pack_into('<H', b, 510, csum(b, 255))
f.seek(512); f.write(b)
PY
out=$($ODS2 $DISK 'SHOW VOLUME' 2>/dev/null)
check "an ODS-2 volume says ODS-2" "ODS-2, version 1" \
    "$(echo "$out" | sed -n 's/^  Structure level: *//p')"
check "...and shows only the ODS-2 characteristic bits" \
    "NOHIGHWATER, 0x0060 (unknown)" \
    "$(echo "$out" | sed -n 's/^  Characteristics: *//p')"

rm -rf "$TMP" "$DISK" "$ORIG"
[ "$fails" -eq 0 ] || { echo "$fails ODS-5 volume check(s) failed"; exit 1; }
