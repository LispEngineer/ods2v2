#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""fault_inject.py - crash consistency of every write operation (review 6, finding 2).

The test build of ods2 (ods2_fault, built with ODS2_FAULT_INJECTION: see src/ods2_volume.c) ends the process, as a kill or a
power cut would, after the k-th write to the volume when ODS2_FAULT_AFTER_WRITES=k. For each operation
this script learns n, the number of writes (ODS2_FAULT_COUNT=1), then runs it on a fresh copy of a base
volume for k = 1..n and checks what is left:

  * the independent structure checker (ods5check.py): the only errors allowed are leaks (a lost file, a
    header or bit with no counterpart, a file marked for delete); never a file that is still listed
    with its clusters free, never a cluster owned twice, never any other kind of damage;
  * every file other than the operation's own targets still reads back with its original bytes;
  * one more COPY then works, and the same two checks hold again (the new file reads back too).

Usage: fault_inject.py [--quick] [--level 2|5] [--op NAME ...] [--keep]   (run from the project root)
Exit status 1 if any check failed; the report names the operation, level and k.
"""
import argparse, hashlib, os, random, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # tests/ext_headers.py and tests/setfile_ext_check.py (the interpreter may run with -I)

os.environ['ODS5CHECK_ALLOW_ODS2V2_INDEXF_EXT'] = '1'   # the index_* operations make INDEXF.SYS extension headers on purpose
ODS2 = os.environ.get('FAULT_ODS2', './ods2_fault')
INIT = './ods2_init'
CHECK = ['python3', '-I', 'tests/ods5check.py']
NEXT = {}   # base image -> the name whose insert grows the directory
WORK = None   # a directory of this run's own (mkdtemp under TMPDIR): two runs at once never meet (review 42); removed at the end unless --keep
KEEP = False

ALLOWED = [
    r'is in no directory \(lost file\)',
    r'is deleted but its index bitmap bit is set',
    r'bitmap bit set but no valid header',
    r'its back link directory \d+ has no entry for it',
    r'is not reachable from the root',
    r'which is marked for delete \(a half-done delete\)',   # DELETE's first write: the header is marked, the entry goes next
]


def run(args, env=None, inp=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, text=True, errors='replace')
    return p.returncode, p.stdout


def ods2(img, cmd, env=None):
    return run([ODS2, img, cmd], env)


def md5(path):
    return hashlib.md5(open(path, 'rb').read()).hexdigest()


def tree(img, tag):
    """relative path -> md5 of every file EXPORTed from the volume (the reserved files are left out)"""
    out = os.path.join(WORK, 'ex_' + tag)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    rc, text = ods2(img, 'EXPORT [000000...]*.*;* %s' % out)
    files = {}
    for dp, dn, fn in os.walk(out):
        for d in dn:                      # an empty directory is a name too: it must stay listed
            files['dir:' + os.path.relpath(os.path.join(dp, d), out)] = 'dir'
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), out)
            if rel.split(os.sep)[-1].upper().startswith(('INDEXF.', 'BITMAP.', 'BADBLK.', 'CORIMG.', 'VOLSET.', 'CONTIN.',
                                                          'BACKUP.', 'BADLOG.', 'SECURITY.')) and os.sep not in rel:
                continue
            full = os.path.join(dp, f)
            files[rel] = ('link:' + os.readlink(full)) if os.path.islink(full) else md5(full)
    return rc, files, text


# A block that splits inside a directory with blocks after it is rewritten so that every name stays listed at
# every write, a name being listed twice for a moment (F2): the checker reports that as duplicate entries and
# a block out of order. Allowed for those operations only; a name missing is never allowed (the tree compare).
SPLIT_TRANSIENT = [r'two entries for', r'block \d+: .* out of order', r"versions not descending"]
# operation -> (old key, new key) of the renamed file as tree() names it (the keys of OPS' targets, with the version); the check is in main()
# the checker's errors a stopped RENAME may show, BY NAME (review 65 C6): none of the generic leak classes of ALLOWED (a lost file, a header or bitmap bit with no counterpart, a file marked
# for delete, a back link directory without an entry): a rename that loses a header or a block must be seen. The two-entry states (one file under two names, in one directory or two) are
# no error of the checker; the transient duplicates of a split or a shrink are in EXTRA_ALLOWED for the operations that make them.
RENAME_ALLOWED = []
HF_NO_WARN_OPS = ('rename_move_limit', 'rename_alias', 'rename_hardlink', 'rename_case', 'rename_move', 'rename_move_alias', 'rename_move_primary', 'rename_move_hardlink', 'rename_move_primary_hardlink', 'rename_move_ext')     # the header is not rewritten (an alias), or the spelling only differs (no name differs)
MOVE_OPS = ['rename_move', 'rename_move_name', 'rename_move_exist', 'rename_move_alias', 'rename_move_primary', 'rename_move_hardlink', 'rename_move_primary_hardlink', 'rename_move_split', 'rename_move_ext', 'rename_move_limit']
MOVE_ALIAS_OPS = ('rename_move_alias', 'rename_move_hardlink')               # the header is not written: no header-first state
MOVE_FIRST_STATE_ERROR = r'its back link directory \d+ has no entry for it'     # the ONE error of the checker the first state of a primary move has (the header names the target directory, which has no entry yet)
RENAME_PAIRS = {'rename_move': ('A1.TXT;1', 'SUB/A1.TXT;1'), 'rename_move_name': ('A1.TXT;1', 'SUB/A9.TXT;1'), 'rename_move_exist': ('A1.TXT;1', 'SUB/D.TXT;2'),
                'rename_move_alias': ('SUB/LNKA.TXT;1', 'EMPTY/LNKA.TXT;1'), 'rename_move_primary': ('L.TXT;1', 'EMPTY/L.TXT;1'), 'rename_move_hardlink': ('SUB/LNKA.TXT;1', 'EMPTY/LNKA.TXT;1'),
                'rename_move_primary_hardlink': ('L.TXT;1', 'EMPTY/L.TXT;1'), 'rename_move_split': ('A1.TXT;1', 'FULL/%(next)s;1'), 'rename_move_ext': ('F/A.DAT;1', 'A2.DAT;1'), 'rename_move_limit': ('SUB/D.TXT;1', 'V.TXT;3'),
                'rename_same_dir': ('A1.TXT;1', 'A9.TXT;1'), 'rename_type': ('B.DAT;1', 'B.TXT;1'), 'rename_newver': ('A1.TXT;1', 'W.TXT;10'),
                'rename_alias': ('SUB/LNKA.TXT;1', 'SUB/LNKB.TXT;1'), 'rename_primary_alias': ('L.TXT;1', 'L2.TXT;1'), 'rename_hardlink': ('SUB/LNKA.TXT;1', 'SUB/LNKB.TXT;1'),
                'rename_primary_hardlink': ('L.TXT;1', 'L2.TXT;1'), 'rename_split': ('FULL/N00000.TXT;1', 'FULL/%(next)s;1'),
                'rename_case': ('abc.txt;1', 'ABC.txt;1'), 'rename_ext': ('F/A.DAT;1', 'F/A2.DAT;1'), 'rename_shrink_last': ('SH/%(next)s;1', 'SH/AAA_FIRST.TXT;1'), 'rename_shrink_mid': ('SH/%(next)s;1', 'SH/AAA_FIRST.TXT;1')}
SHRINK_OPS = ['delete_dirshrink', 'delete_dirshrink_mid', 'rename_shrink_last', 'rename_shrink_mid']     # operations after which no directory block may be all zeros inside the end of file (a WARN of the checker)
SPLIT_TRANSIENT = [r'two entries for', r'block \d+: .* out of order', r"BAD_VERSORDER"]
SPLIT_OPS = ['insert_split', 'insert_split_mid', 'insert_move', 'insert_split_big', 'insert_move_big', 'newver_split']
EXTRA_ALLOWED = {'insert_split': SPLIT_TRANSIENT, 'insert_split_mid': SPLIT_TRANSIENT, 'insert_move': SPLIT_TRANSIENT,
                 'insert_split_big': SPLIT_TRANSIENT, 'insert_move_big': SPLIT_TRANSIENT, 'newver_split': SPLIT_TRANSIENT,
                 'delete_dirshrink_mid': SPLIT_TRANSIENT, 'rename_split': SPLIT_TRANSIENT, 'rename_move_split': SPLIT_TRANSIENT, 'rename_move_limit': ALLOWED, 'rename_shrink_last': SPLIT_TRANSIENT, 'rename_shrink_mid': SPLIT_TRANSIENT}   # the blocks after the emptied one move up one at a time: a name is listed twice for a moment, never missing


# Growing INDEXF.SYS when its last header is an extension header changes two blocks that cannot be written as one: the
# extension header (its map, its link) and the primary (HIBLK, EFBLK). Between the two writes the primary says fewer
# blocks than the map holds, and the new extension header's bit and slot lie past the end-of-file (the checker does not
# find the header: "extension header missing"). Allowed for these operations
# at the interrupted state only: the first change of any kind (a relabel as well as a create) heals both, and the checks
# after it and after the follow-up COPYs allow nothing of it (review 27 F2, review 28 a). A permanent HIBLK error, a header written over INDEXF.SYS's own extension header, a chain
# that cannot grow: all caught by the second check and the follow-up COPYs.
INDEX_TRANSIENT = [r'file 1: HIBLK \d+ but extents hold \d+ blocks', r'index bitmap bit \d+ set beyond the index file EOF',
                   r'file 1: extension header \d+ missing']
INDEX_OPS = ['index_ext1', 'index_tail', 'index_ext2']
OP_ENV = {n: {'ODS2_TEST_INDEXF': 'step=2,nomerge'} for n in INDEX_OPS}


SEEN = {}


def check(img, op=None, transient=True):
    rc, text = run(CHECK + [img])
    errs = [l for l in text.splitlines() if l.startswith('ERROR')]
    bad, leaks = [], []
    extra = EXTRA_ALLOWED.get(op, []) + (INDEX_TRANSIENT if transient and op in INDEX_OPS else [])
    for l in errs:
        for a in extra:
            if re.search(a, l):
                SEEN[a] = SEEN.get(a, 0) + 1
        if any(re.search(a, l) for a in (RENAME_ALLOWED if op in RENAME_PAIRS else ALLOWED) + extra):
            leaks.append(l)
        else:
            bad.append(l)
    return bad, leaks


def dup_names(img):
    """the (name, version) of every entry the checker finds listed twice in one directory"""
    rc, text = run(CHECK + [img])
    return sorted(set(re.findall(r"dir \d+ \([^)]*\): two entries for '([^']+)';(\d+)", text.replace('\n', '\n') if False else text)))


def fid_of(img, path):
    rc, out = ods2(img, 'DIR/FULL ' + path)
    m = re.search(r'File ID:\s+\((\d+),', out)
    return int(m.group(1)) if m else None


def efblk(img, fnum):
    rc, out = run(['./ods2_dump_header', img, str(fnum)])
    m = re.search(r'efblk\(swapped\)=(\d+)', out)
    return int(m.group(1)) if m else None


def blob(n, seed):
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def put_host(name, data):
    p = os.path.join(WORK, name)
    open(p, 'wb').write(data)
    return p


def index_state(img):
    """INDEXF.SYS's chain: HIBLK, the extension header numbers, the map bytes in use in the last header"""
    hiblk, chain, n, used = 0, [], 1, 0
    while len(chain) < 70000:
        rc, out = run(['./ods2_dump_header', img, str(n)])
        m = re.search(r'ext_fid=\((\d+),', out)
        u = re.search(r'map_inuse=(\d+)', out)
        if n == 1:
            hiblk = int(re.search(r'hiblk\(swapped\)=(\d+)', out).group(1))
        used = int(u.group(1)) * 2 if u else 0
        if not m or int(m.group(1)) == 0:
            break
        n = int(m.group(1))
        chain.append(n)
    return {'hiblk': hiblk, 'chain': chain, 'used': used}


def setup_index(level, kind):
    """A base volume in which the next COPY grows INDEXF.SYS in the way `kind` names (test hook: two blocks at a time, in
    separate pieces, so that a few hundred empty files fill the maps): 'ixext1' makes the first extension header (the
    primary is the last header), 'ixtail' adds a piece to the last extension header, 'ixext2' makes the second extension
    header (the last extension header is nearly full)."""
    img = os.path.join(WORK, 'base_%s_%d.img' % (kind, level))
    assert run([INIT, img, '8192', '--structure=%d' % level, '--cluster=1', '--maxfiles=3000', '--label=FAULT'])[0] == 0
    env = {'ODS2_TEST_INDEXF': 'step=2,nomerge'}
    empty = put_host('empty.txt', b'')
    for dst in ('A1.TXT', 'B.TXT'):
        assert ods2(img, 'COPY %s [000000]%s' % (put_host('small.txt', b'one block of text\n'), dst), env)[0] == 0
    counter = [0]

    def copies(count):
        script = os.path.join(WORK, 'ix.ods2')
        with open(script, 'w') as f:
            for _ in range(count):
                counter[0] += 1
                f.write('COPY %s [000000]G%05d.TXT\n' % (empty, counter[0]))
        rc, out = ods2(img, '@' + script, env)
        assert rc == 0, out

    def step_until(fire, batch=8):
        snap = img + '.snap'
        for _ in range(2000):
            shutil.copyfile(img, snap)
            saved = counter[0]
            before = index_state(img)
            copies(batch)
            if fire(before, index_state(img)):
                shutil.copyfile(snap, img)
                counter[0] = saved
                for _ in range(batch):
                    shutil.copyfile(img, snap)
                    saved = counter[0]
                    before = index_state(img)
                    copies(1)
                    if fire(before, index_state(img)):
                        shutil.copyfile(snap, img)
                        counter[0] = saved
                        return
                raise AssertionError('the growth was in the batch but not in its single steps')
        raise AssertionError('no growth of the kind ' + kind)

    ext1 = lambda b, a: not b['chain'] and a['chain']
    if kind == 'ixext1':
        step_until(ext1)
    elif kind == 'ixtail':
        step_until(ext1)
        copies(1)
        step_until(lambda b, a: a['hiblk'] > b['hiblk'] and a['chain'] == b['chain'] and b['chain'])
    else:
        step_until(lambda b, a: len(b['chain']) == 1 and len(a['chain']) == 2)
    return img


def setup_limit(level, kind):
    """A volume for the version limit with versions of DIFFERENT contents of 2.2 MiB each (three content chunks of the write): 'limit3big' has
    A.BIN;1 to ;3 with the limit 3, 'limit1big' has A.BIN;1 with the limit 1. The next COPY removes the oldest version, so every
    interruption of it is a chance to see the old version's blocks overwritten (review 40 F1)."""
    img = os.path.join(WORK, 'base_%s_%d.img' % (kind, level))
    assert run([INIT, img, '32768', '--structure=%d' % level, '--label=FAULT'])[0] == 0
    assert ods2(img, 'CREATE/DIRECTORY [BIG]')[0] == 0
    count = 3 if kind == 'limit3big' else 1
    for v in range(count):
        src = put_host('ver%d.bin' % v, blob(int(2.2 * 1024 * 1024), 10 + v))
        assert ods2(img, 'COPY %s [BIG]A.BIN' % src)[0] == 0
    assert ods2(img, 'SET FILE/VERSION_LIMIT=%d [BIG]A.BIN' % count)[0] == 0
    return img


def setup(level, hardlinks, fill_for=None, cluster=None):
    """the base volume: files of several sizes (one fragmented over holes), subdirectories, links"""
    if fill_for and fill_for.startswith('ix'):
        return setup_index(level, fill_for)
    if fill_for in ('limit3big', 'limit1big'):
        return setup_limit(level, fill_for)
    if fill_for == 'sfext':
        import setfile_ext_check      # two files of about 400 pieces each: a primary header and several extension headers (A.DAT is the one SET FILE changes)
        return setfile_ext_check.build(level, WORK)
    img = os.path.join(WORK, 'base_%d%s%s%s.img' % (level, 'h' if hardlinks else '', fill_for or '', cluster or ''))
    if os.path.exists(img):
        os.remove(img)
    a = [INIT, img, '65536' if cluster else '8192', '--structure=%d' % level, '--label=FAULT']
    if cluster:
        a.append('--cluster=%d' % cluster)
    if hardlinks and level == 5:
        a.append('--hardlinks')
    rc, out = run(a)
    assert rc == 0, out
    cmds = ['CREATE/DIRECTORY [SUB]', 'CREATE/DIRECTORY [EMPTY]', 'CREATE/DIRECTORY [FULL]']
    for c in cmds:
        assert ods2(img, c)[0] == 0
    small = put_host('small.txt', b'one block of text\n')
    mid = put_host('mid.dat', blob(5000, 1))
    big = put_host('big.dat', blob(30000, 2))
    for dst, src in (('[000000]A1.TXT', small), ('[000000]L.TXT', small), ('[000000]B.DAT', mid), ('[000000]C.DAT', big), ('[SUB]D.TXT', small)):
        rc, out = ods2(img, 'COPY %s %s' % (src, dst))
        assert rc == 0, out
    if not cluster:
        # fragmentation: 16-block files fill the volume, every other one is deleted, then FRAG.DAT (1600 blocks)
        # takes 100 of the 16-block holes: 100 extents, so it has extension headers
        filler = put_host('filler.dat', blob(8192, 3))
        script = os.path.join(WORK, 'fill.ods2')
        with open(script, 'w') as f:
            f.write('CREATE/DIRECTORY [FILL]\n')
            for i in range(600):
                f.write('COPY %s [FILL]F%03d.DAT\n' % (filler, i))
            for i in range(0, 600, 2):
                f.write('DELETE [FILL]F%03d.DAT\n' % i)
        run([ODS2, img, '@' + script])
        assert ods2(img, 'COPY %s [000000]FRAG.DAT' % put_host('frag.dat', blob(1600 * 512, 9)))[0] == 0
    # names with versions, for the new-version operations: V.TXT;1 and ;2 with a version limit of 2, W.TXT;1 and ;9
    for _ in (1, 2):
        assert ods2(img, 'COPY %s [000000]V.TXT' % small)[0] == 0
    assert ods2(img, 'SET FILE/VERSION_LIMIT=2 [000000]V.TXT')[0] == 0
    for v in (1, 9):
        assert ods2(img, 'COPY %s [000000]W.TXT;%d' % (small, v))[0] == 0
    # PURGE: [PG]P.TXT;1 to ;4 and Q.TXT;1 ;2 (PLAN_WRITE_OPS item 2)
    assert ods2(img, 'CREATE/DIRECTORY [PG]')[0] == 0
    for nm, count in (('P.TXT', 4), ('Q.TXT', 2)):
        for _ in range(count):
            assert ods2(img, 'COPY %s [PG]%s' % (small, nm))[0] == 0
    # one file with another directory entry
    assert ods2(img, 'LINK [SUB]LNKA.TXT [000000]L.TXT')[0] == 0
    if level == 5:
        assert ods2(img, 'SYMLINK [000000]SYMA.TXT L.TXT')[0] == 0
    if fill_for:
        # [FULL] is filled to just before an insert that grows it by a block: 'split' (one block to two: the
        # new name sorts last), 'splitmid' (names in random order, three or more blocks: the block that
        # splits has blocks after it, which move up one), 'move' (three blocks in use, as many as are
        # allocated: the directory moves to a new, larger area). The next name to add is returned in NEXT.
        if fill_for == 'dirshrink':
            # [SH]: names added in order until the directory holds four blocks, then the last names are deleted one by one (cleanly) until the next delete
            # would empty the last block: that name is the operation's target (review 48 N10: the zeroed block was written before the end of file moved back)
            assert ods2(img, 'CREATE/DIRECTORY [SH]')[0] == 0
            fid = fid_of(img, '[000000]SH.DIR')
            names = []
            for i in range(1, 400):
                name = 'LONGNAME_NUMBER_%03d_ABCDEFGHIJ.TXT' % i
                assert ods2(img, 'COPY %s [SH]%s' % (small, name))[0] == 0
                names.append(name)
                if efblk(img, fid) >= 5:
                    break
            assert efblk(img, fid) >= 5, 'the directory never grew'
            for name in reversed(names):
                probe = img + '.probe'
                shutil.copyfile(img, probe)
                assert ods2(probe, 'DELETE [SH]%s' % name)[0] == 0
                shrinks = efblk(probe, fid) < efblk(img, fid)
                os.remove(probe)
                if shrinks:
                    NEXT[img] = name
                    return img
                assert ods2(img, 'DELETE [SH]%s' % name)[0] == 0
            raise AssertionError('no delete shrinks the directory')
        if fill_for == 'caseonly':
            assert ods2(img, 'COPY %s [000000]abc.txt' % small)[0] == 0      # a lower-case name (ODS-5 keeps the case)
            return img
        if fill_for == 'dirshrinkmid':
            # [SH] of six or more blocks; the names around the middle one are deleted (cleanly), pair by pair, until the delete of the middle name would empty ITS block, which has
            # blocks after it: the shrink moves those blocks up (review 64 S1: the last block was cleared before the end of file moved back on this path too)
            assert ods2(img, 'CREATE/DIRECTORY [SH]')[0] == 0
            fid = fid_of(img, '[000000]SH.DIR')
            names = []
            for i in range(1, 400):
                name = 'LONGNAME_NUMBER_%03d_ABCDEFGHIJ.TXT' % i
                assert ods2(img, 'COPY %s [SH]%s' % (small, name))[0] == 0
                names.append(name)
                if efblk(img, fid) >= 7:
                    break
            assert efblk(img, fid) >= 7, 'the directory never grew'
            m = len(names) // 2
            for d in range(0, 12):
                target = names[m]
                probe = img + '.probe'
                shutil.copyfile(img, probe)
                assert ods2(probe, 'DELETE [SH]%s' % target)[0] == 0
                shrinks = efblk(probe, fid) < efblk(img, fid)
                os.remove(probe)
                if shrinks:
                    assert len(names) - 1 - m >= 12, 'the block that empties is among the last'
                    NEXT[img] = target
                    return img
                for j in (m + d + 1, m - d - 1):
                    assert ods2(img, 'DELETE [SH]%s' % names[j])[0] == 0
            raise AssertionError('no delete of a middle block shrinks the directory')
        if fill_for == 'versplit':
            # versions of one 34-character name between two neighbours: the next version grows the directory by a block (the
            # record holding the versions no longer fits beside its neighbour: it is moved to a block of its own)
            nm = 'VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT'
            for first in ('AAFIRST.TXT', 'ZZLAST.TXT'):
                assert ods2(img, 'COPY %s [FULL]%s' % (small, first))[0] == 0
            fid = fid_of(img, '[000000]FULL.DIR')
            v = 0
            while True:
                v += 1
                e = efblk(img, fid)
                probe = img + '.probe'
                shutil.copyfile(img, probe)
                ods2(probe, 'COPY %s [FULL]%s;%d' % (small, nm, v))
                grows = efblk(probe, fid) > e
                os.remove(probe)
                if grows and v > 3:
                    NEXT[img] = '%s;%d' % (nm, v)
                    break
                assert ods2(img, 'COPY %s [FULL]%s;%d' % (small, nm, v))[0] == 0
                assert v < 200, 'the directory never grew'
            return img
        rnd = random.Random(7)
        order = list(range(2000))
        if fill_for in ('splitmid', 'splitbig'):
            rnd.shuffle(order)
        before_wanted = {'split': 2, 'splitmid': 4, 'move': 4, 'movebig': 17, 'splitbig': 25}[fill_for]
        fid = fid_of(img, '[000000]FULL.DIR')
        for i in order:
            name = 'N%05d.TXT' % i
            e = efblk(img, fid)
            probe = img + '.probe'
            shutil.copyfile(img, probe)
            ods2(probe, 'COPY %s [FULL]%s' % (small, name))
            grows = efblk(probe, fid) > e
            os.remove(probe)
            if grows and e >= before_wanted:
                NEXT[img] = name
                break
            assert ods2(img, 'COPY %s [FULL]%s' % (small, name))[0] == 0
        else:
            raise AssertionError('no growing insert found')
    return img


# name -> (command, paths of the files it may change or remove, needs level 5, HARDLINKS volume, fill)
OPS = {
    'delete_small': ('DELETE [000000]A1.TXT', ['A1.TXT;1'], False, False, None),
    'delete_mid': ('DELETE [000000]B.DAT', ['B.DAT;1'], False, False, None),
    'delete_fragmented': ('DELETE [000000]FRAG.DAT', ['FRAG.DAT;1'], False, False, None),
    'delete_all_versions': ('DELETE [SUB]D.TXT;*', ['SUB/D.TXT;1'], False, False, None),
    'delete_empty_dir': ('DELETE [EMPTY]', ['EMPTY.DIR;1', 'dir:EMPTY'], False, False, None),
    'delete_alias': ('DELETE [SUB]LNKA.TXT', ['SUB/LNKA.TXT;1'], False, False, None),
    'delete_hardlink': ('DELETE [SUB]LNKA.TXT', ['SUB/LNKA.TXT;1'], True, True, None),
    'delete_primary_hardlink': ('DELETE [000000]L.TXT', ['L.TXT;1'], True, True, None),
    'copy_small': ('COPY %(small)s [000000]NEW1.TXT', ['NEW1.TXT;1'], False, False, None),
    'copy_fragmented': ('COPY %(frag2)s [000000]NEW2.DAT', ['NEW2.DAT;1'], False, False, None),
    'copy_empty': ('COPY %(empty)s [000000]NEW3.TXT', ['NEW3.TXT;1'], False, False, None),
    'create_directory': ('CREATE/DIRECTORY [NEWD]', ['NEWD.DIR;1'], False, False, None),
    'link': ('LINK [SUB]LNKB.TXT [000000]B.DAT', ['SUB/LNKB.TXT;1'], False, False, None),
    'link_hardlinks': ('LINK [SUB]LNKB.TXT [000000]B.DAT', ['SUB/LNKB.TXT;1'], True, True, None),
    'symlink': ('SYMLINK [000000]SYMB.TXT B.DAT', ['SYMB.TXT;1'], True, False, None),
    'set_label': ('SET VOLUME/LABEL=RELABEL', [], False, False, None),
    'index_ext1': ('COPY %(empty)s [000000]ZNEW.TXT', ['ZNEW.TXT;1'], False, False, 'ixext1'),
    'index_tail': ('COPY %(empty)s [000000]ZNEW.TXT', ['ZNEW.TXT;1'], False, False, 'ixtail'),
    'index_ext2': ('COPY %(empty)s [000000]ZNEW.TXT', ['ZNEW.TXT;1'], False, False, 'ixext2'),
    'insert_split': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'split'),
    'insert_split_mid': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'splitmid'),
    'insert_split_big': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'splitbig'),
    'insert_move_big': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'movebig'),
    'insert_move': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'move'),
    # COPY makes a new version of a name that exists (PLAN_WRITE_OPS item 1)
    # RENAME within one directory (PLAN_WRITE_OPS item 5, step 1; CORPUS 65): the file must read back with its original bytes under at least ONE of its two names at every
    # interruption, and under the new name once the command completed (RENAME_PAIRS); never under none
    'rename_same_dir': ('RENAME [000000]A1.TXT [000000]A9.TXT', ['A1.TXT;1', 'A9.TXT;1'], False, False, None),
    'rename_type': ('RENAME [000000]B.DAT [000000]B.TXT', ['B.DAT;1', 'B.TXT;1'], False, False, None),
    'rename_newver': ('RENAME [000000]A1.TXT [000000]W.TXT', ['A1.TXT;1', 'W.TXT;10'], False, False, None),
    'rename_alias': ('RENAME [SUB]LNKA.TXT [SUB]LNKB.TXT', ['SUB/LNKA.TXT;1', 'SUB/LNKB.TXT;1'], False, False, None),
    'rename_primary_alias': ('RENAME [000000]L.TXT [000000]L2.TXT', ['L.TXT;1', 'L2.TXT;1'], False, False, None),
    'rename_hardlink': ('RENAME [SUB]LNKA.TXT [SUB]LNKB.TXT', ['SUB/LNKA.TXT;1', 'SUB/LNKB.TXT;1'], True, True, None),
    'rename_primary_hardlink': ('RENAME [000000]L.TXT [000000]L2.TXT', ['L.TXT;1', 'L2.TXT;1'], True, True, None),
    'rename_split': ('RENAME [FULL]N00000.TXT [FULL]%(next)s', ['FULL/N00000.TXT;1', 'FULL/%(next)s;1'], False, False, 'split'),
    # RENAME into ANOTHER directory (step 2; CORPUS 65.1 b, 66.2 k2, 68.3): header first (the back link, and the name if it changes, revision, date: one write), then the entry in the target directory, then the
    # old entry out; an alias entry moves with its header untouched. At every stop the file reads back under one of its names, and a DELETE of either name does what VMS's rule says (the file goes only through
    # the entry that is in the header's back link directory AND carries the header's name)
    'rename_move': ('RENAME [000000]A1.TXT [SUB]', ['A1.TXT;1', 'SUB/A1.TXT;1'], False, False, None),
    'rename_move_name': ('RENAME [000000]A1.TXT [SUB]A9.TXT', ['A1.TXT;1', 'SUB/A9.TXT;1'], False, False, None),
    'rename_move_exist': ('RENAME [000000]A1.TXT [SUB]D.TXT', ['A1.TXT;1', 'SUB/D.TXT;2'], False, False, None),
    'rename_move_alias': ('RENAME [SUB]LNKA.TXT [EMPTY]', ['SUB/LNKA.TXT;1', 'EMPTY/LNKA.TXT;1'], False, False, None),
    'rename_move_primary': ('RENAME [000000]L.TXT [EMPTY]', ['L.TXT;1', 'EMPTY/L.TXT;1'], False, False, None),
    'rename_move_hardlink': ('RENAME [SUB]LNKA.TXT [EMPTY]', ['SUB/LNKA.TXT;1', 'EMPTY/LNKA.TXT;1'], True, True, None),
    'rename_move_primary_hardlink': ('RENAME [000000]L.TXT [EMPTY]', ['L.TXT;1', 'EMPTY/L.TXT;1'], True, True, None),
    'rename_move_split': ('RENAME [000000]A1.TXT [FULL]%(next)s', ['A1.TXT;1', 'FULL/%(next)s;1'], False, False, 'split'),
    'rename_move_ext': ('RENAME [F]A.DAT [000000]A2.DAT', ['F/A.DAT;1', 'A2.DAT;1'], False, False, 'sfext'),
    # a move onto a name at its version limit (review 69 F1, N6): V.TXT;1 ;2 with the limit 2: the rename makes V.TXT;3 and the lowest, V.TXT;1, goes; the moved file must survive every stop
    'rename_move_limit': ('RENAME [SUB]D.TXT [000000]V.TXT', ['SUB/D.TXT;1', 'V.TXT;3', 'V.TXT;1'], False, False, None),
    # a case-only rename at ODS-5 (C3): the record's name bytes are replaced in place; the insert of a spelling that differs only in case is blind to case and would make a new version
    'rename_case': ('RENAME [000000]abc.txt [000000]ABC.txt', ['abc.txt;1', 'ABC.txt;1'], True, False, 'caseonly'),
    # the remove that empties a block (C6b): the old name is alone in the LAST, then a MIDDLE, block of its directory and the new name sorts first, so the insert is in the first block
    # and the remove empties the old name's block (the blocks after it move up, the last is dropped: review 48 N10, review 64 S1)
    # a file in several headers (a primary header and extension headers): the extension headers are not touched (CORPUS 66.2); the file reads back under one of its names at every stop
    'rename_ext': ('RENAME [F]A.DAT [F]A2.DAT', ['F/A.DAT;1', 'F/A2.DAT;1'], False, False, 'sfext'),
    'rename_shrink_last': ('RENAME [SH]%(next)s [SH]AAA_FIRST.TXT', ['SH/%(next)s;1', 'SH/AAA_FIRST.TXT;1'], False, False, 'dirshrink'),
    'rename_shrink_mid': ('RENAME [SH]%(next)s [SH]AAA_FIRST.TXT', ['SH/%(next)s;1', 'SH/AAA_FIRST.TXT;1'], False, False, 'dirshrinkmid'),
    # DELETE of the only name in the last block of a directory: the directory shrinks by a block; no block of zeros inside the end of file at any write (review 48 N10)
    'delete_dirshrink': ('DELETE [SH]%(next)s', ['SH/%(next)s;1'], False, False, 'dirshrink'),
    'delete_dirshrink_mid': ('DELETE [SH]%(next)s', ['SH/%(next)s;1'], False, False, 'dirshrinkmid'),
    'newver_front': ('COPY %(small)s [000000]A1.TXT', ['A1.TXT;2'], False, False, None),
    'newver_limit': ('COPY %(small)s [000000]V.TXT', ['V.TXT;1', 'V.TXT;3'], False, False, None),
    'newver_mid': ('COPY %(small)s [000000]W.TXT;5', ['W.TXT;5'], False, False, None),
    'newver_split': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s'], False, False, 'versplit'),
    # at the version limit, with versions of different contents spread over three content chunks each: the removed version must be
    # intact or gone at every interruption, never changed (review 40 F1)
    'newver_limit3_big': ('COPY %(newbig)s [BIG]A.BIN', ['BIG/A.BIN;1', 'BIG/A.BIN;4'], False, False, 'limit3big'),
    'newver_limit1_big': ('COPY %(newbig)s [BIG]A.BIN', ['BIG/A.BIN;1', 'BIG/A.BIN;2'], False, False, 'limit1big'),
    # PURGE (item 2): each version goes in a change of its own, so every state in between is a purge that has gone part of the way: the versions that
    # may be gone are the targets, none may be changed; SET FILE (item 4) rewrites one header block: whatever state results must pass the checker
    # (the header checksum included)
    'purge_names': ('PURGE/KEEP=1 [PG]*.TXT', ['PG/P.TXT;1', 'PG/P.TXT;2', 'PG/P.TXT;3', 'PG/Q.TXT;1'], False, False, None),
    'purge_keep2': ('PURGE/KEEP=2 [PG]P.TXT', ['PG/P.TXT;1', 'PG/P.TXT;2'], False, False, None),
    'setfile_prot': ('SET FILE/PROTECTION=(W:R) [000000]A1.TXT', [], False, False, None),
    'setfile_owner': ('SET FILE/OWNER_UIC=[200,3] [SUB]D.TXT', [], False, False, None),
    'setfile_dir': ('SET FILE/PROTECTION=(G,W) [000000]SUB.DIR;1', [], False, False, None),
    'setfile_reserved': ('SET FILE/PROTECTION=(W:R) [000000]BITMAP.SYS;1', [], False, False, None),
    'setfile_wild': ('SET FILE/PROTECTION=(G:R,W) [000000]*.TXT;*', [], False, False, None),
    'setfile_owner_ext': ('SET FILE/OWNER_UIC=[200,3] [F]A.DAT', [], False, False, 'sfext'),
}
# the operations that are also run with an I/O error at the k-th write of the commit (the process goes on and says so), not only killed
ERROR_MODE_OPS = ['newver_limit', 'newver_limit3_big', 'newver_limit1_big', 'newver_front', 'purge_names', 'setfile_prot', 'setfile_wild', 'setfile_owner_ext']
QUICK = ['rename_move_name', 'rename_move_primary', 'rename_move_alias', 'rename_move_split', 'rename_same_dir', 'rename_newver', 'rename_alias', 'rename_primary_alias', 'rename_split', 'rename_case', 'rename_shrink_last', 'rename_shrink_mid', 'delete_dirshrink', 'delete_dirshrink_mid', 'index_ext1', 'index_tail', 'index_ext2', 'delete_small', 'delete_fragmented', 'delete_alias', 'copy_small', 'create_directory', 'set_label',
         'insert_split', 'insert_split_mid', 'insert_move', 'newver_front', 'newver_limit', 'newver_mid', 'newver_split', 'newver_limit3_big', 'newver_limit1_big',
         'purge_names', 'purge_keep2', 'setfile_prot', 'setfile_owner', 'setfile_dir', 'setfile_reserved', 'setfile_wild', 'setfile_owner_ext']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--level', type=int, action='append')
    ap.add_argument('--op', action='append')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--every', type=int, default=1, help='test every n-th k only')
    args = ap.parse_args()
    levels = args.level or [2, 5]
    names = args.op or (QUICK if args.quick else list(OPS))
    global WORK, KEEP
    KEEP = args.keep
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    WORK = tempfile.mkdtemp(prefix='fault_inject_', dir=base)
    ctx = {'small': put_host('small.txt', b'one block of text\n'), 'empty': put_host('empty.txt', b''),
           'newbig': put_host('newbig.bin', blob(int(2.2 * 1024 * 1024), 99)),
           'frag2': put_host('frag2.dat', blob(900 * 512, 5))}
    followup = put_host('zz.dat', blob(1536, 77))
    failures = 0
    bases = {}
    for level in levels:
        for name in names:
            cmd, targets, need5, need_hl, fill = OPS[name]
            if need5 and level != 5:
                continue
            key = (level, need_hl, fill)
            if key not in bases:
                bases[key] = setup(level, need_hl, fill, {'movebig': 16, 'splitbig': 64}.get(fill))
            base = bases[key]
            ctx_op = dict(ctx, next=NEXT.get(base, ''))
            cmd = cmd % ctx_op
            targets = [t % ctx_op for t in targets]
            rc, base_files, _ = tree(base, 'base')
            assert rc == 0
            trial = os.path.join(WORK, 'trial.img')
            shutil.copyfile(base, trial)
            openv = OP_ENV.get(name, {})
            rc, out = ods2(trial, cmd, dict(openv, ODS2_FAULT_COUNT='1'))
            m = re.search(r'ODS2_FAULT: (\d+) writes', out)
            if rc != 0 or not m:
                print('FAIL: %s (ODS-%d): the operation itself failed on the base volume: %s' % (name, level, out.strip()))
                failures += 1
                continue
            n = int(m.group(1))
            if rc == 0:
                pass
            counts = {'leak': 0, 'clean': 0}
            bad_here = 0
            runs = [('crash', k) for k in range(1, n + 1, args.every)]
            if name in ERROR_MODE_OPS:
                runs += [('error', k) for k in range(1, n + 1, args.every)]
            for mode, k in runs:
                shutil.copyfile(base, trial)
                faultenv = {'ODS2_FAULT_AFTER_WRITES': str(k)} if mode == 'crash' else {'ODS2_FAULT_ERROR_AT_WRITE': str(k)}
                rc, out = ods2(trial, cmd, dict(openv, **faultenv))
                prob = []
                _mv_first = False
                # the run that completes (k = n) must be clean: no duplicate allowance (R9-3)
                allow = name if (k < n or mode == 'error') else None
                bad, leaks = check(trial, allow)
                _bad0 = bad
                if name in SHRINK_OPS:
                    prob += ['checker warns: ' + l.strip()[:140] for l in run(CHECK + [trial])[1].splitlines() if l.startswith('WARN') and 'end-of-records' in l]
                counts['leak' if leaks else 'clean'] += 1
                rc2, files, text = tree(trial, 'after')
                if name in MOVE_OPS and name not in MOVE_ALIAS_OPS and mode == 'crash':
                    # header first: the state after the header write (the old entry only) has the header naming the target directory, which has no entry yet: exactly ONE such error is allowed there, and
                    # it must be there (otherwise the order is not header first); no other state has any
                    _mo, _mn = (x % ctx_op for x in RENAME_PAIRS[name])
                    _first = _mo in files and _mn not in files
                    _mv_first = _first
                    _bl = [b for b in _bad0 + leaks if re.search(MOVE_FIRST_STATE_ERROR, b)]       # (an operation that allows leak classes files that error under `leaks`)
                    if _first:
                        if len(_bl) != 1:
                            prob.append('the state after the header write of a move should show exactly one "back link directory has no entry" error, it shows %d: not header first?' % len(_bl))
                        _bad0 = [b for b in _bad0 if b not in _bl]
                prob += ['checker: ' + b for b in _bad0]
                for p, h in base_files.items():
                    if p in files and files[p] != h:         # a target may be gone (a removed version), never changed (review 40 F1)
                        prob.append('file %s changed' % p)
                    if p not in files and p not in targets:
                        prob.append('file %s lost' % p)
                if name in RENAME_PAIRS:
                    old_key, new_key = (x % ctx_op for x in RENAME_PAIRS[name])
                    orig = base_files.get(old_key)
                    if orig is None:
                        prob.append('the base has no %s' % old_key)
                    elif files.get(old_key) != orig and files.get(new_key) != orig:
                        prob.append('the file reads back under neither %s nor %s' % (old_key, new_key))
                    if mode == 'crash' and k == n and (files.get(new_key) != orig or old_key in files):
                        prob.append('the completed rename: the file is not under the new name only')
                if name in RENAME_PAIRS and need_hl and mode == 'crash':
                    # review 67 F4: on a HARDLINKS volume, in EVERY stopped state (the old entry gone as well) the checker says it when the file's link count is not the number of entries that name it,
                    # and says nothing when they agree: the count is read from the header, the entries are counted from DIR/FULL of the whole volume, neither by the checker
                    import ext_headers as _E
                    _ok, _nk = (x % ctx_op for x in RENAME_PAIRS[name])
                    _key = _ok if _ok in files else _nk
                    _path, _, _ver = _key.rpartition(';')
                    _d, _, _nm = _path.rpartition('/')
                    _spec = '[%s]%s;%s' % (_d.replace('/', '.') if _d else '000000', _nm, _ver)
                    _ch = _E.chain(trial, _spec)
                    if _ch:
                        _fnum = _ch[0][0]
                        _lc = int.from_bytes(_ch[0][1][74:76], 'little')
                        _rc, _full = ods2(trial, 'DIR/FULL [000000...]*.*;*')
                        _entries = len(re.findall(r'File ID:\s+\(%d,' % _fnum, _full))
                        _txt = run(CHECK + [trial])[1]
                        _warn = bool(re.search(r'file %d \([^)]*\): link count %d but %d directory entr' % (_fnum, _lc, _entries), _txt))
                        _any = bool(re.search(r'file %d \([^)]*\): link count' % _fnum, _txt))
                        if _warn != (_lc != _entries) or _any != (_lc != _entries):
                            prob.append('the link count is %d, %d entries name the file: the checker %s' % (_lc, _entries, 'warns' if _any else 'says nothing'))
                if name in RENAME_PAIRS and mode == 'crash':
                    # warnings (review 67 recheck): header first leaves ONE state with a warning: one entry (the old name) and a header that names the new one: "header name ... differs from its directory
                    # entry". It is allowed in THAT state only (the old entry present, the new one not); every other stopped state, and the completed one, has no warning (a HARDLINKS link count warning is
                    # judged above against the real count)
                    _okey, _nkey = (x % ctx_op for x in RENAME_PAIRS[name])
                    _w = [l for l in run(CHECK + [trial])[1].splitlines() if l.startswith('WARN') and 'link count' not in l and 'allocated but owned by no file' not in l]        # (a directory moved to a larger area by the insert shows its old area as lost for a moment: every insert does)
                    _first_state = (_okey in files and _nkey not in files and not (_okey.lower() == _nkey.lower()))
                    _bad_w = [l for l in _w if not (_first_state and re.search(r"header name '.*' differs from its directory entry", l))]
                    if _bad_w:
                        prob.append('a warning the stopped state may not have: %s' % _bad_w[0].strip()[:150])
                    if _first_state and name not in HF_NO_WARN_OPS and name not in MOVE_OPS and not any("header name '" in l for l in _w):
                        prob.append('the state after the header write (one entry, the old name; the header names the new one) has no "header name ... differs from its directory entry" warning: the order is not header first')
                    if _first_state and name not in HF_NO_WARN_OPS:
                        # CORPUS 69 (V8.4 both levels, VAX V7.3): in this state VMS reads the entry as an ALIAS (the header names the new name, which is not there): DELETE removes the ENTRY only (the file stays
                        # allocated in no directory: the checker's lost file, nothing else), and RENAME to a third name leaves the header untouched. ods2v2 does the same.
                        import ext_headers as _eh
                        _path, _, _ver = _okey.rpartition(';')
                        _d, _, _nm = _path.rpartition('/')
                        _spec = '[%s]%s;%s' % (_d.replace('/', '.') if _d else '000000', _nm, _ver)
                        _third = '[%s]ZZ3.TXT' % (_d.replace('/', '.') if _d else '000000')
                        _third_key = ((_d + '/') if _d else '') + 'ZZ3.TXT;1'
                        _hdr0 = _eh.chain(trial, _spec)
                        _timg = os.path.join(WORK, 'cp69.img')
                        shutil.copyfile(trial, _timg)
                        _rc, _out = ods2(_timg, 'DELETE ' + _spec, openv)
                        _rc2, _files2, _ = tree(_timg, 'cp69')
                        _errs = [l for l in run(CHECK + [_timg])[1].splitlines() if l.startswith('ERROR')]
                        if _okey in _files2 or any(k2 not in _files2 for k2 in files if k2 != _okey):
                            prob.append('DELETE of the old name in the first state (CORPUS 69: VMS removes the entry only) changed more than that entry')
                        if '%ODS2-I-DELETED' in _out or 'no entry left in its directory' not in _out:
                            prob.append('DELETE of the old name in the first state printed a plain deleted, not that the file stays allocated with no entry left in its directory: %s' % _out.strip()[:120])
                        if not _errs or any('lost file' not in l and 'back link directory' not in l for l in _errs):          # (a file with no entry anywhere: lost; one with an alias elsewhere: its back link directory has no entry)
                            prob.append('DELETE of the old name in the first state: the checker should say a lost file (or a back link directory with no entry for it) and nothing else: %s' % (_errs[:2],))
                        shutil.copyfile(trial, _timg)
                        _rc, _out = ods2(_timg, 'RENAME %s %s' % (_spec, _third), openv)
                        _rc2, _files2, _ = tree(_timg, 'cp69')
                        _hdr1 = _eh.chain(_timg, '[%s]ZZ3.TXT;1' % (_d.replace('/', '.') if _d else '000000'))
                        if _rc != 0 or '%ODS2-E' in _out or _third_key not in _files2 or _okey in _files2:
                            prob.append('RENAME of the old name to a third name in the first state failed or did not move the entry: %s' % _out.strip()[:120])
                        elif not (_hdr0 and _hdr1 and _hdr0[0][1] == _hdr1[0][1]):
                            prob.append('RENAME of the old name to a third name in the first state wrote the header (VMS: an alias, the header is untouched)')
                if name in MOVE_OPS and mode == 'crash':
                    # DELETE of each name that exists in this state (CORPUS 68.3 and 69): VMS's rule says the FILE goes only through the entry that is in the header's BACK LINK directory AND carries the
                    # header's name (the version too, and only a link count of 1, on a HARDLINKS volume); through any other entry only that entry goes and the file stays. Restated here from the
                    # image's own header, independently of the code under test.
                    import ext_headers as _E2
                    import rename_check as _RC
                    _ko, _kn = (x % ctx_op for x in RENAME_PAIRS[name])
                    for _vk in (_ko, _kn):
                        if _vk not in files:
                            continue
                        _vp, _, _vv = _vk.rpartition(';')
                        _vd, _, _vn = _vp.rpartition('/')
                        _vspec = '[%s]%s;%s' % (_vd if _vd else '000000', _vn, _vv)
                        _vdirnum = 4 if not _vd else fid_of(trial, '[000000]%s.DIR' % _vd)
                        _ch = _E2.chain(trial, _vspec)
                        if not _ch:
                            prob.append('DELETE oracle: no header for %s' % _vspec)
                            continue
                        _h = _ch[0][1]
                        _f = _RC.fields(_h, level)
                        _hn, _, _hv = _f['name'].partition(';')
                        _primary = _RC.backlink_num(_h) == _vdirnum and _hn.upper() == _vn.upper()
                        if need_hl:
                            _primary = _primary and _hv == _vv
                        _expect_file = _primary and not (need_hl and _f['linkcount'] > 1)
                        _dimg = os.path.join(WORK, 'dele.img')
                        shutil.copyfile(trial, _dimg)
                        _rcd, _outd = ods2(_dimg, 'DELETE ' + _vspec, openv)
                        SEEN['DELETE oracle: ' + ('file' if _expect_file else 'entry only')] = SEEN.get('DELETE oracle: ' + ('file' if _expect_file else 'entry only'), 0) + 1
                        _fr = lambda im: int(re.search(r'Free blocks:\s+(\d+)', ods2(im, 'SHOW VOLUME')[1]).group(1))
                        _plain = (_fr(_dimg) > _fr(trial)) if need_hl else ('%ODS2-I-DELETED' in _outd)       # (a HARDLINKS link removal says "deleted" too: the blocks tell whether the file went)
                        if _rcd != 0 or '%ODS2-E' in _outd:
                            prob.append('DELETE %s in this state failed: %s' % (_vspec, _outd.strip()[:120]))
                        elif _plain != _expect_file:
                            prob.append('DELETE %s: VMS\'s rule (back link directory %s, header name %s) says the %s, ods2v2 said: %s' % (_vspec, _RC.backlink_num(_h), _f['name'], 'FILE goes' if _expect_file else 'ENTRY only goes', _outd.strip()[:100]))
                        if not _expect_file and not _rcd:
                            _errs = [l for l in run(CHECK + [_dimg])[1].splitlines() if l.startswith('ERROR')]
                            _odd = [l for l in _errs if not re.search(r'is in no directory \(lost file\)|' + MOVE_FIRST_STATE_ERROR, l)]
                            if _odd:
                                prob.append('after DELETE %s (entry only): the checker says more than a lost file: %s' % (_vspec, _odd[0].strip()[:120]))
                            if _vn + ';' + _vv in _RC.listing(_dimg, '[%s]' % (_vd if _vd else '000000')):
                                prob.append('DELETE %s left the entry in its directory' % _vspec)
                if name in RENAME_PAIRS and mode == 'crash':
                    # the way back (review 65 C6c): the same RENAME again. A target entry that already points at the source's file ID is an interrupted rename: the run FINISHES it (no new
                    # version). Once the old entry is gone (or the rename completed) the source no longer exists: the run is refused and the image is unchanged (for a rename that changes
                    # only the case the name is still found: "already named", the image unchanged unless the header still has the old spelling: then it is finished).
                    old_k, new_k = (x % ctx_op for x in RENAME_PAIRS[name])
                    case_pair = old_k.lower() == new_k.lower()
                    import ext_headers
                    rerun = os.path.join(WORK, 'rerun.img')
                    shutil.copyfile(trial, rerun)
                    h_before = md5(rerun)
                    rcr, outr = ods2(rerun, cmd, openv)
                    rr_rc, rr_files, _ = tree(rerun, 'rerun')
                    orig = base_files.get(old_k)
                    badr, leakr = check(rerun, name, False)
                    if 'ILLCMD' in outr:
                        prob.append('the RENAME run again: unrecognized command')
                    elif case_pair:
                        if rr_files.get(new_k) != orig or (k == n and md5(rerun) != h_before):
                            prob.append('the RENAME (case only) run again: the file is not under the new spelling, or the completed rename was changed')
                    elif old_k in files:
                        # the old entry is still there: the run finishes the rename
                        if rcr != 0 or '%ODS2-E' in outr:
                            prob.append('the RENAME run again after the stop at write %d failed: %s' % (k, outr.strip()[:120]))
                        if rr_files.get(new_k) != orig or old_k in rr_files:
                            prob.append('after the RENAME run again the file is not under the new name only')
                        extra_keys = sorted(x for x in rr_files if x not in base_files and x != new_k)
                        if extra_keys:
                            prob.append('after the RENAME run again there is an extra entry (a new version?): %s' % extra_keys[:3])
                        prob += ['after the RENAME run again, checker: ' + b for b in badr + leakr]
                        if need_hl:
                            # HARDLINKS: the link count is what it was before the rename (the entries are as many as before); the checker only warns about a wrong count
                            def spec_of(key):
                                path, _, ver = key.rpartition(';')
                                d, _, nm = path.rpartition('/')
                                return '[%s]%s;%s' % (d.replace('/', '.') if d else '000000', nm, ver)
                            hb0 = ext_headers.chain(base, spec_of(old_k))
                            hr = ext_headers.chain(rerun, spec_of(new_k))
                            lc0 = int.from_bytes(hb0[0][1][74:76], 'little') if hb0 else None
                            lcr = int.from_bytes(hr[0][1][74:76], 'little') if hr else None
                            # a stop right after the first write (the link count + 1, before any entry) cannot be told from the start: the run again counts once more, one too high for good
                            # (a leak, never too little; the checker warns about it); every other stop ends with the count as it was
                            # (third recheck: when the first write also named the file's header, the run again sees the header name the target with no target entry, knows the count was taken, and does not count
                            # again: the count ends right. An ALIAS entry leaves the header alone, so nothing shows the count was taken: that stop still leaves it one too high.)
                            leak1 = 1 if (k == 1 and 'primary' not in name) else 0
                            if lc0 is not None and lcr is not None and lcr != lc0 + leak1:
                                prob.append('after the RENAME run again the link count is %d, it was %d%s' % (lcr, lc0, ' (a stop after the first write of an alias rename may leave one too many)' if leak1 else ''))
                    else:
                        # the old name is gone: refused, nothing changed
                        if '%ODS2-E' not in outr:
                            prob.append('the RENAME run again after the old entry was removed did not refuse (the source is gone): %s' % outr.strip()[:100])
                        if md5(rerun) != h_before:
                            prob.append('the RENAME run again after the old entry was removed changed the image')
                        prob += ['after the RENAME run again, checker: ' + b for b in badr]
                if name in INDEX_OPS:
                    # the first change of any kind repairs an interrupted growth, not only a create (review 28 a): a
                    # relabel, then the checker must show nothing of the transient
                    trial3 = os.path.join(WORK, 'trial3.img')
                    shutil.copyfile(trial, trial3)
                    rcl, outl = ods2(trial3, 'SET VOLUME/LABEL=HEALED', openv)
                    badl, leakl = check(trial3, allow, False)
                    if rcl != 0:
                        prob.append('SET VOLUME/LABEL after the fault failed: ' + outl.strip()[:120])
                    prob += ['after SET VOLUME/LABEL, checker: ' + b for b in badl]
                if name == 'setfile_owner_ext':
                    # an owner change on a file with extension headers writes the extension headers first and the primary header last (CORPUS 56.2: VMS changes
                    # the owner in every header): whatever write was the last one done, a primary header that shows the new owner means every extension header
                    # shows it too, and every header's checksum is right
                    import ext_headers
                    hs = [ext_headers.describe(nn, hh) for nn, hh in ext_headers.chain(trial, '[F]A.DAT')]
                    if len(hs) < 4:
                        prob.append('the file has %d headers, the case needs a primary header and 3 extension headers' % len(hs))
                    if any(x['checksum'] != 'ok' for x in hs):
                        prob.append('a header checksum is wrong')
                    if hs and hs[0]['owner'] == '[200,3]' and any(x['owner'] != '[200,3]' for x in hs[1:]):
                        prob.append('the primary header shows the new owner but an extension header has the old one')
                    if hs and mode == 'crash' and k == n and any(x['owner'] != '[200,3]' for x in hs):
                        prob.append('the completed change left an old owner in a header')
                # one more COPY, then both checks again
                rc3, out3 = ods2(trial, 'COPY %s [000000]ZZ.DAT' % followup, openv)
                if rc3 != 0:
                    prob.append('the COPY after the fault failed: ' + out3.strip()[:120])
                if name in INDEX_OPS:
                    # a second and a third COPY: a header written over INDEXF.SYS's extension header shows at the second
                    for extra in ('ZZ2.DAT', 'ZZ3.DAT'):
                        rcx, outx = ods2(trial, 'COPY %s [000000]%s' % (followup, extra), openv)
                        if rcx != 0:
                            prob.append('the COPY of %s after the fault failed: %s' % (extra, outx.strip()[:120]))
                bad2, leaks2 = check(trial, allow, False)
                if _mv_first:             # the state is still the one the move was stopped in (a COPY does not change it): its one error stays
                    bad2 = [b for b in bad2 if not re.search(MOVE_FIRST_STATE_ERROR, b)]
                prob += ['after COPY, checker: ' + b for b in bad2]
                rc4, files2, text = tree(trial, 'after2')
                for p, h in base_files.items():
                    if p in files2 and files2[p] != h:
                        prob.append('after COPY: file %s changed' % p)
                    if p not in files2 and p not in targets:
                        prob.append('after COPY: file %s lost' % p)
                if rc3 == 0 and files2.get('ZZ.DAT;1') != md5(followup):
                    prob.append('the COPY after the fault does not read back')
                # a name listed twice (the residue of an interrupted split): one DELETE takes all of its entries
                if name in SPLIT_OPS:
                    trial2 = os.path.join(WORK, 'trial2.img')
                    shutil.copyfile(trial, trial2)
                    for nm, ver in dup_names(trial2):
                        rcd, outd = ods2(trial2, 'DELETE [FULL]%s;%s' % (nm, ver))
                        listed = ods2(trial2, 'DIR [FULL]%s' % nm)[1]
                        if rcd != 0 or nm.upper() in listed.upper().replace(' ', '') and ';' in listed:
                            prob.append('DELETE of the duplicated name %s;%s: exit %d, still listed or failed: %s'
                                        % (nm, ver, rcd, outd.strip()[:100]))
                    bad3, leaks3 = check(trial2, None)
                    left = [b for b in bad3 + leaks3 if 'two entries for' in b or 'not in use' in b]
                    if left:
                        prob.append('after DELETE of the duplicated names: ' + left[0][:120])
                    trial2_warn = run(CHECK + [trial2])[1]
                    if 'dangling' in trial2_warn:
                        prob.append('a dangling entry is left after DELETE of a duplicated name')
                if prob:
                    bad_here += 1
                    failures += 1
                    print('FAIL: %s (ODS-%d) after %d of %d writes (%s):' % (name, level, k, n, mode))
                    for p in prob[:6]:
                        print('      ' + p[:200])
            print('%s: %s (ODS-%d): %d writes, %d states tested (%d clean, %d leaked), %d bad' % (
                'FAIL' if bad_here else 'PASS', name, level, n, counts['clean'] + counts['leak'],
                counts['clean'], counts['leak'], bad_here))
    if SEEN:
        print('fault_inject: interrupted-growth messages seen: ' + '; '.join('%s x%d' % (k, v) for k, v in sorted(SEEN.items())))
    if failures:
        print('fault_inject: %d bad states' % failures)
        return 1
    print('fault_inject: every state after every interrupted write is one of done / not done / leaked')
    return 0


if __name__ == '__main__':
    try:
        rc = main()
    finally:
        if WORK and not KEEP:
            shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
