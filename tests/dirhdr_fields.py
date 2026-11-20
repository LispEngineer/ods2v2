#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""dirhdr_fields.py IMAGE SPEC [SPEC...] : prints the header fields of follow-up c for each file or directory SPEC names (a path ods2 DIR/FULL accepts), read from the
image by an independent reader (ods5check's volume reader, no ods2v2 code for the header): fileprot, recprot, revision, highwater, hiblk, efblk, filechar.
One line per SPEC: "SPEC fileprot=ba88 recprot=0000 revision=0 highwater=2 hiblk=1 efblk=2 filechar=00002080". Run from the project root; needs ./ods2."""
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402


def fnum(img, spec):
    out = subprocess.run(['./ods2', img, 'DIR/FULL ' + spec], capture_output=True, text=True).stdout
    m = re.search(r'File ID:\s+\((\d+),', out)
    return int(m.group(1)) if m else None


def header(v, num):
    if not hasattr(v, 'ix'):
        h1 = v.blk(v.ibmaplbn + v.ibmapsize)
        v.ix = O.decode_map(h1, O.Findings(False), 'indexf')
    left = O.u16(v.home, 22) + O.u16(v.home, 32) + num - 2
    for lbn, cnt in v.ix:
        if left < cnt:
            return v.blk(lbn + left)
        left -= cnt
    raise ValueError('header %d is past the index file map' % num)


def fields(img, spec):
    n = fnum(img, spec)
    if n is None:
        return None
    h = header(O.Vol(img), n)
    ident = h[0] * 2
    level5 = (h[7] if len(h) > 7 else 0) == 5
    rev_off = 2 if level5 else 20
    hiblk = struct.unpack_from('<I', h, 24)[0]
    efblk = struct.unpack_from('<I', h, 28)[0]
    swap = lambda x: ((x >> 16) | (x << 16)) & 0xFFFFFFFF
    return {'fileprot': '%04x' % O.u16(h, 64), 'recprot': '%04x' % O.u16(h, 56), 'revision': O.u16(h, ident + rev_off),
            'highwater': struct.unpack_from('<I', h, 76)[0], 'hiblk': swap(hiblk), 'efblk': swap(efblk), 'filechar': '%08x' % struct.unpack_from('<I', h, 52)[0]}


if __name__ == '__main__':
    for spec in sys.argv[2:]:
        f = fields(sys.argv[1], spec)
        print('%s %s' % (spec, ' '.join('%s=%s' % kv for kv in f.items()) if f else 'not found'))
