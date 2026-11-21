#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""zerofill_check.py : whatever ods2v2 makes must not show the old contents of free blocks (review 56 F1). The stored HIGHWATER of a new directory or file is HIBLK + 1, which says
every allocated block was written: so every allocated block must hold what was written, the data or zeros, never what a deleted file left on the disk.
For both levels and the clusters 8 and 64: the free space is filled with 0xA5 (a file of that byte is copied in and deleted), then CREATE/DIRECTORY, a sub-directory made by
IMPORT, a file and a file of several clusters are made; every allocated block of each is read from the image by an independent reader (ods5check's volume reader, no ods2v2 code)
and compared: a directory is its entries in block 1 (no 0xA5 byte among them) and zeros in every other block; a file is its bytes and zeros after them; and the header's HIGHWATER is HIBLK + 1.
Run from the project root (./ods2, ./ods2_init, tests/ods5check.py). Prints PASS/FAIL lines, exit 1 on any FAIL. Written before the fix (tests first): on ods5-dirhdr a1ec409 a new
directory at ODS-2 with a cluster over 1 kept 0xA5 in blocks 2 to the end of its allocation."""
import os
import re
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


def ods2(img, *cmds):
    p = subprocess.run(['./ods2', img] + list(cmds), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def script(img, cmds, work):
    path = os.path.join(work, 'cmds.txt')
    with open(path, 'w') as f:
        f.write('\n'.join(cmds) + '\n')
    p = subprocess.run(['./ods2', img, '@' + path], capture_output=True, text=True)
    return [ln for ln in (p.stdout + p.stderr).split('\n') if ln.startswith('%ODS2-E-') or ln.startswith('%ODS2-F-')]


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


def swap(x):
    return ((x >> 16) | (x << 16)) & 0xFFFFFFFF


def blocks_of(img, spec):
    """(header, [the 512-byte blocks of the whole allocation, in VBN order]) of a file, read from the image"""
    n = fnum(img, spec)
    assert n is not None, spec
    v = O.Vol(img)
    h = header(v, n)
    ext = O.decode_map(h, O.Findings(False), spec)
    data = []
    with open(img, 'rb') as f:
        for lbn, cnt in ext:
            f.seek(lbn * 512)
            raw = f.read(cnt * 512)
            data += [raw[i * 512:(i + 1) * 512] for i in range(cnt)]
    return h, data


def build(level, cluster, work):
    img = os.path.join(work, 'z%d-%d.img' % (level, cluster))
    p = subprocess.run(['./ods2_init', img, '32768', '--structure=%d' % level, '--label=ZFILL', '--cluster=%d' % cluster], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert not script(img, ['CREATE/DIRECTORY [T]'], work)
    rc, out = ods2(img, 'SHOW VOLUME')
    free = [int(ln.split()[2]) for ln in out.split('\n') if 'Free blocks:' in ln][0]
    fill = os.path.join(work, 'fill%d-%d.bin' % (level, cluster))
    with open(fill, 'wb') as f:
        f.write(b'\xa5' * ((free - 400) * 512))
    errs = script(img, ['COPY %s [T]FILL.BIN' % fill], work)
    assert not errs, errs[:2]
    os.remove(fill)
    assert not script(img, ['DELETE [T]FILL.BIN;1'], work)
    return img


def main():
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='zerofill_', dir=base)
    try:
        for level in (2, 5):
            for cluster in (8, 64):
                tag = 'ODS-%d cluster %d' % (level, cluster)
                img = build(level, cluster, work)
                imp = os.path.join(work, 'imp%d-%d' % (level, cluster))
                os.makedirs(os.path.join(imp, 'SUB'))
                body = bytes((i * 7 + 3) & 0xFF for i in range(1300))
                big = bytes((i * 11 + 5) & 0xFF for i in range(cluster * 512 * 2 + 700))
                with open(os.path.join(imp, 'SUB', 'IN.DAT'), 'wb') as f:
                    f.write(body[:100])
                for nm, data in (('S.DAT', body), ('B.DAT', big)):
                    with open(os.path.join(work, nm + str(level) + str(cluster)), 'wb') as f:
                        f.write(data)
                errs = script(img, ['CREATE/DIRECTORY [D1]', 'CREATE/DIRECTORY [IMP]', 'IMPORT %s [IMP]' % imp, 'COPY %s [D1]S.DAT' % os.path.join(work, 'S.DAT' + str(level) + str(cluster)),
                                    'COPY %s [D1]B.DAT' % os.path.join(work, 'B.DAT' + str(level) + str(cluster))], work)
                check('%s: the objects were made' % tag, [], errs)
                # the free space really held 0xA5 where these were made: the old contents are in the free blocks before the writes (sanity of the test itself)
                with open(img, 'rb') as f:
                    raw = f.read()
                check('%s: the volume has 0xA5 blocks left in its free space (the test can see stale data)' % tag, True, raw.count(b'\xa5' * 512) > 1000)
                for spec, kind, data in (('[000000]D1.DIR', 'dir', None), ('[000000]IMP.DIR', 'dir', None), ('[IMP]SUB.DIR', 'dir', None),
                                         ('[D1]S.DAT', 'file', body), ('[D1]B.DAT', 'file', big), ('[IMP.SUB]IN.DAT', 'file', body[:100])):
                    h, bl = blocks_of(img, spec)
                    hiblk = swap(struct.unpack_from('<I', h, 24)[0])
                    hw = struct.unpack_from('<I', h, 76)[0]
                    if kind == 'dir':
                        expect = [bl[0]] + [bytes(512)] * (len(bl) - 1)      # block 1 holds the entries (checked below for stale bytes); every other block is zeros
                        if b'\xa5' in bl[0]:
                            check('%s: %s: block 1 holds no old 0xA5 byte' % (tag, spec), False, True)
                    else:
                        raw = data + bytes(len(bl) * 512 - len(data))
                        expect = [raw[i * 512:(i + 1) * 512] for i in range(len(bl))]
                    bad = [i + 1 for i, b in enumerate(bl) if b != expect[i]]
                    check('%s: %s: all %d allocated blocks hold what was written (no old 0xA5 data)' % (tag, spec, len(bl)), [], bad[:8])
                    check('%s: %s: HIGHWATER is 0 or HIBLK + 1 (%d)' % (tag, spec, hiblk + 1), True, hw in (0, hiblk + 1))
                r = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True).stdout.strip().split('\n')[-1]
                check('%s: ods5check is clean' % tag, '0 error(s), 0 warning(s)', r)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('zerofill_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


main()
