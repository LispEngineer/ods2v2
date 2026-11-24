#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""rename_wild_check.py - RENAME with wildcards (step 4; CORPUS 65.4 e1 and e2, 70.8; reviews 75). Each matched file is its own transaction (the step 1 to 3 code); the list is made first; what is not
measured is refused with the image byte for byte unchanged. At ODS-2, ODS-5 and ODS-5 HARDLINKS. Run from the project root (./ods2, ./ods2_init, ./ods2_fault built)."""
import hashlib
import os
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rename_check as R     # noqa: E402
import versorder_check as V  # noqa: E402

KINDS = (('ODS-2', 2, False), ('ODS-5', 5, False), ('ODS-5 HARDLINKS', 5, True))
passes = fails = 0
INIT = './ods2_init'


def check(desc, expected, got):
    global passes, fails
    if expected == got:
        passes += 1
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %r' % (expected,))
        print('      got:      %r' % (got,))


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def fresh(base, work, tag):
    c = os.path.join(work, 'w_%s.img' % re.sub(r'\W', '_', tag))
    shutil.copyfile(base, c)
    return c


def mk(work, level, hl):
    """[W]: P1.TXT P2.TXT P3.TXT, Q.TXT;1;2;3, QQ.TXT;1;3 (not contiguous), R.TXT;1;2, S.DAT, A.TXT and A.BAK (two files), a directory SUB.DIR and SUB2.DIR; [Y] empty. Each file has text of its own."""
    img = os.path.join(work, 'rw_%d%s.img' % (level, 'h' if hl else ''))
    if os.path.exists(img):
        os.remove(img)
    rc, out = R.run([INIT, img, '8192', '--structure=%d' % level, '--label=RWILD'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    cmds = ['CREATE/DIRECTORY [W]', 'CREATE/DIRECTORY [Y]', 'CREATE/DIRECTORY [W.SUB]', 'CREATE/DIRECTORY [W.SUB2]']
    files = [('P1.TXT', 1), ('P2.TXT', 1), ('P3.TXT', 1), ('Q.TXT', 1), ('Q.TXT', 2), ('Q.TXT', 3), ('QQ.TXT', 1), ('QQ.TXT', 2), ('QQ.TXT', 3), ('R.TXT', 1), ('R.TXT', 2), ('S.DAT', 1), ('A.TXT', 1), ('A.BAK', 1)]
    for nm, v in files:
        p = os.path.join(work, 'rwh_%s_%d.txt' % (nm.replace('.', '_'), v))
        open(p, 'w').write('text of %s version %d\n' % (nm, v))
        cmds.append('COPY %s [W]%s' % (p, nm))
    cmds.append('DELETE [W]QQ.TXT;2')
    for c in cmds:
        rc, out = R.ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    import time
    time.sleep(1.1)           # (a rename's date is later than the creation's)
    return img


def texts(img, specs):
    """the text each spec reads back (a file's identity)"""
    res = {}
    for s in specs:
        res[s] = R.ods2(img, 'TYPE %s' % s)[1].strip()
    return res


def entries(img, d='[W]'):
    return R.listing(img, d)


def renamed_ok(label, base, im, level, pairs):
    """every (old spec, new spec): the file's header changed only in its name, revision +1, the date, the checksum; same file (the text reads back under the new name)"""
    for old, new in pairs:
        hb, ha = R.hdr(base, old), R.hdr(im, new)
        if hb is None or ha is None:
            check('%s: %s became %s (the header is found)' % (label, old, new), True, False)
            continue
        fb, fa = R.fields(hb, level), R.fields(ha, level)
        check('%s: %s: the header names %s, revision +1, only that and the date and the checksum changed' % (label, old, new.split(']')[1]),
              (new.split(']')[1], fb['revision'] + 1, []), (fa['name'], fa['revision'], R.other_bytes_same(hb, ha, level)))
        check('%s: %s: the date is set and the checksum is right' % (label, new), (True, True), ((fa['revdate'] > fb['revdate']) if level == 2 else (fa['attdate'] > fb['attdate']), R.cksum_ok(ha)))


def cases(work, kind, level, hl):
    base = mk(work, level, hl)
    n = lambda s: s       # (the specs are written for ODS-2 and ODS-5 alike)

    def run(tag, cmd):
        im = fresh(base, work, tag)
        rc, out = R.ods2(im, cmd)
        return im, rc, out
    # e1 (CORPUS 65.4): P*.TXT to *.OLD: three files in one command
    im, rc, out = run('e1', 'RENAME [W]P*.TXT [W]*.OLD')
    check('%s e1: RENAME [W]P*.TXT [W]*.OLD succeeds, one TOTAL line, nothing else wrong' % kind, (0, True, False), (rc, '%ODS2-I-TOTAL, 3 files renamed' in out, '%ODS2-E' in out))
    ents = entries(im)
    check('%s e1: P1.OLD;1, P2.OLD;1, P3.OLD;1 and no P?.TXT' % kind, ([f'P{i}.OLD;1' for i in (1, 2, 3)], []), ([e for e in ents if e.startswith('P') and e.endswith('.OLD;1')], [e for e in ents if e.startswith('P') and '.TXT' in e]))
    renamed_ok('%s e1' % kind, base, im, level, [('[W]P%d.TXT;1' % i, '[W]P%d.OLD;1' % i) for i in (1, 2, 3)])
    check('%s e1: the checker finds nothing wrong, no warning either' % kind, ([], []), checker(im))
    # e2: Q.TXT;* to *.OLD: versions kept (the file of Q.TXT;n is Q.OLD;n)
    im, rc, out = run('e2', 'RENAME [W]Q.TXT;* [W]*.OLD')
    check('%s e2: RENAME [W]Q.TXT;* [W]*.OLD: Q.OLD;1, ;2, ;3 and no Q.TXT' % kind, (0, ['Q.OLD;1', 'Q.OLD;2', 'Q.OLD;3'], []), (rc, [e for e in entries(im) if e.startswith('Q.OLD')], [e for e in entries(im) if e.startswith('Q.TXT')]))
    check('%s e2: the file of Q.TXT;n is Q.OLD;n (the text reads back under its version)' % kind, {'[W]Q.OLD;%d' % v: 'text of Q.TXT version %d' % v for v in (1, 2, 3)}, texts(im, ['[W]Q.OLD;%d' % v for v in (1, 2, 3)]))
    renamed_ok('%s e2' % kind, base, im, level, [('[W]Q.TXT;%d' % v, '[W]Q.OLD;%d' % v) for v in (1, 2, 3)])
    check('%s e2: the checker finds nothing wrong' % kind, ([], []), checker(im))
    # the other accepted forms
    im, rc, out = run('pct', 'RENAME [W]P%.TXT [W]*.NEW')
    check('%s accepted: %% matches one character: P1.NEW, P2.NEW, P3.NEW' % kind, (0, ['P1.NEW;1', 'P2.NEW;1', 'P3.NEW;1']), (rc, [e for e in entries(im) if e.startswith('P') and '.NEW' in e]))
    im, rc, out = run('typ', 'RENAME [W]S.* [W]*.BAK')
    check('%s accepted: a type wildcard: S.DAT becomes S.BAK;1 (one file: no TOTAL line)' % kind, (0, True, 'S.BAK;1' in entries(im), 'TOTAL' in out), (rc, True, 'S.DAT;1' not in entries(im), 'TOTAL' in out))
    im, rc, out = run('dir', 'RENAME [W]P*.TXT [Y]')
    check('%s accepted: a directory destination: P1.TXT, P2.TXT, P3.TXT move to [Y] keeping their names, the back link is [Y]' % kind, (0, ['P1.TXT;1', 'P2.TXT;1', 'P3.TXT;1'], True),
          (rc, entries(im, '[Y]'), all(backlink(im, '[Y]P%d.TXT;1' % i) == R.E.D.fnum(im, '[000000]Y.DIR') for i in (1, 2, 3))))
    im, rc, out = run('hi', 'RENAME [W]R.TXT [W]*.OLD')
    check('%s accepted: no version takes the highest of each name: R.TXT;2 becomes R.OLD;1, R.TXT;1 stays' % kind, (0, ['R.OLD;1'], ['R.TXT;1']), (rc, [e for e in entries(im) if e.startswith('R.OLD')], [e for e in entries(im) if e.startswith('R.TXT')]))
    check('%s accepted: ... and it is the file of version 2' % kind, 'text of R.TXT version 2', texts(im, ['[W]R.OLD;1'])['[W]R.OLD;1'])
    im, rc, out = run('m1', 'RENAME [W]R.TXT;-1 [W]*.OLD')
    check('%s accepted: ;-1 takes the second highest: R.TXT;1 becomes R.OLD;1' % kind, (0, 'text of R.TXT version 1'), (rc, texts(im, ['[W]R.OLD;1'])['[W]R.OLD;1']))
    im, rc, out = run('one', 'RENAME [W]S.DAT [W]*.BAK')
    check('%s accepted: a * in the new name with no wildcard in the old one is the omitted field: S.DAT to S.BAK' % kind, (0, True), (rc, 'S.BAK;1' in entries(im)))
    im, rc, out = run('star', 'RENAME [W]S.DAT [W]*.*')
    check('%s accepted: *.* with one file: already named, nothing changes' % kind, (0, sha(base)), (rc, sha(im)) if 'already named' in out else (rc, None))
    # no match
    im, rc, out = run('nomatch', 'RENAME [W]ZZ*.TXT [W]*.OLD')
    check('%s no file matches: exit 1, "no files match", the image is unchanged' % kind, (1, True, sha(base)), (rc, 'no files match' in out, sha(im)))
    # refusals, nothing written
    refusals = [
        ('partial wildcard in the new name', 'RENAME [W]P*.TXT [W]Q*.OLD', 'not measured'),
        ('a % in the new name', 'RENAME [W]P*.TXT [W]%.OLD', 'not measured'),
        ('a wildcard in the new directory', 'RENAME [W]P*.TXT [*]', 'WILDDIR'),
        ('an ellipsis in the new directory', 'RENAME [W]P*.TXT [Y...]*.TXT', 'WILDDIR'),
        ('an ellipsis in the old directory', 'RENAME [W...]P*.TXT [Y]', 'WILDDIR'),
        ('many sources, one target name', 'RENAME [W]P*.TXT [W]Z.DAT', 'would all become'),
        ('the target is in the match set', 'RENAME [W]A.* [W]*.BAK', 'also renames'),
        (';* source, a destination version number', 'RENAME [W]Q.TXT;* [W]*.OLD;5', 'not measured'),
        ('a destination ;* with a one-version selector', 'RENAME [W]P*.TXT [W]*.OLD;*', 'not measured'),
        ('a comma list', 'RENAME [W]P1.TXT,[W]P2.TXT [W]*.OLD', None),
        (';* with versions that are not 1..n', 'RENAME [W]QQ.TXT;* [W]*.OLD', 'not measured'),
    ]
    for what, cmd, word in refusals:
        im, rc, out = run('ref', cmd)
        check('%s refused (%s): exit 1, the image byte for byte unchanged%s' % (kind, what, ', "%s" in the message' % word if word else ''), (1, sha(base), True if word else True),
              (rc, sha(im), (word in out) if word else True))
    # ;* when the destination name exists: refused, the message names the single-version commands
    im = fresh(base, work, 'exists')
    assert R.ods2(im, 'RENAME [W]Q.TXT;1 [W]Q.OLD;1')[0] == 0
    h1 = sha(im)
    rc, out = R.ods2(im, 'RENAME [W]Q.TXT;* [W]*.OLD')
    check('%s ;* onto a destination name that exists: refused, exit 1, nothing written, the message names the one-version commands to finish it' % kind, (1, h1, True, True), (rc, sha(im), 'one version at a time' in out, 'RENAME [W]Q.TXT;3 [W]Q.OLD;3' in out))
    # review 76 S1: an explicit new version on the file's own name is renamed, as the single-file command does (CORPUS 67.4), not reported as "already named"
    im = fresh(base, work, 's1w')
    imw = im
    rc, out = R.ods2(imw, 'RENAME [W]P*.TXT;1 [W]*.TXT;5')
    ims = fresh(base, work, 's1s')
    for i in (1, 2, 3):
        R.ods2(ims, 'RENAME [W]P%d.TXT;1 [W]P%d.TXT;5' % (i, i))
    check('%s S1: RENAME [W]P*.TXT;1 [W]*.TXT;5 renames the versions (no "already named"), exit 0, the three single commands give the same directory' % kind,
          (0, False, [e for e in entries(ims) if e.startswith('P')]), (rc, 'already named' in out, [e for e in entries(imw) if e.startswith('P')]))
    check('%s S1: ... and the same headers (name and revision) file by file' % kind, [(R.fields(R.hdr(ims, '[W]P%d.TXT;5' % i), level)['name'], R.fields(R.hdr(ims, '[W]P%d.TXT;5' % i), level)['revision']) for i in (1, 2, 3)],
          [(R.fields(R.hdr(imw, '[W]P%d.TXT;5' % i), level)['name'], R.fields(R.hdr(imw, '[W]P%d.TXT;5' % i), level)['revision']) for i in (1, 2, 3)] if R.hdr(imw, '[W]P1.TXT;5') is not None else None)
    # review 76 N1: only directories match, nothing is renamed: exit 1 and a "no files renamed" line, the image unchanged
    im, rc, out = run('n1', 'RENAME [W]SUB*.* [Y]')
    check('%s N1: only directories match: SKIPDIR lines, "no files renamed", exit 1, the image unchanged' % kind, (1, True, True, sha(base)), (rc, 'SKIPDIR' in out, 'no files renamed' in out, sha(im)))
    # review 76 S2: many files to one name: one line for the target name, not one for every pair
    mw = os.path.join(work, 'many_%d%s.img' % (level, 'h' if hl else ''))
    rc0, out0 = R.run([INIT, mw, '8192', '--structure=%d' % level, '--label=RMANY'] + (['--hardlinks'] if hl else []))
    assert rc0 == 0, out0
    assert R.ods2(mw, 'CREATE/DIRECTORY [M]')[0] == 0
    hp = os.path.join(work, 'many.txt')
    open(hp, 'w').write('many\n')
    for i in range(1, 31):
        assert R.ods2(mw, 'COPY %s [M]F%02d.TXT' % (hp, i))[0] == 0
    h0 = sha(mw)
    rc, out = R.ods2(mw, 'RENAME [M]F*.TXT [M]Z.DAT')
    lines = [l for l in out.splitlines() if 'would both become' in l or 'would all become' in l]
    check('%s S2: 30 files to one name: refused, the image unchanged, ONE line for the target name (not 435), it names two sources and the count of the others' % kind, (1, h0, 1, True),
          (rc, sha(mw), len(lines), bool(lines) and 'and 28 more' in lines[0]))
    # review 76 N3: on ODS-2 the reported new name is the stored (upper case) one
    if level == 2:
        im, rc, out = run('n3', 'RENAME [W]p*.txt [Y]*.old')
        check('%s N3: ODS-2: the reported new name is upper case (P1.OLD;1), as stored' % kind, (0, True, False), (rc, 'P1.OLD;1' in out, 'P1.old' in out))
    # a failure in the middle: P2.OLD;1 exists, the destination asks for ;1: P1 and P3 are renamed, P2 is refused, exit 1, the TOTAL line says so
    im = fresh(base, work, 'mid')
    assert R.ods2(im, 'RENAME [W]S.DAT [W]P2.OLD')[0] == 0
    rc, out = R.ods2(im, 'RENAME [W]P*.TXT [W]*.OLD;1')
    ents = entries(im)
    check('%s a failure in the middle: P1 and P3 renamed, P2 refused (P2.OLD;1 exists), exit 1, the TOTAL line counts both, the volume is sound' % kind,
          (1, True, ['P1.OLD;1', 'P2.OLD;1', 'P3.OLD;1'], ['P2.TXT;1'], True, ([], [])),
          (rc, '%ODS2-I-TOTAL, 2 files renamed, 1 not renamed' in out, [e for e in ents if e.startswith('P') and '.OLD' in e], [e for e in ents if e == 'P2.TXT;1'], 'text of S.DAT version 1' == texts(im, ['[W]P2.OLD;1'])['[W]P2.OLD;1'], checker(im)))
    # directories: D3 and C6
    im, rc, out = run('dirs', 'RENAME [W]*.* [Y]')
    check('%s directories: RENAME [W]*.* [Y] skips SUB.DIR and SUB2.DIR (info line), moves the files, and the TOTAL line counts the skipped directories' % kind, (True, True, True, True),
          ('SKIPDIR' in out, 'SUB.DIR;1' in entries(im) and 'SUB2.DIR;1' in entries(im), 'P1.TXT;1' in entries(im, '[Y]'), '2 directories skipped' in out))
    im, rc, out = run('dir2', 'RENAME [W]*.DIR [Y]')
    check('%s directories: RENAME [W]*.DIR [Y] moves both directories (each by the directory rename), their entries are in [Y]' % kind, (0, ['SUB.DIR;1', 'SUB2.DIR;1'], [], ([], [])),
          (rc, entries(im, '[Y]'), [e for e in entries(im) if e.endswith('.DIR;1')], checker(im)))
    # escapes (ODS-5): a name with a literal dot, a space and a literal star keeps its characters
    if level == 5:
        im = fresh(base, work, 'esc')
        for nm in ('FOO^.TAR.GZ', 'A^_B.TXT'):
            p = os.path.join(work, 'rwe.txt')
            open(p, 'w').write('esc %s\n' % nm)
            assert R.ods2(im, 'COPY %s [W]%s' % (p, nm))[0] == 0, nm
        rc, out = R.ods2(im, 'RENAME [W]*.GZ [W]*.OLD')
        rc2, out2 = R.ods2(im, 'RENAME [W]A^_B.TXT [W]*.OLD')
        rc3 = 0
        names = entries(im)
        old = [x for x in names if x.endswith('.OLD;1')]
        check('%s escapes: FOO^.TAR.GZ and A^_B.TXT become .OLD with their dot and space kept (the type is the only change); no .GZ is left' % kind, (0, 0, 2, 1, 1, []),
              (rc, rc2, len(old), sum('FOO' in x and 'TAR.OLD' in x for x in old), sum('_' in x or ' ' in x for x in old), [x for x in names if x.endswith('.GZ;1')]))
    # H1: X.TXT of [D] is stored out of order: refused once without a version, renamed with ;*
    vw = os.path.join(work, 'v_%s' % re.sub(r'\W', '_', kind))
    os.makedirs(vw, exist_ok=True)
    vb = V.build(vw, level)
    for with_v, cmd in ((False, 'RENAME [D]*.TXT [D]*.OLD'), (True, 'RENAME [D]X.TXT;* [D]*.OLD')):
        im = fresh(vb, work, 'h1')
        rc, out = R.ods2(im, cmd)
        ents = R.listing(im, '[D]')
        if not with_v:
            check('%s H1: a name stored out of order is refused once (VERSORDER), the others are renamed, exit 1' % kind, (1, True, True, True), (rc, 'VERSORDER' in out, 'Z.OLD;1' in ents, 'X.TXT;1' in ents))
        else:
            check('%s H1: ;* renames the three versions of the name stored out of order' % kind, (0, ['X.OLD;1', 'X.OLD;2', 'X.OLD;3']), (rc, sorted(e for e in ents if e.startswith('X.OLD'))))
    # the second stopped state: a file with two entries (B.TXT and B.BAK), the same RENAME run again is not refused wholesale (review 75 C3); ODS-5 has LINK
    if level == 5:
        im = fresh(base, work, 'c3')
        assert R.ods2(im, 'LINK [W]S.BAK [W]S.DAT')[0] == 0
        rc, out = R.ods2(im, 'RENAME [W]S.* [W]*.BAK')
        ents = entries(im)
        check('%s C3: an alias entry of the same file under the target name is not a collision: the run is not refused as a whole, S.DAT is finished into S.BAK (one entry left)' % kind,
              (0, True, False, False), (rc, 'S.BAK;1' in ents, 'S.DAT;1' in ents, 'would both become' in out or 'would all become' in out or 'also renames' in out))
    fault_wild(work, kind, level, hl)
    print('%s: %d passes, %d fails so far' % (kind, passes, fails))


def fault_wild(work, kind, level, hl):
    """the command stopped after every write k (a kill or a power cut: ods2_fault, ODS2_FAULT_AFTER_WRITES): the files done come first, at most ONE file is in a stopped state of step 1 or 2 (a state the checker knows),
    the rest are untouched; every text reads back under the old or the new name; the checker has no error and only the warnings of those states. The same command run again: for names with one version it ends with every
    file renamed; for a name with two versions it is the same as typing the command afresh (review 75 C1): the newer text is T.OLD;1, the older one stays T.TXT;1 or, when the first rename had finished, becomes T.OLD;2 (a
    version limit would prune it): this test states that, it does not promise a finish."""
    if not os.path.exists('./ods2_fault'):
        print('SKIP: ods2_fault is not built')
        return
    img = os.path.join(work, 'fw_%d%s.img' % (level, 'h' if hl else ''))
    if os.path.exists(img):
        os.remove(img)
    rc, out = R.run([INIT, img, '8192', '--structure=%d' % level, '--label=FWILD'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    cmds = ['CREATE/DIRECTORY [W]']
    for nm, v in (('A1.TXT', 1), ('A2.TXT', 1), ('A3.TXT', 1), ('T.TXT', 1), ('T.TXT', 2)):
        p = os.path.join(work, 'fwh_%s_%d.txt' % (nm.replace('.', '_'), v))
        open(p, 'w').write('text of %s version %d\n' % (nm, v))
        cmds.append('COPY %s [W]%s' % (p, nm))
    for c in cmds:
        rc, out = R.ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    import time
    time.sleep(1.1)
    allowed = [r"header name '.*' differs from its directory entry", r'link count \d+ but \d+ directory entr', r"its back link directory \d+ has no entry for it"]
    for tag, cmd in (('names', 'RENAME [W]A*.TXT [W]*.OLD'), ('versions', 'RENAME [W]T.TXT [W]*.OLD')):
        trial = os.path.join(work, 'fwt.img')
        shutil.copyfile(img, trial)
        rc, out = R.run(['./ods2_fault', trial, cmd], {**os.environ, 'ODS2_FAULT_COUNT': '1'}) if False else R.run(['env', 'ODS2_FAULT_COUNT=1', './ods2_fault', trial, cmd])
        m = re.search(r'ODS2_FAULT: (\d+) writes', out)
        check('%s fault %s: the command itself works and its writes are counted' % (kind, tag), True, rc == 0 and bool(m))
        if not m:
            continue
        n = int(m.group(1))
        bad = []
        for k in range(1, n + 1):
            shutil.copyfile(img, trial)
            R.run(['env', 'ODS2_FAULT_AFTER_WRITES=%d' % k, './ods2_fault', trial, cmd])
            ents = entries(trial)
            ch = R.run(['python3', '-I', 'tests/ods5check.py', trial])[1].splitlines()
            errs = [l for l in ch if l.startswith('ERROR')]
            warns = [l for l in ch if l.startswith('WARN') and not any(re.search(a, l) for a in allowed)]
            if errs or warns:
                bad.append('k=%d: checker %s' % (k, (errs + warns)[0][:120]))
            if tag == 'names':
                states = []
                for i in (1, 2, 3):
                    old, new = 'A%d.TXT;1' % i, 'A%d.OLD;1' % i
                    oe, ne = old in ents, new in ents
                    h = R.hdr(trial, '[W]' + (old if oe else new))
                    nm = R.fields(h, level)['name'] if h is not None else None
                    if oe and not ne and nm == old:
                        states.append('untouched')
                    elif ne and not oe and nm == new:
                        states.append('done')
                    else:
                        states.append('stopped')
                    t = R.ods2(trial, 'TYPE [W]%s' % (old if oe else new))[1].strip()
                    if t != 'text of A%d.TXT version 1' % i:
                        bad.append('k=%d: the text of A%d does not read back (%r)' % (k, i, t[:40]))
                order = ''.join({'done': 'd', 'stopped': 's', 'untouched': 'u'}[x] for x in states)
                if not re.match(r'^d*s?u*$', order) or order.count('s') > 1:
                    bad.append('k=%d: the files are in states %s (done first, at most one stopped, then untouched)' % (k, order))
                shutil.copyfile(trial, trial + '.2')
                R.ods2(trial + '.2', cmd)
                e2 = entries(trial + '.2')
                if sorted(e for e in e2 if e.startswith('A')) != ['A1.OLD;1', 'A2.OLD;1', 'A3.OLD;1']:
                    bad.append('k=%d: the same command run again did not end with the three files renamed: %s' % (k, [e for e in e2 if e.startswith('A')]))
                os.remove(trial + '.2')
            else:
                shutil.copyfile(trial, trial + '.2')
                R.ods2(trial + '.2', cmd)
                e2 = [e for e in entries(trial + '.2') if e.startswith('T.')]
                t1 = R.ods2(trial + '.2', 'TYPE [W]T.OLD;1')[1].strip()
                rest = ('T.TXT;1' in e2, 'T.OLD;2' in e2)
                t2 = R.ods2(trial + '.2', 'TYPE [W]%s' % ('T.TXT;1' if rest[0] else 'T.OLD;2'))[1].strip() if any(rest) else ''
                if t1 != 'text of T.TXT version 2' or t2 != 'text of T.TXT version 1' or rest[0] == rest[1]:
                    bad.append('k=%d: after the command was run again T entries %s, T.OLD;1 reads %r, the other %r' % (k, e2, t1[:30], t2[:30]))
                os.remove(trial + '.2')
        check('%s fault %s: after each of %d writes the volume is in a state step 1 to 3 knows%s' % (kind, tag, n, '; a re-run of a two-version name is the command typed afresh' if tag == 'versions' else ', and the same command run again finishes it'), [], bad[:3])


def backlink(img, spec):
    h = R.hdr(img, spec)
    return struct.unpack_from('<H', h, 66)[0] if h is not None else None


def checker(img):
    out = R.run(['python3', '-I', 'tests/ods5check.py', img])[1]
    return ([l for l in out.splitlines() if l.startswith('ERROR')], [l for l in out.splitlines() if l.startswith('WARN')])


def main():
    work = tempfile.mkdtemp(prefix='rename_wild_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for kind, level, hl in KINDS:
            print('=== %s' % kind)
            cases(work, kind, level, hl)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('rename_wild_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
