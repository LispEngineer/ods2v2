#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""regress_split_crash.py - found by crash.py (finding F2): a directory block split is not crash safe.

COPY of a file whose new directory entry does not fit its block splits the block.  If the process ends (kill, power cut)
between the writes of the split, files that were already in the directory are no longer listed in it: with ODS2_FAULT_AFTER_WRITES=7
or 8 on the repro, 8 of the root directory's 16 entries vanish (their headers and data are still on the volume, as
lost files; nothing lists them).  An interrupted operation may leave a leak or a half-made new entry, never take
other files' names away.

Method: a fresh volume; files with long names are added to the root directory one at a time; before each addition the
addition is run with ods2_fault for every k, and every name that was in the directory before must still be listed.
Run from the project root (ods2, ods2_fault, ods2_init built).  Exit 0 = no loss, 1 = the bug."""
import os, re, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5read   # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
work = tempfile.mkdtemp(prefix='regress_', dir=os.environ.get('TMPDIR', None))
img, trial = os.path.join(work, 'v.img'), os.path.join(work, 't.img')
src = os.path.join(work, 'f.dat')
open(src, 'wb').write(b'x' * 700)
bad = 0
for level in (2, 5):
    if os.path.exists(img):
        os.unlink(img)
    subprocess.run([os.path.join(ROOT, 'ods2_init'), img, '16384', '--structure=%d' % level], check=True, capture_output=True)
    for i in range(14):
        name = ('N%02d' % i + 'A' * (35 if level == 2 else 96)) + '.' + 'T' * (36 if level == 2 else 3)
        cmd = 'COPY %s [000000]%s' % (src, name)
        before = set(ods5read.read_volume(img, want_md5=False).files)
        shutil.copyfile(img, trial)
        r = subprocess.run([os.path.join(ROOT, 'ods2_fault'), trial, cmd], capture_output=True, text=True,
                           env=dict(os.environ, ODS2_FAULT_COUNT='1'))
        n = int(re.search(r'(\d+) writes', r.stderr).group(1))
        for k in range(1, n + 1):
            shutil.copyfile(img, trial)
            subprocess.run([os.path.join(ROOT, 'ods2_fault'), trial, cmd], capture_output=True, env=dict(os.environ, ODS2_FAULT_AFTER_WRITES=str(k)))
            lost = before - set(ods5read.read_volume(trial, want_md5=False).files)
            if lost:
                bad += 1
                print('level %d, adding entry %d (%d writes), fault after write %d: %d of %d existing entries no longer listed'
                      % (level, i, n, k, len(lost), len(before)))
        subprocess.run([os.path.join(ROOT, 'ods2'), img, cmd], check=True, capture_output=True)
# part 2: DELETE that empties a directory block (the following blocks move up): same rule, the other names stay listed
for level in (2, 5):
    if os.path.exists(img):
        os.unlink(img)
    subprocess.run([os.path.join(ROOT, 'ods2_init'), img, '16384', '--structure=%d' % level], check=True, capture_output=True)
    names = [('N%02d' % i + 'A' * (35 if level == 2 else 96)) + '.' + 'T' * (36 if level == 2 else 3) for i in range(18)]
    for nm in names:
        subprocess.run([os.path.join(ROOT, 'ods2'), img, 'COPY %s [000000]%s' % (src, nm)], check=True, capture_output=True)
    order = names[:]
    import random
    random.Random(level).shuffle(order)
    for nm in order:
        cmd = 'DELETE [000000]%s;1' % nm
        before = set(ods5read.read_volume(img, want_md5=False).files)
        shutil.copyfile(img, trial)
        r = subprocess.run([os.path.join(ROOT, 'ods2_fault'), trial, cmd], capture_output=True, text=True, env=dict(os.environ, ODS2_FAULT_COUNT='1'))
        n = int(re.search(r'(\d+) writes', r.stderr).group(1))
        for k in range(1, n + 1):
            shutil.copyfile(img, trial)
            subprocess.run([os.path.join(ROOT, 'ods2_fault'), trial, cmd], capture_output=True, env=dict(os.environ, ODS2_FAULT_AFTER_WRITES=str(k)))
            lost = {e for e in before - set(ods5read.read_volume(trial, want_md5=False).files) if e[1] != nm}
            if lost:
                bad += 1
                print('level %d, deleting %s (%d writes), fault after write %d: %d other entries no longer listed' % (level, nm[:4], n, k, len(lost)))
        subprocess.run([os.path.join(ROOT, 'ods2'), img, cmd], check=True, capture_output=True)
shutil.rmtree(work, ignore_errors=True)
print('PASS' if not bad else 'FAIL: %d interrupted splits lost directory entries (finding F2)' % bad)
sys.exit(1 if bad else 0)
