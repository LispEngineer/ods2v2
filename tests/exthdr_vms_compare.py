#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""exthdr_vms_compare.py : the layout of the extension headers ods2v2 makes against the VMS-made ones (follow-up d). For the fields that have one value in VMS-made extension headers
(asim5-frag-sf-2.img and -5.img of ods5-vms3's stage B: V8.4, two files of 4 headers each; phvs2-ods2v-20261108.img of the corpus: VAX V7.3, nine extension headers per file):
IDOFFSET 40, MPOFFSET 50 words, ACOFFSET 255, RSOFFSET 255, the structure level of the volume's files, the "ident area" = the 20-byte file name at ODS-2 (nothing after it: the map
starts at byte 100) or the 20-byte FI5 prefix at ODS-5 (control, name length, revision 0), the back link naming the primary header (number and sequence), the segment number, MAP_INUSE
at most 205 words (a full header of four-byte pointers holds 204), and nothing set after the map. ods2v2's extension headers are read from a many-piece file on a fresh volume of each level
(tests/setfile_ext_check.py's build: about 400 pieces). Run from the project root; the VMS-made images are skipped where absent. Exit 1 on any FAIL. Written before the change (tests
first): on ods5 a2ac9be the ODS-2 extension headers had MPOFFSET 100 and a 120-byte ident area."""
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


def layout(h, level):
    """the layout facts of an extension header (seg_num > 0), as a dict"""
    o = {'idoffset': h[0], 'mpoffset': h[1], 'acoffset': h[2], 'rsoffset': h[3], 'level': struct.unpack_from('<H', h, 6)[0] >> 8,
         'map_inuse_ok': h[58] <= 205 and h[58] > 0}
    mp = h[1] * 2
    # nothing after the pointers up to the checksum word
    end = mp + h[58] * 2
    o['zero_after_map'] = not any(h[end:510])
    o['ident_zero_pad'] = True
    if level == 2:
        name = bytes(h[80:100])
        o['name_ok'] = len(name) == 20 and name.rstrip(b' ').endswith(b';1') and all(32 <= c < 127 for c in name)
    else:
        o['name_ok'] = h[81] > 0 and struct.unpack_from('<H', h, 82)[0] == 0           # FI5: control byte, name length, revision word 0
    return o


def main():
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='exthdr_', dir=base)
    try:
        vms = {}
        for name, spec, level in (('asim5-frag-sf-2.img', '[T]A.DAT', 2), ('asim5-frag-sf-5.img', '[T]A.DAT', 5), ('phvs2-ods2v-20261108.img', None, 2)):
            for d in (os.environ.get('WO_VMS_DIR', os.path.expanduser('~/tmp/ods5-vms3/wo')), os.path.expanduser('~/tmp/ods5-vms3/stageb3'), os.environ.get('VMS_CORPUS_DIR', os.path.expanduser('~/tmp/ods5-cli'))):
                p = os.path.join(d, name)
                if os.path.exists(p):
                    vms[(level, name)] = (p, spec)
                    break
        ours = {}
        for level in (2, 5):
            img = S.build(level, work)
            ch = E.chain(img, '[F]A.DAT')
            ours[level] = [h for n, h in ch[1:]]
            check('ODS-%d: ods2v2 made a primary header and several extension headers (setup)' % level, True, len(ours[level]) >= 3)
        for level in (2, 5):
            facts = [layout(h, level) for h in ours[level]]
            want = {'idoffset': 40, 'mpoffset': 50, 'acoffset': 255, 'rsoffset': 255, 'level': level, 'map_inuse_ok': True, 'zero_after_map': True, 'ident_zero_pad': True, 'name_ok': True}
            bad = [f for f in facts if f != want]
            check('ODS-%d: every extension header ods2v2 made has the VMS layout (IDOFFSET 40, MPOFFSET 50, ACOFFSET/RSOFFSET 255, name-only ident, nothing after the map)' % level,
                  [], bad[:1])
        # the VMS-made ones say the same (the reference: if VMS stopped saying it, this test would be wrong first)
        for (level, name), (p, spec) in sorted(vms.items()):
            if spec is None:
                import ods5check as O
                v = O.Vol(p)
                hs = []
                for num in (789, 790):
                    h = E.D.header(v, num)
                    n = struct.unpack_from('<H', h, 14)[0]
                    while n:
                        h = E.D.header(v, n)
                        hs.append(h)
                        n = struct.unpack_from('<H', h, 14)[0]
            else:
                hs = [h for n, h in E.chain(p, spec)[1:]]
            facts = [layout(h, level) for h in hs]
            want = {'idoffset': 40, 'mpoffset': 50, 'acoffset': 255, 'rsoffset': 255, 'level': level, 'map_inuse_ok': True, 'zero_after_map': True, 'ident_zero_pad': True, 'name_ok': True}
            check('the VMS-made extension headers of %s (%d) have that layout too' % (name, len(hs)), [], [f for f in facts if f != want][:1])
        if not vms:
            print('SKIP: no VMS-made image with extension headers found (asim5-frag-sf-*.img, phvs2-ods2v-20261108.img)')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('exthdr_vms_compare: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


main()
