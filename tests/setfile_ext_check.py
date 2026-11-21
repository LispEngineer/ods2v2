#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""setfile_ext_check.py : SET FILE/OWNER_UIC on a file with extension headers (CORPUS 56.2, OpenVMS Alpha V8.4, ODS-2 and ODS-5): VMS writes the new owner into EVERY
extension header and leaves their FILEPROT alone; the protection change touches the primary header only. Also: the primary header's revision goes up by 1 per command, the
header checksums are right, a second fragmented file that was not named is not touched, the file's bytes read back as before, ods5check is clean.
Run from the project root (./ods2, ./ods2_init, tests/ods5check.py). Prints PASS/FAIL lines; exit status 1 on any FAIL. Written before the change (tests first): on ods5-ops234
2ce9fa6 SET FILE/OWNER_UIC changed the primary header only."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402

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
    errs = [ln for ln in (p.stdout + p.stderr).split('\n') if ln.startswith('%ODS2-E-') or ln.startswith('%ODS2-F-')]
    return errs


def build(level, work, fixed_b=False, odd_a=False):
    """a volume with two files in about 400 pieces each (primary header and 3 extension headers each): A.DAT and B.DAT in [F]"""
    img = os.path.join(work, 'ext%d.img' % level)
    p = subprocess.run(['./ods2_init', img, '8192', '--structure=%d' % level, '--label=EXTSET', '--cluster=1', '--maxfiles=2600'], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    nfill = 1600
    d = os.path.join(work, 'fill%d' % level)
    os.makedirs(d)
    for i in range(nfill):
        with open(os.path.join(d, 'F%05d.DAT' % i), 'wb') as f:
            f.write(bytes(3 * 512))
    errs = script(img, ['CREATE/DIRECTORY [F]', 'CREATE/DIRECTORY [FILL]', 'IMPORT %s [FILL]' % d], work)
    assert not errs, errs[:3]
    rc, out = ods2(img, 'SHOW VOLUME')
    free = [int(ln.split()[2]) for ln in out.split('\n') if 'Free blocks:' in ln][0]
    filler = os.path.join(work, 'filler%d.bin' % level)
    with open(filler, 'wb') as f:
        f.truncate(free * 512)
    assert not script(img, ['COPY %s [000000]FILLER.BIN' % filler], work)
    os.remove(filler)
    assert not script(img, ['DELETE [FILL]F%05d.DAT;1' % i for i in range(0, nfill, 2)], work)
    holes = nfill // 2
    pieces = holes // 2          # each of the two files takes half of the holes: 400 pieces, a primary header and 3 extension headers or more
    for nm, seed in (('A.DAT', 1), ('B.DAT', 2)):
        p = os.path.join(work, nm + str(level))
        with open(p, 'wb') as f:
            f.write(bytes((i * 7 + seed + (i >> 8)) & 0xFF for i in range(pieces * 3 * 512 - (100 if (odd_a and nm == 'A.DAT') else 0))))
        assert not script(img, ['COPY%s %s [F]%s' % ('/FORMAT=FIX512' if (fixed_b and nm == 'B.DAT') else '', p, nm)], work)
    assert not script(img, ['DELETE [000000]FILLER.BIN;1'], work)
    return img


def md5_of(img, spec, work, tag):
    out = os.path.join(work, 'ex_' + tag)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    ods2(img, 'EXPORT %s %s' % (spec, out))
    p = os.path.join(out, os.listdir(out)[0]) if os.listdir(out) else None
    return hashlib.md5(open(p, 'rb').read()).hexdigest() if p else None


def main():
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='setfile_ext_', dir=base)
    try:
        for level in (2, 5):
            img = build(level, work)
            tag = 'ODS-%d' % level
            a0 = [E.describe(n, h) for n, h in E.chain(img, '[F]A.DAT')]
            b0 = [E.describe(n, h) for n, h in E.chain(img, '[F]B.DAT')]
            check('%s: A.DAT has a primary header and at least 3 extension headers (setup)' % tag, True, len(a0) >= 4)
            check('%s: so does B.DAT, the file that is not named (setup)' % tag, True, len(b0) >= 4)
            check('%s: the owner of every header of A.DAT is [1,4] to begin with' % tag, True, all(x['owner'] == '[1,4]' for x in a0))
            md0 = md5_of(img, '[F]A.DAT', work, 'a0')
            # 1. the owner
            rc, out = ods2(img, 'SET FILE/OWNER_UIC=[200,3] [F]A.DAT')
            check('%s: SET FILE/OWNER_UIC succeeds' % tag, 0, rc)
            a1 = [E.describe(n, h) for n, h in E.chain(img, '[F]A.DAT')]
            check('%s: the owner is [200,3] in the primary header and in every extension header' % tag, ['[200,3]'] * len(a0), [x['owner'] for x in a1])
            check('%s: the protection of every header is as it was (FILEPROT of the extension headers stays)' % tag, [x['fileprot'] for x in a0], [x['fileprot'] for x in a1])
            check('%s: every header checksum is right' % tag, ['ok'] * len(a0), [x['checksum'] for x in a1])
            check('%s: the primary header has revision +1' % tag, a0[0]['revision'] + 1, a1[0]['revision'])
            check('%s: the chain is the same headers in the same order' % tag, [x['num'] for x in a0], [x['num'] for x in a1])
            b1 = [E.describe(n, h) for n, h in E.chain(img, '[F]B.DAT')]
            check('%s: B.DAT (not named) is not touched in any header' % tag, b0, b1)
            check('%s: the bytes of A.DAT read back as before' % tag, md0, md5_of(img, '[F]A.DAT', work, 'a1'))
            r = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True).stdout.strip().split('\n')[-1]
            check('%s: ods5check is clean' % tag, '0 error(s), 0 warning(s)', r)
            # 2. the protection only: the extension headers are not touched
            before = [(x['owner'], x['fileprot'], x['revision']) for x in a1]
            rc, out = ods2(img, 'SET FILE/PROTECTION=(S:RWED,O:R,G:R,W) [F]A.DAT')
            a2 = [E.describe(n, h) for n, h in E.chain(img, '[F]A.DAT')]
            check('%s: SET FILE/PROTECTION changes the primary header only' % tag, [('[200,3]', 'fee0', a1[0]['revision'] + 1)] + before[1:],
                  [(x['owner'], x['fileprot'], x['revision']) for x in a2])
            # 3. both, one command each (ods2 takes one qualifier per command), on B
            ods2(img, 'SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W:R) [F]B.DAT')
            ods2(img, 'SET FILE/OWNER_UIC=[7,6] [F]B.DAT')
            b2 = [E.describe(n, h) for n, h in E.chain(img, '[F]B.DAT')]
            check('%s: both, in two commands: the owner in every header, the protection in the primary header only' % tag,
                  [('[7,6]', 'ea00', b0[0]['revision'] + 2)] + [('[7,6]', b0[i]['fileprot'], b0[i]['revision']) for i in range(1, len(b0))],
                  [(x['owner'], x['fileprot'], x['revision']) for x in b2])
            check('%s: every header checksum is right after both commands' % tag, ['ok'] * len(b2), [x['checksum'] for x in b2])
            r = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True).stdout.strip().split('\n')[-1]
            check('%s: ods5check is clean after both' % tag, '0 error(s), 0 warning(s)', r)
            # 4. a chain that cannot be followed (the checksum of the second extension header is wrong): the whole change is refused, nothing is written
            bad = os.path.join(work, 'bad%d.img' % level)
            shutil.copyfile(img, bad)
            ods2(bad, 'SET FILE/OWNER_UIC=[1,4] [F]B.DAT')
            v = E.O.Vol(bad)
            ch = E.chain(bad, '[F]B.DAT', v)
            target = E.D.lbn(v, ch[2][0])
            with open(bad, 'r+b') as f:
                f.seek(target * 512 + 510)
                f.write(bytes([ch[2][1][510] ^ 0xFF]))
            snap = [E.describe(n, h) for n, h in E.chain(bad, '[F]B.DAT')]
            rc, out = ods2(bad, 'SET FILE/OWNER_UIC=[5,5] [F]B.DAT')
            after = [E.describe(n, h) for n, h in E.chain(bad, '[F]B.DAT')]
            check('%s: an extension header that is not the next segment of the file (a wrong checksum) refuses the change' % tag, True, rc != 0 and 'nothing is changed' in out)
            check('%s: and nothing was written to any header of the file' % tag, snap, after)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('setfile_ext_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
