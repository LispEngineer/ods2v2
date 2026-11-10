#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""verify_bundle.py DIR - for every image of an S4 directory: ods5check (0 errors; 0 warnings except the known Bug A one of the repair image),
ods2 mounts it (SHOW VOLUME), the SCB geometry (BITMAP.SYS block 1) is printed, and the independent reader's tree equals MANIFEST_LINUX
(every file's size and MD5, every directory, nothing else)."""
import glob, os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ods5read, stress   # noqa: E402
bad = 0
for img in sorted(glob.glob(os.path.join(sys.argv[1], '*.img'))):
    name = os.path.basename(img)[:-4]
    summ, out, _ = stress.run_checker(img)
    okc = summ == '0 error(s), 0 warning(s)' or (name == 's4-repair-f2' and summ == '0 error(s), 1 warning(s)')
    sv = subprocess.run([stress.ODS2, img, 'SHOW VOLUME'], capture_output=True, text=True, env=stress.ENV)
    mounts = sv.returncode == 0 and 'Structure level' in sv.stdout
    v = ods5read.read_volume(img)
    blk = open(img, 'rb').read(512 * (v.info[2].extents[0][0] + 1))[-512:]
    geom = 'SCB sectors %d tracks %d cylinders %d' % (struct.unpack_from('<H', blk, 12)[0], struct.unpack_from('<H', blk, 14)[0], struct.unpack_from('<I', blk, 16)[0])
    want = {}
    for line in open(os.path.join(sys.argv[1], name + '.MANIFEST_LINUX.TXT'), encoding='utf-8'):
        md5, size, kind, path = line.rstrip('\n').split(' ', 3)
        want[path.split('\t')[0]] = (kind, md5, int(size))
    got = {}
    for (dpath, nm, ver), fid in v.files.items():
        if not fid or v.info[fid].volatile:
            continue
        fi = v.info[fid]
        lp = '/'.join([stress.linux_escape(x) for x in dpath] + ['%s;%d' % (stress.linux_escape(stress.linux_name(nm)), ver)])
        got[lp] = ('symlink', '-' * 32, len(fi.target or b'')) if fi.special == 5 else ('file', fi.md5, fi.size)
    for dp in v.dirs:
        if dp:
            got['/'.join(stress.linux_escape(x) for x in dp)] = ('dir', '-' * 32, 0)
    diff = [k for k in set(want) | set(got) if want.get(k) != got.get(k)]
    print('%-14s checker %s | mounts %s | %s | manifest: %d entries, %d differences%s' % (name, 'ok' if okc else summ, 'yes' if mounts else 'NO', geom, len(want), len(diff), (' e.g. %r' % diff[:2]) if diff else ''))
    if not okc or not mounts or diff:
        bad += 1
print('ALL OK' if not bad else '%d image(s) with problems' % bad)
sys.exit(1 if bad else 0)
