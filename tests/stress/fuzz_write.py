#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""fuzz_write.py - S2: damaged VMS-made images, then WRITE operations.

For each iteration: copy a VMS-made image, damage it (tests/fuzz_ods5.py's mutations: home block, headers, FI5
names, map pointers, directory records, zeroed/0xFF blocks, truncation; plus bitmap and index-bitmap damage), then
run ONE write command (COPY, CREATE/DIRECTORY, DELETE, LINK, SYMLINK, SET VOLUME/LABEL, IMPORT, DELETE of a
directory) with ASan+UBSan.  Pass/fail rules (what "never crashes, never writes wild, never fixes what it does
not understand" means here):

  CRASH   signal, ASan/UBSan report, timeout                         -> failure, image kept
  REFUSED (exit 1) but the volume's meaning changed (independent reader: every directory, every name, every
          file's size and md5, the free-cluster count) -> failure ("refused but changed something")
  DONE    (exit 0) but a file that existed before and was not the target of the command changed or vanished
          -> failure ("wrote into another file")
  Damage to the storage bitmap is treated separately (INFO): a bitmap that says in-use blocks are free makes any
  allocator overwrite live data; ods2v2 cannot know.  Those are counted, not failed.

usage: fuzz_write.py BASE_IMAGE ITERATIONS SEED WORKDIR [--bitmap]
Failing images and commands are kept as WORKDIR/fail-N.{img,txt}.  Exit status 1 if any run failed.
"""
import hashlib, os, random, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import fuzz_ods5      # noqa: E402
import ods5read       # noqa: E402
import stress         # noqa: E402

ODS2 = stress.ODS2
TIMEOUT = 60


def digest(path):
    """the meaning of the volume: {entry: (size, md5)} + free clusters; None if the reader cannot make sense of it"""
    try:
        v = ods5read.read_volume(path)
    except Exception:
        return None
    ents = {}
    for k, fid in v.files.items():
        fi = v.info.get(fid) if fid else None
        ents[k] = (fi.size, fi.md5, fi.special) if fi else None
    return {'ents': ents, 'fids': dict(v.files), 'dirs': set(v.dirs), 'free': v.free_clusters, 'problems': len(v.problems)}


def mutate_bitmap(path, rng):
    img = fuzz_ods5.Img(path)
    home = img.blk(1)
    ibl, ibs = fuzz_ods5.u32(home, 24), fuzz_ods5.u16(home, 32)
    lbn = rng.choice([ibl, ibl + rng.randrange(0, max(ibs, 1))])
    with open(path, 'r+b') as f:
        f.seek(lbn * 512)
        b = bytearray(f.read(512))
        for _ in range(rng.randrange(1, 20)):
            b[rng.randrange(0, 512)] ^= 1 << rng.randrange(8)
        f.seek(lbn * 512)
        f.write(b)
    return 'ibitmap'


def pick_commands(base_digest, rng, tree, srcfile, ods5):
    names = [k for k, v in base_digest['ents'].items() if v and k[0] != () or (v and k[1] not in ('INDEXF.SYS',))]
    cmds = []
    dirs = sorted(base_digest['dirs'])
    d = rng.choice(dirs) if dirs else ()
    sd = stress.spec_dir(d, rng, ods5)
    cmds.append(('COPY', 'COPY %s %sFZNEW.TXT' % (srcfile, sd), None))
    cmds.append(('MKDIR', 'CREATE/DIRECTORY %s' % stress.spec_dir(d + ('FZDIR',), rng, ods5), None))
    cmds.append(('LABEL', 'SET VOLUME/LABEL=FUZZ', None))
    cmds.append(('IMPORT', 'IMPORT %s %s' % (tree, sd), None))
    if ods5:
        cmds.append(('SYMLINK', 'SYMLINK %sFZSL.LNK target' % sd, None))
    if names:
        k = rng.choice(names)
        spec = '%s%s;%d' % (stress.spec_dir(k[0], rng, ods5), stress.spec_file_name(k[1], rng, ods5), k[2])
        cmds.append(('DELETE', 'DELETE ' + spec, k))
        cmds.append(('LINK', 'LINK %sFZLINK.X %s' % (sd, spec.rsplit(';', 1)[0]), None))
    if len(d) > 0:
        cmds.append(('RMDIR', 'DELETE %s%s.DIR' % (stress.spec_dir(d[:-1], rng, ods5), stress.spec_dir_name(d[-1], rng, ods5)), None))
    return cmds


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    use_bitmap = '--bitmap' in sys.argv
    base, iters, seed, work = args[0], int(args[1]), int(args[2]), args[3]
    os.makedirs(work, exist_ok=True)
    rng = random.Random(seed)
    ods5 = ods5read.C.Vol(base).level == 5
    home, headers, dirs = fuzz_ods5.interesting(base)
    bd = digest(base)
    bvol = ods5read.read_volume(base, want_md5=False)
    fid_dirs = {}
    for dp, fid in bvol.dirs.items():
        fid_dirs.setdefault(fid, []).append(dp)
    alias_dirs = {dp for ps in fid_dirs.values() if len(ps) > 1 for dp in ps}
    tree = os.path.join(work, 'tree')
    os.makedirs(os.path.join(tree, 'sub'), exist_ok=True)
    open(os.path.join(tree, 'hello.txt'), 'w').write('hello\n')
    open(os.path.join(tree, 'sub', 'b.bin'), 'wb').write(os.urandom(3000))
    srcfile = os.path.join(work, 'src.dat')
    open(srcfile, 'wb').write(b'fuzz write test\n' * 100)
    print('base %s ODS-%d: %d headers, %d dir blocks, %d entries' % (os.path.basename(base), 5 if ods5 else 2, len(headers), len(dirs), len(bd['ents'])))
    fails = info = refused = done = skipped = 0
    t0 = time.time()
    for n in range(iters):
        img = os.path.join(work, 'cur.img')
        if os.path.exists(img):
            os.remove(img)
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', base, img], check=True)
        os.chmod(img, 0o644)
        kinds = [fuzz_ods5.mutate(img, rng, headers, dirs) for _ in range(rng.choice([1, 1, 2, 3]))]
        bitmap_damage = False
        if use_bitmap and rng.random() < 0.3:
            kinds.append(mutate_bitmap(img, rng))
            bitmap_damage = True
        before = digest(img)
        pre = os.path.join(work, 'pre.img')
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', img, pre], check=True)
        label, cmd, target = rng.choice(pick_commands(bd, rng, tree, srcfile, ods5))
        r = subprocess.run([ODS2, img, cmd], capture_output=True, timeout=TIMEOUT, env=stress.ENV)
        err = r.stderr.decode('utf-8', 'replace')
        why = None
        if r.returncode not in (0, 1) or 'Sanitizer' in err or 'runtime error' in err:
            why = 'CRASH rc=%d' % r.returncode
        else:
            after = digest(img)
            if before is None or after is None:
                skipped += 1
            else:
                if r.returncode == 1:
                    refused += 1
                    if label == 'IMPORT':
                        # a partial import is allowed to leave what it already imported, but nothing that existed may change
                        changed = [k for k, v in before['ents'].items() if after['ents'].get(k) != v]
                        if changed or not before['dirs'] <= after['dirs']:
                            why = 'REFUSED IMPORT changed or removed existing entries: %r' % (changed[:3],)
                    elif (after['ents'] != before['ents'] or after['dirs'] != before['dirs']
                            or after['free'] != before['free']):
                        why = 'REFUSED but changed the volume'
                else:
                    done += 1
                    for k, v in before['ents'].items():
                        if k == target or v is None or k[0] in alias_dirs or (target and (k[1], k[2]) == (target[1], target[2]) and k[0] in alias_dirs):
                            continue
                        if (target and after['ents'].get(k) is None and before['fids'].get(k) == before['fids'].get(target)):
                            continue    # another name of the deleted file (an alias whose primary link went): dangling, as VMS leaves it
                        if after['ents'].get(k) != v and not (k[1].upper().endswith('.SYS') and k[0] == ()):
                            why = 'DONE but another file changed: %r %r -> %r' % (k, v, after['ents'].get(k))
                            break
        if why and bitmap_damage and why.startswith(('DONE but another', 'REFUSED')):
            info += 1
            why = None
        if why:
            fails += 1
            shutil.copyfile(img, os.path.join(work, 'fail-%d.img' % fails))
            shutil.copyfile(pre, os.path.join(work, 'fail-%d.pre.img' % fails))
            with open(os.path.join(work, 'fail-%d.txt' % fails), 'w') as f:
                f.write('%s\nmutations: %s\ncommand: %s\nrc %s\n%s\n' % (why, kinds, cmd, r.returncode, err[-3000:]))
            print('FAIL %d: %s  [%s] %s' % (fails, why, ','.join(kinds), cmd[:120]), flush=True)
    import resource
    print('peak RSS of one ods2 child: %d MB' % (resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss // 1024))
    print('%d runs in %.0fs: %d failed, %d refused, %d done, %d unreadable by the reader, %d bitmap-damage infos'
          % (iters, time.time() - t0, fails, refused, done, skipped, info))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
