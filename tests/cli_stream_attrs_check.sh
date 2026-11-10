#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_stream_attrs_check.sh - a stream (Stream_LF, Stream, Stream_CR) or undefined-format file made with an explicit
# record format has MRS 0 and its own longest record as LRL, whatever the lines: COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR)
# used to leave RSIZE = MRS = 512, and VMS refused to read such a file with a longer line (%RMS-W-RTB, "record too large
# for the user's buffer", found on VAX V7.3 in the after-images of the stress harness). A record longer than a
# given MRS or LRL is refused. ODS-2 and ODS-5; COPY, IMPORT and ods2_put. Called by `make test`; run from the project root.

TMP=samples/cli_stream_tmp
INIT=./ods2_init
ODS2=./ods2
PUT=./ods2_put
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

rec() { # rec <image> <spec>: the record format line DIR/FULL shows
    $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^ *Record format: *//p'
}

rm -rf "$TMP"
mkdir -p "$TMP/tree"
python3 -c "
open('$TMP/long.txt','wb').write(b'a' * 1500 + b'\n' + b'b' * 20 + b'\n')
open('$TMP/crlong.txt','wb').write(b'x' * 700 + b'\r' + b'y\r')
open('$TMP/crlf.txt','wb').write(b'q' * 900 + b'\r\n' + b'r\r\n')
open('$TMP/tree/t.txt','wb').write(b'z' * 3000 + b'\n')
"
for level in 2 5; do
    img="$TMP/s$level.img"
    $INIT "$img" 8192 --structure=$level --label=STREAM >/dev/null 2>&1
    t="ODS-$level"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) $TMP/long.txt [000000]A.TXT" >/dev/null 2>&1
    check "$t: COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) of a 1500-byte line: MRS 0, LRL 1500" \
        "Stream_LF, maximum 0 bytes, longest 1500 bytes" "$(rec "$img" '[000000]A.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMCR,RAT:CR) $TMP/crlong.txt [000000]B.TXT" >/dev/null 2>&1
    check "$t: RFM:STMCR: lines end at CR, LRL 700" "Stream_CR, maximum 0 bytes, longest 700 bytes" "$(rec "$img" '[000000]B.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STM,RAT:CR) $TMP/crlf.txt [000000]C.TXT" >/dev/null 2>&1
    check "$t: RFM:STM: lines end at CR LF (the CR not counted), LRL 900" "Stream, maximum 0 bytes, longest 900 bytes" "$(rec "$img" '[000000]C.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:UDF) $TMP/long.txt [000000]D.DAT" >/dev/null 2>&1
    check "$t: RFM:UDF: LRL the length (1522), MRS 0" "Undefined, maximum 0 bytes, longest 1522 bytes" "$(rec "$img" '[000000]D.DAT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF,MRS:4000) $TMP/long.txt [000000]E.TXT" >/dev/null 2>&1
    check "$t: an MRS that is given is kept: 4000, LRL 1500" "Stream_LF, maximum 4000 bytes, longest 1500 bytes" "$(rec "$img" '[000000]E.TXT')"
    out=$($ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF,MRS:512) $TMP/long.txt [000000]F.TXT" 2>&1); rc=$?
    case "$out" in *"a line of 1500 bytes is longer than MRS:512"*"RMS-W-RTB"*) msg=refused ;; *) msg="$out" ;; esac
    check "$t: MRS:512 with a 1500-byte line is refused (VMS could not read the file back), exit 1, no file" "refused 1 0" \
        "$msg $rc $($ODS2 "$img" 'DIR [000000]F.TXT' 2>&1 | grep -c '^F\.TXT')"
    out=$($ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF,LRL:100) $TMP/long.txt [000000]G.TXT" 2>&1); rc=$?
    case "$out" in *"longer than LRL:100"*) msg=refused ;; *) msg="$out" ;; esac
    check "$t: LRL:100 with a 1500-byte line is refused, exit 1" "refused 1" "$msg $rc"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF) $TMP/long.txt [000000]H.TXT" >/dev/null 2>&1
    check "$t: no RAT keyword at all: the same" "Stream_LF, maximum 0 bytes, longest 1500 bytes" "$(rec "$img" '[000000]H.TXT')"
    $ODS2 "$img" "IMPORT/ATTRIBUTES=(RFM:STMLF,RAT:CR) $TMP/tree [000000]" >/dev/null 2>&1
    check "$t: IMPORT/ATTRIBUTES of a tree: MRS 0, LRL 3000" "Stream_LF, maximum 0 bytes, longest 3000 bytes" "$(rec "$img" '[000000]T.TXT')"
    # plain COPY and /FORMAT were already right: they stay
    $ODS2 "$img" "COPY $TMP/long.txt [000000]I.TXT" >/dev/null 2>&1
    check "$t: plain COPY (unchanged)" "Stream_LF, maximum 0 bytes, longest 1500 bytes" "$(rec "$img" '[000000]I.TXT')"
    python3 -I tests/ods5check.py "$img" | tail -1 > "$TMP/ck.txt"
    check "$t: ods5check clean" "0 error(s), 0 warning(s)" "$(cat "$TMP/ck.txt")"
    rm -f "$img"
done

# ods2_put (ODS-2 only): rtype 5 without a size: the longest line, MRS 0; with a size that is too small: refused
$INIT "$TMP/p.img" 8192 --structure=2 --label=PUT >/dev/null 2>&1
$PUT "$TMP/p.img" "$TMP/long.txt" '' PUT1.TXT 5 >/dev/null 2>&1
check "ods2_put rtype 5 (no size): MRS 0, LRL 1500" "Stream_LF, maximum 0 bytes, longest 1500 bytes" "$(rec "$TMP/p.img" '[000000]PUT1.TXT')"
$PUT "$TMP/p.img" "$TMP/long.txt" '' PUT2.TXT 5 100 >/dev/null 2>&1
check "ods2_put rtype 5 with size 100: refused (a longer line), exit 1" 1 "$($PUT "$TMP/p.img" "$TMP/long.txt" '' PUT3.TXT 5 100 >/dev/null 2>&1; echo $?)"
# SET FILE/ATTRIBUTES=(MRS:0) repairs files made the old way (MRS = LRL = 512): the checker warns of a line longer than the MRS
cat > "$TMP/damage.py" <<'PY'
import struct, sys
img, n = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
ibm = struct.unpack_from('<I', home, 24)[0]; size = struct.unpack_from('<H', home, 32)[0]; cl = struct.unpack_from('<H', home, 14)[0]
f.seek((ibm + size) * 512); h1 = f.read(512)
p = h1[1] * 2; q = 0; ex = []
while q < h1[58]:
    w0 = struct.unpack_from('<H', h1, p)[0]; fmt = (w0 >> 14) & 3
    if fmt == 1: cnt = (w0 & 0xff) + 1; lbn = (((w0 >> 8) & 0x3f) << 16) | struct.unpack_from('<H', h1, p + 2)[0]; p += 4; q += 2
    elif fmt == 2: cnt = (w0 & 0x3fff) + 1; lbn = struct.unpack_from('<I', h1, p + 2)[0]; p += 6; q += 3
    else: cnt = (((w0 & 0x3fff) << 16) | struct.unpack_from('<H', h1, p + 2)[0]) + 1; lbn = struct.unpack_from('<I', h1, p + 4)[0]; p += 8; q += 4
    ex.append((lbn, cnt))
vbn = cl * 4 + size + n; cur = 1
for l, c in ex:
    if cur <= vbn < cur + c: at = l + vbn - cur
    cur += c
f.seek(at * 512); h = bytearray(f.read(512))
struct.pack_into('<H', h, 22, 512); struct.pack_into('<H', h, 36, 512)      # RSIZE (LRL) 512, MAXREC (MRS) 512
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xffff)
f.seek(at * 512); f.write(h)
PY
fid_num() { $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p'; }
for level in 2 5; do
    img="$TMP/r$level.img"; t="ODS-$level"
    $INIT "$img" 8192 --structure=$level --label=REPAIR >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/long.txt [000000]LONG.TXT" >/dev/null 2>&1
    printf 'short\nlines\n' > "$TMP/short.txt"
    $ODS2 "$img" "COPY $TMP/short.txt [000000]SHORT.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY/FORMAT=FIX512 $TMP/long.txt [000000]BIN.DAT" >/dev/null 2>&1
    python3 "$TMP/damage.py" "$img" "$(fid_num "$img" '[000000]LONG.TXT')"
    python3 "$TMP/damage.py" "$img" "$(fid_num "$img" '[000000]SHORT.TXT')"
    check "$t: files made the old way: Stream_LF, maximum 512, longest 512" "Stream_LF, maximum 512 bytes, longest 512 bytes" "$(rec "$img" '[000000]LONG.TXT')"
    $ODS2 "$img" "EXPORT [000000]LONG.TXT $TMP/before.txt" >/dev/null 2>&1
    check "$t: ods5check warns of the 1500-byte record in a file with MRS 512, and only of that one" "1 1" \
        "$(python3 -I tests/ods5check.py "$img" | grep -c '^WARN.*LONG.TXT.*maximum record size 512 holds a record of 1500 bytes') $(python3 -I tests/ods5check.py "$img" | grep -c '^WARN')"
    out=$($ODS2 "$img" 'SET FILE/ATTRIBUTES=(MRS:0) [000000]*.TXT' 2>&1); rc=$?
    case "$out" in *"RECSIZES, 2 files looked at, 2 stream files changed"*) msg=repaired ;; *) msg="$out" ;; esac
    check "$t: SET FILE/ATTRIBUTES=(MRS:0) [000000]*.TXT: both files repaired, exit 0" "repaired 0" "$msg $rc"
    check "$t: ... the long one: MRS 0, LRL 1500" "Stream_LF, maximum 0 bytes, longest 1500 bytes" "$(rec "$img" '[000000]LONG.TXT')"
    check "$t: ... the short one: MRS 0, LRL 5" "Stream_LF, maximum 0 bytes, longest 5 bytes" "$(rec "$img" '[000000]SHORT.TXT')"
    $ODS2 "$img" "EXPORT [000000]LONG.TXT $TMP/after.txt" >/dev/null 2>&1
    check "$t: ... the data is unchanged" "$(md5sum < "$TMP/before.txt")" "$(md5sum < "$TMP/after.txt")"
    check "$t: ... a fixed file is left alone" "Fixed length 512 byte records" "$(rec "$img" '[000000]BIN.DAT')"
    check "$t: ods5check clean after the repair" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" | tail -1)"
    out=$($ODS2 "$img" 'SET FILE/ATTRIBUTES=(MRS:0) [000000]*.*' 2>&1)
    case "$out" in *", 0 stream files changed"*) msg=nothing-more ;; *) msg="$out" ;; esac
    check "$t: run again: nothing to change" "nothing-more" "$msg"
    check "$t: any other attribute change is refused (exit 1)" 1 "$($ODS2 "$img" 'SET FILE/ATTRIBUTES=(MRS:100) [000000]*.TXT' >/dev/null 2>&1; echo $?)"
    rm -f "$img"
done
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_stream_attrs_check: $fails FAILED"
    exit 1
fi
echo "cli_stream_attrs_check: all checks passed"
