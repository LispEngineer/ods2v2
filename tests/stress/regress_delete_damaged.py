#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""regress_delete_damaged.py - found by fuzz_write.py (finding F1): DELETE of a file whose header holds a map pointer that
is not cluster-aligned says "%ODS2-E-DELETEERR ... must be cluster-aligned" and exits 1, but has ALREADY written the file's
final (deleted) header: the directory entry now points at a deleted header, the index bit stays set.  An operation that is
refused must change nothing (the damaged file stays as it was: ANALYZE/REPAIR on VMS is the way to deal with it).

Run from the project root (ods2, ods2_init built).  Exit 0 = refused and nothing changed; 1 = the bug."""
import os, shutil, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5read   # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
# upstream's build: ODS2 = its ods2, ODS2_FAULT = its binary built with fault_shim.c, ODS2_INIT = OUR ods2_init (it has none), UP2=1 = ODS-2 only
ODS2 = os.environ.get('ODS2') or os.path.join(ROOT, 'ods2')
ODS2_FAULT = os.environ.get('FAULT_ODS2') or os.path.join(ROOT, 'ods2_fault')
ODS2_INIT = os.environ.get('ODS2_INIT') or os.path.join(ROOT, 'ods2_init')
UP2 = os.environ.get('UP2') == '1'
work = tempfile.mkdtemp(prefix='regress_', dir=os.environ.get('TMPDIR', None))
img = os.path.join(work, 'v.img')
rc = 0
for level in ((2,) if UP2 else (2, 5)):
    if os.path.exists(img):
        os.unlink(img)
    subprocess.run([ODS2_INIT, img, '16384', '--structure=%d' % level, '--cluster=4'], check=True, capture_output=True)
    src = os.path.join(work, 'f.dat')
    open(src, 'wb').write(bytes(range(256)) * 20)
    subprocess.run([ODS2, img, 'COPY %s [000000]VICTIM.DAT' % src], check=True, capture_output=True)
    v = ods5read.read_volume(img)
    fid = v.files[((), 'VICTIM.DAT', 1)]
    home = open(img, 'rb').read(1024)[512:]
    base = struct.unpack_from('<I', home, 24)[0] + struct.unpack_from('<H', home, 32)[0]
    with open(img, 'r+b') as f:
        f.seek((base + fid - 1) * 512)
        h = bytearray(f.read(512))
        o = h[1] * 2                              # map area: a format 1 pointer (word0: type/count, word1: LBN low)
        assert h[o + 1] >> 6 == 1, 'expected a format 1 pointer'
        struct.pack_into('<H', h, o + 2, (struct.unpack_from('<H', h, o + 2)[0] + 1) & 0xFFFF)   # LBN + 1: not cluster-aligned
        struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xFFFF)
        f.seek((base + fid - 1) * 512)
        f.write(h)
    before = ods5read.read_volume(img)
    r = subprocess.run([ODS2, img, 'DELETE [000000]VICTIM.DAT;1'], capture_output=True, text=True)
    after = ods5read.read_volume(img)
    refused = r.returncode == 1
    same = (after.files == before.files and {k: (i.size, i.md5) for k, i in after.info.items() if not i.volatile} ==
            {k: (i.size, i.md5) for k, i in before.info.items() if not i.volatile} and after.free_clusters == before.free_clusters)
    deleted_hdr = after.info.get(fid) is None
    print('level %d: DELETE exit %d, %s; volume unchanged: %s; victim header %s' % (
        level, r.returncode, r.stderr.strip().split('\n')[-1][:80] or r.stdout.strip()[-80:], same, 'GONE' if deleted_hdr else 'intact'))
    if not (refused and same and not deleted_hdr):
        rc = 1
shutil.rmtree(work, ignore_errors=True)
print('PASS' if rc == 0 else 'FAIL: a refused DELETE changed the volume (finding F1)')
sys.exit(rc)
