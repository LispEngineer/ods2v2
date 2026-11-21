#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""exthdr_attr_check.py : the record-attribute fields of the extension headers of a file made by ONE allocation (follow-up d item 6; CORPUS 61, ods5-vms3's measurement on ASIM5 V8.4 and
PHVS2 VAX V7.3, ODS-2 and ODS-5, byte-identical on both machines): each extension header holds the primary header's RTYPE, RATTRIB, RSIZE and MAXREC and the values HIBLK 0, EFBLK 1,
FFBYTE 0, stored HIGHWATER 1; the primary header holds the file's own HIBLK, EFBLK, FFBYTE and stored HIGHWATER = HIBLK + 1. (VMS writes RSIZE 0 and HIBLK = the blocks the primary maps in
the extension headers of a file it GREW by one-block extends: ods2v2 never grows a file, it allocates in one go like COPY.)
The rule is checked on (1) the VMS-made F1, F2 and F3 containers of ods5-vms3 (stageb8/asim5-fd-*-after.img, phvs2-fd-*-after.img; skipped when absent): the reference; (2) the files
ods2v2 makes on a fragmented volume of each level (tests/setfile_ext_check.py's build: A.DAT with ods2v2's default record attributes, B.DAT with COPY/FORMAT=FIX512, about 400 pieces each:
a primary header and 4 or more extension headers). Run from the project root. Exit 1 on any FAIL. Written before the change (tests first): ods2v2 wrote the whole file's RSIZE, MAXREC,
HIBLK, EFBLK and FFBYTE in every extension header."""
import glob
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import setfile_ext_check as S   # noqa: E402

fails = 0


def check(desc, expected, got):
    global fails
    if expected == got:
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %s' % (expected,))
        print('      got:      %s' % (got,))


def swap(x):
    return ((x >> 16) | (x << 16)) & 0xFFFFFFFF


def attrs(h):
    return {'rtype': h[20], 'rattrib': h[21], 'rsize': struct.unpack_from('<H', h, 22)[0], 'maxrec': struct.unpack_from('<H', h, 34)[0], 'hiblk': swap(struct.unpack_from('<I', h, 24)[0]),
            'efblk': swap(struct.unpack_from('<I', h, 28)[0]), 'ffbyte': struct.unpack_from('<H', h, 32)[0], 'highwater': struct.unpack_from('<I', h, 76)[0]}


def rule(chain):
    """[] when the rule holds for a chain of (number, header): the problems otherwise"""
    prim = attrs(chain[0][1])
    bad = []
    for n, h in chain[1:]:
        a = attrs(h)
        want = {'rtype': prim['rtype'], 'rattrib': prim['rattrib'], 'rsize': prim['rsize'], 'maxrec': prim['maxrec'], 'hiblk': 0, 'efblk': 1, 'ffbyte': 0, 'highwater': 1}
        if a != want:
            bad.append('extension header %d: %s, want %s' % (n, a, want))
    return bad


def main():
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='exthdr_attr_', dir=base)
    try:
        d = os.path.expanduser('~/tmp/ods5-vms3/stageb8')
        refs = [(p, spec) for p, spec in ((os.path.join(d, 'asim5-fd-f1-2-after.img'), '[DST]F1.DAT'), (os.path.join(d, 'asim5-fd-f3-2-after.img'), '[DST]F3.DAT'),
                                          (os.path.join(d, 'asim5-fd-f2-2-after2.img'), '[DST]F2.DAT'), (os.path.join(d, 'phvs2-fd-f1-2-after.img'), '[DST]F1.DAT'),
                                          (os.path.join(d, 'asim5-fd-f1-5-after.img'), '[DST]F1.DAT')) if os.path.exists(p)]
        if not refs:
            print('SKIP: the VMS-made F1/F2/F3 containers of ods5-vms3 are not in %s' % d)
        for p, spec in refs:
            ch = E.chain(p, spec)
            check('the VMS-made %s (%d headers) has the rule' % (os.path.basename(p), len(ch)), [], rule(ch) if len(ch) >= 3 else ['no extension headers'])
        for level in (2, 5):
            img = S.build(level, work, fixed_b=True, odd_a=True)       # A.DAT 100 bytes short of a block: FFBYTE of its primary header is not 0
            for spec in ('[F]A.DAT', '[F]B.DAT'):
                ch = E.chain(img, spec)
                check('ODS-%d: %s: a primary header and %d extension headers made by ods2v2 have the rule' % (level, spec, len(ch) - 1), [], rule(ch) if len(ch) >= 4 else ['too few headers'])
            pa = attrs(E.chain(img, '[F]A.DAT')[0][1])
            check('ODS-%d: A.DAT has a nonzero FFBYTE in its primary header (so the extension headers writing 0 is a rule, not a coincidence): %d' % (level, pa['ffbyte']), True, pa['ffbyte'] != 0)
            check('ODS-%d: the primary header of A.DAT holds the file: HIBLK, EFBLK and stored HIGHWATER = HIBLK + 1' % level, True, pa['hiblk'] > 0 and pa['highwater'] == pa['hiblk'] + 1 and pa['efblk'] >= pa['hiblk'])
            ca, cb = attrs(E.chain(img, '[F]A.DAT')[0][1]), attrs(E.chain(img, '[F]B.DAT')[0][1])
            check('ODS-%d: B.DAT (COPY/FORMAT=FIX512) has RSIZE 512 in its primary header and so in every extension header' % level, (512, 1), (cb['rsize'], cb['rtype']))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('exthdr_attr_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


main()
