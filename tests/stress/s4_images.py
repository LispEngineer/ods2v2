#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""s4_images.py - S4: after-images for VMS to confirm (MOUNT, ANALYZE/DISK_STRUCTURE/READ_CHECK, CHECKSUM, TYPE).

Builds each image with the stress harness (so every operation was checked: ods5check 0/0, independent reader = model,
EXPORT = model), then writes the image (mode 444), its SHA-256 and two manifests made from the MODEL, never from
ods2v2's output: MANIFEST_LINUX (md5 size kind path) and MANIFEST_VMS (md5 size DCL file specification with ^-escapes).

usage: s4_images.py OUTDIR [--only NAME ...] [--target ld|ra|...]      (from the project root, ods2 and ods2_init built, run inside a memory scope)
"""
import argparse, hashlib, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stress   # noqa: E402

# name: (profile, config, seed, ops, stop, opts, description)
SPECS = {
    's4-f2-fresh':   ('mix', 'f2', 4101, 150, None, {'size': 131072, 'cluster': 4}, 'fresh ODS-2 (ods2_init), 150 random operations: directories, files of 0..6 MB, deletes, aliases, relabel'),
    's4-f5-hl':      ('mix', 'f5', 4102, 150, None, {'size': 131072, 'hl': True, 'cluster': 4}, 'fresh ODS-5 with HARDLINKS: names with spaces, dots, ISO Latin-1, UCS-2, 236-char names, hard links, symbolic links, IMPORT trees'),
    's4-f5-nohl':    ('mix', 'f5', 4103, 150, None, {'size': 100002, 'hl': False, 'cluster': 3}, 'fresh ODS-5 without HARDLINKS (aliases), cluster 3 (100002 blocks: a whole number of clusters, V8.4 ANALYZE objects to a partial last cluster)'),
    's4-vms5-heavy': ('mix', 'vms:asim12-ods5t-20261108', 4104, 250, None, {}, 'the VMS-made ODS-5 corpus volume (asim12-ods5t, V8.4, HARDLINKS) with 250 further ods2v2 operations'),
    's4-vms2-heavy': ('mix', 'vms:asim6-ods2t6-20261108', 4105, 250, None, {}, 'the V7.3-2-made ODS-2 corpus volume (asim6-ods2t6) with 250 further ods2v2 operations'),
    's4-full':       ('fill', 'f5', 4106, 1, 'full', {'size': 16384, 'hl': True, 'cluster': 2}, 'ODS-5 volume filled to the brim (the last copy refused, 1-3 byte files until nothing fits)'),
    's4-full2':      ('fill', 'f2', 4107, 1, 'full', {'size': 20001, 'cluster': 3}, 'ODS-2 volume filled to the brim (20001 blocks = 6667 clusters of 3)'),
    's4-exthdr':     ('frag', 'f5', 4108, 1, 'frag', {'size': 16384, 'hl': True, 'cluster': 1}, 'ODS-5, cluster 1: 1000 small files, half deleted, then 3 big files fragmented over the holes: extension headers'),
    's4-exthdr2':    ('frag', 'f2', 4109, 1, 'frag', {'size': 16384, 'cluster': 1}, 'ODS-2, the same: files with 500+ extents and extension headers'),
    's4-bigdir':     ('bigdir', 'f5', 4110, 1, 'bigdir', {'size': 131072, 'hl': True, 'cluster': 4}, 'ODS-5: one directory with 3000-5000 entries (long and short names), many block splits'),
    's4-bigdir2':    ('bigdir', 'f2', 4111, 1, 'bigdir', {'size': 131072, 'cluster': 4}, 'ODS-2: one directory with thousands of entries'),
    's4-bigindex':   ('bigdir', 'f5', 4113, 1, 'bigdir', {'size': 262144, 'hl': True, 'cluster': 4, 'n': 5200}, 'ODS-5: one directory with 5200 files: INDEXF.SYS grows past 4,096 blocks (ods5 22f0811 grows it in bigger steps there)'),
    's4-bigindex2':  ('bigdir', 'f2', 4114, 1, 'bigdir', {'size': 262144, 'cluster': 4, 'n': 5200}, 'ODS-2: one directory with 5200 files, INDEXF.SYS past 4,096 blocks'),
    's4-deep':       ('deep', 'f5', 4112, 1, 'deep', {'size': 32768}, 'ODS-5: nested directories 30-255 levels deep with files at several depths'),
}


def build(name, outdir, target=None):
    profile, config, seed, ops, stop, opts, desc = SPECS[name]
    wd = os.path.join(stress.WORK, 's4', name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    h = stress.Harness(seed, profile, config, ops, wd, check_every=1, big=True)
    h.stop = stop
    h.opts = dict(opts)
    if target:
        h.opts['target'] = target
    h.run()                     # raises Failure if any check fails
    base = os.path.join(outdir, name)
    shutil.copyfile(h.img, base + '.img')
    if target and config.startswith('vms:'):
        # a VMS-made image keeps its own geometry unless asked: fit its storage control block to the target device
        r = subprocess.run([stress.ODS2, base + '.img', 'SET VOLUME/GEOMETRY=%s' % target], capture_output=True, text=True)
        if r.returncode != 0:
            raise stress.Failure('SET VOLUME/GEOMETRY=%s on %s: %s' % (target, name, (r.stdout + r.stderr)[-300:]))
        summ = stress.run_checker(base + '.img')[0]
        if summ != '0 error(s), 0 warning(s)':
            raise stress.Failure('%s after SET VOLUME/GEOMETRY: checker %s' % (name, summ))
    h.write_manifests(base)
    os.chmod(base + '.img', 0o444)
    for ext in ('.MANIFEST_LINUX.TXT', '.MANIFEST_VMS.TXT'):
        os.chmod(base + ext, 0o444)
    shutil.rmtree(wd, ignore_errors=True)
    return desc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--target', help='device the images are for: ld (physical VAX96, LD driver), ra (SIMH VAX), ... passed to ods2_init --target= (default: ods2_init as it is)')
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    for name in (a.only or SPECS):
        for f in os.listdir(a.outdir):
            if f.startswith(name + '.'):
                p = os.path.join(a.outdir, f)
                os.chmod(p, 0o644)
                os.unlink(p)
        try:
            desc = build(name, a.outdir, a.target)
            print('%-14s ok   %s' % (name, desc), flush=True)
        except stress.Failure as e:
            print('%-14s FAIL %s' % (name, str(e)[:500]), flush=True)
    lines = []
    for f in sorted(os.listdir(a.outdir)):
        if f.endswith('.img'):
            h = hashlib.sha256()
            with open(os.path.join(a.outdir, f), 'rb') as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b''):
                    h.update(chunk)
            lines.append('%s  %s' % (h.hexdigest(), f))
    sums = os.path.join(a.outdir, 'SHA256SUMS.TXT')
    if os.path.exists(sums):
        os.chmod(sums, 0o644)
    open(sums, 'w').write('\n'.join(lines) + '\n')
    os.chmod(sums, 0o444)


if __name__ == '__main__':
    main()
