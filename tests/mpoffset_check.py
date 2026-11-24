#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""mpoffset_check.py - ods5check's rule for the map offset of an ODS-5 primary header (CORPUS 67.5, V8.4): the map starts AT OR ABOVE what the name needs (a long name renamed to a short one
keeps its MPOFFSET: the ident area keeps its length and the name field is zero-filled to its end); BELOW it is an error, and so is anything but zeros in the slack. The files here are made
by ods2v2 (a 62-byte name renamed to a short one by ods2 RENAME is the real case: tests/rename_check.py n1, n3) and patched on the image. Run from the project root."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import ods5check as O     # noqa: E402

fails = passes = 0


def check(desc, expected, got):
    global fails, passes
    if expected == got:
        passes += 1
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %r' % (expected,))
        print('      got:      %r' % (got,))


def run(args):
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return p.returncode, p.stdout + p.stderr


def patch(img, spec, fn):
    num = E.D.fnum(img, spec)
    v = O.Vol(img)
    lbn = E.D.lbn(v, num)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        h = bytearray(f.read(512))
        fn(h)
        struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(h))


def errors(img):
    rc, out = run(['python3', '-I', 'tests/ods5check.py', img])
    return [l for l in out.splitlines() if l.startswith('ERROR')]


def main():
    work = tempfile.mkdtemp(prefix='mpoffset_check_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        base = os.path.join(work, 'v.img')
        assert run(['./ods2_init', base, '8192', '--structure=5', '--label=MPO'])[0] == 0
        src = os.path.join(work, 's.txt')
        open(src, 'w').write('x\n')
        for c in ('CREATE/DIRECTORY [D]', 'COPY %s [D]%s' % (src, 'L' * 58 + '.TXT'), 'COPY %s [D]SHORT.TXT' % src):
            rc, out = run(['./ods2', base, c])
            assert rc == 0 and '%ODS2-E' not in out, (c, out)
        check('the long name (62 bytes, MPOFFSET 109) as ods2v2 makes it: clean', [], errors(base))
        img = os.path.join(work, 'a.img')
        shutil.copyfile(base, img)
        rc, out = run(['./ods2', img, 'RENAME [D]%s [D]OTHER.TXT' % ('L' * 58 + '.TXT')])
        check('after ods2 RENAME of it to a short name (MPOFFSET stays 109, the name field zero-filled): clean', (0, []), (rc, errors(img)))
        # MPOFFSET below what the name needs
        shutil.copyfile(base, img)

        def lower(h):
            h[1] = h[1] - 2
        patch(img, '[D]%s;1' % ('L' * 58 + '.TXT'), lower)
        e = errors(img)
        check('MPOFFSET below what the name needs: an error', True, any('at least' in x and 'MPOFFSET' in x for x in e))
        # anything but zeros after the name
        shutil.copyfile(base, img)
        rc, out = run(['./ods2', img, 'RENAME [D]%s [D]OTHER.TXT' % ('L' * 58 + '.TXT')])

        def dirty(h):
            ido = h[0] * 2
            h[ido + 76 + 12 + 3] = 0x41                  # a byte of the old name left behind
        patch(img, '[D]OTHER.TXT;1', dirty)
        e = errors(img)
        check('a byte left after the name in the slack of the ident area: an error', True, any('not zero after the name' in x for x in e))
        # MPOFFSET above what the name needs, slack zero (the VMS state after long to short): clean (the first rename above)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('mpoffset_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
