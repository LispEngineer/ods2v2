#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""newver_setprot.py IMAGE FILENUMBER OWNER_GROUP OWNER_MEMBER PROT_WORD : rewrites FH2$L_FILEOWNER and FH2$W_FILEPROT of one file's primary header in an
image (the test of cli_newver_check.sh needs files with a protection and an owner that ods2 itself cannot set; ods2 has no SET FILE/PROTECTION).
The header is found through INDEXF.SYS's own map; the header checksum is made again. Run from the project root."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402

img, num = sys.argv[1], int(sys.argv[2])
group, member, prot = int(sys.argv[3], 0), int(sys.argv[4], 0), int(sys.argv[5], 0)
v = O.Vol(img)
h1 = v.blk(v.ibmaplbn + v.ibmapsize)
ix = O.decode_map(h1, O.Findings(False), 'indexf')
vbn = O.u16(v.home, 22) + O.u16(v.home, 32) + num - 1
left = vbn - 1
lbn = None
for l0, c in ix:
    if left < c:
        lbn = l0 + left
        break
    left -= c
with open(img, 'r+b') as f:
    f.seek(lbn * 512)
    h = bytearray(f.read(512))
    assert struct.unpack_from('<H', h, 8)[0] == num, 'not the header of file %d' % num
    struct.pack_into('<HHH', h, 60, member, group, prot)      # FILEOWNER (member, group) and FILEPROT
    struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xffff)
    f.seek(lbn * 512)
    f.write(h)
