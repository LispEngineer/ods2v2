#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""crash.py - S3: interruption of writes on randomly shaped volumes (complements tests/fault_inject.py, which does
every k of a fixed set of operations on one fixed base).

Here the base volume is made by the stress harness (random tree, names, sizes, links, fragmentation: a model of
every file's md5 is kept), or is a copy of a VMS-made image.  A random write operation is then run

  * with ODS2_FAULT_AFTER_WRITES=k (ods2_fault, built with -DODS2_FAULT_INJECTION; the process ends at once, as
    a power cut would, the k-th written block already flushed), for k = 1..n (n learned with ODS2_FAULT_COUNT=1),
    or a random sample of at most 40 k when n is larger; and
  * killed with a real SIGKILL at a random moment (a stdio buffer may be cut in the middle of a block).

After each interruption:
  1. mount: ods2 must still open the volume (SHOW VOLUME) and the independent reader must read it;
  2. ods5check.py: the only errors allowed are leaks / half-done states (the ALLOWED patterns of tests/fault_inject.py);
  3. DATA: every file that existed before and is not the operation's own target has its size and md5 unchanged;
  4. R7-1 (review 7): no cluster is mapped by both a header marked for delete and a live (listed) file;
  5. a COPY of a new file is then made; steps 2-4 again, and the new file must read back exactly.
The outcome is classified: done (the operation's effect is complete), notdone (volume as before), partial
(leak, dangling or half-done state, no data lost).  A pre-existing non-target file that changed or vanished is DATA LOSS.

usage: crash.py [--seeds LO-HI] [--ops N] [--kills N]   (from the project root; ods2_fault and ods2_init built)
"""
import argparse, hashlib, json, os, random, re, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import ods5read   # noqa: E402
import stress     # noqa: E402

FAULT = os.environ.get('FAULT_ODS2', os.path.join(ROOT, 'ods2_fault'))
CHECKER = stress.CHECKER

# errors that a half-done operation may leave (same list as tests/fault_inject.py): leaks and dangling states only
ALLOWED = [
    r'is in no directory \(lost file\)',
    r'is deleted but its index bitmap bit is set',
    r'bitmap bit set but no valid header',
    r'index bitmap bit \d+ set beyond the index file EOF',
    r'its back link directory \d+ has no entry for it',
    r'is not reachable from the root',
    r'which is marked for delete \(a half-done delete\)',
    r'cluster \d+ \(LBN \d+\) is in use by file \d+ but marked free',    # only when that file is an unlisted one (checked below)
]


def run_fault(img, cmd, k=None, count=False, timeout=300):
    env = dict(stress.ENV)
    if k is not None:
        env['ODS2_FAULT_AFTER_WRITES'] = str(k)
    if count:
        env['ODS2_FAULT_COUNT'] = '1'
    p = subprocess.run([FAULT, img, cmd], capture_output=True, env=env, timeout=timeout)
    return p.returncode, p.stdout.decode('utf-8', 'replace') + p.stderr.decode('utf-8', 'replace')


TRANSIENT = [r'two entries for', r'out of order']     # ods5-wr2 (F2 fix): a split/move may leave a name listed twice or briefly out of order
TRANSIENT_OPS = ('COPY', 'MKDIR', 'LINK', 'SYMLINK', 'IMPORT', 'SCRIPT')


def checker_errors(img, label=''):
    r = subprocess.run([sys.executable, '-I', CHECKER, img], capture_output=True, text=True, env=stress.ENV)
    errs = [l for l in r.stdout.split('\n') if l.startswith('ERROR')]
    warns = [l for l in r.stdout.split('\n') if l.startswith('WARN')]
    lost = set(re.findall(r'file (\d+) \(.*\) is in no directory \(lost file\)', r.stdout))
    bad = []
    for l in errs:
        m = re.search(r'cluster \d+ \(LBN \d+\) is in use by file (\d+) but marked free', l)
        if m:
            if m.group(1) not in lost:
                bad.append(l)       # a LISTED file whose clusters are free: the forbidden state
        elif not any(re.search(a, l) for a in ALLOWED) and not (label in TRANSIENT_OPS and any(re.search(a, l) for a in TRANSIENT)):
            bad.append(l)
    return bad, errs, warns, r.stdout[-300:]


def r71_violations(vol):
    """review 7 R7-1: a cluster mapped by a header marked for delete AND by a live (listed) file"""
    cl = vol.cluster
    live = {fid for fid in vol.files.values() if fid}
    live |= {fid for fid in vol.dirs.values()}
    owner = {}
    for fid in live:
        fi = vol.info.get(fid)
        if not fi:
            continue
        for l, c in fi.extents:
            for k in range(l // cl, (l + c - 1) // cl + 1):
                owner[k] = fid
    bad = []
    for fid, fi in vol.info.items():
        if fi.markdel and fid not in live:
            for l, c in fi.extents:
                for k in range(l // cl, (l + c - 1) // cl + 1):
                    if k in owner:
                        bad.append('cluster %d mapped by marked-for-delete file %d and live file %d' % (k, fid, owner[k]))
    return bad


def snapshot(img):
    v = ods5read.read_volume(img)
    return v, {k: (v.info[fid].size, v.info[fid].md5, v.info[fid].special) if fid else None for k, fid in v.files.items()}


def build_base(seed, work):
    """a populated volume + model. kind by seed: 0 random mix, 1 fragmented (extension headers), 2 big directory,
    3 nearly full, 4 INDEXF near its limit"""
    kind = seed % 5
    cfg = 'f5' if seed % 2 else 'f2'
    if kind == 0:
        h = stress.Harness(seed, 'mix', stress.pick_config(seed, None, 'mix') if seed % 3 else cfg, 25, work, check_every=1000, big=False)
        h.setup()
        names = [n for n, _ in h.OPS['mix']]
        for i in range(25):
            h.do_op(h.rng.choices(names, [w for _, w in h.OPS['mix']])[0])
        h.verify('base')
        h.focus = None
        return h
    prof = {1: 'frag', 2: 'bigdir', 3: 'fill', 4: 'indexf'}[kind]
    h = stress.Harness(seed, prof, cfg, 1, work, check_every=1000, big=False)
    h.stop = {1: 'frag', 2: 'bigdir', 3: 'full', 4: None}[kind]
    if kind == 2:
        h.opts = {'size': 65536}
    h.run()
    h.focus = {2: ('BIG',), 1: ('FRAG',), 4: ('IDX',)}.get(kind)
    if kind == 3:
        # make a little room: delete a few files, so COPYs fit or just fail to fit
        ents = [(m_dp, d, v) for (fp, key, v), (d, o) in h.model.ents.items() for m_dp in [h.model.dirs[fp]] if not o.volatile and o.kind == 'file']
        h.batch_delete(ents[:3])
    h.verify('base (%s)' % prof)
    return h


def random_op(h, rng):
    """-> (label, command, target keys (display path tuples) that the op may change or remove)"""
    m = h.model
    r = rng
    kind = r.choice(['copy', 'copy', 'delete', 'delete', 'mkdir', 'link', 'symlink', 'label', 'copybig', 'rmdir', 'import'])
    dp = h.pick_dir()
    if getattr(h, 'focus', None) and h.model.has_dir(h.focus) and r.random() < 0.7:
        dp = h.model.find_dir(h.focus)
    if kind in ('copy', 'copybig'):
        disp, strict = h.names.file_name('plain' if r.random() < 0.7 else None)
        if not h.ods5:
            disp = disp.upper()
        if m.name_taken(dp, disp):
            return None
        size = r.choice([0, 1, 511, 512, 513, 4000, 20000]) if kind == 'copy' else r.randint(40000, 400000)
        data = stress.gen_content(r, size, r.choice(['bin', 'tagged', 'lf']))
        src = h.write_src(data)
        return ('COPY', 'COPY %s %s%s' % (src, stress.spec_dir(dp, r, h.ods5), stress.spec_file_name(disp, r, h.ods5)),
                {(dp, disp, 1)})
    if kind == 'delete':
        e = h.pick_entry()
        if e is None:
            return None
        (fp, key, ver), disp, edp, obj = e
        tg = {(edp, disp, ver)}
        # other entries of the same file (aliases) may change too
        for (f2, k2, v2), (d2, o2) in m.ents.items():
            if o2 is obj:
                tg.add((m.dirs[f2], d2, v2))
        return ('DELETE', 'DELETE %s%s;%d' % (stress.spec_dir(edp, r, h.ods5), stress.spec_file_name(disp, r, h.ods5), ver), tg)
    if kind == 'mkdir':
        nm = h.names.dir_name()
        if m.dir_taken(dp, nm) or (not h.ods5 and len(dp) >= 7):
            return None
        return ('MKDIR', 'CREATE/DIRECTORY ' + stress.spec_dir(tuple(dp) + (nm,), r, h.ods5), set())
    if kind == 'rmdir':
        cands = [p for p in m.dirs.values() if p and not m.is_frozen(p)]
        if not cands:
            return None
        p = r.choice(cands)
        return ('RMDIR', 'DELETE %s%s.DIR' % (stress.spec_dir(p[:-1], r, h.ods5), stress.spec_dir_name(p[-1], r, h.ods5)), {(p[:-1], p[-1] + '.DIR', 1)})
    if kind == 'link':
        e = h.pick_entry(False)
        if e is None or e[3].kind != 'file' or e[0][2] != 1:
            return None
        (fp, key, ver), disp, edp, obj = e
        nd, _ = h.names.file_name('plain')
        nd = nd.upper() if not h.ods5 else nd
        if m.name_taken(dp, nd):
            return None
        return ('LINK', 'LINK %s%s %s%s;1' % (stress.spec_dir(dp, r, h.ods5), stress.spec_file_name(nd, r, h.ods5),
                                                 stress.spec_dir(edp, r, h.ods5), stress.spec_file_name(disp, r, h.ods5)), set())
    if kind == 'symlink' and h.ods5:
        nd, _ = h.names.file_name('plain')
        if m.name_taken(dp, nd):
            return None
        return ('SYMLINK', 'SYMLINK %s%s some/target' % (stress.spec_dir(dp, r, h.ods5), stress.spec_file_name(nd, r, h.ods5)), set())
    if kind == 'label':
        return ('LABEL', 'SET VOLUME/LABEL=CRASH%d' % r.randint(0, 99), set())
    return None


dups = []   # states that left a name listed twice (allowed after a split, but counted)


def check_state(tag, img, before, targets, fails, label=''):
    """steps 1-4 after an interruption. before: snapshot dict from before the op. returns state name"""
    sv = subprocess.run([stress.ODS2, img, 'SHOW VOLUME'], capture_output=True, env=stress.ENV)
    if sv.returncode != 0:
        fails.append('%s: the volume no longer mounts: %s' % (tag, sv.stderr.decode()[-200:]))
        return 'unmountable'
    bad, errs, warns, tail = checker_errors(img, label)
    if bad:
        fails.append('%s: checker (non-leak) errors: %s' % (tag, bad[:3]))
    try:
        v, snap = snapshot(img)
    except Exception as e:
        fails.append('%s: reader failed: %r' % (tag, e))
        return 'unreadable'
    probs = [p for p in v.problems if not (label in TRANSIENT_OPS and ('listed twice' in p or 'duplicate directory entry' in p))]
    if probs:
        fails.append('%s: reader problems: %s' % (tag, probs[:3]))
    if len(probs) != len(v.problems):
        dups.append(tag)
    for k, val in before.items():
        if k in targets or val is None:
            continue
        if snap.get(k) != val and not (k[0] == () and k[1].upper().endswith('.SYS')):
            fails.append('%s: DATA LOSS: %r was %r now %r' % (tag, k, val, snap.get(k)))
            break
    for msg in r71_violations(v):
        fails.append('%s: R7-1: %s' % (tag, msg))
    new = set(snap) - set(before)
    gone = {k for k in before if k not in snap}
    if not errs and not warns:
        return 'done' if (new or gone or any(snap.get(k) != before[k] for k in before)) else 'notdone'
    return 'partial'


def trial(h, rng, label, cmd, targets, k, kill_at, out, fails, tag):
    img = h.img
    trial_img = img + '.trial'
    subprocess.run(['cp', '--reflink=auto', img, trial_img], check=True)
    before_v, before = snapshot(trial_img)
    if kill_at is None:
        rc, text = run_fault(trial_img, cmd, k=k)
        fired = rc == 99
    else:
        p = subprocess.Popen([FAULT, trial_img, cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=stress.ENV)
        time.sleep(kill_at)
        fired = p.poll() is None
        if fired:
            p.send_signal(signal.SIGKILL)
        p.wait()
    t = '%s [%s k=%s kill=%s]' % (tag, label, k, kill_at)
    state = check_state(t, trial_img, before, targets, fails, label)
    out[state] = out.get(state, 0) + 1
    # a COPY afterwards: must work, must not disturb anything, new file must read back
    if state not in ('unmountable', 'unreadable'):
        data = stress.gen_content(rng, 3000, 'tagged')
        src = h.write_src(data, 'after.dat')
        rc, text = run_fault(trial_img, 'COPY %s [000000]AFTERCRASH.DAT' % src)
        if rc not in (0, 1):
            fails.append('%s: COPY after the interruption crashed: rc=%s %s' % (t, rc, text[-300:]))
        else:
            ok = rc == 0
            if ok:
                v2 = ods5read.read_volume(trial_img)
                fid = v2.files.get(((), 'AFTERCRASH.DAT', 1))
                if not fid or v2.info[fid].md5 != hashlib.md5(data).hexdigest():
                    fails.append('%s: the COPY after the interruption does not read back' % t)
            bad, errs, warns, tail = checker_errors(trial_img, label)
            if bad:
                fails.append('%s: after the follow-up COPY, checker (non-leak) errors: %s' % (t, bad[:3]))
            try:
                v2, snap2 = snapshot(trial_img)
                for k2, val in before.items():
                    if k2 in targets or val is None:
                        continue
                    if snap2.get(k2) != val and not (k2[0] == () and k2[1].upper().endswith('.SYS')):
                        fails.append('%s: DATA LOSS after the follow-up COPY: %r' % (t, k2))
                        break
                for msg in r71_violations(v2):
                    fails.append('%s: R7-1 after the follow-up COPY: %s' % (t, msg))
            except Exception as e:
                fails.append('%s: reader failed after the follow-up COPY: %r' % (t, e))
    if fails and not os.path.exists(os.path.join(h.dir, 'FAILED.img')):
        shutil.copyfile(trial_img, os.path.join(h.dir, 'FAILED.img'))
        shutil.copyfile(img, os.path.join(h.dir, 'FAILED.base.img'))
        open(os.path.join(h.dir, 'FAILED.txt'), 'w').write('%s\n%s\n' % (cmd, '\n'.join(fails)))
    os.unlink(trial_img)


def run_seed(seed, nops, nkills, work_root):
    work = os.path.join(work_root, 'c%d' % seed)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    h = build_base(seed, work)
    rng = random.Random(seed ^ 0x5eed)
    totals, fails = {}, []
    done_ops = 0
    attempts = 0
    while done_ops < nops and attempts < nops * 6:
        attempts += 1
        op = random_op(h, rng)
        if op is not None and rng.random() < 0.3:
            # a script of 2-4 commands in ONE process: the buffer, the commit and the fault counter span several commands
            ops = [op] + [o for o in (random_op(h, rng) for _ in range(rng.randint(1, 3))) if o]
            if len(ops) > 1:
                sp = os.path.join(h.dir, 'trial%d.cmd' % attempts)
                open(sp, 'w', encoding='utf-8').write('\n'.join(o[1] for o in ops) + '\n')
                op = ('SCRIPT', '@' + sp, set().union(*[o[2] for o in ops]))
        if op is None:
            continue
        label, cmd, targets = op
        # learn n
        probe = h.img + '.probe'
        subprocess.run(['cp', '--reflink=auto', h.img, probe], check=True)
        rc, text = run_fault(probe, cmd, count=True)
        os.unlink(probe)
        m = re.search(r'ODS2_FAULT: (\d+) writes', text)
        n = int(m.group(1)) if m else 0
        if rc != 0 or n == 0:
            continue                       # refused by itself or no writes: not interesting here
        done_ops += 1
        ks = list(range(1, n + 1)) if n <= 60 else sorted(set([1, 2, n - 1, n] + rng.sample(range(1, n + 1), 40)))
        for k in ks:
            trial(h, rng, label, cmd, targets, k, None, totals, fails, 'seed %d op %d' % (seed, done_ops))
        # real SIGKILLs: the op takes a few tens of ms; kill at random fractions
        for j in range(nkills):
            trial(h, rng, label, cmd, targets, None, rng.uniform(0.0, 0.25), totals, fails, 'seed %d op %d' % (seed, done_ops))
        # then really apply it (clean run) so the next op starts from a changed volume; keep the model in step by
        # resynchronising from the volume (the operation is the harness's own, its effect already verified in S1)
        rc, text = subprocess.run([stress.ODS2, h.img, cmd], capture_output=True, env=stress.ENV).returncode, ''
        if rc == 0:
            resync(h)
    return seed, totals, fails


def resync(h):
    """rebuild the model from the (just verified) volume"""
    v = ods5read.read_volume(h.img)
    h.model = stress.model_from_volume(v, h.ods5, h.hardlinks, skip_dangling=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-4')
    ap.add_argument('--ops', type=int, default=8)
    ap.add_argument('--kills', type=int, default=3)
    a = ap.parse_args()
    lo, _, hi = a.seeds.partition('-')
    allfails = 0
    grand = {}
    for seed in range(int(lo), int(hi or lo) + 1):
        try:
            s, totals, fails = run_seed(seed, a.ops, a.kills, stress.WORK)
        except stress.Failure as e:
            print('seed %d: base volume failed: %s' % (seed, e))
            allfails += 1
            continue
        for k, v in totals.items():
            grand[k] = grand.get(k, 0) + v
        print('seed %d: %s, %d failures' % (seed, json.dumps(totals), len(fails)), flush=True)
        for f in fails[:6]:
            print('    ' + f[:400])
        allfails += len(fails)
    print('TOTAL outcomes %s; %d failure(s)' % (json.dumps(grand), allfails))
    return 1 if allfails else 0


if __name__ == '__main__':
    sys.exit(main())
