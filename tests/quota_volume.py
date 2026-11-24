#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""quota_volume.py - makes the volumes tests/quota_check.py uses. A volume with ACTIVE disk quotas is a volume with a file [000000]QUOTA.SYS (CORPUS 71/72: the only mark). ods2v2 refuses to make that
name, so the file is made as QUOTX.SYS and the directory record's name is changed in place to QUOTA.SYS (the same length: no other byte of the directory changes). Run from the project root."""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import ods5check as O     # noqa: E402

ODS2 = './ods2'
INIT = './ods2_init'


def run(args):
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return p.returncode, p.stdout + p.stderr


def ods2(img, cmd, opts=()):
    return run([ODS2] + list(opts) + [img, cmd])


def rename_record(img, dir_spec, old, new, exact=False):
    """change the name of one directory record of the directory `dir_spec` ('[000000]' or '[A]') from `old` to `new` (the same length), in place"""
    assert len(old) == len(new)
    num = 4 if dir_spec == '[000000]' else E.D.fnum(img, '[000000]%s.DIR' % dir_spec.strip('[]'))
    v = O.Vol(img)
    hdr = v.blk(E.D.lbn(v, num))
    ext = O.decode_map(hdr, O.Findings(False), 'dir')
    o, n = old.upper().encode('latin-1'), (new if exact else new.upper()).encode('latin-1')
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
                    if bytes(blk[pos + 6:pos + 6 + namecount]) == o:
                        blk[pos + 6:pos + 6 + namecount] = n
                        f.seek((lbn + b) * 512)
                        f.write(blk)
                        return
                    pos += 2 + size
    raise AssertionError('no record %s in %s' % (old, dir_spec))


def rename_header(img, number, old, new):
    """the same name change in the file's header (the name in the ident area, with its ;version) and the header's checksum made again"""
    v = O.Vol(img)
    lbn = E.D.lbn(v, number)
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        i = bytes(hb).find(old.upper().encode('latin-1') + b';')
        assert i > 0, 'no name %s in header %d' % (old, number)
        hb[i:i + len(old)] = new.upper().encode('latin-1')
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(hb)


def damage_record(img, dir_spec, name):
    """give the directory record `name` of `dir_spec` the impossible size 3 (a damaged master directory: the reader cannot go past it)"""
    num = 4 if dir_spec == '[000000]' else E.D.fnum(img, '[000000]%s.DIR' % dir_spec.strip('[]'))
    v = O.Vol(img)
    hdr = v.blk(E.D.lbn(v, num))
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
                    if bytes(blk[pos + 6:pos + 6 + namecount]) == nm:
                        struct.pack_into('<H', blk, pos, 3)
                        f.seek((lbn + b) * 512)
                        f.write(blk)
                        return
                    pos += 2 + size
    raise AssertionError('no record %s in %s' % (name, dir_spec))


def dangle_quota(img):
    """the entry [000000]QUOTA.SYS names a file whose header slot is empty (all zeros): an entry that names no file"""
    v = O.Vol(img)
    lbn = E.D.lbn(v, E.D.fnum(img, '[000000]QUOTA.SYS'))
    with open(img, 'r+b') as f:
        f.seek(lbn * 512)
        f.write(bytes(512))


def alias_of_quota(img):
    """[A]ALIAS.SYS is a second entry (an alias) of the quota file"""
    import dirstate as DS
    DS.entry_fid(img, E.D.fnum(img, '[000000]A.DIR'), 'ALIAS.SYS', E.D.fnum(img, '[000000]QUOTA.SYS'))


def make(work, level, hl, quota, tag, decoy=False, top=False, alias=False, lower=False):
    """[A] holds F.TXT (two versions) and G.TXT; with `quota` [000000] holds QUOTA.SYS; with `decoy` [A] holds QUOTA.SYS and [000000] QUOTA.DAT (neither is the quota file)"""
    img = os.path.join(work, 'qv_%s_%d%s.img' % (tag, level, 'h' if hl else ''))
    if os.path.exists(img):
        os.remove(img)
    rc, out = run([INIT, img, '8192', '--structure=%d' % level, '--label=QUOTAV'] + (['--hardlinks'] if hl else []))
    assert rc == 0, out
    host = os.path.join(work, 'qv_host.txt')
    open(host, 'w').write('a file\n')
    cmds = ['CREATE/DIRECTORY [A]', 'COPY %s [A]F.TXT' % host, 'COPY %s [A]F.TXT' % host, 'COPY %s [A]G.TXT' % host]
    if quota:
        cmds.append('COPY %s [000000]QUOTX.SYS' % host)
    if decoy:
        cmds += ['COPY %s [A]QUOTX.SYS' % host, 'COPY %s [000000]QUOTA.DAT' % host]
    if top:
        cmds.append('COPY %s [000000]TOP.TXT' % host)
    if alias:
        cmds.append('COPY %s [A]ALIAS.SYS' % host)
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    if quota:
        rename_header(img, E.D.fnum(img, '[000000]QUOTX.SYS'), 'QUOTX.SYS', 'QUOTA.SYS')
        rename_record(img, '[000000]', 'QUOTX.SYS', 'quota.sys' if lower else 'QUOTA.SYS', exact=lower)
    if alias and quota:
        alias_of_quota(img)
    if decoy:
        rename_header(img, E.D.fnum(img, '[A]QUOTX.SYS'), 'QUOTX.SYS', 'QUOTA.SYS')
        rename_record(img, '[A]', 'QUOTX.SYS', 'QUOTA.SYS')
    return img
