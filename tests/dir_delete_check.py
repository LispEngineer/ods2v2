#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""dir_delete_check.py - the DELETE rule for a DIRECTORY file and ods5check's two rules about a directory entered more than once (review 71 C2 and the checker section; ods5-dirdel). The stopped states of
a directory move are made by hand (tests/dirstate.py: no RENAME code). The rule, in delete_version_body before all its branches (so DELETE, a wildcard DELETE, PURGE and every limit prune have it):
  "never remove the last entry of a directory that is not empty":
  1. if the header's back-link directory holds ANOTHER entry for this whole file ID, remove THIS entry alone (HARDLINKS: the link count goes down too);
  2. otherwise this is the last entry known: a directory that is not empty is refused ("directory is not empty"), an empty one is deleted, entry and file, as before.
Two holes of be4cb96 that these checks must catch: (a) HARDLINKS, the first state (link count 2, one entry), a non-empty directory: the link branch removes the entry and orphans the subtree;
(b) the second state with an EMPTY directory: DELETE of the old entry deleted the directory file and left the new entry dangling.
ods5check: a WARN "directory N (name) is entered k times" (k >= 2, by whole file ID) and an ERROR for a cycle (a directory entered inside its own subtree)."""
import hashlib
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dirstate as S  # noqa: E402

fails = passes = 0
KINDS = (('ODS-2', 2, False), ('ODS-5', 5, False), ('ODS-5 HARDLINKS', 5, True))


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


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def listing(img, spec):
    out = S.ods2(img, 'DIR ' + spec)[1]
    return sorted('%s;%s' % (m.group(1), m.group(2)) for m in (re.match(r'^(\S+?)\s*;(\d+)\s*$', l) for l in out.splitlines()) if m)


def checker(img):
    out = S.run(['python3', '-I', 'tests/ods5check.py', img])[1]
    return ([l for l in out.splitlines() if l.startswith('ERROR')], [l for l in out.splitlines() if l.startswith('WARN')])


def free_blocks(img):
    return int(re.search(r'Free blocks:\s+(\d+)', S.ods2(img, 'SHOW VOLUME')[1]).group(1))


def readable(img, base):
    """F.TXT and G.TXT of the directory read back through `base` ([P.X] or [Q.X])"""
    return ('file F of X' in S.ods2(img, 'TYPE %s]F.TXT' % base[:-1])[1], 'file G of X.S' in S.ods2(img, 'TYPE %s.S]G.TXT' % base[:-1])[1])


def copy(img, work, tag):
    c = os.path.join(work, 'c_%s.img' % tag)
    shutil.copyfile(img, c)
    return c


def delete_rule(work, name, level, hl):
    for state in ('first', 'second'):
        for empty in (False, True):
            label = '%s %s state, %s directory' % (name, state, 'EMPTY' if empty else 'non-empty')
            base = S.build(work, level, hl, state, empty)
            # DELETE of the OLD entry [P]X.DIR
            for how, cmd in (('DELETE', 'DELETE [P]X.DIR'), ('wildcard DELETE', 'DELETE [P]*.DIR;*')):
                im = copy(base, work, 'old')
                h0 = sha(im)
                rc, out = S.ods2(im, cmd)
                if state == 'first':
                    if empty:
                        check('%s: %s: the last entry of an EMPTY directory: the directory is deleted as before' % (label, how), True, 'X.DIR;1' not in listing(im, '[P]'))
                    else:
                        check('%s: %s is refused: directory is not empty (the last entry of a directory that holds files), the image is unchanged, the subtree reads' % (label, how),
                              (True, h0, (True, True)), ('not empty' in out, sha(im), readable(im, '[P.X]')))
                else:
                    check('%s: %s removes the OLD entry alone (the back-link directory [Q] holds another entry): [P] loses X.DIR, [Q] keeps it, the directory file stays%s' % (label, how, ' and reads' if not empty else ''),
                          (False, True, False), ('X.DIR;1' in listing(im, '[P]'), 'X.DIR;1' in listing(im, '[Q]'), 'not empty' in out))
                    if not empty:
                        check('%s: %s: the whole subtree still reads through [Q.X]' % (label, how), (True, True), readable(im, '[Q.X]'))
                    check('%s: %s: the directory [Q.X] can still be opened and the checker sees no dangling entry' % (label, how), (True, []),
                          ('DIRERR' not in S.ods2(im, 'DIR [Q.X]')[1], [w for w in checker(im)[1] if 'not in use' in w or 'dangling' in w]))
                    errs, warns = checker(im)
                    check('%s: %s: the checker finds no error afterwards' % (label, how), [], errs)
                    if hl:
                        # (the link count went down with the entry: two entries and a count of 2 became one entry and a count of 1)
                        pass
            if state == 'second':
                # DELETE of the NEW entry [Q]X.DIR: the back-link directory holds no other entry for it: the last entry known
                im = copy(base, work, 'new')
                h1 = sha(im)
                rc, out = S.ods2(im, 'DELETE [Q]X.DIR')
                if empty:
                    check('%s: DELETE [Q]X.DIR of an EMPTY directory deletes it (the last entry known, as before)' % label, True, 'X.DIR;1' not in listing(im, '[Q]'))
                else:
                    check('%s: DELETE [Q]X.DIR is refused: not empty, the image is unchanged, both paths read' % label, (True, h1, (True, True), (True, True)),
                          ('not empty' in out, sha(im), readable(im, '[Q.X]'), readable(im, '[P.X]')))


def checker_rules(work, name, level, hl):
    # the second state: one directory, two entries: a WARN naming it; the first state and the state before: none
    for state, want in (('none', 0), ('first', 0), ('second', 1)):
        im = S.build(work, level, hl, state, False, 'ck')
        errs, warns = checker(im)
        ent = [w for w in warns if re.search(r'directory \d+ \(.*\) is entered \d+ times', w)]
        check('%s ods5check, %s state: %d WARN "directory N is entered k times"' % (name, state, want), want, len(ent))
        if want and ent:
            check('%s ods5check, second state: the WARN says 2 times and names X.DIR' % name, (True, True), ('entered 2 times' in ent[0], 'X.DIR' in ent[0]))
        check('%s ods5check, %s state: no ERROR' % (name, state), [], errs)
    # a cycle: [P.X.S] enters [P] (a second entry of P inside its own subtree): an ERROR of its own
    im = S.build(work, level, hl, 'none', False, 'cy')
    S.ods2(im, 'COPY %s [P.X.S]STUBC.TXT' % os.path.join(work, 'ds_stub.txt'))
    S.ods2(im, 'LINK [P.X.S]P.DIR [P.X.S]STUBC.TXT')
    S.entry_fid(im, S.fnum(im, '[P.X]S.DIR'), 'P.DIR', S.fnum(im, '[000000]P.DIR'))
    if hl:
        S.set_count(im, S.fnum(im, '[P.X.S]STUBC.TXT'), 1)
    errs, warns = checker(im)
    check('%s ods5check: a directory entered inside its own subtree is an ERROR (a cycle)' % name, (1, True), (len([e for e in errs if 'cycle' in e]), any('entered 2 times' in w for w in warns)))


def main():
    work = tempfile.mkdtemp(prefix='dir_delete_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for name, level, hl in KINDS:
            print('=== %s' % name)
            delete_rule(work, name, level, hl)
            checker_rules(work, name, level, hl)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('dir_delete_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
