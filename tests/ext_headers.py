#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""ext_headers.py IMAGE SPEC : every header of a file's chain, read from the image by an independent reader (ods5check's volume reader; no ods2v2 code for the header):
the primary header first, then each extension header in the order of the EXT_FID links. One line per header:
"seg=0 num=12 owner=[1,4] fileprot=fa00 revision=1 checksum=ok" (owner is [group,member] in octal, as OpenVMS prints a UIC; revision is the FI2 revision at ident+20 or the FI5 one at ident+2).
Used by tests/setfile_ext_check.py and tests/fault_inject.py (SET FILE/OWNER_UIC on a file with extension headers). Run from the project root; needs ./ods2 (a path with DIR/FULL)."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re
import subprocess
import ods5check as O   # noqa: E402


class D:   # the two helpers of tests/dirhdr_fields.py (ods5-dirhdr), kept here so that this file stands alone on ods5-ops234
    @staticmethod
    def fnum(img, spec):
        out = subprocess.run(['./ods2', img, 'DIR/FULL ' + spec], capture_output=True, text=True).stdout
        m = re.search(r'File ID:\s+\((\d+),', out)
        return int(m.group(1)) if m else None

    @staticmethod
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


def chain(img, spec, v=None):
    """[(number, header bytes)] of the primary header and its extension headers; [] when the file is not found"""
    n = D.fnum(img, spec)
    if n is None:
        return []
    v = v or O.Vol(img)
    out = []
    seen = set()
    while n and n not in seen and len(out) < 70000:
        seen.add(n)
        h = D.header(v, n)
        out.append((n, h))
        n = struct.unpack_from('<H', h, 14)[0] | (h[19] << 16)
    return out


def describe(num, h):
    ident = h[0] * 2
    level5 = (h[7] if len(h) > 7 else 0) == 5
    rev = O.u16(h, ident + (2 if level5 else 20)) if h[4] == 0 and h[5] == 0 else None
    ck = 'ok' if (sum(struct.unpack_from('<255H', h, 0)) & 0xffff) == O.u16(h, 510) else 'BAD'
    return {'seg': O.u16(h, 4), 'num': num, 'owner': '[%o,%o]' % (O.u16(h, 62), O.u16(h, 60)), 'fileprot': '%04x' % O.u16(h, 64),
            'revision': rev if rev is not None else '-', 'checksum': ck}


if __name__ == '__main__':
    c = chain(sys.argv[1], sys.argv[2])
    if not c:
        print('not found')
        sys.exit(1)
    for num, h in c:
        print(' '.join('%s=%s' % kv for kv in describe(num, h).items()))
