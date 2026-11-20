#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""swap_versions.py IMAGE DIRFILENUMBER NAME [setver INDEX VERSION | rename NEWNAME] : reverses the order of the (version, file id) entries of the first directory record of NAME
in the directory file DIRFILENUMBER (a record keeps a name's versions highest first; ods2v2 and VMS never write them in another order, a damaged volume or
another tool can); with `setver INDEX VERSION` it instead sets the version word of the INDEX-th entry (0 first) of that record to VERSION (0, a number over
32767, the number of another entry: the damage of review 49 S1); with `rename NEWNAME` it gives the record another name of the same length (a name then held in two
records with another name between them: review 50 F1). Used by cli_purge_check.sh (review 48 F3, review 49 S1). Run from the project root; the image
must be writable."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402

img, dirnum, name = sys.argv[1], int(sys.argv[2]), sys.argv[3].upper().encode('latin-1')
op = sys.argv[4] if len(sys.argv) > 4 else 'swap'
v = O.Vol(img)
h1 = v.blk(v.ibmaplbn + v.ibmapsize)
ix = O.decode_map(h1, O.Findings(False), 'indexf')
vbn = O.u16(v.home, 22) + O.u16(v.home, 32) + dirnum - 1
left = vbn - 1
hl = None
for lbn, cnt in ix:
    if left < cnt:
        hl = lbn + left
        break
    left -= cnt
hdr = v.blk(hl)
ext = O.decode_map(hdr, O.Findings(False), 'dir')
done = False
with open(img, 'r+b') as f:
    for lbn, cnt in ext:
        for b in range(cnt):
            f.seek((lbn + b) * 512)
            blk = bytearray(f.read(512))
            pos = 0
            while pos + 6 <= 512:
                size = struct.unpack_from('<H', blk, pos)[0]
                if size == 0xffff or size == 0:
                    break
                namecount = blk[pos + 5]
                nm = bytes(blk[pos + 6:pos + 6 + namecount])
                first = pos + 6 + namecount + (namecount % 2)
                end = pos + 2 + size
                if nm == name and (end - first) // 8 >= 2:
                    if op == 'setver':
                        struct.pack_into('<H', blk, first + 8 * int(sys.argv[5]), int(sys.argv[6]))
                    elif op == 'rename':
                        new = sys.argv[5].upper().encode('latin-1')
                        assert len(new) == namecount, 'the new name must have the length of the old one'
                        blk[pos + 6:pos + 6 + namecount] = new
                    else:
                        ents = [bytes(blk[e:e + 8]) for e in range(first, end, 8)]
                        ents.reverse()
                        blk[first:end] = b''.join(ents)
                    f.seek((lbn + b) * 512)
                    f.write(blk)
                    done = True
                    break
                pos = end
            if done:
                break
        if done:
            break
if not done:
    sys.exit('no directory record of %s with two or more versions' % name.decode())
