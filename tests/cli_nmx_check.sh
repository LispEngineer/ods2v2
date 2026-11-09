#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_nmx_check.sh - file numbers above 65535 (FID NMX != 0: file number = NMX << 16 | NUM), ODS-2 and ODS-5.
# The index bitmap of a fresh volume is marked "in use" for numbers 11..65600 (no headers behind them, so the
# structure checker reports exactly those bits and nothing else), so the first files made get numbers
# 65601 and up: INDEXF.SYS has to grow past 65,600 headers, FIDs, directory entries and back links carry NMX.
# `make test-large` (tests/cli_nmx_big_check.sh) does the same with 75,000 real files.
# Called by `make test`; run from the project root.

TMP=samples/cli_nmx_tmp
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
mkdir -p "$TMP/tree/sub" "$TMP/ex"
printf 'high file one\n' > "$TMP/a.txt"
printf 'high file two, longer\n' > "$TMP/b.txt"
printf 'in the tree\n' > "$TMP/tree/t1.txt"
printf 'in the subtree\n' > "$TMP/tree/sub/t2.txt"
cat > "$TMP/bits.py" <<'PY'
import struct, sys
img, upto = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
ibm = struct.unpack_from('<I', home, 24)[0]; size = struct.unpack_from('<H', home, 32)[0]
f.seek(ibm * 512); bm = bytearray(f.read(size * 512))
for n in range(11, upto + 1):
    bm[(n - 1) // 8] |= 1 << ((n - 1) % 8)
f.seek(ibm * 512); f.write(bm)
PY

fid_of() { # fid_of <image> <path>: the file number DIR/FULL shows
    $ODS2 "$1" "DIR/FULL $2" 2>&1 | sed -n 's/^ *File ID: *(\([0-9]*\),.*/\1/p'
}
checker_rest() { # the checker's findings other than the marked bits
    python3 -I tests/ods5check.py "$1" | grep "^ERROR\|^WARN" | grep -v "bitmap bit set but no valid header" | head -5
}

# A file number whose low 16 bits are 0 (65536, 131072, ...) is never used: FID_NUM 0 means "no file", and on VMS (the 70,000-file
# run of ods5-vms2) DUMP/HEADER of file 65536 gives %DUMP-E-BADHEADER while 65535 and 65537 are normal. The allocator skips such
# numbers (the first VMS run is to show what VMS does with the bit and the slot).
for level in 2 5; do
    img="$TMP/skip.img"; rm -f "$img"
    $INIT "$img" 200000 --structure=$level --cluster=1 --maxfiles=70000 --label=SKIP64K >/dev/null 2>&1
    python3 -I "$TMP/bits.py" "$img" 65533
    got=""
    for i in 1 2 3 4; do
        $ODS2 "$img" "COPY $TMP/a.txt [000000]S$i.TXT" >/dev/null 2>&1
        got="$got $(fid_of "$img" "[000000]S$i.TXT")"
    done
    check "ODS-$level: the allocator skips file number 65536 (FID_NUM 0): 65534 65535 65537 65538" "65534 65535 65537 65538" "$(echo $got)"
    rm -f "$img"
done

for spec in "2 plain" "5 plain" "5 hardlinks"; do
    set -- $spec
    level=$1; kind=$2
    img="$TMP/n.img"
    rm -f "$img"; rm -f "$TMP/ex"/*
    opts=""; [ "$kind" = hardlinks ] && opts="--hardlinks"
    $INIT "$img" 200000 --structure=$level --cluster=1 --maxfiles=70000 --label=NMXTEST $opts >/dev/null 2>&1
    python3 -I "$TMP/bits.py" "$img" 65600
    t="ODS-$level $kind"

    $ODS2 "$img" "COPY $TMP/a.txt [000000]A.TXT" >/dev/null 2>&1
    $ODS2 "$img" "COPY $TMP/b.txt [000000]B.TXT" >/dev/null 2>&1
    na=$(fid_of "$img" '[000000]A.TXT'); nb=$(fid_of "$img" '[000000]B.TXT')
    check "$t: the first file made gets number 65601 (NMX 1)" 65601 "$na"
    check "$t: the next gets 65602" 65602 "$nb"
    check "$t: TYPE reads file 65601 (its entry carries NMX)" "high file one" "$($ODS2 "$img" 'TYPE [000000]A.TXT' 2>&1)"
    check "$t: TYPE reads file 65602" "high file two, longer" "$($ODS2 "$img" 'TYPE [000000]B.TXT' 2>&1)"
    check "$t: nothing but the marked bits is wrong" "" "$(checker_rest "$img")"

    $ODS2 "$img" "EXPORT [000000]A.TXT $TMP/ex" >/dev/null 2>&1
    check "$t: EXPORT gives the same bytes" "$(cat "$TMP/a.txt")" "$(cat "$TMP/ex"/[Aa].[Tt][Xx][Tt]* 2>/dev/null)"

    # a directory with a number above 65535: its entry, its header's back link, the files made in it
    check "$t: CREATE/DIRECTORY" "%ODS2-I-CREATED, created [HI]" "$($ODS2 "$img" 'CREATE/DIRECTORY [HI]' 2>&1)"
    nd=$(fid_of "$img" '[000000]HI.DIR')
    check "$t: the directory has number 65603" 65603 "$nd"
    $ODS2 "$img" "COPY $TMP/a.txt [HI]IN.TXT" >/dev/null 2>&1
    check "$t: a file made in it is number 65604 and reads back" "65604 high file one" "$(fid_of "$img" '[HI]IN.TXT') $($ODS2 "$img" 'TYPE [HI]IN.TXT' 2>&1)"
    check "$t: IMPORT into it" "%ODS2-I-IMPORTED, 2 files" "$($ODS2 "$img" "IMPORT $TMP/tree [HI]" 2>&1 | cut -c1-25)"
    check "$t: the imported file in the subdirectory reads back" "in the subtree" "$($ODS2 "$img" 'TYPE [HI.SUB]T2.TXT' 2>&1)"
    check "$t: wildcard DIR through the directory finds it" "t2.txt" "$($ODS2 "$img" 'DIR [*...]T2.TXT' 2>&1 | grep -io 't2.txt' | head -1 | tr 'A-Z' 'a-z')"
    check "$t: still nothing but the marked bits is wrong" "" "$(checker_rest "$img")"

    # links: another entry for a file with NMX
    check "$t: LINK" "ok" "$($ODS2 "$img" 'LINK [HI]LINKED.TXT [000000]B.TXT' >/dev/null 2>&1 && echo ok)"
    check "$t: the link reads the same bytes" "high file two, longer" "$($ODS2 "$img" 'TYPE [HI]LINKED.TXT' 2>&1)"
    check "$t: the checker still finds nothing wrong" "" "$(checker_rest "$img")"
    $ODS2 "$img" 'DELETE [HI]LINKED.TXT' >/dev/null 2>&1
    check "$t: the other name still reads after the link is deleted" "high file two, longer" "$($ODS2 "$img" 'TYPE [000000]B.TXT' 2>&1)"

    # deletes
    check "$t: DELETE a file with NMX" "%ODS2-I-DELETED, deleted A.TXT;1" "$($ODS2 "$img" 'DELETE [000000]A.TXT' 2>&1)"
    check "$t: it is gone" "1" "$($ODS2 "$img" 'TYPE [000000]A.TXT' >/dev/null 2>&1; echo $?)"
    check "$t: the other file is untouched" "high file two, longer" "$($ODS2 "$img" 'TYPE [000000]B.TXT' 2>&1)"
    check "$t: a directory with files in it is not deleted" "1" "$($ODS2 "$img" 'DELETE [HI]' >/dev/null 2>&1; echo $?)"
    $ODS2 "$img" 'DELETE [HI.SUB]T2.TXT' >/dev/null 2>&1; $ODS2 "$img" 'DELETE [HI.SUB]' >/dev/null 2>&1
    $ODS2 "$img" 'DELETE [HI]T1.TXT' >/dev/null 2>&1; $ODS2 "$img" 'DELETE [HI]IN.TXT' >/dev/null 2>&1
    check "$t: an emptied directory with NMX is deleted" "%ODS2-I-DELETED, deleted HI.DIR;1" "$($ODS2 "$img" 'DELETE [HI]' 2>&1)"
    check "$t: the volume afterwards: nothing but the marked bits is wrong" "" "$(checker_rest "$img")"
    # the freed numbers are reused: the next file gets the lowest free number (65601 again, seq 2)
    $ODS2 "$img" "COPY $TMP/a.txt [000000]AGAIN.TXT" >/dev/null 2>&1
    check "$t: the lowest freed number is used again" "65601" "$(fid_of "$img" '[000000]AGAIN.TXT')"
    check "$t: and nothing but the marked bits is wrong" "" "$(checker_rest "$img")"
done

rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_nmx_check: $fails FAILED"
    exit 1
fi
echo "cli_nmx_check: all checks passed"
