#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""s4_up2.py - VMS batch for upstream report #2: ODS-2 images written by UPSTREAM's ods2 (branch fix-integrity, no ods2_init, no EXPORT).

Writes go through upstream's binary (env ODS2), every read-back (EXPORT, SHOW VOLUME) through ours (env ODS2_READ); after every
operation: ods5check 0/0, the independent reader = the model (names, sizes, MD5, version limits), EXPORT = the model.
usage: ODS2=<upstream ods2> ODS2_READ=<our ods2> s4_up2.py OUTDIR STARTER.dsk"""
import hashlib, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stress   # noqa: E402

# name: (profile, config ('starter' = the starter volume given on the command line), seed, ops, stop, label, description)
SPECS = {
    'up2-starter': ('up2', 'starter', 5101, 100, None, 'UP2START'),
    'up2-asim6':   ('up2', 'vms:asim6-ods2t6-20261108', 5102, 100, None, 'UP2A6'),
    'up2-phvs2':   ('up2', 'vms:phvs2-ods2v-20261108', 5103, 100, None, 'UP2PH'),
    'up2-full':    ('fill', 'vms:phvs2-ods2v-20261108', 5104, 1, 'full', 'UP2FULL'),
}


def build(name, outdir, starter):
    profile, config, seed, ops, stop, label = SPECS[name]
    if config == 'starter':
        config = 'starter:' + starter
    wd = os.path.join(stress.WORK, 's4up2', name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    h = stress.Harness(seed, profile, config, ops, wd, check_every=1, big=(profile == 'fill'))
    h.stop = stop
    h.run()
    # a unique label (done with upstream's own SET VOLUME/LABEL), then everything is checked once more
    r = subprocess.run([stress.ODS2, h.img, 'SET VOLUME/LABEL=%s' % label], capture_output=True, text=True)
    if r.returncode != 0:
        raise stress.Failure('relabel %s: %s' % (name, (r.stdout + r.stderr)[-300:]))
    h.verify('after relabel', export=False)
    base = os.path.join(outdir, name)
    shutil.copyfile(h.img, base + '.img')
    h.write_manifests(base)
    os.chmod(base + '.img', 0o444)
    for ext in ('.MANIFEST_LINUX.TXT', '.MANIFEST_VMS.TXT'):
        os.chmod(base + ext, 0o444)
    shutil.rmtree(wd, ignore_errors=True)


def main():
    out, starter = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if not os.environ.get('ODS2') or not os.environ.get('ODS2_READ'):
        sys.exit('set ODS2 (upstream binary) and ODS2_READ (our binary)')
    os.makedirs(out, exist_ok=True)
    for name in SPECS:
        for f in os.listdir(out):
            if f.startswith(name + '.'):
                os.chmod(os.path.join(out, f), 0o644)
                os.unlink(os.path.join(out, f))
        try:
            build(name, out, starter)
            print('%-12s ok' % name, flush=True)
        except stress.Failure as e:
            print('%-12s FAIL %s' % (name, str(e)[:600]), flush=True)


if __name__ == '__main__':
    main()
