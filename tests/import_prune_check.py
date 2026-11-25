#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""import_prune_check.py - IMPORT/NEW_VERSION never deletes a file the same command imported (review 77 O1). Host files that fold to one name (a.txt, A.TXT, A.txt) imported into a directory whose default
version limit is 1 used to end "3 files imported, 0 errors", exit 0, with only A.TXT;3 on the volume: two imported versions were pruned in silence. Now the prune of a file THIS command created is refused, for that file,
before any write, and reported (exit 1); an older version that was already there is still pruned (the single-file rule, CORPUS 66.1); COPY of one file cannot meet it. At ODS-2, ODS-5 and ODS-5 HARDLINKS.
Run from the project root (./ods2, ./ods2_init built)."""
import hashlib
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rename_wild_check as W   # noqa: E402  (set_dir_default, file_ids)
import rename_check as R        # noqa: E402

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


def mkvol(work, level, hl, tag, limit, cmds=()):
    img = os.path.join(work, 'ip_%s_%d%s.img' % (tag, level, 'h' if hl else ''))
    if os.path.exists(img):
        os.remove(img)
    rc, out = R.run([INIT, img, '8192', '--structure=%d' % level, '--label=IMPRUNE'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    assert R.ods2(img, 'CREATE/DIRECTORY [W]')[0] == 0
    if limit:
        W.set_dir_default(img, '[W]', limit)            # first: a name made afterwards takes the directory's default limit in its record
    for c in cmds:
        rc, out = R.ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    return img


def hosttree(work, names):
    tree = os.path.join(work, 'tree_%d' % len(names))
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(tree)
    for n in names:
        open(os.path.join(tree, n), 'w').write('host file %s\n' % n)
    return tree


def entries(img):
    return sorted(R.listing(img, '[W]'))


def main():
    work = tempfile.mkdtemp(prefix='import_prune_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for kind, level, hl in KINDS:
            print('=== %s' % kind)
            tree = hosttree(work, ['a.txt', 'A.TXT', 'A.txt'])
            first = 'host file A.TXT\n'            # the first of the three in the order IMPORT reads them (strcmp: A.TXT, A.txt, a.txt)
            # O1: default limit 1: the first file makes A.TXT;1, each later one would prune a file this command imported: refused, reported, exit 1
            im = mkvol(work, level, hl, 'o1', 1)
            ids0 = set()
            rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % tree)
            ents = entries(im)
            check('%s O1: three files that fold to one name, default limit 1: only the first is imported, the other two are refused and reported, exit 1, "2 errors"' % kind,
                  (1, ['A.TXT;1'], True, True), (rc, ents, out.count('ODS2-E-COPYERR') == 2, '2 errors' in out))
            check('%s O1: ... and A.TXT;1 is the FIRST imported file, nothing imported was pruned' % kind, first.strip(), R.ods2(im, 'TYPE [W]A.TXT;1')[1].strip())
            # a limit of 2: two versions arrive, the third would prune the first of them
            im = mkvol(work, level, hl, 'o2', 2)
            rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % tree)
            check('%s O1: default limit 2: A.TXT;1 and ;2 are imported, the third is refused (it would prune A.TXT;1), exit 1' % kind, (1, ['A.TXT;1', 'A.TXT;2'], 1), (rc, entries(im), out.count('ODS2-E-COPYERR')))
            # a version that was there before the command is pruned as ever (not a file of this IMPORT)
            im = mkvol(work, level, hl, 'o3', 1, ['COPY %s [W]A.TXT' % os.path.join(tree, 'a.txt')])
            one = hosttree(work, ['b.txt'])
            os.rename(os.path.join(one, 'b.txt'), os.path.join(one, 'a.txt'))
            rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % one)
            check('%s O1: an older version that was already there (limit 1) is still pruned by the import of one file: A.TXT;2 only, exit 0' % kind, (0, ['A.TXT;2']), (rc, entries(im)))
            # a plain volume (no limit): all three arrive
            im = mkvol(work, level, hl, 'o4', 0)
            rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % tree)
            check('%s O1: no limit: all three are imported (A.TXT;1, ;2, ;3), exit 0' % kind, (0, ['A.TXT;1', 'A.TXT;2', 'A.TXT;3']), (rc, entries(im)))
            # the checker finds the volume sound after the refusals
            im = mkvol(work, level, hl, 'o5', 1)
            R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % tree)
            ck = R.run(['python3', '-I', 'tests/ods5check.py', im])[1].splitlines()
            check('%s O1: ods5check finds the volume sound after the refused imports (no error; the only warning is the limit of 1 it always gives)' % kind, ([], []),
                  ([l for l in ck if l.startswith('ERROR')], [l for l in ck if l.startswith('WARN') and 'version limit 1' not in l]))
            # review 78 F1: a symbolic link made by the same IMPORT/NEW_VERSION is protected too (A.TXT -> a.txt is made first, the file a.txt would prune it with a default limit of 1)
            if level == 5:
                lt = os.path.join(work, 'linktree')
                shutil.rmtree(lt, ignore_errors=True)
                os.makedirs(lt)
                open(os.path.join(lt, 'a.txt'), 'w').write('host file a.txt\n')
                os.symlink('a.txt', os.path.join(lt, 'A.TXT'))
                im = mkvol(work, level, hl, 'o7', 1)
                rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % lt)
                check('%s review 78 F1: a symbolic link made by this IMPORT is not pruned by a later file: A.TXT;1 (the link) stays, the file is refused, exit 1, 1 error' % kind,
                      (1, ['A.TXT;1'], 1, True), (rc, entries(im), out.count('ODS2-E-COPYERR'), '1 error' in out))
            # review 78 N3: two host directories (x and X) with f.txt and F.TXT: the list is the command's, not one directory's: one file is made in [W.X], two are refused
            tw = os.path.join(work, 'twodirs')
            shutil.rmtree(tw, ignore_errors=True)
            for d in ('x', 'X'):
                os.makedirs(os.path.join(tw, d))
            open(os.path.join(tw, 'x', 'f.txt'), 'w').write('x f\n')
            open(os.path.join(tw, 'x', 'F.TXT'), 'w').write('x F\n')
            open(os.path.join(tw, 'X', 'f.txt'), 'w').write('X f\n')
            im = mkvol(work, level, hl, 'o8', 1)
            rc, out = R.ods2(im, 'IMPORT/NEW_VERSION %s [W]' % tw)
            check('%s review 78 N3: host directories x and X (the same VMS name) with f.txt, F.TXT: one F.TXT;1 in [W.X], two refused, exit 1' % kind, (1, ['F.TXT;1'], 2), (rc, sorted(e.upper() for e in R.listing(im, '[W.X]')), out.count('ODS2-E-COPYERR')))
            # COPY of one file cannot meet it: three COPY commands to one name, default limit 2: the single-file rule (CORPUS 66.1): ;3 and ;2 remain
            im = mkvol(work, level, hl, 'o6', 2)
            hp = os.path.join(tree, 'A.TXT')
            for _ in range(3):
                R.ods2(im, 'COPY %s [W]Z.DAT' % hp)
            check('%s COPY: three COPY commands to one name with a default limit of 2 leave Z.DAT;2 and ;3 (the single-file rule; each command is one file)' % kind, ['Z.DAT;2', 'Z.DAT;3'], entries(im))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('import_prune_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
