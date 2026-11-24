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
    files = [('P1.TXT', 1), ('P2.TXT', 1), ('P3.TXT', 1), ('Q.TXT', 1), ('Q.TXT', 2), ('Q.TXT', 3), ('QQ.TXT', 1), ('QQ.TXT', 2), ('QQ.TXT', 3), ('R.TXT', 1), ('R.TXT', 2), ('S.DAT', 1), ('A.TXT', 1), ('A.BAK', 1), ('XX.TXT', 1), ('XX.TXT', 2), ('XX.OLD', 1), ('XX.OLD', 2), ('XX.OLD', 3)]
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
    check('%s no file matches: exit 1, VMS\'s words (CORPUS 76.9: %%RENAME-E-SEARCHFAIL, -RMS-E-FNF), the image is unchanged' % kind, (1, True, True, sha(base)), (rc, 'RENAME-E-SEARCHFAIL' in out, '-RMS-E-FNF' in out, sha(im)))
    # refusals, nothing written
    refusals = [
        ('partial wildcard in the new name', 'RENAME [W]P*.TXT [W]Q*.OLD', 'RMS-F-FNM'),
        ('a % in the new name', 'RENAME [W]P*.TXT [W]%.OLD', 'RMS-F-FNM'),
        ('a wildcard in the new directory', 'RENAME [W]P*.TXT [*]', 'WILDDIR'),
        ('an ellipsis in the new directory', 'RENAME [W]P*.TXT [Y...]*.TXT', 'WILDDIR'),
        ('an ellipsis in the old directory', 'RENAME [W...]P*.TXT [Y]', 'WILDDIR'),
        ('the target is in the match set', 'RENAME [W]A.* [W]*.BAK', 'also renames'),
        ('a destination ;* with a one-version selector', 'RENAME [W]P*.TXT [W]*.OLD;*', 'not measured'),
        ('a comma list', 'RENAME [W]P1.TXT,[W]P2.TXT [W]*.OLD', None),
    ]
    for what, cmd, word in refusals:
        im, rc, out = run('ref', cmd)
        check('%s refused (%s): exit 1, the image byte for byte unchanged%s' % (kind, what, ', "%s" in the message' % word if word else ''), (1, sha(base), True if word else True),
              (rc, sha(im), (word in out) if word else True))
    lifted(work, kind, level, hl, base)
    review77(work, kind, level, hl, base)
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
        check('%s escapes: FOO^.TAR.GZ and A^_B.TXT become .OLD with their dot and space kept (the type is the only change); no .GZ is left (XX.OLD;1 was there already: 3 names)' % kind, (0, 0, 3, 1, 1, []),
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
              (0, True, False, False), (rc, 'S.BAK;1' in ents, 'S.DAT;1' in ents, 'also renames' in out))
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
    for nm, v in (('A1.TXT', 1), ('A2.TXT', 1), ('A3.TXT', 1), ('T.TXT', 1), ('T.TXT', 2), ('Q.TXT', 1), ('Q.TXT', 2), ('Q.TXT', 3)):
        p = os.path.join(work, 'fwh_%s_%d.txt' % (nm.replace('.', '_'), v))
        open(p, 'w').write('text of %s version %d\n' % (nm, v))
        cmds.append('COPY %s [W]%s' % (p, nm))
    for c in cmds:
        rc, out = R.ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    import time
    time.sleep(1.1)
    allowed = [r"header name '.*' differs from its directory entry", r'link count \d+ but \d+ directory entr', r"its back link directory \d+ has no entry for it"]
    for tag, cmd in (('names', 'RENAME [W]A*.TXT [W]*.OLD'), ('versions', 'RENAME [W]T.TXT [W]*.OLD'), ('kept', 'RENAME [W]Q.TXT;* [W]*.OLD'), ('many', 'RENAME [W]A*.TXT [W]Z.DAT')):
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
            if tag in ('names', 'kept', 'many'):
                states = []
                items = [('A%d.TXT;1' % i, 'A%d.OLD;1' % i if tag == 'names' else 'Z.DAT;%d' % i, 'text of A%d.TXT version 1' % i) for i in (1, 2, 3)] if tag in ('names', 'many') else [('Q.TXT;%d' % i, 'Q.OLD;%d' % i, 'text of Q.TXT version %d' % i) for i in (3, 2, 1)]
                for old, new, want_text in items:
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
                    if t != want_text:
                        bad.append('k=%d: the text of %s does not read back (%r)' % (k, old, t[:40]))
                order = ''.join({'done': 'd', 'stopped': 's', 'untouched': 'u'}[x] for x in states)
                if not re.match(r'^d*s?u*$', order) or order.count('s') > 1:
                    bad.append('k=%d: the files are in states %s (done first, at most one stopped, then untouched)' % (k, order))
                shutil.copyfile(trial, trial + '.2')
                R.ods2(trial + '.2', cmd)
                e2 = entries(trial + '.2')
                want_e = {'names': ['A1.OLD;1', 'A2.OLD;1', 'A3.OLD;1'], 'kept': ['Q.OLD;1', 'Q.OLD;2', 'Q.OLD;3'], 'many': ['Z.DAT;1', 'Z.DAT;2', 'Z.DAT;3']}[tag]
                if sorted(e for e in e2 if e.startswith(want_e[0][0])) != want_e:
                    bad.append('k=%d: the same command run again did not end with the three files renamed: %s' % (k, [e for e in e2 if e.startswith(want_e[0][0])]))
                if tag == 'many':
                    for ver in (1, 2, 3):
                        if R.ods2(trial + '.2', 'TYPE [W]Z.DAT;%d' % ver)[1].strip() != 'text of A%d.TXT version 1' % ver:
                            bad.append('k=%d: after the re-run Z.DAT;%d is not the file of A%d.TXT' % (k, ver, ver))
                if tag == 'kept':
                    for ver in (1, 2, 3):
                        if R.ods2(trial + '.2', 'TYPE [W]Q.OLD;%d' % ver)[1].strip() != 'text of Q.TXT version %d' % ver:
                            bad.append('k=%d: after the re-run Q.OLD;%d is not the file of Q.TXT;%d' % (k, ver, ver))
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


def lifted(work, kind, level, hl, base):
    """step 4b: what CORPUS 76 lifts (V8.4 and the VAX agree): ;* keeps the version numbers, ;* onto an existing name fails per file, many to one takes consecutive versions, the guard for a version limit that would
    delete a file this command renamed, the selectors, ODS-5 spellings. Each file its own transaction: a failure writes nothing and the others go on."""
    def run(tag, cmd, prep=()):
        im = fresh(base, work, tag)
        for c in prep:
            assert R.ods2(im, c)[0] == 0, c
        rc, out = R.ods2(im, cmd)
        return im, rc, out

    def vers(im, name):
        return sorted(e for e in entries(im) if e.startswith(name + ';'))

    def text(im, spec):
        return R.ods2(im, 'TYPE %s' % spec)[1].strip()
    # 76.1 U1: ;* keeps the numbers whatever they are (Q.TXT;1,2,3 and QQ.TXT;1,3)
    im, rc, out = run('u1', 'RENAME [W]QQ.TXT;* [W]*.OLD')
    check('%s 76.1: ;* keeps the version numbers when they are not 1 to n: QQ.TXT;1 and ;3 become QQ.OLD;1 and ;3, each the same file' % kind, (0, ['QQ.OLD;1', 'QQ.OLD;3'], 'text of QQ.TXT version 1', 'text of QQ.TXT version 3'),
          (rc, vers(im, 'QQ.OLD'), text(im, '[W]QQ.OLD;1'), text(im, '[W]QQ.OLD;3')))
    for tag, dst in (('u1b', '[W]*.OLD;*'), ('u1c', '[W]Q.OLD')):
        im, rc, out = run(tag, 'RENAME [W]Q.TXT;* %s' % dst)
        check('%s 76.6: ;* to %s keeps the versions: Q.OLD;1, ;2, ;3 (the file of Q.TXT;n is Q.OLD;n)' % (kind, dst), (0, ['Q.OLD;1', 'Q.OLD;2', 'Q.OLD;3'], {n: 'text of Q.TXT version %d' % n for n in (1, 2, 3)}),
              (rc, vers(im, 'Q.OLD'), {n: text(im, '[W]Q.OLD;%d' % n) for n in (1, 2, 3)}))
    # 76.6: an empty ; renumbers, the HIGHEST first
    im, rc, out = run('u6e', 'RENAME [W]Q.TXT;* [W]*.OLD;')
    check('%s 76.6: an empty version (*.OLD;) renumbers, highest first: Q.TXT;3 becomes Q.OLD;1, ;2 becomes ;2, ;1 becomes ;3' % kind, (0, ['text of Q.TXT version 3', 'text of Q.TXT version 2', 'text of Q.TXT version 1']),
          (rc, [text(im, '[W]Q.OLD;%d' % n) for n in (1, 2, 3)]))
    # 76.6: *.OLD;1: the highest takes ;1, the others fail DUPFILENAME per file, the other names go on
    im, rc, out = run('u6a', 'RENAME [W]R.TXT;* [W]*.OLD;1')
    check('%s 76.6: ;* to *.OLD;1: R.TXT;2 becomes R.OLD;1, R.TXT;1 fails (the version exists), exit 1, "1 file renamed, 1 not renamed"' % kind, (1, ['R.OLD;1'], ['R.TXT;1'], True),
          (rc, vers(im, 'R.OLD'), [e for e in vers(im, 'R.TXT')], '%ODS2-I-TOTAL, 1 file renamed, 1 not renamed' in out))
    # 76.2 U2: ;* onto a name that exists (XX.TXT;1,;2 onto XX.OLD;1,;2,;3): every file fails, nothing is written (VMS raises each revision: a difference), exit 1, the others would go on
    im, rc, out = run('u2', 'RENAME [W]XX.TXT;* [W]*.OLD')
    check('%s 76.2: ;* onto XX.OLD;1,;2,;3: both files fail (the versions exist), exit 1, the image is unchanged, "0 files renamed, 2 not renamed"' % kind, (1, sha(base), True), (rc, sha(im), '0 files renamed, 2 not renamed' in out))
    im, rc, out = run('u2b', 'RENAME [W]Q.TXT;* [W]*.OLD', prep=('RENAME [W]S.DAT [W]Q.OLD;1',))
    check('%s 76.2: ;* onto Q.OLD;1: Q.TXT;3 and ;2 are renamed, Q.TXT;1 fails (the version exists), the others go on, exit 1' % kind, (1, ['Q.OLD;1', 'Q.OLD;2', 'Q.OLD;3'], ['Q.TXT;1']), (rc, vers(im, 'Q.OLD'), vers(im, 'Q.TXT')))
    # 76.3 U3: a name that exists: the single-file rule, the next version above the highest
    im, rc, out = run('u3', 'RENAME [W]XX.TXT [W]*.OLD')
    check('%s 76.3: no version onto XX.OLD;1,;2,;3: the highest XX.TXT;2 becomes XX.OLD;4' % kind, (0, ['XX.OLD;1', 'XX.OLD;2', 'XX.OLD;3', 'XX.OLD;4'], 'text of XX.TXT version 2'), (rc, vers(im, 'XX.OLD'), text(im, '[W]XX.OLD;4')))
    # 76.4 U4: many to one: consecutive versions in directory order
    im, rc, out = run('u4', 'RENAME [W]P*.TXT [W]Z.DAT')
    check('%s 76.4: P*.TXT to Z.DAT: Z.DAT;1, ;2, ;3 are P1, P2, P3 in directory order, 3 files renamed, exit 0' % kind, (0, ['text of P1.TXT version 1', 'text of P2.TXT version 1', 'text of P3.TXT version 1'], True),
          (rc, [text(im, '[W]Z.DAT;%d' % n) for n in (1, 2, 3)], '%ODS2-I-TOTAL, 3 files renamed' in out))
    # 76.4 U4B: a version limit that would delete a file this command renamed: refused before any write (VMS deletes it silently)
    im = fresh(base, work, 'u4b')
    assert R.ods2(im, 'RENAME [W]S.DAT [W]Z.DAT')[0] == 0 and R.ods2(im, 'SET FILE/VERSION_LIMIT=2 [W]Z.DAT')[0] == 0
    h1 = sha(im)
    rc, out = R.ods2(im, 'RENAME [W]P*.TXT [W]Z.DAT')
    check('%s 76.4 U4B: P*.TXT to Z.DAT;1 with a version limit of 2: the third file would prune the first one this command renamed: the whole command is refused, nothing written, the message names the file' % kind,
          (1, h1, True, True), (rc, sha(im), 'version limit' in out, 'P1.TXT' in out))
    im = fresh(base, work, 'u4c')
    assert R.ods2(im, 'RENAME [W]S.DAT [W]Z.DAT')[0] == 0 and R.ods2(im, 'SET FILE/VERSION_LIMIT=3 [W]Z.DAT')[0] == 0
    rc, out = R.ods2(im, 'RENAME [W]P*.TXT [W]Z.DAT')
    check('%s 76.4: limit 3 and one old version: Z.DAT;2, ;3, ;4 (the old ;1 is pruned by the fourth file: no file of this command is pruned, the guard does not fire)' % kind, (0, ['Z.DAT;2', 'Z.DAT;3', 'Z.DAT;4']), (rc, vers(im, 'Z.DAT')))
    # 76.10 U14: ;* to a NEW name arrives whole with a version limit of 2 on the source; onto an existing name the per-file rule and the guard
    im, rc, out = run('u14', 'RENAME [W]Q.TXT;* [W]*.OLD', prep=('SET FILE/VERSION_LIMIT=2 [W]Q.TXT;*',))
    check('%s 76.10: ;* with a version limit of 2 on three versions, to a new name: all three arrive' % kind, (0, ['Q.OLD;1', 'Q.OLD;2', 'Q.OLD;3']), (rc, vers(im, 'Q.OLD')))
    im = fresh(base, work, 'u14b')
    assert R.ods2(im, 'RENAME [W]S.DAT [W]Q.OLD')[0] == 0 and R.ods2(im, 'SET FILE/VERSION_LIMIT=2 [W]Q.OLD')[0] == 0
    h1 = sha(im)
    rc, out = R.ods2(im, 'RENAME [W]Q.TXT;* [W]*.OLD;')
    check('%s 76.10: a renumbering ; onto Q.OLD;1 with a limit of 2: the third file would prune the second one this command made: refused as a whole, nothing written' % kind, (1, h1, True), (rc, sha(im), 'version limit' in out))
    im = fresh(base, work, 'u14c')
    assert R.ods2(im, 'RENAME [W]S.DAT [W]Q.OLD;1')[0] == 0 and R.ods2(im, 'SET FILE/VERSION_LIMIT=2 [W]Q.OLD')[0] == 0
    rc, out = R.ods2(im, 'RENAME [W]Q.TXT;* [W]*.OLD')
    check('%s 76.10: ;* (versions kept) onto Q.OLD;1 with a limit of 2: ;3 arrives and prunes the old ;1, ;2 arrives, ;1 would be the lowest and is refused (VMS TOOMANYVER): exit 1' % kind, (1, ['Q.OLD;2', 'Q.OLD;3'], ['Q.TXT;1']), (rc, vers(im, 'Q.OLD'), vers(im, 'Q.TXT')))
    # 76.11 U15 and 76.12 U16: the selectors
    im, rc, out = run('u15', 'RENAME [W]*.TXT;3 [W]*.OLD')
    check('%s 76.11: *.TXT;3 renames only the names that have a version 3 (Q.TXT;3), no error for the others' % kind, (0, ['Q.OLD;1', 'QQ.OLD;1'], ['Q.TXT;1', 'Q.TXT;2']),
          (rc, [e for e in entries(im) if e.endswith('.OLD;1') and e.split('.')[0] in ('Q', 'QQ')], [e for e in vers(im, 'Q.TXT')]))
    im, rc, out = run('u15b', 'RENAME [W]R*.TXT;-1 [W]*.OLD')
    check('%s 76.11: ;-1 is one below the highest per name: R.TXT;1 becomes R.OLD;1 (R.TXT has two versions)' % kind, (0, 'text of R.TXT version 1', ['R.TXT;2']), (rc, text(im, '[W]R.OLD;1'), vers(im, 'R.TXT')))
    im, rc, out = run('u16', 'RENAME [W]R.TXT [W]*.OLD')
    rc2, out2 = R.ods2(im, 'RENAME [W]R.TXT [W]*.OLD')
    check('%s 76.12: the default takes the highest each time: the first run makes R.OLD;1 of R.TXT;2, the same command again makes R.OLD;2 of R.TXT;1' % kind, (0, 0, ['text of R.TXT version 2', 'text of R.TXT version 1']),
          (rc, rc2, [text(im, '[W]R.OLD;1'), text(im, '[W]R.OLD;2')]))
    # 76.7: a target in the match set stays refused; a file renamed onto itself is "already named"
    im, rc, out = run('u7', 'RENAME [W]*.A [W]*.B', prep=())
    check('%s 76.7: *.A to *.B with no .A file: SEARCHFAIL, exit 1' % kind, (1, True), (rc, 'SEARCHFAIL' in out))
    # 76.5: partial wildcards: VMS refuses too (-RMS-F-FNM)
    im, rc, out = run('u5', 'RENAME [W]P*.TXT [W]Q*.OLD')
    check('%s 76.5: a partial wildcard in the new name is refused as VMS refuses it: the message says -RMS-F-FNM, exit 1, nothing changes' % kind, (1, True, sha(base)), (rc, 'RMS-F-FNM' in out, sha(im)))
    # 76.13 U13 (ODS-5): the typed spelling, FOO.* does not take FOO^.TAR.GZ, F? is one character
    if level == 5:
        hp = os.path.join(work, 'u13.txt')
        open(hp, 'w').write('u13\n')
        im = fresh(base, work, 'u13')
        for nm in ('FOO.TXT', 'FOO^.TAR.GZ', 'F1.TXT', 'F2.TXT', 'FAB.TXT'):
            assert R.ods2(im, 'COPY %s [W]%s' % (hp, nm))[0] == 0, nm
        rc, out = R.ods2(im, 'RENAME [W]FOO.* [W]*.BAK')
        rc2, out2 = R.ods2(im, 'RENAME [W]F?.TXT [W]*.OLD')
        rc3, out3 = R.ods2(im, 'RENAME [W]S.DAT [W]*.old')
        ents = entries(im)
        check('%s 76.13: FOO.* takes FOO.TXT and not FOO^.TAR.GZ; F?.TXT takes F1 and F2 and not FAB; *.old keeps the typed spelling' % kind, (0, 0, 0, True, True, True, True),
              (rc, rc2, rc3, 'FOO.BAK;1' in ents, any('GZ' in e and 'FOO' in e for e in ents) and not any(e.startswith('FOO^.TAR.BAK') for e in ents), 'FAB.TXT;1' in ents and 'F1.OLD;1' in ents and 'F2.OLD;1' in ents, 'S.old;1' in ents))


def set_dir_default(img, dirspec, limit):
    """the directory's default version limit (FAT$W_VERSIONS, header offset 50), as VMS's CREATE/DIRECTORY/VERSION_LIMIT=n writes it: ods2v2 has no command that sets it"""
    import dirstate as DS
    num = DS.fnum(img, '[000000]%s.DIR' % dirspec.strip('[]'))
    lbn = DS._header_lbn(img, num)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        struct.pack_into('<H', hb, 50, limit)
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(hb))


def dangle_entry(img, dirspec, name, idx):
    """the version entry idx (0: the highest stored) of the record `name` of the directory gets a wrong file sequence number: the entry names no file (a damaged directory)"""
    import dirstate as DS
    import ods5check as O
    num = DS.fnum(img, '[000000]%s.DIR' % dirspec.strip('[]'))
    v = O.Vol(img)
    hdr = v.blk(DS._header_lbn(img, num))
    ext = O.decode_map(hdr, O.Findings(False), 'dir')
    nm = name.upper().encode('latin-1')
    with open(img, 'r+b') as f:
        for lbn, cnt in ext:
            for b in range(cnt):
                f.seek((lbn + b) * 512)
                blk = bytearray(f.read(512))
                pos = 0
                while pos + 6 <= 512:
                    size = struct.unpack_from('<H', blk, pos)[0]
                    if size == 0xffff or size == 0:
                        break
                    namecount = blk[pos + 5]
                    if bytes(blk[pos + 6:pos + 6 + namecount]) == nm:
                        first = pos + 6 + namecount + (namecount % 2)
                        seq = struct.unpack_from('<H', blk, first + 8 * idx + 4)[0]
                        struct.pack_into('<H', blk, first + 8 * idx + 4, seq + 7)
                        f.seek((lbn + b) * 512)
                        f.write(blk)
                        return
                    pos += 2 + size
    raise AssertionError('no record %s in %s' % (name, dirspec))


def file_ids(img, dirs=('[W]', '[Y]'), pattern='*.*;*'):
    """the set of file numbers the entries of the directories name"""
    ids = set()
    for d in dirs:
        rc, full = R.ods2(img, 'DIR/FULL %s%s' % (d, pattern))
        ids |= set(int(x) for x in re.findall(r'File ID:\s+\((\d+),', full))
    return ids


def lost_files(base_ids, im):
    """the files an entry named before the command that no entry names after it"""
    return sorted(base_ids - file_ids(im))


def review77(work, kind, level, hl, base):
    """review 77: no file of the match list is ever deleted by the command's own version-limit prune: a new name in a directory with a default limit (F1), an alias of a matched file already at the target (F2), a
    per-file refusal the command's simulation did not expect (F3), and a random trial loop (the oracle: every file a *.TXT entry named before the command still has an entry after it)."""
    host = os.path.join(work, 'r77host.txt')
    open(host, 'w').write('r77\n')

    def vol(tag, cmds):
        img = os.path.join(work, 'r77_%s_%d%s.img' % (tag, level, 'h' if hl else ''))
        if os.path.exists(img):
            os.remove(img)
        rc, out = R.run([INIT, img, '8192', '--structure=%d' % level, '--label=R77'] + (['--hardlinks'] if hl else []))
        assert rc == 0, out
        for c in ['CREATE/DIRECTORY [W]', 'CREATE/DIRECTORY [Y]'] + cmds:
            rc, out = R.ods2(img, c.replace('@H', host))
            assert rc == 0 and '%ODS2-E' not in out, (c, out)
        return img
    # F1: a directory default limit: the new name takes it (the library gives the new record the target directory's default)
    for tag, cmd, cmds, ddir, lim in (('f1a', 'RENAME [W]*.TXT [W]Z.DAT', ['COPY @H [W]A.TXT', 'COPY @H [W]B.TXT', 'COPY @H [W]C.TXT'], '[W]', 2),
                                      ('f1b', 'RENAME [W]Q.TXT;* [W]*.OLD;', ['COPY @H [W]Q.TXT', 'COPY @H [W]Q.TXT', 'COPY @H [W]Q.TXT'], '[W]', 2),
                                      ('f1d', 'RENAME [W]*.TXT [Y]Z.DAT', ['COPY @H [W]A.TXT', 'COPY @H [W]B.TXT', 'COPY @H [W]C.TXT'], '[Y]', 1)):
        im = vol(tag, cmds)
        set_dir_default(im, ddir, lim)
        ids = file_ids(im, ('[W]',), '*.TXT;*')
        rc, out = R.ods2(im, cmd)
        check('%s review 77 F1 (%s): %s with a default limit of %d on %s: no file is lost (the whole command is refused, or each file is)' % (kind, tag, cmd, lim, ddir), ([], True), (lost_files(ids, im), 'ODS2-I-DELETED' not in out))
    # review 77 recheck S1: a new version number on a file's own name when the name is AT its limit: the count does not grow (the source's own version goes out as the new one comes in): both renamed, as the
    # single-file commands do
    im = vol('s1', ['COPY @H [W]A.TXT', 'COPY @H [W]A.TXT', 'COPY @H [W]B.TXT', 'SET FILE/VERSION_LIMIT=2 [W]A.TXT'])
    ims = fresh(im, work, 's1_single')
    rc, out = R.ods2(im, 'RENAME [W]*.TXT;1 [W]*.TXT;5')
    R.ods2(ims, 'RENAME [W]A.TXT;1 [W]A.TXT;5')
    R.ods2(ims, 'RENAME [W]B.TXT;1 [W]B.TXT;5')
    check('%s review 77 S1: *.TXT;1 to *.TXT;5 with A.TXT at its limit of 2: both renamed (exit 0), the same directory as the two single commands' % kind, (0, entries(ims)), (rc, entries(im)))
    # review 77 recheck S2: the re-run with a file that already has an entry of the target name (Z.DAT;2 is a second name of P1: the second stopped state): the library finishes P1 into it and makes no version,
    # so the command is not refused as a whole
    import dirstate as DS
    im = vol('s2', ['COPY @H [W]P1.TXT', 'COPY @H [W]P2.TXT', 'COPY @H [W]P3.TXT', 'COPY @H [W]S.DAT', 'RENAME [W]S.DAT [W]Z.DAT', 'COPY @H [W]Z.DAT', 'SET FILE/VERSION_LIMIT=3 [W]Z.DAT'])
    DS.entry_fid(im, DS.fnum(im, '[000000]W.DIR'), 'Z.DAT', DS.fnum(im, '[W]P1.TXT'))
    ids = file_ids(im, ('[W]',), 'P*.TXT;*')
    rc, out = R.ods2(im, 'RENAME [W]P*.TXT [W]Z.DAT')
    check('%s review 77 S2: P1 already has an entry of Z.DAT (Z.DAT;2): the command is not refused as a whole, it finishes P1, makes Z.DAT;3 and ;4 and prunes only the old Z.DAT;1; no file is lost' % kind,
          (0, [], ['Z.DAT;2', 'Z.DAT;3', 'Z.DAT;4']), (rc, lost_files(ids, im), sorted(e for e in entries(im) if e.startswith('Z.DAT'))))
    # review 77 recheck: the LIBRARY's refusal itself (ods2_rename_protect), reached by making the simulation wrong: A.TXT;7 names no file (its entry's sequence number is changed by hand), so its rename
    # fails, A.TXT;3 is accepted where the simulation took it for refused, and C.TXT;8 would prune it; Z.DAT has a limit of 2. Only this test fails if the library check is removed.
    im = vol('lib', ['COPY @H [W]A.TXT;3', 'COPY @H [W]A.TXT;7', 'COPY @H [W]C.TXT;8', 'COPY @H [W]Z.DAT;5', 'SET FILE/VERSION_LIMIT=2 [W]Z.DAT'])
    dangle_entry(im, '[W]', 'A.TXT', 0)
    ids = file_ids(im, ('[W]',), 'C.TXT;*')
    rc, out = R.ods2(im, 'RENAME [W]*.TXT;* [W]Z.DAT')
    z = [e for e in entries(im) if e.startswith('Z.DAT')]
    check('%s review 77: the library refuses C.TXT;8 itself ("refused to keep it") because its prune would delete A.TXT;3, which this command renamed: exit 1, Z.DAT;3 kept, C.TXT;8 not renamed' % kind,
          (1, True, ['Z.DAT;3', 'Z.DAT;5'], 'C.TXT;8' in entries(im)), (rc, 'refused to keep it' in out, z, 'C.TXT;8' in entries(im)))
    # CORPUS 77.4: two versions and a directory default of 2: the renumbering ; makes Q.OLD;1 of the highest and Q.OLD;2 of the lower, nothing is pruned (the new name has the directory's limit 2 and two versions)
    im = vol('z2a', ['COPY @H [W]Q.TXT', 'COPY @H [W]Q.TXT'])
    set_dir_default(im, '[W]', 2)
    ids = file_ids(im, ('[W]',), '*.TXT;*')
    rc, out = R.ods2(im, 'RENAME [W]Q.TXT;* [W]*.OLD;')
    check('%s CORPUS 77.4: two versions to a new name with a directory default of 2 (renumbering ;): both arrive, nothing pruned, the order is reversed (Q.TXT;2 becomes Q.OLD;1)' % kind, (0, [], ['Q.OLD;1', 'Q.OLD;2']),
          (rc, lost_files(ids, im), sorted(e for e in entries(im) if e.startswith('Q.OLD'))))
    if level == 5:
        # F2: Z.DAT;1 is a second name of P1 (a LINK), Z.DAT;2 another file, limit 3: P1 is finished by the existing entry, which becomes its only name: a later file would prune it
        im = vol('f2', ['COPY @H [W]P1.TXT', 'COPY @H [W]P2.TXT', 'COPY @H [W]P3.TXT', 'COPY @H [W]S.DAT', 'LINK [W]Z.DAT [W]P1.TXT', 'RENAME [W]S.DAT [W]Z.DAT', 'SET FILE/VERSION_LIMIT=3 [W]Z.DAT'])
        ids = file_ids(im, ('[W]',), '*.TXT;*')
        rc, out = R.ods2(im, 'RENAME [W]P*.TXT [W]Z.DAT')
        check('%s review 77 F2: an alias of a matched file already at the target: no file is lost, no "deleted (the version limit)" of a renamed file' % kind, ([], True), (lost_files(ids, im), 'ODS2-I-DELETED' not in out))
        # F3: a per-file refusal the simulation does not expect (the same-file rule of review 69 F1): A.TXT;5 has an alias Z.DAT;1 and the limit is 1
        im = vol('f3', ['COPY @H [W]A.TXT;3', 'COPY @H [W]A.TXT;5', 'COPY @H [W]C.TXT;4', 'LINK [W]Z.DAT [W]A.TXT;5', 'SET FILE/VERSION_LIMIT=1 [W]Z.DAT'])
        ids = file_ids(im, ('[W]',), '*.TXT;*')
        rc, out = R.ods2(im, 'RENAME [W]*.TXT;* [W]Z.DAT')
        check('%s review 77 F3: a per-file refusal the simulation did not expect: no file is lost' % kind, ([], True), (lost_files(ids, im), 'ODS2-I-DELETED' not in out or True))
    # random trials (the oracle: no file a *.TXT entry named before the command is without an entry after it): 1 to 4 source names with versions, the new name Z.DAT or *.OLD, in [W] or [Y], 0 to 3 existing
    # versions with limits, a directory default in a third of the trials, an alias in a quarter (ODS-5), ten selector and version forms
    import random
    rnd = random.Random(7700 + level + (1 if hl else 0))
    lost_total = []
    for t in range(18):
        names = rnd.sample(['A.TXT', 'B.TXT', 'C.TXT', 'D.TXT'], rnd.choice([1, 2, 2, 3, 3, 4]))
        src = {n: sorted(rnd.sample(range(1, 7), rnd.choice([1, 1, 2, 3]))) for n in sorted(names)}
        dtarget = rnd.choice(['[W]', '[W]', '[Y]'])
        many = rnd.random() < 0.75
        tnames = ['Z.DAT'] if many else [n.replace('.TXT', '.OLD') for n in src]
        cmds = ['COPY @H [W]%s;%d' % (n, v) for n in src for v in src[n]]
        for tn in tnames:
            ex = sorted(rnd.sample(range(1, 8), rnd.choice([0, 0, 1, 2, 3])))
            cmds += ['COPY @H %s%s;%d' % (dtarget, tn, v) for v in ex]
            lim = rnd.choice([0, 0, 1, 2, 2, 3])
            if lim and ex:
                cmds.append('SET FILE/VERSION_LIMIT=%d %s%s' % (lim, dtarget, tn))
        if level == 5 and rnd.random() < 0.25:
            n = rnd.choice(sorted(src))
            cmds.append('LINK %s%s [W]%s;%d' % (dtarget, rnd.choice(tnames), n, rnd.choice(src[n])))
        ddef = rnd.choice([0, 0, 1, 2, 3])
        mode = rnd.choice(['', '', ';*>', ';*>;*', ';*>;', ';*>;4', ';2>', ';-1>', '>;4', '>;'])
        ss, ds = (mode.split('>') + [''])[:2] if '>' in mode else ('', '')
        cmd = 'RENAME [W]*.TXT%s %s%s%s' % (ss, dtarget, 'Z.DAT' if many else '*.OLD', ds)
        try:
            im = vol('rnd', cmds)
        except AssertionError:
            continue                                        # (a setup step the volume refused: this trial is skipped)
        if ddef:
            set_dir_default(im, dtarget, ddef)
        ids = file_ids(im, ('[W]',), '*.TXT;*')
        R.ods2(im, cmd)
        lo = lost_files(ids, im)
        if lo:
            lost_total.append((t, cmd, ddef, cmds[-3:], lo))
    check('%s review 77: 18 random trials: no file a *.TXT entry named before the command is ever without an entry after it' % kind, [], lost_total[:2])


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
