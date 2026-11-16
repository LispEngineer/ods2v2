#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""repair_image.py SRC.img OUTDIR NAME - a repair image for VMS: a copy of SRC (an image written by an older ods2v2, e.g. B321's) after
`SET FILE/ATTRIBUTES=(MRS:0) [000000...]*.*` with the current ods2.  Records, from the independent reader, what changed (the record
attributes MRS / LRL per file, before and after), proves that no file's size or bytes changed, and writes NAME.img (mode 444),
NAME.MANIFEST_LINUX.TXT / NAME.MANIFEST_VMS.TXT (md5 size spec; a name below a directory that starts with '-' on an ODS-2 volume is
`unreachable-bugA <path>`: VAX V7.3 cannot reach it) and NAME.CHANGES.TXT (the attribute changes)."""
import hashlib, os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ods5read, stress   # noqa: E402

src, out, name = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)
img = os.path.join(out, name + '.img')
if os.path.exists(img):
    os.chmod(img, 0o644)
shutil.copyfile(src, img)
os.chmod(img, 0o644)
before = ods5read.read_volume(img)
r = subprocess.run([stress.ODS2, img, 'SET FILE/ATTRIBUTES=(MRS:0) [000000...]*.*'], capture_output=True, text=True, env=stress.ENV)
msg = (r.stdout + r.stderr).strip()
after = ods5read.read_volume(img)
summ = stress.run_checker(img)[0]
changes, bad = [], []
for k, fid in before.files.items():
    if not fid or fid not in before.info:
        continue
    b, a = before.info[fid], after.info.get(after.files.get(k) or -1)
    if a is None:
        bad.append('lost %r' % (k,))
        continue
    if (b.size, b.md5) != (a.size, a.md5):
        bad.append('content changed %r' % (k,))
    if (b.maxrec, b.rsize) != (a.maxrec, a.rsize):
        changes.append((k, b.maxrec, b.rsize, a.maxrec, a.rsize, b.rtype & 0xF))
changes.sort(key=lambda c: repr(c[0]))
with open(os.path.join(out, name + '.CHANGES.TXT'), 'w', encoding='utf-8') as f:
    f.write('SET FILE/ATTRIBUTES=(MRS:0) [000000...]*.* with ods2v2 %s on a copy of %s\n' % (os.environ.get('REV', '?'), os.path.basename(src)))
    f.write('ods2 said: %s\nchecker after: %s\n%d files changed their record attributes (MRS = FAT$W_MAXREC, LRL = FAT$W_RSIZE), %d lost, content changed: %d\n' % (msg[-300:], summ, len(changes), len([b for b in bad if b.startswith('lost')]), len([b for b in bad if b.startswith('content')])))
    f.write('# file | rtype(low nibble, 1=fixed 2=var 3=vfc 4=stream 5=stlf 6=stcr) | MRS before -> after | LRL before -> after\n')
    for k, bm, bl, am, al, rt in changes:
        f.write('%s%s;%d | %d | %d -> %d | %d -> %d\n' % ('/'.join(k[0]) + '/' if k[0] else '', k[1], k[2], rt, bm, am, bl, al))
r0 = stress.EscRng()
lines, vms = [], []
for (dpath, nm, ver), fid in sorted(after.files.items(), key=lambda kv: repr(kv[0])):
    if not fid or after.info[fid].volatile or after.info[fid].special == 5:
        continue
    fi = after.info[fid]
    lp = '/'.join([stress.linux_escape(x) for x in dpath] + ['%s;%d' % (stress.linux_escape(stress.linux_name(nm)), ver)])
    lines.append('%s %d file %s' % (fi.md5, fi.size, lp))
    if any(c.startswith('-') for c in dpath):
        vms.append('unreachable-bugA %s' % lp)
    elif fi.fileorg in (1, 2, 3):
        vms.append('skip-rms-org %s%s;%d' % (stress.spec_dir(dpath, r0, False), stress.spec_file_name(nm, r0, False), ver))
    else:
        vms.append('%s %d %s%s;%d' % (fi.md5, fi.size, stress.spec_dir(dpath, r0, False), stress.spec_file_name(nm, r0, False), ver))
for dp in sorted(after.dirs):
    if dp:
        lines.append('%s 0 dir %s' % ('-' * 32, '/'.join(stress.linux_escape(x) for x in dp)))
for suffix, body in (('.MANIFEST_LINUX.TXT', lines), ('.MANIFEST_VMS.TXT', vms)):
    open(os.path.join(out, name + suffix), 'w', encoding='utf-8').write('\n'.join(body) + '\n')
os.chmod(img, 0o444)
for suffix in ('.MANIFEST_LINUX.TXT', '.MANIFEST_VMS.TXT', '.CHANGES.TXT'):
    os.chmod(os.path.join(out, name + suffix), 0o444)
print('%s: ods2 rc %d, checker %s, %d attribute changes, %s' % (name, r.returncode, summ, len(changes), bad[:3] or 'no content changed, none lost'))
print('  ' + msg[-200:].replace('\n', ' | '))
