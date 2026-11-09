#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""script_vs_single.py - the write buffer (ods5-del: a change is kept in memory and written only after it succeeded) is
state kept between commands of ONE process.  Differential test: the stress harness runs a random operation sequence with one
ods2 process per command (every step verified as in S1); the very same command lines are then replayed on a copy of the
starting image as ONE script in ONE process.  Same sequence, same results: every command must succeed or fail exactly as
before, and the final volumes must have the same names, sizes, MD5s and free space (the independent reader), and the
checker must be clean.   usage: script_vs_single.py LO-HI [OPS]     (from the project root, inside a memory scope)"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stress, ods5read   # noqa: E402

REC = []
orig_run = stress.run_ods2


def recording(img, *cmd, **kw):
    r = orig_run(img, *cmd, **kw)
    if len(cmd) == 1 and not cmd[0].startswith(('TYPE', 'SHOW')):
        REC.append((cmd[0], r.ok))
    return r


stress.run_ods2 = recording


def digest(img):
    v = ods5read.read_volume(img)
    return (sorted((k, (v.info[f].size, v.info[f].md5, v.info[f].special) if f else None) for k, f in v.files.items()),
            sorted(v.dirs), v.free_clusters)


def one(seed, nops):
    wd = os.path.join(stress.WORK, 'svs', 's%d' % seed)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    del REC[:]
    cfg = stress.pick_config(seed, None, 'mix')
    h = stress.Harness(seed, 'mix', cfg, nops, wd, check_every=1, big=False)
    h.setup()
    start = os.path.join(wd, 'start.img')
    subprocess.run(['cp', '--reflink=auto', h.img, start], check=True)
    del REC[:]
    names = [n for n, _ in h.OPS['mix'] if n not in ('type', 'set_default', 'import')]
    weights = [w for n, w in h.OPS['mix'] if n not in ('type', 'set_default', 'import')]
    for i in range(nops):
        h.opno = i + 1
        h.do_op(h.rng.choices(names, weights)[0])
    h.verify('single-process final')
    want = digest(h.img)
    script = os.path.join(wd, 'all.cmd')
    open(script, 'w', encoding='utf-8').write('\n'.join(c for c, ok in REC) + '\n')
    r = subprocess.run([stress.ODS2, start, '@' + script], capture_output=True, env=stress.ENV)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    if 'Sanitizer' in out or 'runtime error' in out:
        return 'CRASH: ' + out[-500:]
    got = digest(start)
    summ = stress.run_checker(start)[0]
    if summ != '0 error(s), 0 warning(s)':
        return 'script run: checker: ' + summ
    if got != want:
        a, b = set(map(repr, want[0])), set(map(repr, got[0]))
        return 'one process per command and one script differ: only-single %s; only-script %s; free %s vs %s' % (
            sorted(a - b)[:2], sorted(b - a)[:2], want[2], got[2])
    shutil.rmtree(wd, ignore_errors=True)
    return None


def main():
    lo, _, hi = sys.argv[1].partition('-')
    nops = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        try:
            why = one(seed, nops)
        except stress.Failure as e:
            why = 'harness check failed: %s' % str(e)[:300]
        print('seed %d: %s (%d commands)' % (seed, why or 'identical', len(REC)), flush=True)
        bad += 1 if why else 0
    print('%d differ' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
