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
lrl = int(sys.argv[3]) if len(sys.argv) > 3 else 512; mrs = int(sys.argv[4]) if len(sys.argv) > 4 else 512
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
struct.pack_into('<H', h, 22, lrl); struct.pack_into('<H', h, 36, mrs)      # RSIZE (LRL), MAXREC (MRS): 512 and 512 by default
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
        "$(python3 -I tests/ods5check.py "$img" | grep -c '^WARN.*LONG.TXT.*whose longest record is 1500 bytes has LRL 512 and MRS 512') $(python3 -I tests/ods5check.py "$img" | grep -c '^WARN')"
    check "$t: the warning says what fails (TYPE, COPY, and CHECKSUM on the VAX: RSZ, RTB), that CONVERT drops the record and block I/O reads the file" "1" \
        "$(python3 -I tests/ods5check.py "$img" | grep -c 'TYPE, COPY and (VAX) CHECKSUM fail (%RMS-F-RSZ, %RMS-W-RTB); CONVERT drops the record; block I/O and COPY to disk work')"
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
# ---- Stream (STM) record ends as RMS applies them (VAX V7.3 and V8.4, notes 15.28): LF, FF and VT end a record and stay in it,
# CR LF ends one and is stripped, a lone CR is data. COPY wrote LRL 600 for a 600-byte line ended by a lone LF, which is a 601-byte
# record: COPY and CHECKSUM then failed on the VAX (%RMS-W-RTB).
python3 -c "
open('$TMP/lf600.txt','wb').write(b'a' * 600 + b'\n' + b'b')
open('$TMP/mixed.txt','wb').write(b'x' * 10 + b'\r\n' + b'y' * 700 + b'\n' + b'z')
open('$TMP/ff.txt','wb').write(b'p' * 300 + b'\f' + b'q' * 5 + b'\v' + b'r' * 299 + b'\r' + b's')
open('$TMP/mrs0.txt','wb').write(b'm' * 900 + b'\n')
"
for level in 2 5; do
    img="$TMP/m$level.img"; t="ODS-$level"
    $INIT "$img" 8192 --structure=$level --label=STM >/dev/null 2>&1
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STM,RAT:CR) $TMP/lf600.txt [000000]LF600.TXT" >/dev/null 2>&1
    check "$t: RFM:STM, a 600-byte line ended by a lone LF is a 601-byte record: LRL 601" "Stream, maximum 0 bytes, longest 601 bytes" "$(rec "$img" '[000000]LF600.TXT')"
    $ODS2 "$img" "COPY/FORMAT=STM $TMP/mixed.txt [000000]MIXED.TXT" >/dev/null 2>&1
    check "$t: /FORMAT=STM of CR LF and lone LF ends: 10, 701 (LF stays), 1: LRL 701" "Stream, maximum 0 bytes, longest 701 bytes" "$(rec "$img" '[000000]MIXED.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STM) $TMP/ff.txt [000000]FF.TXT" >/dev/null 2>&1
    check "$t: RFM:STM, FF and VT end a record and stay in it, a lone CR is data: 301, 6, 300+1+1 = 301: LRL 301" "Stream, maximum 0 bytes, longest 301 bytes" "$(rec "$img" '[000000]FF.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STMLF) $TMP/lf600.txt [000000]STMLF.TXT" >/dev/null 2>&1
    check "$t: Stream_LF: the same file is 600 (the LF is not in the record)" "Stream_LF, maximum 0 bytes, longest 600 bytes" "$(rec "$img" '[000000]STMLF.TXT')"
    $ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STM,LRL:600) $TMP/lf600.txt [000000]TOOSHORT.TXT" >/dev/null 2>&1
    check "$t: an LRL of 600 for that 601-byte record is refused (exit 1)" 1 "$($ODS2 "$img" "COPY/ATTRIBUTES=(RFM:STM,LRL:600) $TMP/lf600.txt [000000]TOOSHORT.TXT" >/dev/null 2>&1; echo $?)"
    # a file with MRS 0 and LRL 512 (VMS's own half repair): COPY and CHECKSUM still fail; checker warns, repair fixes it
    $ODS2 "$img" "COPY $TMP/mrs0.txt [000000]HALF.TXT" >/dev/null 2>&1
    python3 "$TMP/damage.py" "$img" "$(fid_num "$img" '[000000]HALF.TXT')" 512 0
    check "$t: Stream_LF with MRS 0 and LRL 512 and a 900-byte record: ods5check warns" "1" \
        "$(python3 -I tests/ods5check.py "$img" | grep -c '^WARN.*HALF.TXT.*longest record is 900 bytes has LRL 512 and MRS 0')"
    out=$($ODS2 "$img" 'SET FILE/ATTRIBUTES=(MRS:0) [000000]HALF.TXT' 2>&1)
    case "$out" in *"RECSIZES, 1 file looked at, 1 stream file changed"*) msg=repaired ;; *) msg="$out" ;; esac
    check "$t: the repair fixes MRS-0 files with a short LRL too: MRS 0, LRL 900" "repaired Stream_LF, maximum 0 bytes, longest 900 bytes" "$msg $(rec "$img" '[000000]HALF.TXT')"
    # MRS 0 with an LRL that is enough (even much larger): nothing to repair, no warning
    python3 "$TMP/damage.py" "$img" "$(fid_num "$img" '[000000]HALF.TXT')" 4000 0
    out=$($ODS2 "$img" 'SET FILE/ATTRIBUTES=(MRS:0) [000000]HALF.TXT' 2>&1)
    case "$out" in *", 0 stream files changed"*) msg=unchanged ;; *) msg="$out" ;; esac
    check "$t: MRS 0 and LRL 4000 for a 900-byte record: left alone, and ods5check has no warning for it" "unchanged 0" \
        "$msg $(python3 -I tests/ods5check.py "$img" | grep -c '^WARN.*HALF.TXT')"
    # a damaged file written as the old code did (lone LF, LRL = 600): the checker sees the 601-byte record
    python3 "$TMP/damage.py" "$img" "$(fid_num "$img" '[000000]LF600.TXT')" 600 0
    check "$t: an STM file with LRL 600 and a 601-byte record (the old COPY's output): ods5check warns" "1" \
        "$(python3 -I tests/ods5check.py "$img" | grep -c '^WARN.*LF600.TXT.*longest record is 601 bytes has LRL 600 and MRS 0')"
    # /CONVERT (VAR): LRL is the longest line (the record is the line without its terminator), MRS 0
    $ODS2 "$img" "COPY/CONVERT $TMP/mixed.txt [000000]VARFILE.TXT" >/dev/null 2>&1
    check "$t: COPY/CONVERT: variable-length records, LRL the longest line (one per LF, a CR before it dropped: 10, 700, 1)" \
        "Variable length, maximum 0 bytes, longest 700 bytes" "$(rec "$img" '[000000]VARFILE.TXT')"
    rm -f "$img"
done
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_stream_attrs_check: $fails FAILED"
    exit 1
fi
echo "cli_stream_attrs_check: all checks passed"
