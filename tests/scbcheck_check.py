#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""scbcheck_check.py : ods5check checks the storage control block (BITMAP.SYS VBN 1) against the home block (review 57 item 4): the structure level and the cluster factor must be
the home block's, the volume size must not lie past the end of the image, the checksum word (the sum of words 0 to 254) must be right and the block-size field must be 1, as INITIALIZE
writes it (every VMS-made SCB of the corpus; set_volume_geometry). VMS V7.3 answers a wrong SCB with %ANALDISK-W-CHKSCB, and neither ods5check nor SHOW VOLUME looked at these fields.
For both levels and the clusters 1 and 8: a new volume is clean; each field made wrong in a copy (the checksum kept right except in its own case) is an ERROR that names the field; a
placeholder SCB (the starter volume's all-ones one) is not an error; the VMS-made volumes of the corpus, where present, stay clean. Run from the project root. Exit 1 on any FAIL.
Written before the check (tests first): on ods5 a2ac9be the five wrong SCBs gave 0 errors."""
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402

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


def run_check(img):
    p = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def scb_lbn(img):
    v = O.Vol(img)
    h1 = v.blk(v.ibmaplbn + v.ibmapsize)
    ix = O.decode_map(h1, O.Findings(False), 'indexf')
    left = O.u16(v.home, 22) + O.u16(v.home, 32)
    for lbn, cnt in ix:
        if left < cnt:
            h = v.blk(lbn + left)
            break
        left -= cnt
    ext = O.decode_map(h, O.Findings(False), 'bitmap')
    return ext[0][0]


def patch(img, fn, keep_checksum=True):
    lbn = scb_lbn(img)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        b = bytearray(f.read(512))
        fn(b)
        if keep_checksum:
            struct.pack_into('<H', b, 510, sum(struct.unpack_from('<255H', b, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(b)


def main():
    work = tempfile.mkdtemp(prefix='scbcheck_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for level in (2, 5):
            for cluster in (1, 8):
                tag = 'ODS-%d cluster %d' % (level, cluster)
                base = os.path.join(work, 'b%d-%d.img' % (level, cluster))
                p = subprocess.run(['./ods2_init', base, '16384', '--structure=%d' % level, '--label=SCBT', '--cluster=%d' % cluster], capture_output=True, text=True)
                assert p.returncode == 0, p.stdout + p.stderr
                rc, o = run_check(base)
                check('%s: a new volume has a sound SCB and 0 errors' % tag, (0, True), (rc, '0 error(s)' in o))
                cases = (('structure level 0', lambda b: struct.pack_into('<H', b, 0, 0), True, 'structure level'),
                         ('cluster factor 9', lambda b: struct.pack_into('<H', b, 2, 9), True, 'cluster factor'),
                         ('block-size field 0', lambda b: struct.pack_into('<I', b, 8, 0), True, 'block-size'),
                         ('volume size past the end of the image', lambda b: struct.pack_into('<I', b, 4, 16384 + 1000), True, 'volume size'),
                         ('a wrong checksum word', lambda b: struct.pack_into('<H', b, 510, struct.unpack_from('<H', b, 510)[0] ^ 0x00ff), False, 'checksum'))
                for what, fn, keep, word in cases:
                    img = os.path.join(work, 'w%d-%d.img' % (level, cluster))
                    shutil.copyfile(base, img)
                    patch(img, fn, keep)
                    rc, o = run_check(img)
                    errs = [ln for ln in o.split('\n') if ln.startswith('ERROR') and 'SCB' in ln]
                    check('%s: %s is an error that names the field' % (tag, what), (1, True), (rc, any(word in e for e in errs)))
                # the starter volume's placeholder SCB (volume size all ones): not an error
                img = os.path.join(work, 'ph%d-%d.img' % (level, cluster))
                shutil.copyfile(base, img)
                patch(img, lambda b: struct.pack_into('<I', b, 4, 0xFFFFFFFF), False)
                rc, o = run_check(img)
                check('%s: a placeholder SCB (volume size all ones) is not an SCB error' % tag, [], [ln for ln in o.split('\n') if ln.startswith('ERROR') and 'SCB' in ln])
        # the VMS-made volumes of the corpus stay clean
        c = os.environ.get('VMS_CORPUS_DIR', os.path.expanduser('~/tmp/ods5-cli'))
        d = os.environ.get('WO_VMS_DIR', os.path.expanduser('~/tmp/ods5-vms3/wo'))
        for img in (os.path.join(c, 'asim12-ods2t-20261108.img'), os.path.join(c, 'asim12-ods5t-20261108.img'), os.path.join(c, 'asim6-ods2t6-20261108.img'),
                    os.path.join(d, 'img', 'phvs2-copy-2.img'), os.path.join(d, 'a5', 'img', 'asim5-copy-5.img')):
            if not os.path.exists(img):
                print('SKIP: %s not found' % img)
                continue
            rc, o = run_check(img)
            check('the VMS-made volume %s has no SCB error' % os.path.basename(img), [], [ln for ln in o.split('\n') if ln.startswith('ERROR') and 'SCB' in ln])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('scbcheck_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


main()
