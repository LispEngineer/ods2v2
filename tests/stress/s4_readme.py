#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""s4_readme.py OUTDIR CODE_REV - writes OUTDIR/README.md and SHA256SUMS.TXT for the S4 images (table numbers read from the images
by the independent reader), runs tests/ods5check.py on each image and refuses to continue unless it says 0 errors, 0 warnings."""
import glob, hashlib, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ods5read, stress   # noqa: E402

out, rev = os.path.abspath(sys.argv[1]), sys.argv[2]
WHAT = {
    's4-f2-fresh': 'ods2_init volume, 150 random operations (directories up to 7 deep, files 0..6 MB, deletes, aliases, relabel)',
    's4-f5-hl': 'ods2_init volume, 150 random operations: spaces, dots, ISO Latin-1, UCS-2, 236-character names, hard links, symbolic links, IMPORT trees',
    's4-f5-nohl': 'the same without HARDLINKS (aliases, link count 0), odd size',
    's4-vms5-heavy': 'VMS-made and VMS-written w1c-f5-after-asim12, then 250 more ods2v2 operations (deletes of VMS files, new trees, links); label S4VMS5',
    's4-vms2-heavy': 'V7.3-2-made ODS2T6 corpus volume (asim6-ods2t6), then 250 more operations; label S4VMS2',
    's4-full': 'filled to the brim: 0 free blocks (last COPY refused, then 1-3 byte files until nothing fit)',
    's4-full2': 'the same on ODS-2',
    's4-exthdr': '1000 small files, half deleted, 3 big files fragmented into the holes: extension headers',
    's4-exthdr2': 'the same on ODS-2',
    's4-bigdir': 'one directory [BIG] with 3000 entries (short and 100+ character names): many block splits; INDEXF grown',
    's4-bigdir2': 'one directory [BIG] with 3000 entries on ODS-2',
    's4-bigindex': 'one directory [BIG] with 5200 files: INDEXF.SYS grows past 4,096 blocks (6204 allocated), the case the 22f0811 growth change is about',
    's4-bigindex2': 'the same on ODS-2',
    's4-deep': '255 nested directories (names mixed ASCII / ISO Latin-1 / UCS-2), files LEAF1.TXT.. at 7 depths. V8.4 ONLY: the full path is ~1300 characters (V7.3-2 and VAX V7.3 limit a file specification to 255)',
}
rows, sums = [], []
for f in sorted(glob.glob(os.path.join(out, '*.img'))):
    name = os.path.basename(f)[:-4]
    summ = stress.run_checker(f)[0]
    if summ != '0 error(s), 0 warning(s)':
        sys.exit('%s: checker says %s' % (name, summ))
    v = ods5read.read_volume(f)
    if v.problems:
        sys.exit('%s: reader problems %s' % (name, v.problems[:3]))
    files = [(k, fid) for k, fid in v.files.items() if fid and not v.info[fid].volatile]
    nfile = sum(1 for k, fid in files if v.info[fid].special != 5)
    nsym = sum(1 for k, fid in files if v.info[fid].special == 5)
    groups = {}
    for k, fid in v.files.items():
        if fid:
            groups.setdefault(fid, []).append(k)
    multi = sum(1 for g in groups.values() if len(g) > 1)
    bigdir = max(sum(c for _, c in v.info[fid].extents) for fid in v.dirs.values())
    deepest = max(len(p) for p in v.dirs)
    kind = 'ODS-%d%s' % (v.level, (' HARDLINKS' if v.level == 5 and v.volchar & 0x40 else ' no HARDLINKS') if v.level == 5 else '')
    rows.append('| %s | %s | %d | %d | %s | %d / %d / %d | free %d blocks; extension headers %d; most extents of a file %d; biggest directory %d blocks; deepest path %d levels; %d files with 2+ names |' % (
        name, kind, v.size_blocks, v.cluster, WHAT.get(name, ''), nfile, nsym, len(v.dirs) - 1, v.free_clusters * v.cluster, v.nheaders_ext, v.max_extents, bigdir, deepest, multi))
    h = hashlib.sha256()
    with open(f, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            h.update(chunk)
    sums.append('%s  %s.img' % (h.hexdigest(), name))
txt = """# S4: after-images of the ods5-stress harness, for VMS to confirm

Made %s (-0500) by ods5-st (Claude Sonnet 5.5) with ods2v2 built from origin/ods5 %s (the stress branch ods5-stress is that plus tests only). Every image was built by the stress harness: after EVERY
operation tests/ods5check.py (0 errors, 0 warnings), an independent reader (names, sizes, MD5 of every file = the model) and ods2's EXPORT agreed; the final images were checked again (0/0, just before this file was written).
All images are mode 444; SHA-256 in SHA256SUMS.TXT. Labels are unique, so any number can be mounted together.

| image | ODS | blocks | cluster | what it is | files / symlinks / dirs | read from the image |
| :-- | :-- | --: | :-: | :-- | :-- | :-- |
%s

Manifests (made from the harness's MODEL, not from ods2v2's output): `NAME.MANIFEST_LINUX.TXT` = `md5 size kind path` (kind file / symlink / dir; Linux path with `%%XX` escapes and `;version`, as in the earlier batches);
`NAME.MANIFEST_VMS.TXT` = `md5 size DCL-file-specification` in ASCII (every non-ASCII character as ^xx or ^Uxxxx, `^_` a space, `^.` a dot) or `symlink <spec>`. The md5 is that of the file's data bytes up to its end-of-file mark
(what `CHECKSUM/ALGORITHM=MD5` and `md5sum` of a VMS-FTPed binary file give; files written as Stream_LF text or fixed 512-byte records, as ods2v2 writes them, have the bytes as their data). Files made with COPY/CONVERT (VAR records) have the record bytes
(count words included) as their data. The reserved files (INDEXF.SYS, BITMAP.SYS, ...) are not listed: they change by themselves.

Suggested run on VMS (per image): MOUNT it (read-only is enough; V8.4: `MOUNT/NOWRITE`); `ANALYZE/DISK_STRUCTURE/READ_CHECK/NOREPAIR/LIST`; `DIRECTORY/FULL [000000...]*.*;*`; CHECKSUM of every file of the manifest and compare; TYPE a few; DISMOUNT.
Expected from ods2v2's side: s4-full and s4-full2 have 0 free blocks, s4-exthdr and s4-exthdr2 1 free block; ANALYZE should report no errors on any image. Anything ANALYZE says is of interest, warnings too.
Version-limit check (added after the real VAX96 found ods2v2 writing version limit 1 instead of VMS's 32767, so that VMS purged ;1 when ;2 was made): on a WRITABLE mount of a COPY of the image, `COPY` (DCL) some small file onto the name of an
ods2v2-made file of the manifest (so VMS creates the next version, e.g. `COPY SYS$LOGIN:LOGIN.COM DKA0:[D]F.TXT`), then `DIRECTORY DKA0:[D]F.TXT;*`: **both ;1 (the old one, unchanged) and the new ;2 must be listed**. Do the same for a directory-listed alias or symlink name and for
one file in each of the three kinds of directory (the root [000000], a subdirectory, a big one). Also `DIRECTORY/FULL` on an ods2v2-made file and on a VMS-made one next to it: nothing may differ but dates, ids and sizes.
The images are what ods2v2 leaves after normal, completed commands; interrupted-write and damaged-image behaviour is not in them.
""" % (time.strftime('%Y-%m-%d %H:%M'), rev, '\n'.join(rows))
for name, body in (('README.md', txt), ('SHA256SUMS.TXT', '\n'.join(sums) + '\n')):
    p = os.path.join(out, name)
    if os.path.exists(p):
        os.chmod(p, 0o644)
    open(p, 'w').write(body)
    os.chmod(p, 0o444)
print('ok: %d images' % len(sums))
