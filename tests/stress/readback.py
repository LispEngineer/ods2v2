#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""readback.py ORIGINAL.img AFTER.img - what VMS left in a copy of one of our images: ods5check on the after-image, and with the
independent reader: every entry of the original (name, version) must still be there with the same size and MD5 (VMS may only ADD
entries or versions, or remove what it was told to); prints the additions, the removals and any change."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ods5read, stress   # noqa: E402

a, b = ods5read.read_volume(sys.argv[1]), ods5read.read_volume(sys.argv[2])
summ, out, _ = stress.run_checker(sys.argv[2])
warn = [l for l in out.split('\n') if l.startswith(('ERROR', 'WARN'))]
changed, removed, added = [], [], []
for k, fid in a.files.items():
    if fid is None:
        continue
    fi = a.info[fid]
    if fi.volatile:
        continue
    if k not in b.files or b.files[k] is None:
        removed.append(k)
        continue
    gi = b.info[b.files[k]]
    if (gi.size, gi.md5) != (fi.size, fi.md5) and gi.special == fi.special:
        changed.append(k)
for k, fid in b.files.items():
    if k not in a.files:
        added.append(k)
dirs_added = sorted(set(b.dirs) - set(a.dirs))
dirs_removed = sorted(set(a.dirs) - set(b.dirs))
print('%s: checker %s | entries: %d changed, %d removed, %d added; dirs +%d -%d; problems %s' % (
    os.path.basename(sys.argv[2]), summ, len(changed), len(removed), len(added), len(dirs_added), len(dirs_removed), b.problems[:2]))
for l in warn[:4]:
    print('    ' + l[:170])
for k in changed[:3]:
    print('    CHANGED', repr(k)[:150])
for k in removed[:3]:
    print('    REMOVED', repr(k)[:150])
for k in added[:4]:
    print('    ADDED  ', repr(k)[:150])
