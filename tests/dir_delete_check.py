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


def checker(img, verbose=False):
    out = S.run(['python3', '-I', 'tests/ods5check.py'] + (['-v'] if verbose else []) + [img])[1]
    errs, warns = ([l for l in out.splitlines() if l.startswith('ERROR')], [l for l in out.splitlines() if l.startswith('WARN')])
    return (errs, warns, [l for l in out.splitlines() if l.startswith('info')]) if verbose else (errs, warns)


def free_blocks(img):
    return int(re.search(r'Free blocks:\s+(\d+)', S.ods2(img, 'SHOW VOLUME')[1]).group(1))


def readable(img, base):
    """F.TXT and G.TXT of the directory read back through `base` ([P.X] or [Q.X])"""
    return ('file F of X' in S.ods2(img, 'TYPE %s]F.TXT' % base[:-1])[1], 'file G of X.S' in S.ods2(img, 'TYPE %s.S]G.TXT' % base[:-1])[1])


def copy(img, work, tag):
    c = os.path.join(work, 'c_%s.img' % tag)
    shutil.copyfile(img, c)
    return c


def header_of(img, spec):
    ch = S.E.chain(img, spec)
    return ch[0][1] if ch else None


def count_by_number(img, num):
    import struct
    h = S.O.Vol(img).blk(S._header_lbn(img, num))
    return struct.unpack_from('<H', h, 74)[0]


def count_and_backlink(img, spec):
    h = header_of(img, spec)
    if h is None:
        return None
    import struct
    return (struct.unpack_from('<H', h, 74)[0], struct.unpack_from('<H', h, 66)[0] | (h[71] << 16))


def delete_rule(work, name, level, hl):
    for state in ('first', 'second', 'twin'):
        for empty in (False, True):
            label = '%s %s state, %s directory' % (name, state, 'EMPTY' if empty else 'non-empty')
            base = S.build(work, level, hl, state, empty)
            qnum = S.fnum(base, '[000000]Q.DIR')
            # the OLD entry [P]X.DIR (state first: the only entry, back link Q; second: the old entry, back link Q; twin: the primary entry, back link P)
            for how, cmd in (('DELETE', 'DELETE [P]X.DIR'), ('wildcard DELETE', 'DELETE [P]*.DIR;*')):
                im = copy(base, work, 'old')
                h0 = sha(im)
                f0 = free_blocks(im)
                rc, out = S.ods2(im, cmd)
                if not empty:
                    if state == 'second':
                        check('%s: %s removes the OLD entry alone (the back-link directory [Q] holds another entry): [P] loses X.DIR, [Q] keeps it, the directory file stays and reads' % (label, how),
                              (False, True, False, (True, True), True, free_blocks(im) == f0), ('X.DIR;1' in listing(im, '[P]'), 'X.DIR;1' in listing(im, '[Q]'), 'not empty' in out, readable(im, '[Q.X]'),
                                                                                                      checker(im)[0] == [], True))
                    else:
                        check('%s: %s is refused: directory is not empty (the last entry known of a directory that holds files), the image is unchanged, the subtree reads' % (label, how),
                              (True, h0, (True, True)), ('not empty' in out, sha(im), readable(im, '[P.X]')))
                    continue
                # an EMPTY directory
                if state == 'second':
                    check('%s: %s removes the OLD entry alone: [P] loses X.DIR, [Q] keeps it, the directory [Q.X] opens, no dangling entry, no error' % (label, how),
                          (False, True, True, [], free_blocks(im) == f0), ('X.DIR;1' in listing(im, '[P]'), 'X.DIR;1' in listing(im, '[Q]'), 'DIRERR' not in S.ods2(im, 'DIR [Q.X]')[1],
                                                                          [w for w in checker(im)[1] if 'not in use' in w or 'dangling' in w], True))
                    if hl:
                        check('%s: %s: the link count went down with the entry (2 to 1)' % (label, how), 1, (count_and_backlink(im, '[Q]X.DIR;1') or (None,))[0])
                elif hl:
                    # review 72 S1: an EMPTY directory with a level-5 header and a count above 1 takes the link branch: the entry goes, the count goes down; the file stays
                    cb = count_and_backlink(im, '[Q]X.DIR;1') if state == 'twin' else None
                    check('%s: %s takes the HARDLINKS link branch (count 2): the entry goes, the directory file STAYS allocated (no blocks come back), the count is 1' % (label, how),
                          (False, True, 1), ('X.DIR;1' in listing(im, '[P]'), free_blocks(im) == f0, count_by_number(im, S.fnum(base, '[P]X.DIR'))))
                    if state == 'twin':
                        check('%s: %s: the twin entry [Q]X.DIR is still a valid directory (it opens), the checker finds no dangling entry' % (label, how),
                              (True, []), ('DIRERR' not in S.ods2(im, 'DIR [Q.X]')[1], [w for w in checker(im)[1] if 'not in use' in w or 'dangling' in w]))
                else:
                    check('%s: %s: the last entry known of an EMPTY directory: the directory FILE is deleted (its blocks come back), [P] no longer lists it' % (label, how),
                          (True, True), ('X.DIR;1' not in listing(im, '[P]'), free_blocks(im) > f0))
            if state == 'second':
                # DELETE of the NEW entry [Q]X.DIR: the back-link directory holds no other entry for it: the last entry known
                im = copy(base, work, 'new')
                h1 = sha(im)
                f1 = free_blocks(im)
                rc, out = S.ods2(im, 'DELETE [Q]X.DIR')
                if empty and hl:
                    cb = count_and_backlink(im, '[P]X.DIR;1')
                    check('%s: DELETE [Q]X.DIR of an EMPTY directory with a count of 2 takes the link branch: the entry and one off the count, the back link becomes (16777215,0,0), [P]X.DIR stays a valid directory' % label,
                          (False, True, 1, 0xFFFFFF, True, True), ('X.DIR;1' in listing(im, '[Q]'), 'X.DIR;1' in listing(im, '[P]'), cb[0] if cb else None, cb[1] if cb else None, 'DIRERR' not in S.ods2(im, 'DIR [P.X]')[1],
                                                                  free_blocks(im) == f1))
                elif empty:
                    check('%s: DELETE [Q]X.DIR of an EMPTY directory deletes the directory FILE (its header is freed and its blocks come back), as before' % label, (True, True, True),
                          ('X.DIR;1' not in listing(im, '[Q]'), free_blocks(im) > f1, header_of(im, '[Q]X.DIR;1') is None))
                else:
                    check('%s: DELETE [Q]X.DIR is refused: not empty, the image is unchanged, both paths read' % label, (True, h1, (True, True), (True, True)),
                          ('not empty' in out, sha(im), readable(im, '[Q.X]'), readable(im, '[P.X]')))


def no_hint(work, name, level, hl):
    """rev2's note on review 72: the refusal of a non-empty directory names the RENAME that finishes a stopped move only when the header names another directory: not for a back link of (16777215,0,0) or 0"""
    import struct
    for label, bl in (('(16777215,0,0)', (0xFFFF, 0, 0, 0xFF)), ('(0,0,0)', (0, 0, 0, 0))):
        im = S.build(work, level, hl, 'none', False, 'nh')
        num = S.fnum(im, '[P]X.DIR')
        lbn = S._header_lbn(im, num)
        with open(im, 'r+b') as f:
            f.seek(lbn * 512)
            hb = bytearray(f.read(512))
            struct.pack_into('<HHBB', hb, 66, *bl)
            struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
            f.seek(lbn * 512)
            f.write(bytes(hb))
        h0 = sha(im)
        rc, out = S.ods2(im, 'DELETE [P]X.DIR')
        check('%s a non-empty directory with the back link %s is refused without the stopped-RENAME hint, the image is unchanged' % (name, label), (True, False, h0), ('not empty' in out, 'RENAME' in out, sha(im)))
    # and with a back link that names another directory the hint is there
    im = S.build(work, level, hl, 'first', False, 'nh2')
    rc, out = S.ods2(im, 'DELETE [P]X.DIR')
    check('%s a non-empty directory whose header names another directory: the refusal says which RENAME finishes the move' % name, True, 'RENAME' in out and 'not empty' in out)


def checker_rules(work, name, level, hl):
    # the second state: one directory, two entries: an INFO line (not a warning: VMS makes such directories and its ANALYZE is silent); the first state and the state before: none
    for state, want in (('none', 0), ('first', 0), ('second', 1), ('twin', 1)):
        im = S.build(work, level, hl, state, False, 'ck')
        errs, warns, infos = checker(im, True)
        ent = [w for w in infos if re.search(r'directory \d+ \(.*\) is entered \d+ times', w)]
        check('%s ods5check -v, %s state: %d INFO line "directory N is entered k times"' % (name, state, want), want, len(ent))
        if want and ent:
            check('%s ods5check, %s state: the INFO says 2 times and names X.DIR' % (name, state), (True, True), ('entered 2 times' in ent[0], 'X.DIR' in ent[0]))
        check('%s ods5check, %s state: no ERROR, and the entered-twice line is no WARN' % (name, state), ([], []), (errs, [w for w in checker(im)[1] if 'entered' in w]))
    # a cycle: [P.X.S] enters [P] (a second entry of P inside its own subtree): an ERROR of its own
    im = S.build(work, level, hl, 'none', False, 'cy')
    S.ods2(im, 'COPY %s [P.X.S]STUBC.TXT' % os.path.join(work, 'ds_stub.txt'))
    S.ods2(im, 'LINK [P.X.S]P.DIR [P.X.S]STUBC.TXT')
    S.entry_fid(im, S.fnum(im, '[P.X]S.DIR'), 'P.DIR', S.fnum(im, '[000000]P.DIR'))
    if hl:
        S.set_count(im, S.fnum(im, '[P.X.S]STUBC.TXT'), 1)
    errs, warns, infos = checker(im, True)
    check('%s ods5check: a directory entered inside its own subtree is an ERROR (a cycle)' % name, (1, True), (len([e for e in errs if 'cycle' in e]), any('entered 2 times' in w for w in infos)))


def main():
    work = tempfile.mkdtemp(prefix='dir_delete_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for name, level, hl in KINDS:
            print('=== %s' % name)
            delete_rule(work, name, level, hl)
            checker_rules(work, name, level, hl)
            no_hint(work, name, level, hl)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('dir_delete_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
