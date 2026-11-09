#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_ods5_names_check.sh - DIR on a hand-built ODS-5 volume: names
# with mixed case, spaces, many dots, ISO Latin-1 and UCS-2 characters
# are shown the way DCL shows them (^-escapes), or as UTF-8 under
# /UTF8; lookups and wildcards are case blind and take ^-escapes;
# directories whose names have dots or spaces are listed and entered.
# The volume is the synthetic disk with its root directory replaced by
# ODS-5 records and its home block patched to level 5.
# Called by `make test`; run from the project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_ods5n_disk.img
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

[ -f "$SRC_DISK" ] || { echo "FAIL: $SRC_DISK missing"; exit 1; }
cp "$SRC_DISK" "$DISK"

python3 -I - "$DISK" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
def blk(l): f.seek(l * 512); return bytearray(f.read(512))
def put(l, b): f.seek(l * 512); f.write(b)
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
home = blk(1)
cluster, = struct.unpack_from('<H', home, 14)
ibmaplbn, = struct.unpack_from('<I', home, 24)
ibmapsize, = struct.unpack_from('<H', home, 32)
root = blk(ibmaplbn + ibmapsize + 4 - 1)          # header of file 4
mp, inuse = root[1], root[58]
w0, w1 = struct.unpack_from('<HH', root, mp * 2)
assert w0 >> 14 == 1
rootlbn = ((w0 >> 8) & 0x3f) << 16 | w1           # first extent of the root directory

def rec(flags, name, versions):                    # versions: [(version, fid)]
    pad = b'\0' if len(name) & 1 else b''
    body = struct.pack('<HBB', 0x7fff, flags, len(name)) + name + pad
    for v, fid in versions:
        body += struct.pack('<HHHBB', v, fid, fid if fid <= 10 else 1, 0, 0)   # sequence number: a reserved file's is its number
    return struct.pack('<H', len(body)) + body
ISL1, UCS2 = 1 << 3, 3 << 3
recs = [
    (b'000000.DIR', 0, [(1, 4)]),
    (b'BACKUP.SYS', 0, [(1, 8)]),
    (b'DECUS.DIR', 0, [(1, 11)]),
    (b'Lots.Of.Periods.DIR', ISL1, [(1, 11)]),
    (b'MixedCase.Txt', 0, [(1, 8)]),
    (b'a&b.txt', ISL1, [(1, 8)]),
    (b'caf\xe9.txt', ISL1, [(1, 8)]),
    (b'multi.dot.name.txt', ISL1, [(2, 8), (1, 9)]),
    (b'two words.txt', ISL1, [(1, 8)]),
    (b'x\xffy\xa0z.dat', ISL1, [(1, 8)]),
    (u'euro€.txt'.encode('utf-16-le'), UCS2, [(1, 8)]),
]
out = b''.join(rec(fl, nm, vs) for nm, fl, vs in recs) + b'\xff\xff'
assert len(out) < 512
put(rootlbn, bytearray(out.ljust(512, b'\0')))
inner = b''.join(rec(fl, nm, vs) for nm, fl, vs in [
    (b'Inner File.txt', ISL1, [(1, 8)]),
    (b'inner.TXT', 0, [(1, 8)]),
]) + b'\xff\xff'
put(6, bytearray(inner.ljust(512, b'\0')))            # first block of DECUS.DIR (file 11)
home = blk(1)
struct.pack_into('<H', home, 12, 0x0501)
struct.pack_into('<H', home, 58, csum(home, 29))
struct.pack_into('<H', home, 510, csum(home, 255))
put(1, home)
PY

dir_names() { "$ODS2" "$DISK" "$@" 2>/dev/null | grep ';' | sed 's/ *;/;/'; }

check "DIR shows names with DCL's ^-escapes" \
"000000.DIR;1
BACKUP.SYS;1
DECUS.DIR;1
Lots^.Of^.Periods.DIR;1
MixedCase.Txt;1
a^&b.txt;1
café.txt;1
multi^.dot^.name.txt;2
multi^.dot^.name.txt;1
two^_words.txt;1
x^FFy^A0z.dat;1
euro^U20AC.txt;1" "$(dir_names 'DIR [000000]')"

check "DIR/UTF8 shows them as UTF-8" \
"Lots.Of.Periods.DIR;1
MixedCase.Txt;1
a&b.txt;1
café.txt;1
multi.dot.name.txt;2
multi.dot.name.txt;1
two words.txt;1
x"$(printf '\303\277')"y"$(printf '\302\240')"z.dat;1
euro€.txt;1" "$(dir_names 'DIR/UTF8 [000000]' | sed -n '4,$p')"

check "a name is found whatever the case" "MixedCase.Txt;1" "$(dir_names 'DIR [000000]MIXEDCASE.TXT')"
check "...and by wildcard" "MixedCase.Txt;1" "$(dir_names 'DIR [000000]mixed*')"
check "^_ in a pattern is a space" "two^_words.txt;1" "$(dir_names 'DIR [000000]two^_*')"
check "^xx is an ISO Latin-1 character" "café.txt;1" "$(dir_names 'DIR [000000]caf^E9.txt')"
check "% is one character, not one byte" "café.txt;1" "$(dir_names 'DIR [000000]caf%.txt')"
check "a typed UTF-8 character is itself" "café.txt;1" "$(dir_names 'DIR [000000]café.txt')"
check "^Uxxxx is a UCS-2 character" "euro^U20AC.txt;1" "$(dir_names 'DIR [000000]euro^U20ac.txt')"
check "^. is a dot, a plain . in the name part too" "multi^.dot^.name.txt;2
multi^.dot^.name.txt;1" "$(dir_names 'DIR [000000]multi.dot.name.txt')"
check "a version selects" "multi^.dot^.name.txt;1" "$(dir_names 'DIR [000000]multi^.dot^.name.txt;1')"
check "^& in a pattern" "a^&b.txt;1" "$(dir_names 'DIR [000000]a^&b.txt')"
check "a pattern with a malformed escape finds nothing" "" "$(dir_names 'DIR [000000]bad^Q.txt')"

check "a directory with dots in its name is entered with ^." "Inner^_File.txt;1
inner.TXT;1" "$(dir_names 'DIR [Lots^.Of^.Periods]')"
check "...in any case" "Inner^_File.txt;1" "$(dir_names 'DIR [LOTS^.OF^.PERIODS]inner^_file.txt')"
check "...and is shown with its stored case" "Directory [Lots^.Of^.Periods]" \
    "$("$ODS2" "$DISK" 'DIR [LOTS^.OF^.PERIODS]' 2>/dev/null | grep '^Directory' | head -1)"
check "DIR [...] lists the directories, with escapes and stored case" \
"Directory [000000]
Directory [DECUS]
Directory [Lots^.Of^.Periods]" "$("$ODS2" "$DISK" 'DIR [...]*.*' 2>/dev/null | grep '^Directory')"
check "SET DEFAULT takes an escaped directory name" "%ODS2-I-DEFSET, default set to [Lots^.Of^.Periods]" \
    "$("$ODS2" "$DISK" 'SET DEFAULT [lots^.of^.periods]' 2>/dev/null)"

check "DIR/FULL shows the file ID and the header's dates" "File ID:            (8,8,0)" \
    "$("$ODS2" "$DISK" 'DIR/FULL [000000]BACKUP.SYS' 2>/dev/null | grep 'File ID' | sed 's/^ *//')"
check "DIR/FULL needs no header name for a name that matches" 0 \
    "$("$ODS2" "$DISK" 'DIR/FULL [000000]BACKUP.SYS' 2>/dev/null | grep -c 'Primary link')"

# Deep directories: 120 levels, made by ods2 itself on the ODS-2 disk, then read as ODS-5.
cp "$SRC_DISK" "$DISK"
python3 -I - > "$DISK.cmds" <<'PY'
p = []
for i in range(1, 121):
    p.append('D%d' % i)
    print('CREATE/DIRECTORY [%s]' % '.'.join(p))
PY
"$ODS2" "$DISK" @"$DISK.cmds" >/dev/null 2>&1
python3 -I - "$DISK" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); b = bytearray(f.read(512))
struct.pack_into('<H', b, 12, 0x0501)
def csum(x, words): return sum(struct.unpack_from('<%dH' % words, x, 0)) & 0xffff
struct.pack_into('<H', b, 58, csum(b, 29))
struct.pack_into('<H', b, 510, csum(b, 255))
f.seek(512); f.write(b)
PY
check "DIR [...] walks 120 directory levels" 131 "$("$ODS2" "$DISK" 'DIR/PATHS [...]' 2>/dev/null | wc -l)"
deep=$(python3 -I -c "print('.'.join('D%d' % i for i in range(1, 120)))")
check "a path of 119 names resolves" "D120.DIR;1" \
    "$("$ODS2" "$DISK" "DIR [$deep]" 2>/dev/null | grep ';' | sed 's/ *;/;/')"
check "...and so does the same path in lower case" "D120.DIR;1" \
    "$("$ODS2" "$DISK" "DIR [$(echo "$deep" | tr D d)]" 2>/dev/null | grep ';' | sed 's/ *;/;/')"
rm -f "$DISK.cmds"

rm -f "$DISK"
[ "$fails" -eq 0 ] || { echo "$fails ODS-5 names check(s) failed"; exit 1; }
