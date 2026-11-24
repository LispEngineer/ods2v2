#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""dirstate.py - host-side makers of the stopped states of a directory move (RENAME step 3, review 71), WITHOUT any RENAME code: ods2v2 builds the volume (CREATE/DIRECTORY, COPY, LINK of a stub file) and a host
patch sets the directory file's back link, revision and date (what the first, header, write of a header-first RENAME does) and turns a stub link into a second entry of the directory. The same method as
ods5-cli's corpus/followup_f/make_stageb_images.py (b2-d3). Used by tests/dir_delete_check.py. Run from the project root (./ods2, ./ods2_init built)."""
import calendar
import os
import re
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import ods5check as O     # noqa: E402

ODS2 = './ods2'
INIT = './ods2_init'


def run(args):
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return p.returncode, p.stdout + p.stderr


def ods2(img, cmd):
    return run([ODS2, img, cmd])


def fnum(img, spec):
    return E.D.fnum(img, spec)


def _header_lbn(img, num):
    return E.D.lbn(O.Vol(img), num)


def move_header(img, num, to_dir_num, hl=False):
    """the header-first write of a directory move: back link := the target parent (its own sequence), revision +1, the level's date := local now; HARDLINKS: link count + 1"""
    lbn = _header_lbn(img, num)
    dl = _header_lbn(img, to_dir_num)
    with open(img, 'r+b') as f:
        f.seek(dl * 512)
        dseq = struct.unpack_from('<H', f.read(512), 10)[0]
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        struct.pack_into('<HHBB', hb, 66, to_dir_num, dseq, 0, 0)
        ident = hb[0] * 2
        now = int((calendar.timegm(time.localtime()) + 3506716800) * 1e7)
        if (struct.unpack_from('<H', hb, 6)[0] >> 8) == 5:
            struct.pack_into('<H', hb, ident + 2, struct.unpack_from('<H', hb, ident + 2)[0] + 1)
            struct.pack_into('<Q', hb, ident + 44, now)
            if hl:
                struct.pack_into('<H', hb, 74, struct.unpack_from('<H', hb, 74)[0] + 1)
        else:
            struct.pack_into('<H', hb, ident + 20, struct.unpack_from('<H', hb, ident + 20)[0] + 1)
            struct.pack_into('<Q', hb, ident + 30, now)
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(hb))


def set_count(img, num, count):
    lbn = _header_lbn(img, num)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        struct.pack_into('<H', hb, 74, count)
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(hb))


def entry_fid(img, dir_num, name, to_num):
    """the first entry of the directory record `name` of directory `dir_num` is made to name the file `to_num` (with its own sequence): a second entry of a directory, made by hand"""
    v = O.Vol(img)
    tseq = struct.unpack_from('<H', v.blk(_header_lbn(img, to_num)), 10)[0]
    hdr = v.blk(_header_lbn(img, dir_num))
    ext = O.decode_map(hdr, O.Findings(False), 'dir')
    nm = name.upper().encode('latin-1')
    with open(img, 'r+b') as f:
        for lbn, cnt in ext:
            for b in range(cnt):
                f.seek((lbn + b) * 512)
                blk = bytearray(f.read(512))
                pos = 0
                while pos + 6 <= 512:
                    size = struct.unpack_from('<H', blk, pos)[0]
                    if size == 0xffff or size == 0:
                        break
                    namecount = blk[pos + 5]
                    rn = bytes(blk[pos + 6:pos + 6 + namecount])
                    first = pos + 6 + namecount + (namecount % 2)
                    if rn == nm:
                        struct.pack_into('<HHBB', blk, first + 2, to_num, tseq, 0, 0)
                        f.seek((lbn + b) * 512)
                        f.write(blk)
                        return
                    pos += 2 + size
    raise AssertionError('no directory record %s in directory %d' % (name, dir_num))


def build(work, level, hl, state, empty, tag='ds'):
    """a volume with the old parent [P], the new parent [Q], the directory [P.X] (with F.TXT and a subdirectory S with G.TXT unless `empty`), and the stopped state of RENAME [P]X.DIR [Q]:
    state 'first' (ONE entry [P]X.DIR, the header names Q: back link Q, revision 2; HARDLINKS: count 2) or 'second' (TWO entries [P]X.DIR and [Q]X.DIR; HARDLINKS: count 2 for 2 entries) or 'none'"""
    img = os.path.join(work, '%s_%d%s_%s_%s.img' % (tag, level, 'h' if hl else '', state, 'e' if empty else 'n'))
    if os.path.exists(img):
        os.remove(img)
    rc, out = run([INIT, img, '8192', '--structure=%d' % level, '--label=DIRST'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    f1 = os.path.join(work, 'ds_f1.txt')
    f2 = os.path.join(work, 'ds_g.txt')
    stub = os.path.join(work, 'ds_stub.txt')
    open(f1, 'w').write('file F of X\n')
    open(f2, 'w').write('file G of X.S\n')
    open(stub, 'w').write('stub\n')
    cmds = ['CREATE/DIRECTORY [P]', 'CREATE/DIRECTORY [Q]', 'CREATE/DIRECTORY [P.X]', 'COPY %s [P]B1.TXT' % stub, 'COPY %s [Q]B2.TXT' % stub]
    if not empty:
        cmds += ['COPY %s [P.X]F.TXT' % f1, 'CREATE/DIRECTORY [P.X.S]', 'COPY %s [P.X.S]G.TXT' % f2]
    if state == 'second':
        cmds += ['COPY %s [Q]STUB.TXT' % stub, 'LINK [Q]X.DIR [Q]STUB.TXT']
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    time.sleep(1.1)
    x, q = fnum(img, '[P]X.DIR'), fnum(img, '[000000]Q.DIR')
    if state in ('first', 'second'):
        move_header(img, x, q, hl)
    if state == 'second':
        entry_fid(img, q, 'X.DIR', x)
        if hl:
            set_count(img, fnum(img, '[Q]STUB.TXT'), 1)
    return img


def exports(img, spec='[000000...]*.*;*'):
    """the names and bytes of every file that EXPORT reaches: {relative path: bytes}"""
    out = img + '.export'
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    ods2(img, 'EXPORT %s %s' % (spec, out))
    res = {}
    for dp, dn, fn in os.walk(out):
        for f in fn:
            full = os.path.join(dp, f)
            res[os.path.relpath(full, out)] = open(full, 'rb').read()
    shutil.rmtree(out, ignore_errors=True)
    return res
