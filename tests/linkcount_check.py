#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""linkcount_check.py - ods5check warns when a file's link count on a HARDLINKS volume is not the number of directory entries that name it (review 67 F4). On the five volumes VMS wrote
(a3-links-5, a8-vmsinit-ods5t, a1-names-5, ODS5T, ODS5T6: 4,185 files) the two are equal. A RENAME stopped after its first write leaves the count one too high (the new entry is not there
yet), and so does one stopped after the old entry went and before the count came back; ods2v2 chooses too high: with the count too low a DELETE of one name would free a file another
name still enters. The warning is the checker's way of showing it. Run from the project root (./ods2, ./ods2_init, tests/ods5check.py)."""
import os
import re
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


def warnings(img):
    rc, out = run(['python3', '-I', 'tests/ods5check.py', img])
    return [l for l in out.splitlines() if 'link count' in l and 'directory entr' in l], out.strip().splitlines()[-1]


def set_count(img, spec, n):
    num = E.D.fnum(img, spec)
    v = O.Vol(img)
    lbn = E.D.lbn(v, num)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        h = bytearray(f.read(512))
        struct.pack_into('<H', h, 74, n)
        struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(h))


def main():
    work = tempfile.mkdtemp(prefix='linkcount_check_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for hl in (True, False):
            tag = 'ODS-5 HARDLINKS' if hl else 'ODS-5 without HARDLINKS'
            img = os.path.join(work, 'v%d.img' % hl)
            assert run(['./ods2_init', img, '8192', '--structure=5', '--label=LC'] + (['--hardlinks'] if hl else []))[0] == 0
            src = os.path.join(work, 's.txt')
            open(src, 'w').write('x\n')
            for c in ('CREATE/DIRECTORY [D]', 'COPY %s [D]A.TXT' % src, 'COPY %s [D]B.TXT' % src, 'LINK [D]AL.TXT [D]A.TXT;1', 'LINK [D]AL2.TXT [D]A.TXT;1'):
                rc, out = run(['./ods2', img, c])
                assert rc == 0 and '%ODS2-E' not in out, (c, out)
            w, last = warnings(img)
            check('%s: a file with a link and an alias (count and entries agree): no warning' % tag, [], w)
            if not hl:
                set_count(img, '[D]A.TXT;1', 5)
                check('%s: a count 5 on a volume that keeps none is not judged here (its own rule: VMS writes 0)' % tag, 0, len(warnings(img)[0]))
                continue
            import copy
            for desc, n, want in (('count one too high (a RENAME stopped after its first write)', 4, 'link count 4 but 3 directory entries'), ('count one too low', 2, 'link count 2 but 3 directory entries')):
                bad = os.path.join(work, 'bad.img')
                shutil.copyfile(img, bad)
                set_count(bad, '[D]A.TXT;1', n)
                w, last = warnings(bad)
                check('%s: %s: one warning naming both numbers' % (tag, desc), (1, True), (len(w), bool(w) and want in w[0]))
                check('%s: %s: no error, the warning only' % (tag, desc), True, last.startswith('0 error(s), 1 warning'))
            # a delete that takes the count down with the entry: agree again
            run(['./ods2', img, 'DELETE [D]AL2.TXT'])
            w, last = warnings(img)
            check('%s: after DELETE of one link the count and the entries agree again' % tag, [], w)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('linkcount_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
