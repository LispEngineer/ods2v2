#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""regress_verlimit.py - the version limit (DIR$W_VERLIMIT) of the directory records ods2v2 writes (found on the real VAX96:
ods2v2 wrote 1, so VMS purged ;1 when a user made ;2).

Part 1 (the rule, from VMS-made images only): in the pure VMS-made corpus volumes every entry VMS made for a file or a directory
has version limit 32767; only the nine reserved files in the MFD have 1.
Part 2: every entry ods2v2 writes - COPY, CREATE/DIRECTORY, IMPORT (files, directories, symlinks), LINK/alias, SYMLINK - on a fresh
ODS-2 and ODS-5 volume must carry the same value.   Run from the project root.  Exit 0 = all 32767, 1 = not."""
import os, subprocess, sys, tempfile, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5read   # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CORPUS = os.path.expanduser('~/tmp/ods5-cli/')
RESERVED = {'INDEXF.SYS', 'BITMAP.SYS', 'BADBLK.SYS', 'CORIMG.SYS', 'VOLSET.SYS', 'CONTIN.SYS', 'BACKUP.SYS', 'BADLOG.SYS', 'SECURITY.SYS'}
rc = 0

print('part 1: VMS-made images')
for name in ('asim12-ods5t-20261108', 'asim12-ods2t-20261108', 'asim6-ods5t6-20261108', 'asim6-ods2t6-20261108', 'asim8-dka100-ods5',
             'phvs2-ods2v-20261108'):
    f = CORPUS + name + '.img'
    if not os.path.exists(f):
        print('  %s: not present, skipped' % name)
        continue
    v = ods5read.read_volume(f, want_md5=False)
    bad = {k: vl for k, vl in v.verlimit.items() if vl != 32767 and not (k[0] == () and k[1] in RESERVED)}
    n = len(v.verlimit)
    print('  %-24s ODS-%d: %d entries, %d not 32767 outside the reserved files' % (name, v.level, n, len(bad)))
    if bad:
        rc = 1
        print('    e.g. %r' % list(bad.items())[:3])

print('part 2: what ods2v2 writes')
work = tempfile.mkdtemp(prefix='verlimit_', dir=os.environ.get('TMPDIR', None))
tree = os.path.join(work, 'tree')
os.makedirs(os.path.join(tree, 'sub'))
open(os.path.join(tree, 'a.txt'), 'w').write('a\n')
open(os.path.join(tree, 'sub', 'b.bin'), 'wb').write(os.urandom(2000))
os.symlink('a.txt', os.path.join(tree, 'l.lnk'))
src = os.path.join(work, 'f.dat')
open(src, 'wb').write(b'x' * 700)
for level in (2, 5):
    img = os.path.join(work, 'v%d.img' % level)
    subprocess.run([os.path.join(ROOT, 'ods2_init'), img, '16384', '--structure=%d' % level], check=True, capture_output=True)
    cmds = ['CREATE/DIRECTORY [D]', 'CREATE/DIRECTORY [D.E]', 'COPY %s [D]F.TXT' % src, 'COPY %s [000000]G.TXT' % src,
            'LINK [D.E]L.TXT [000000]G.TXT', 'CREATE/DIRECTORY [IMP]', 'IMPORT %s [IMP]' % tree]
    if level == 5:
        cmds.append('SYMLINK [D]S.LNK target')
    for c in cmds:
        r = subprocess.run([os.path.join(ROOT, 'ods2'), img, c], capture_output=True, text=True)
        if r.returncode != 0 and not c.startswith('IMPORT'):
            print('  command failed: %s: %s' % (c, (r.stdout + r.stderr)[-150:]))
    v = ods5read.read_volume(img, want_md5=False)
    ours = {k: vl for k, vl in v.verlimit.items() if not (k[0] == () and k[1] in RESERVED) and k != ((), '000000.DIR', 1)}
    bad = {k[1]: vl for k, vl in ours.items() if vl != 32767}
    print('  ODS-%d: %d entries written, %d with a version limit other than 32767 %s' % (level, len(ours), len(bad), bad if bad else ''))
    if bad or not ours:
        rc = 1
shutil.rmtree(work, ignore_errors=True)
print('PASS' if rc == 0 else 'FAIL: ods2v2 writes a version limit that VMS does not (VMS purges ;1 when ;2 is made)')
sys.exit(rc)
