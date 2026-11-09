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
import argparse, hashlib, os, random, re, shutil, subprocess, sys

ODS2 = os.environ.get('FAULT_ODS2', './ods2_fault')
INIT = './ods2_init'
CHECK = ['python3', '-I', 'tests/ods5check.py']
NEXT = {}   # base image -> the name whose insert grows the directory
WORK = os.path.join(os.environ.get('TMPDIR', 'samples'), 'fault_inject_work')

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
SPLIT_TRANSIENT = [r'two entries for', r'block \d+: .* out of order']
EXTRA_ALLOWED = {'insert_split': SPLIT_TRANSIENT, 'insert_split_mid': SPLIT_TRANSIENT, 'insert_move': SPLIT_TRANSIENT,
                 'insert_split_big': SPLIT_TRANSIENT, 'insert_move_big': SPLIT_TRANSIENT}


def check(img, op=None):
    rc, text = run(CHECK + [img])
    errs = [l for l in text.splitlines() if l.startswith('ERROR')]
    bad, leaks = [], []
    for l in errs:
        if any(re.search(a, l) for a in ALLOWED + EXTRA_ALLOWED.get(op, [])):
            leaks.append(l)
        else:
            bad.append(l)
    return bad, leaks


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


def setup(level, hardlinks, fill_for=None, cluster=None):
    """the base volume: files of several sizes (one fragmented over holes), subdirectories, links"""
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
    # one file with another directory entry
    assert ods2(img, 'LINK [SUB]LNKA.TXT [000000]L.TXT')[0] == 0
    if level == 5:
        assert ods2(img, 'SYMLINK [000000]SYMA.TXT L.TXT')[0] == 0
    if fill_for:
        # [FULL] is filled to just before an insert that grows it by a block: 'split' (one block to two: the
        # new name sorts last), 'splitmid' (names in random order, three or more blocks: the block that
        # splits has blocks after it, which move up one), 'move' (three blocks in use, as many as are
        # allocated: the directory moves to a new, larger area). The next name to add is returned in NEXT.
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
    'insert_split': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'split'),
    'insert_split_mid': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'splitmid'),
    'insert_split_big': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'splitbig'),
    'insert_move_big': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'movebig'),
    'insert_move': ('COPY %(small)s [FULL]%(next)s', ['FULL/%(next)s;1'], False, False, 'move'),
}
QUICK = ['delete_small', 'delete_fragmented', 'delete_alias', 'copy_small', 'create_directory', 'set_label',
         'insert_split', 'insert_split_mid', 'insert_move']


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
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ctx = {'small': put_host('small.txt', b'one block of text\n'), 'empty': put_host('empty.txt', b''),
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
            rc, out = ods2(trial, cmd, {'ODS2_FAULT_COUNT': '1'})
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
            for k in range(1, n + 1, args.every):
                shutil.copyfile(base, trial)
                rc, out = ods2(trial, cmd, {'ODS2_FAULT_AFTER_WRITES': str(k)})
                prob = []
                bad, leaks = check(trial, name)
                prob += ['checker: ' + b for b in bad]
                counts['leak' if leaks else 'clean'] += 1
                rc2, files, text = tree(trial, 'after')
                for p, h in base_files.items():
                    if p in files and files[p] != h and p not in targets:
                        prob.append('file %s changed' % p)
                    if p not in files and p not in targets:
                        prob.append('file %s lost' % p)
                # one more COPY, then both checks again
                rc3, out3 = ods2(trial, 'COPY %s [000000]ZZ.DAT' % followup)
                if rc3 != 0:
                    prob.append('the COPY after the fault failed: ' + out3.strip()[:120])
                bad2, leaks2 = check(trial, name)
                prob += ['after COPY, checker: ' + b for b in bad2]
                rc4, files2, text = tree(trial, 'after2')
                for p, h in base_files.items():
                    if p in files2 and files2[p] != h and p not in targets:
                        prob.append('after COPY: file %s changed' % p)
                    if p not in files2 and p not in targets:
                        prob.append('after COPY: file %s lost' % p)
                if rc3 == 0 and files2.get('ZZ.DAT;1') != md5(followup):
                    prob.append('the COPY after the fault does not read back')
                if prob:
                    bad_here += 1
                    failures += 1
                    print('FAIL: %s (ODS-%d) after %d of %d writes:' % (name, level, k, n))
                    for p in prob[:6]:
                        print('      ' + p[:200])
            print('%s: %s (ODS-%d): %d writes, %d states tested (%d clean, %d leaked), %d bad' % (
                'FAIL' if bad_here else 'PASS', name, level, n, counts['clean'] + counts['leak'],
                counts['clean'], counts['leak'], bad_here))
    if not args.keep:
        shutil.rmtree(WORK, ignore_errors=True)
    if failures:
        print('fault_inject: %d bad states' % failures)
        return 1
    print('fault_inject: every state after every interrupted write is one of done / not done / leaked')
    return 0


if __name__ == '__main__':
    sys.exit(main())
