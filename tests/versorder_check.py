#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""versorder_check.py - a name whose versions are stored OUT OF ORDER (;1 ;2 ;3 in the directory record: written by another tool or damaged; ods2v2 and VMS never write it), H1 of
review 50, settled by CORPUS 66.4 (VMS V8.4 and VAX V7.3, ODS-2 and ODS-5, the images fe-h1-2 and fe-h1-5). VMS follows the STORED order everywhere:
  * TYPE with no version, ;0 and ;-1 take the first and the second entry as stored (not the highest numbers); DIRECTORY lists in stored order;
  * DELETE with no version is refused (%DELETE-E-DELVER); PURGE keeps the FIRST STORED entry; SET FILE with no version changes the first stored;
  * ANALYZE says %ANALDISK-W-BAD_VERSORDER "version ordering incorrect" and ANALYZE/REPAIR changes nothing.
ods2v2's rule (this test), a deliberate divergence from VMS for the commands that change something, so that no file is lost on a damaged directory:
  * reads (TYPE, COPY from, DIRECTORY, EXPORT) keep the stored-order lookup, which is VMS's;
  * a command that changes something (DELETE, SET FILE, PURGE; RENAME when it exists) on a name whose versions are out of order is REFUSED, the volume unchanged, unless it names an
    explicit version number (;n) or all versions (;*): then it acts as it is told. A name in the proper order works as always. A refused name does not stop the others of a wildcard;
  * ods5check calls it BAD_VERSORDER (VMS's word), once for each pair of versions that is not descending, in a record and across the records of a name.
The directories are made by ods2v2 (versions inserted newest first, as it insists) and then patched on the image: [D]X.TXT;1 ;2 ;3 stored ascending in one record, [D]Z.TXT;3 ;2 ;1 in order
(the control); [DY]Y.TXT;1 to ;70 as two records in two blocks, the block of the low versions FIRST. Every version is a different one-line file ("X VERSION 2"), so a TYPE shows which one.
Usage: versorder_check.py   (run from the project root, with ./ods2 and ./ods2_init built; TMPDIR holds the scratch images)."""
import hashlib
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

ODS2 = './ods2'
INIT = './ods2_init'
fails = 0
passes = 0


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


def ods2(img, cmd):
    return run([ODS2, img, cmd])


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def dir_lbns(img, spec):
    num = E.D.fnum(img, spec)
    v = O.Vol(img)
    h = E.D.header(v, num)
    return [lbn + i for lbn, cnt in O.decode_map(h, O.Findings(False), 'dir') for i in range(cnt)]


def records(block):
    out, pos = [], 0
    while pos + 6 <= 512:
        size = struct.unpack_from('<H', block, pos)[0]
        if size == 0xFFFF or size == 0:
            break
        nc = block[pos + 5]
        out.append((pos, size + 2, bytes(block[pos + 6:pos + 6 + nc]).decode('latin-1')))
        pos += size + 2
    return out


def patch(img, kind):
    """X's entries ascending (one record); Y alone in [DY]: its two blocks swapped"""
    with open(img, 'r+b') as f:
        if kind == 'X':
            lbns = dir_lbns(img, '[000000]D.DIR')
            for lb in lbns:
                f.seek(lb * 512)
                blk = bytearray(f.read(512))
                for pos, size, nm in records(blk):
                    if nm.upper() == 'X.TXT':
                        nc = blk[pos + 5]
                        ent = pos + 6 + nc
                        ent += ent % 2
                        es = [bytes(blk[i:i + 8]) for i in range(ent, pos + size, 8)]
                        es.reverse()
                        blk[ent:pos + size] = b''.join(es)
                        f.seek(lb * 512)
                        f.write(bytes(blk))
        else:
            lbns = dir_lbns(img, '[000000]DY.DIR')
            assert len(lbns) >= 2, 'Y.TXT must fill two blocks'
            f.seek(lbns[0] * 512)
            b0 = f.read(512)
            f.seek(lbns[1] * 512)
            b1 = f.read(512)
            f.seek(lbns[0] * 512)
            f.write(b1)
            f.seek(lbns[1] * 512)
            f.write(b0)


def build(work, level):
    img = os.path.join(work, 'h1_%d.img' % level)
    rc, out = run([INIT, img, '8192', '--structure=%d' % level, '--label=VERSORDER'])
    assert rc == 0, out
    cmds = ['CREATE/DIRECTORY [D]', 'CREATE/DIRECTORY [DY]']
    for nm, vers in (('X', (3, 2, 1)), ('Z', (3, 2, 1)), ('Y', tuple(range(70, 0, -1)))):
        for v in vers:                              # newest first: ods2v2 refuses to insert a version that is not lower than every one there
            p = os.path.join(work, '%s%d.txt' % (nm, v))
            open(p, 'w').write('%s VERSION %d\n' % (nm, v))
            cmds.append('COPY %s [%s]%s.TXT;%d' % (p, 'DY' if nm == 'Y' else 'D', nm, v))
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    patch(img, 'X')
    patch(img, 'Y')
    return img


def fresh(base, work, tag):
    img = os.path.join(work, 'w_%s.img' % re.sub(r'\W', '_', tag))
    if os.path.exists(img):
        os.remove(img)
    shutil.copyfile(base, img)
    return img


def first_line(out):
    ls = [l for l in out.strip().splitlines() if l.strip() and not l.startswith('%')]
    return ls[-1].strip() if ls else ''


def listing(img, spec):
    out = ods2(img, 'DIR ' + spec)[1]
    return [m.group(1) + ';' + m.group(2) for m in (re.match(r'^(\S+?)\s*;(\d+)\s*$', l) for l in out.splitlines()) if m]


def refused(out):
    return 'ILLCMD' not in out and ('BAD_VERSORDER' in out or 'out of order' in out)


def main():
    work = tempfile.mkdtemp(prefix='versorder_check_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for level in (2, 5):
            kind = 'ODS-%d' % level
            base = build(work, level)
            t = lambda s: '%s %s' % (kind, s)
            # reads follow the stored order, as VMS does (CORPUS 66.4)
            img = fresh(base, work, 'r')
            check(t('TYPE X.TXT (no version) shows the FIRST STORED entry'), 'X VERSION 1', first_line(ods2(img, 'TYPE [D]X.TXT')[1]))
            check(t('TYPE X.TXT;0 shows the first stored entry'), 'X VERSION 1', first_line(ods2(img, 'TYPE [D]X.TXT;0')[1]))
            check(t('TYPE X.TXT;-1 shows the SECOND stored entry'), 'X VERSION 2', first_line(ods2(img, 'TYPE [D]X.TXT;-1')[1]))
            check(t('TYPE X.TXT;1 shows version 1'), 'X VERSION 1', first_line(ods2(img, 'TYPE [D]X.TXT;1')[1]))
            ly = listing(img, '[DY]Y.TXT;*')
            check(t('TYPE Y.TXT (two records, the block of the low versions first) shows the first stored entry'), 'Y VERSION %s' % ly[0].split(';')[1], first_line(ods2(img, 'TYPE [DY]Y.TXT')[1]))
            check(t('TYPE Z.TXT (in order) shows ;3, ;-1 shows ;2'), ('Z VERSION 3', 'Z VERSION 2'), (first_line(ods2(img, 'TYPE [D]Z.TXT')[1]), first_line(ods2(img, 'TYPE [D]Z.TXT;-1')[1])))
            check(t('DIRECTORY lists X in the stored order'), ['X.TXT;1', 'X.TXT;2', 'X.TXT;3'], listing(img, '[D]X.TXT;*'))
            # changes are refused unless a version is named
            for desc, cmd in (('DELETE X.TXT', 'DELETE [D]X.TXT'), ('DELETE X.TXT;0', 'DELETE [D]X.TXT;0'), ('DELETE X.TXT;-1', 'DELETE [D]X.TXT;-1'),
                              ('SET FILE/PROTECTION X.TXT', 'SET FILE/PROTECTION=(W:R) [D]X.TXT'), ('SET FILE/PROTECTION X.TXT;0', 'SET FILE/PROTECTION=(W:R) [D]X.TXT;0'),
                              ('SET FILE/OWNER_UIC X.TXT', 'SET FILE/OWNER_UIC=[200,3] [D]X.TXT'), ('PURGE X.TXT', 'PURGE [D]X.TXT'), ('PURGE/KEEP=2 X.TXT', 'PURGE/KEEP=2 [D]X.TXT'),
                              ('PURGE Y.TXT (two records)', 'PURGE [DY]Y.TXT'), ('PURGE X.TXT;*', 'PURGE [D]X.TXT;*'), ('DELETE *.TXT (the wildcard refuses X)', 'DELETE [D]X*.TXT')):
                img = fresh(base, work, desc)
                h0 = sha(img)
                rc, out = ods2(img, cmd)
                check(t('%s is refused (versions out of order: BAD_VERSORDER) and the image is unchanged' % desc), (True, h0), (refused(out), sha(img)))
            # an explicit version, or all versions, acts as it is told
            img = fresh(base, work, 'del2')
            rc, out = ods2(img, 'DELETE [D]X.TXT;2')
            check(t('DELETE X.TXT;2 (explicit) deletes ;2 only'), (['X.TXT;1', 'X.TXT;3'], True), (listing(img, '[D]X.TXT;*'), 'ILLCMD' not in out and not refused(out)))
            img = fresh(base, work, 'del3')
            rc, out = ods2(img, 'DELETE [D]X.TXT;3')
            check(t('DELETE X.TXT;3 (explicit: VMS says file not found, ods2v2 finds it as listed and deletes it) works'), ['X.TXT;1', 'X.TXT;2'], listing(img, '[D]X.TXT;*'))
            img = fresh(base, work, 'delall')
            ods2(img, 'DELETE [D]X.TXT;*')
            check(t('DELETE X.TXT;* deletes all three (as VMS)'), [], listing(img, '[D]X.TXT;*'))
            img = fresh(base, work, 'delwild')
            rc, out = ods2(img, 'DELETE [D]*.TXT')
            check(t('DELETE [D]*.TXT: X is refused and stays whole, Z (in order) loses its highest version'), (True, ['X.TXT;1', 'X.TXT;2', 'X.TXT;3'], ['Z.TXT;1', 'Z.TXT;2']),
                  (refused(out), listing(img, '[D]X.TXT;*'), listing(img, '[D]Z.TXT;*')))
            img = fresh(base, work, 'setf2')
            ods2(img, 'SET FILE/PROTECTION=(W:R) [D]X.TXT;2')
            prot = {v: re.search(r'Protection:\s+(\(.*\))', ods2(img, 'DIR/FULL [D]X.TXT;%d' % v)[1]) for v in (1, 2, 3)}
            check(t('SET FILE/PROTECTION X.TXT;2 (explicit) changes ;2 only'), (True, False, False), tuple('W:R' in (prot[v].group(1) if prot[v] else '') for v in (1, 2, 3)))
            # a name in the proper order works as always
            img = fresh(base, work, 'purgez')
            rc, out = ods2(img, 'PURGE [D]Z.TXT')
            check(t('PURGE Z.TXT (in order) keeps ;3'), (['Z.TXT;3'], False), (listing(img, '[D]Z.TXT;*'), refused(out)))
            img = fresh(base, work, 'copyx')
            h0 = sha(img)
            rc, out = ods2(img, 'COPY %s [D]X.TXT' % os.path.join(work, 'X1.txt'))
            check(t('COPY into X.TXT (a new version) is refused and the image is unchanged (it would be stored out of order)'), (True, h0), ('ILLCMD' not in out and 'ODS2-E' in out, sha(img)))
            # ods5check names it after VMS
            rc, out = run(['python3', '-I', 'tests/ods5check.py', base])
            lines = [l for l in out.splitlines() if 'BAD_VERSORDER' in l]
            check(t('ods5check says BAD_VERSORDER for X (2 pairs) and Y (1 pair, across its two records), nothing for Z'),
                  (3, 0, 0), (len(lines), sum(1 for l in lines if 'Z.TXT' in l), sum(1 for l in out.splitlines() if 'versions not descending' in l)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('versorder_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
