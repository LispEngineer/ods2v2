#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""rename_dir_check.py - RENAME of a DIRECTORY file (PLAN_WRITE_OPS item 5, step 3; CORPUS 65.3, 66.3, 68.3 to 70; reviews 65 C4 and 71): within the parent, to another parent, moved and renamed at once,
the children untouched, the refusals (a move under itself by the typed path, the back links and the subtree below the source; the MFD; a name that is not name.DIR;1 or breaks the directory-name
rules; an alias entry; the depth limit), the finish of a stopped move, the adoption of a dead back link, the warning at the 9th name on ODS-2. At ODS-2, ODS-5 and ODS-5 HARDLINKS.
Called by tests/cli_rename_dir_check.sh; run from the project root (./ods2, ./ods2_init, ./ods2_fault built)."""
import hashlib
import os
import re
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dirstate as S    # noqa: E402
import ext_headers as E  # noqa: E402
import ods5check as O   # noqa: E402
import rename_check as R  # noqa: E402     (its helpers: hdr, fields, listing, other_bytes_same, cksum_ok, vms_now)

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


def checker(img):
    out = S.run(['python3', '-I', 'tests/ods5check.py', img])[1]
    return ([l for l in out.splitlines() if l.startswith('ERROR')], [l for l in out.splitlines() if l.startswith('WARN')])


def mk(work, level, hl, tag):
    """[P] holds X (F.TXT, the subdirectory S with G.TXT), Y (an empty directory) and B1; [Q] holds B2; [R] is empty; sleeps so that a RENAME's date is later than the creation's"""
    img = os.path.join(work, 'rd_%s_%d%s.img' % (tag, level, 'h' if hl else ''))
    if os.path.exists(img):
        os.remove(img)
    rc, out = S.run([S.INIT, img, '8192', '--structure=%d' % level, '--label=RDIR'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    texts = {'f': 'file F of X\n', 'g': 'file G of X.S\n', 'b': 'bystander\n'}
    paths = {}
    for k, t in texts.items():
        paths[k] = os.path.join(work, 'rd_%s.txt' % k)
        open(paths[k], 'w').write(t)
    cmds = ['CREATE/DIRECTORY [P]', 'CREATE/DIRECTORY [Q]', 'CREATE/DIRECTORY [R]', 'CREATE/DIRECTORY [P.X]', 'CREATE/DIRECTORY [P.X.S]', 'CREATE/DIRECTORY [P.Y]', 'COPY %s [P.X]F.TXT' % paths['f'],
            'COPY %s [P.X.S]G.TXT' % paths['g'], 'COPY %s [P]B1.TXT' % paths['b'], 'COPY %s [Q]B2.TXT' % paths['b']]
    for c in cmds:
        rc, out = S.ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    time.sleep(1.1)
    return img


def fresh(base, work, tag):
    c = os.path.join(work, 'w_%s.img' % tag)
    shutil.copyfile(base, c)
    return c


def tree_ok(img, base):
    """F and G read back through `base` ([Q.X], [P.X]...): the subtree is reachable"""
    return ('file F of X' in S.ods2(img, 'TYPE %s]F.TXT' % base[:-1])[1], 'file G of X.S' in S.ods2(img, 'TYPE %s.S]G.TXT' % base[:-1])[1])


def moved(work, base, kind, level, hl, tag, cmd, new_spec, new_name, parent_new, parent_old='[P]'):
    """one successful directory RENAME: header fields, the entries, the children and the parents untouched, the subtree reachable, the checker clean"""
    im = fresh(base, work, tag)
    before = R.hdr(im, '[P]X.DIR;1' if parent_old == '[P]' else parent_old + 'S.DIR;1')
    kids = {s_: R.hdr(im, s_) for s_ in ('[P.X]F.TXT;1', '[P.X.S]G.TXT;1', '[P.X]S.DIR;1') if s_ != (parent_old + 'S.DIR;1' if parent_old != '[P]' else None)}
    pdirs = {s: R.hdr(im, s) for s in ('[000000]P.DIR;1', '[000000]Q.DIR;1')}
    rc, out = S.ods2(im, cmd)
    label = '%s %s' % (kind, tag)
    check('%s: %s succeeds' % (label, cmd), (0, True), (rc, '%ODS2-E' not in out and '%RENAME-E' not in out and 'ILLCMD' not in out))
    after = R.hdr(im, new_spec)
    if before is None or after is None:
        check('%s: the directory is found under %s' % (label, new_spec), True, after is not None)
        return im
    fb, fa = R.fields(before, level), R.fields(after, level)
    check('%s: the header names %s' % (label, new_name), new_name, fa['name'])
    check('%s: revision number +1' % label, fb['revision'] + 1, fa['revision'])
    check('%s: no byte outside the back link, the name, the revision, the date field and the checksum changed' % label, [], R.other_bytes_same(before, after, level, True, ()))
    if level == 2:
        check('%s: ODS-2: the revision date is set' % label, True, fa['revdate'] > fb['revdate'])
    else:
        check('%s: ODS-5: the attribute change date is set, the revision date is not changed' % label, (True, fb['revdate']), (fa['attdate'] > fb['attdate'], fa['revdate']))
    if hl:
        check('%s: the link count is as it was (%d)' % (label, fb['linkcount']), fb['linkcount'], fa['linkcount'])
    want_bl = 4 if parent_new == '[000000]' else S.fnum(im, '[000000]%s.DIR' % parent_new.strip('[]'))
    check('%s: the back link is the directory %s' % (label, parent_new), want_bl, struct.unpack_from('<H', after, 66)[0])
    check('%s: the children (F.TXT, G.TXT, S.DIR) keep their headers byte for byte' % label, kids, {s_: R.hdr(im, _remap(s_, new_spec, cmd)) for s_ in kids})
    check('%s: the checker finds nothing wrong, no warning either' % label, ([], []), checker(im))
    return im


def _remap(spec, new_spec, cmd):
    """the child's spec under the moved directory's NEW path"""
    m = re.match(r'(\[.*?\])(.*)', new_spec)
    newdir = m.group(1)[1:-1] + '.' + m.group(2).split('.DIR')[0] if m.group(1) != '[000000]' else m.group(2).split('.DIR')[0]
    if cmd.startswith('RENAME [P.X]S.DIR'):
        return spec if not spec.startswith('[P.X.S]') else '[%s]%s' % (newdir, spec.split(']', 1)[1])
    return spec.replace('[P.X.S]', '[%s.S]' % newdir).replace('[P.X]', '[%s]' % newdir)


def table_cases(work, kind, level, hl):
    """the rows c1 and c2 of tests/rename_expect.py (CORPUS 65.3, checked against VMS's own dumps by ods5-cli's rename_expect_check.py): the same command, the same header changes"""
    key = {'ODS-2': '2', 'ODS-5': '5', 'ODS-5 HARDLINKS': '5h'}[kind]
    setup = {'c1': (['CREATE/DIRECTORY [X]', 'COPY {m} [X]M.TXT'], ('[X]M.TXT;1', '[X2]M.TXT;1')),
             'c2': (['CREATE/DIRECTORY [X2]', 'CREATE/DIRECTORY [X2.SUB]', 'COPY {m} [X2.SUB]S.TXT', 'CREATE/DIRECTORY [Y]'], ('[X2.SUB]S.TXT;1', '[Y.SUB]S.TXT;1'))}
    for cid in sorted(c for c, t in R.T.CASES.items() if t.get('dirbase')):
        t = R.T.CASES[cid]
        if key not in t['vols']:
            continue
        img = os.path.join(work, 'tc_%s_%d%s.img' % (cid, level, 'h' if hl else ''))
        if os.path.exists(img):
            os.remove(img)
        assert S.run([S.INIT, img, '8192', '--structure=%d' % level, '--label=TCASE'] + (['--hardlinks'] if hl else []))[0] == 0
        m = os.path.join(work, 'tc_m.txt')
        open(m, 'w').write('content of the child\n')
        for c in setup[cid][0]:
            rc, out = S.ods2(img, c.format(m=m))
            assert rc == 0, (c, out)
        time.sleep(1.1)
        tag = '%s %s' % (kind, cid)
        before = R.hdr(img, t['old'])
        others = {x: R.hdr(img, x) for x in (setup[cid][1][0],)}
        rc, out = S.ods2(img, 'RENAME %s %s' % (t['old'], t['new']))
        check('%s: RENAME %s %s succeeds' % (tag, t['old'], t['new']), (0, True), (rc, '%ODS2-E' not in out and '%RENAME-E' not in out))
        after = R.hdr(img, t['after'])
        if before is None or after is None:
            check('%s: the directory is found under %s' % (tag, t['after']), True, after is not None)
            continue
        fb, fa = R.fields(before, level), R.fields(after, level)
        check('%s: the stored name goes %s to %s' % (tag, t['stored'][0], t['stored'][1]), t['stored'], (fb['name'], fa['name']))
        check('%s: revision number +%d' % (tag, t['revision']), fb['revision'] + t['revision'], fa['revision'])
        check('%s: no byte outside the name, the revision, the date field and the checksum%s changed' % (tag, ' (and the back link)' if t.get('backlink') == 'moved' else ''), [],
              R.other_bytes_same(before, after, level, t.get('backlink') == 'moved', ()))
        if t.get('backlink') == 'moved':
            check('%s: the back link is %s' % (tag, t['dir2']), S.fnum(img, '[000000]%s.DIR' % t['dir2'].strip('[]')), struct.unpack_from('<H', after, 66)[0])
        if level == 2:
            check('%s: ODS-2: the revision date is set' % tag, True, fa['revdate'] > fb['revdate'])
        else:
            check('%s: ODS-5: the attribute change date is set, the revision date is not changed' % tag, (True, fb['revdate']), (fa['attdate'] > fb['attdate'], fa['revdate']))
        lb = R.listing(img, t['dir'])
        la2 = R.listing(img, t['dir2'] if t.get('dir2') else t['dir'])
        check('%s: %s loses %s and %s gains %s' % (tag, t['dir'], t['removed'], t.get('dir2') or t['dir'], t['added']), (False, True), (any(x in lb for x in t['removed']) and t['removed'][0] != t['added'][0] or (t['removed'][0] == t['added'][0] and t.get('dir2') is None), all(x in la2 for x in t['added'])))
        check('%s: the child is untouched (its header is byte for byte the same) and reads under the new path' % tag, (others[setup[cid][1][0]], True), (R.hdr(img, setup[cid][1][1]), 'content of the child' in S.ods2(img, 'TYPE %s' % setup[cid][1][1])[1]))
        check('%s: the checker finds nothing wrong' % tag, ([], []), checker(img))


def cases(work, kind, level, hl):
    base = mk(work, level, hl, 'base')
    # --- success: within the parent, to another parent, moved and renamed, a subdirectory out, into the MFD (CORPUS 65.3 c1 c2 and the deep case)
    im = moved(work, base, kind, level, hl, 'a1', 'RENAME [P]X.DIR [P]X2.DIR', '[P]X2.DIR;1', 'X2.DIR;1', '[P]')
    check('%s a1: [P] lists X2.DIR, not X.DIR; the subtree reads under [P.X2]' % kind, (True, False, (True, True)), ('X2.DIR;1' in R.listing(im, '[P]'), 'X.DIR;1' in R.listing(im, '[P]'), tree_ok(im, '[P.X2]')))
    im = moved(work, base, kind, level, hl, 'a2', 'RENAME [P]X.DIR [Q]', '[Q]X.DIR;1', 'X.DIR;1', '[Q]')
    check('%s a2: [P] loses X.DIR, [Q] gains it; the subtree reads under [Q.X] and not under [P.X]' % kind, (False, True, (True, True), (False, False)),
          ('X.DIR;1' in R.listing(im, '[P]'), 'X.DIR;1' in R.listing(im, '[Q]'), tree_ok(im, '[Q.X]'), tree_ok(im, '[P.X]')))
    im = moved(work, base, kind, level, hl, 'a3', 'RENAME [P]X.DIR [Q]Z.DIR', '[Q]Z.DIR;1', 'Z.DIR;1', '[Q]')
    check('%s a3: the subtree reads under [Q.Z]' % kind, (True, True), tree_ok(im, '[Q.Z]'))
    im = moved(work, base, kind, level, hl, 'a4', 'RENAME [P]X.DIR [Q]Z', '[Q]Z.DIR;1', 'Z.DIR;1', '[Q]')       # S3: the missing type comes from the source
    im = moved(work, base, kind, level, hl, 'a5', 'RENAME [P.X]S.DIR [Q]', '[Q]S.DIR;1', 'S.DIR;1', '[Q]', '[P.X]')
    check('%s a5: G.TXT reads under [Q.S] and X no longer holds S' % kind, (True, False), ('file G of X.S' in S.ods2(im, 'TYPE [Q.S]G.TXT')[1], 'S.DIR;1' in R.listing(im, '[P.X]')))
    im = fresh(base, work, 'a6')
    rc, out = S.ods2(im, 'RENAME [P]X.DIR [000000]')
    check('%s a6: into the master directory: X is listed in [000000] and its subtree reads, the checker is clean' % kind, (0, True, (True, True), ([], [])),
          (rc, 'X.DIR;1' in R.listing(im, '[000000]'), tree_ok(im, '[X]'), checker(im)))
    # --- refusals: the image is unchanged
    refuse = (('b1 into itself', 'RENAME [P]X.DIR [P.X]', 'IDR'), ('b2 into its own subdirectory', 'RENAME [P]X.DIR [P.X.S]', 'IDR'), ('b3 an ancestor into its subtree', 'RENAME [000000]P.DIR [P.X.S]', 'IDR'),
              ('b4 the master directory', 'RENAME [000000]000000.DIR [Q]', 'IDR'), ('b5 a name that is not .DIR', 'RENAME [P]X.DIR [Q]X.TXT', 'not'), ('b6 a version other than 1', 'RENAME [P]X.DIR [Q]Z.DIR;2', 'not'),
              ('b7 an existing directory name', 'RENAME [P]X.DIR [P]Y.DIR', 'exists'), ('b8 a typed type that is not DIR', 'RENAME [P]X.DIR [Q]Z.DAT', 'not'))
    for tag, cmd, word in refuse:
        im = fresh(base, work, tag[:2])
        h0 = sha(im)
        rc, out = S.ods2(im, cmd)
        ok_text = ('-RMS-E-IDR' in out) if word == 'IDR' else (word in out)
        check('%s %s: %s is refused (%s), the image is unchanged' % (kind, tag, cmd, word), (True, True, h0), (ok_text, '%ODS2-I-RENAMED' not in out, sha(im)))
    if level == 2:
        im = fresh(base, work, 'b9')
        h0 = sha(im)
        rc, out = S.ods2(im, 'RENAME [P]X.DIR [Q]-B.DIR')
        check('%s b9: a name CREATE/DIRECTORY would refuse (-B on ODS-2) is refused, the image is unchanged' % kind, (True, h0), ('%ODS2-I-RENAMED' not in out, sha(im)))
    # --- the stopped states (made by hand): finish, adopt, alias, the cycle of review 71 C1
    for state in ('first', 'second'):
        sb = S.build(work, level, hl, state, False, 'rs')
        im = fresh(sb, work, 'fin_' + state)
        rc, out = S.ods2(im, 'RENAME [P]X.DIR [Q]')
        check('%s %s state: RENAME [P]X.DIR [Q] again finishes the move: one entry, in [Q], the subtree reads, the checker is clean' % (kind, state), (0, False, True, (True, True), ([], [])),
              (rc, 'X.DIR;1' in R.listing(im, '[P]'), 'X.DIR;1' in R.listing(im, '[Q]'), tree_ok(im, '[Q.X]'), checker(im)))
        if state == 'second':
            im = fresh(sb, work, 'alias')
            h0 = sha(im)
            rc, out = S.ods2(im, 'RENAME [P]X.DIR [P]W.DIR')
            check('%s second state: RENAME of the old entry to another name in [P]: the entry is not the directory\'s primary one and [Q] holds another: refused (an alias entry), the image is unchanged' % kind, (True, h0),
                  ('%ODS2-I-RENAMED' not in out and 'alias' in out.lower(), sha(im)))
            # C1: the cycle: P would be entered in Q.X, which is X, which P holds
            im = fresh(sb, work, 'cyc')
            h0 = sha(im)
            rc, out = S.ods2(im, 'RENAME [000000]P.DIR [Q.X]')
            check('%s second state: RENAME [000000]P.DIR [Q.X] (X is entered in P and in Q: P would be entered inside itself) is refused, IDR, the image is unchanged' % kind, (True, h0), ('-RMS-E-IDR' in out, sha(im)))
        else:
            im = fresh(sb, work, 'adopt')
            S.ods2(im, 'CREATE/DIRECTORY [R]')
            rc, out = S.ods2(im, 'RENAME [P]X.DIR [R]')
            check('%s first state: RENAME to a THIRD parent adopts the directory (its back link named [Q], which has no entry): [R] gains it, [P] loses it, the checker is clean, the subtree reads' % kind,
                  (0, False, True, (True, True), ([], [])), (rc, 'X.DIR;1' in R.listing(im, '[P]'), 'X.DIR;1' in R.listing(im, '[R]'), tree_ok(im, '[R.X]'),
                  (lambda e, w: (e, [x for x in w if not (hl and 'link count 2 but 1' in x)]))(*checker(im))))     # (HARDLINKS: the count the first write added stays: the known leak, a WARN)
    # --- the depth limit (the test binary lowers it) and the ODS-2 warning at the 9th name
    deep = mk(work, level, hl, 'deep')
    for c in ('CREATE/DIRECTORY [D1]', 'CREATE/DIRECTORY [D1.D2]', 'CREATE/DIRECTORY [D1.D2.D3]'):
        assert S.ods2(deep, c)[0] == 0, c
    im = fresh(deep, work, 'dep')
    h0 = sha(im)
    rc, out = S.run(['env', 'ODS2_TEST_MAX_DIR_DEPTH=4', './ods2_fault', im, 'RENAME [P]X.DIR [D1.D2.D3]'])
    check('%s depth: with the limit 4, X (one directory below it) into [D1.D2.D3] (depth 3) would reach level 5: refused, the image is unchanged' % kind, (True, h0), ('deep' in out.lower() and '%ODS2-I-RENAMED' not in out, sha(im)))
    rc, out = S.run(['env', 'ODS2_TEST_MAX_DIR_DEPTH=4', './ods2_fault', im, 'RENAME [P]X.DIR [D1.D2]'])
    check('%s depth: the same into [D1.D2] (level 4) goes through' % kind, (0, True), (rc, 'X.DIR;1' in R.listing(im, '[D1.D2]')))
    if level == 2:
        # the VAX opens 8 names and fails on the 9th (CORPUS 12.3, 65.3; 66.3 after a RENAME): warn ABOVE 8 (review 73 S5)
        im = fresh(deep, work, 'nine')
        chain = 'D1.D2.D3'
        for i in range(4, 8):
            chain += '.D%d' % i
            assert S.ods2(im, 'CREATE/DIRECTORY [%s]' % chain)[0] == 0
        rc, out = S.ods2(im, 'RENAME [P]Y.DIR [%s]' % chain)
        check('%s eight names: the empty directory Y moved to [%s] is the 8th name: succeeds, no warning' % (kind, chain), (0, False), (rc, 'DEEPDIR' in out))
        rc, out = S.ods2(im, 'RENAME [P]X.DIR [%s]' % chain)
        check('%s nine names: X moved to [%s] (its subdirectory S is the 9th name) succeeds and WARNS (the VAX cannot open the 9th name: CORPUS 66.3)' % (kind, chain), (0, True), (rc, 'DEEPDIR' in out and 'more than 8 names' in out))
    # review 73 S1: a tree that holds a directory alias cannot be moved; the message says how to go on
    im = mk(work, level, hl, 'alias')
    assert S.ods2(im, 'COPY %s [P.X.S]STUBA.TXT' % os.path.join(work, 'rd_b.txt'))[0] == 0
    if not hl:
        assert S.ods2(im, 'LINK [P.X.S]Y.DIR [P.X.S]STUBA.TXT')[0] == 0
        S.entry_fid(im, S.fnum(im, '[P.X]S.DIR'), 'Y.DIR', S.fnum(im, '[P]Y.DIR'))
        h0 = sha(im)
        rc, out = S.ods2(im, 'RENAME [000000]P.DIR [R]')
        check('%s S1: a tree with a directory alias (Y entered in P and inside P.X.S) cannot be moved: IDR-free refusal naming the entry and saying to run the stopped RENAME again OR DELETE the alias entry, the image is unchanged' % kind,
              (True, True, h0), ('entered twice' in out and 'DELETE the alias entry' in out, '%ODS2-I-RENAMED' not in out, sha(im)))
    # review 73 S2: moving into a directory whose back link is dead: the message names the way out; renaming that directory first adopts it
    im = fresh(base, work, 'dead')
    qnum = S.fnum(im, '[000000]Q.DIR')
    lbn = S._header_lbn(im, qnum)
    with open(im, 'r+b') as f:
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        struct.pack_into('<HHBB', hb, 66, 0xFFFF, 0, 0, 0xFF)
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(hb))
    h0 = sha(im)
    rc, out = S.ods2(im, 'RENAME [P]X.DIR [Q]')
    check('%s S2: a move into a directory whose back link is (16777215,0,0) is refused with the way out (RENAME that directory first), the image is unchanged' % kind, (True, True, h0),
          ('RENAME that directory first' in out, '%ODS2-I-RENAMED' not in out, sha(im)))
    rc, out = S.ods2(im, 'RENAME [000000]Q.DIR [000000]Q2.DIR')
    rc2, out2 = S.ods2(im, 'RENAME [P]X.DIR [Q2]')
    check('%s S2: RENAME of that directory adopts it (name and back link written); then the move into it goes through' % kind, (0, 0, True), (rc, rc2, 'X.DIR;1' in R.listing(im, '[Q2]')))
    im = fresh(base, work, 'wording')
    rc, out = S.ods2(im, 'RENAME [P]X.DIR [P]Y.DIR')
    check('%s review 73: an existing target name answers for a DIRECTORY ("a directory is not given another version"), not the file wording' % kind, (True, False), ('a directory is not given another version' in out, 'a file cannot be made' in out))


def main():
    work = tempfile.mkdtemp(prefix='rename_dir_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for kind, level, hl in KINDS:
            print('=== %s' % kind)
            table_cases(work, kind, level, hl)
            cases(work, kind, level, hl)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('rename_dir_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
