#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""rename_check.py - RENAME within one directory (PLAN_WRITE_OPS item 5, step 1), against what OpenVMS does (CORPUS 65, ods5-vms3's RENCASE.COM cases a1 to a3, d1, d3, f1 to f5, g1,
g2, h3: V8.4 ODS-2, ODS-5 and ODS-5 HARDLINKS, VAX V7.3 ODS-2, identical in the compared fields). Written BEFORE the code: ods2 has no RENAME yet, so every check fails on the base.

For each volume kind (ODS-2, ODS-5, ODS-5 with HARDLINKS) a tree is made and the commands of the cases are run on a copy of it. The header of the renamed file is read from the image by
an independent reader (ods5check's volume reader, via tests/ext_headers.py) before and after, and compared byte by byte: ONLY the name area (and the name length at ODS-5),
the revision number, the date field the level changes and the checksum may differ; every other byte (file ID, owner, protection, creation date, map, end of file, highest block written,
back link, link count, ACL ...) must be the same (65.1). The date field: ODS-2: the revision date is set to the time of the RENAME; ODS-5: the revision date is NOT changed, the last
attribute change date is (65.1). The revision number is +1. The checksum is right. The directory lists the new name and not the old one; the data still reads back.

  a1 a2 a3  a new name, a new type, a new version (B.DAT;1 -> B.DAT;5: the version is part of the stored name), in one directory
  d1        the target name exists (T.TXT;1 ;2 ;3): the file becomes T.TXT;4 and the other versions' headers are untouched (65.2)
  d3        an explicit version that exists: refused with -RMS-E-ENT / -SYSTEM-W-DUPFILENAME; the directory is unchanged. VMS also bumps the SOURCE header (revision +1, date): ods2v2 does
            NOT copy that (the decision of 23-NOV-2026, an intended difference: the whole image is byte for byte the same)
  f1 f2 f3  ODS-5 names: a case-only rename (abc.txt -> ABC.txt) stores the new case; a name with a space (ISL-1) and with a euro sign (UCS-2, the length in bytes) keep their name type
  g1 g2     an alias entry (LINK): renaming the alias changes ONLY the directory entry (the header is byte for byte the same, 65.5); renaming the primary entry changes the header's name,
            revision and date, and the alias entry stays as it was
  f4 f5     the same on an ODS-5 HARDLINKS volume, with the link count 2 unchanged
  h3        a read-only image: refused (READONLY), nothing changed
Usage: rename_check.py [--keep]   (run from the project root, with ./ods2 and ./ods2_init built; TMPDIR is used for the scratch images)."""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import ods5check as O     # noqa: E402
import rename_expect as T  # noqa: E402     # what VMS does, case by case (checked against VMS's dumps by ods5-cli's corpus/tools/rename_expect_check.py)

ODS2 = './ods2'
INIT = './ods2_init'
fails = 0
passes = 0


def check(desc, expected, got):
    global fails, passes
    if expected == got:
        passes += 1
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %r' % (expected,))
        print('      got:      %r' % (got,))


def run(args):
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return p.returncode, p.stdout + p.stderr


def ods2(img, cmd):
    return run([ODS2, img, cmd])


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def vms_now():
    """VMS time (100 ns since 17-NOV-1858) of the host's UTC now"""
    return int((time.time() + 3506716800) * 1e7)


def hdr(img, spec):
    ch = E.chain(img, spec)
    return ch[0][1] if ch else None


def fields(h, level):
    ident = h[0] * 2
    u16 = lambda o: struct.unpack_from('<H', h, o)[0]
    u64 = lambda o: struct.unpack_from('<Q', h, o)[0]
    if level == 5:
        nl = h[ident + 1]
        raw = bytes(h[ident + 76:ident + 76 + nl])
        nt = h[ident] & 3
        name = raw.decode('utf-16-le') if nt == 3 else raw.decode('latin-1')
        return {'name': name, 'nametype': nt, 'namelen': nl, 'revision': u16(ident + 2), 'revdate': u64(ident + 12), 'attdate': u64(ident + 44), 'linkcount': u16(74)}
    name = bytes(h[ident:ident + 20]).decode('latin-1').rstrip(' \x00')
    if len(name) == 20 and True:                  # a name over 20 characters goes on in FILENAMEXT (the ident area has it when it is 120 bytes long)
        name += bytes(h[ident + 54:ident + 120]).decode('latin-1').rstrip(' \x00')
    return {'name': name, 'nametype': 0, 'namelen': len(name), 'revision': u16(ident + 20), 'revdate': u64(ident + 30), 'attdate': 0, 'linkcount': 0}


def linkcount(img, spec):
    h = hdr(img, spec)
    return fields(h, 5)['linkcount'] if h is not None else None


def may_differ(h, level):
    """the byte ranges of a header a rename may change"""
    ident = h[0] * 2
    if level == 5:
        return [(ident + 1, ident + 2), (ident + 2, ident + 4), (ident + 44, ident + 52), (ident + 76, ident + 76 + 248), (510, 512)]
    return [(ident, ident + 20), (ident + 20, ident + 22), (ident + 30, ident + 38), (ident + 54, ident + 120), (510, 512)]


def other_bytes_same(a, b, level, adopt=False, extra=()):
    ok = [(x, y) for x, y in may_differ(a, level)] + ([(66, 72)] if adopt else []) + list(extra)
    bad = [i for i in range(512) if a[i] != b[i] and not any(x <= i < y for x, y in ok)]
    return bad


def cksum_ok(h):
    return (sum(struct.unpack_from('<255H', h, 0)) & 0xffff) == struct.unpack_from('<H', h, 510)[0]


def listing(img, d):
    """the 'NAME.TYP;n' entries of a directory (ods2's DIR output: one line per entry, the name then ;version)"""
    rc, out = ods2(img, 'DIR %s*.*;*' % d)
    res = []
    for ln in out.splitlines():
        m = re.match(r'^(\S+?)\s*;(\d+)\s*$', ln)
        if m:
            res.append('%s;%s' % (m.group(1), m.group(2)))
    return sorted(res)


def checker_errors(img):
    rc, out = run(['python3', '-I', 'tests/ods5check.py', img])
    return [ln for ln in out.splitlines() if ln.startswith('ERROR')]


NAMETYPE = {'ODS-2': 0, 'ISL-1': 1, 'UCS-2': 3}
KINDS = (('2', 2, False, 'ODS-2'), ('5', 5, False, 'ODS-5'), ('5h', 5, True, 'ODS-5 HARDLINKS'))
PLAIN = re.compile(r'^[A-Za-z0-9_.;-]+$')


def build(work, level, hl, link, noprimary=False):
    """the base volume of one kind: the files of the cases of the table; `link` = (alias entry, file) made with LINK (None: none); noprimary: the file's header gets back link (0,0,0) (VMS: SET FILE/REMOVE of the
    primary entry), which ods2 cannot make"""
    img = os.path.join(work, 'base_%d%s_%s.img' % (level, 'h' if hl else '', re.sub(r'\W', '_', str(link))))
    a = [INIT, img, '16384', '--structure=%d' % level, '--label=RENAME'] + (['--hardlinks'] if hl else [])
    rc, out = run(a)
    assert rc == 0, out

    def put(name, text):
        p = os.path.join(work, name)
        open(p, 'w').write(text)
        return p
    cmds = ['CREATE/DIRECTORY [X]', 'CREATE/DIRECTORY [X2]', 'CREATE/DIRECTORY [Y]', 'CREATE/DIRECTORY [K]', 'CREATE/DIRECTORY [L]', 'CREATE/DIRECTORY [VL]', 'CREATE/DIRECTORY [M]', 'CREATE/DIRECTORY [ML]']
    if level == 5:
        cmds.append('CREATE/DIRECTORY [N]')
    files = [('[X]A.TXT', 'content of A'), ('[X2]M.TXT', 'content of M'), ('[X2]M2.TXT', 'content of M2'),
             ('[X2]T.TXT', 'T version 1'), ('[X2]T.TXT', 'T version 2'), ('[X2]T.TXT', 'T version 3'), ('[X2]FILE.TXT', 'content of FILE'), ('[X2]H3.TXT', 'content of H3'),
             # CORPUS 66 groups K and L
             ('[K]P1.TXT', 'P1'), ('[K]P2.TXT', 'P2'), ('[X]C.TXT', 'content of C'), ('[X]C2.TXT', 'content of C2'), ('[K]F3.TXT', 'F3'), ('[K]A6.TXT', 'A6'), ('[K]S7.TXT', 'S7'), ('[K]Q5.TXT', 'Q5'), ('[K]T.TXT', 'K T1'), ('[K]T.TXT', 'K T2'), ('[K]T.TXT', 'K T3'),
             ('[K]L.TXT', 'L1'), ('[K]L.TXT', 'L2'), ('[K]L.TXT', 'L3'), ('[K]N8.TXT', 'N8'),
             ('[L]M.TXT;5', 'M v5'), ('[L]T.TXT', 'L T1'), ('[L]T.TXT', 'L T2'), ('[L]T.TXT', 'L T3'), ('[L]U.TXT;5', 'U v5'), ('[L]F5.TXT', 'F5'),
             ('[VL]A.TXT', 'A1'), ('[VL]A.TXT', 'A2'), ('[VL]A.TXT', 'A3'), ('[VL]A.TXT', 'A4'), ('[VL]A.TXT', 'A5'), ('[VL]D.TXT', 'D1'),
             ('[VL]C.TXT', 'C1'), ('[VL]C.TXT', 'C2'), ('[VL]C.TXT', 'C3'), ('[VL]C.TXT', 'C4'), ('[VL]E.TXT', 'E1')]
    for v in (1, 2, 3):                                       # CORPUS 67.4: three versions each; B5 and B6 get a FILE limit of 2 after the three exist, [ML] names the limit 3
        files += [('[M]B1.TXT', 'B1 v%d' % v), ('[M]B2.TXT', 'B2 v%d' % v), ('[ML]B3.TXT', 'B3 v%d' % v), ('[ML]B4.TXT', 'B4 v%d' % v), ('[M]B5.TXT', 'B5 v%d' % v), ('[M]B6.TXT', 'B6 v%d' % v),
                  ('[ML]B7.TXT', 'B7 v%d' % v)]
    files.append(('[ML]X7.TXT', 'X7 v1'))
    if level == 5:                                           # CORPUS 67.5: names of 60 and 140 characters, and short ones
        files += [('[N]' + T.L60, 'long name 60'), ('[N]SHORT2.TXT', 'short name'), ('[N]' + T.L140, 'long name 140'), ('[N]SHORT4.TXT', 'short name')]
    if level == 5:
        files += [('[X2]abc.txt', 'lower case'), ('[X2]sp^_ace.txt', 'a name with a space'), ('[X2]eur^U20ACo.txt', 'a name with a euro sign')]
    for i, (dst, text) in enumerate(files):
        cmds.append('COPY %s %s' % (put('f%d.txt' % i, text + '\n'), dst))
    # the lowest versions go, the limit is the entry's own (66.1): K L.TXT ;1-3 limit 3 (as VMS), VL A.TXT ;3-5 limit 3, VL C.TXT ;3-4 limit 2
    cmds += ['DELETE [K]T.TXT;2', 'DELETE [VL]A.TXT;1', 'DELETE [VL]A.TXT;2', 'DELETE [VL]C.TXT;1', 'DELETE [VL]C.TXT;2',
             'SET FILE/VERSION_LIMIT=3 [K]L.TXT;3', 'SET FILE/VERSION_LIMIT=3 [VL]A.TXT;5', 'SET FILE/VERSION_LIMIT=2 [VL]C.TXT;4',
             'SET FILE/VERSION_LIMIT=2 [M]B5.TXT', 'SET FILE/VERSION_LIMIT=2 [M]B6.TXT', 'SET FILE/VERSION_LIMIT=3 [ML]B3.TXT', 'SET FILE/VERSION_LIMIT=3 [ML]B4.TXT', 'SET FILE/VERSION_LIMIT=3 [ML]B7.TXT']
    if link:
        cmds.append('LINK %s %s' % link)
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    if noprimary:
        v = O.Vol(img)
        num = E.D.fnum(img, link[1])
        lbn = E.D.lbn(v, num)
        with open(img, 'r+b') as f:
            f.seek(lbn * 512)
            hb = bytearray(f.read(512))
            hb[66:72] = bytes(6)                                  # back link (0,0,0)
            struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
            f.seek(lbn * 512)
            f.write(bytes(hb))
    return img


def fresh(base, work, tag):
    img = os.path.join(work, 'w_%s.img' % re.sub(r'\W', '_', tag))
    if os.path.exists(img):
        os.chmod(img, 0o644)
        os.remove(img)
    shutil.copyfile(base, img)
    return img


def run_case(cid, t, kind, level, hl, kname, base, work, start=None):
    """returns the image after the case (the next case of a chain starts from it)"""
    """one case of the table on one volume kind: the same command, the same changes as VMS's (rename_expect.py)"""
    tag = '%s %s' % (kname, cid)
    img = fresh(start or base, work, tag)
    if start:
        time.sleep(1.1)                   # a chained case follows the one before it within the same second otherwise: its date could not be told from the one before
    if cid == 'h3':
        os.chmod(img, 0o444)
    h0 = sha(img)
    hspec = t.get('file') or t['old']
    before = hdr(img, hspec)
    others = {u: hdr(img, t['dir'] + u) for u in t.get('untouched', []) + t.get('untouched_extra', [])}
    list_before = listing(img, t['dir'])
    list_before2 = listing(img, t['dir2']) if t.get('dir2') else None
    rc, out = ods2(img, 'RENAME %s %s' % (t['old'], t['new']))
    if t['status'] == 'refused':
        msgs = t.get('ods2v2_msgs', t.get('msgs', []))
        check('%s: RENAME %s %s is refused with %s' % (tag, t['old'], t['new'], msgs), (True, True), (all(m in out.upper() or m in out for m in msgs), 'ILLCMD' not in out))
        check('%s: the image is byte for byte the same%s' % (tag, ' (VMS bumps the source header: ods2v2 does not, an intended difference)' if t.get('vms_bumps_refused') else ''), h0, sha(img))
        return img
    check('%s: RENAME %s %s succeeds' % (tag, t['old'], t['new']), (0, True), (rc, '%ODS2-E' not in out and '%RENAME-E' not in out and 'ILLCMD' not in out))
    if t.get('ods2v2_unchanged'):
        check('%s: the target is the source: nothing is written (VMS bumps the revision and the date: an intended difference)' % tag, h0, sha(img))
        return img
    new_spec = t.get('after') or t.get('file') or (t['new'] if ';' in t['new'] else t['new'] + ';1')
    after = hdr(img, new_spec)
    if before is None or after is None:
        check('%s: the file is found under %s' % (tag, new_spec), True, after is not None)
        return img
    fb, fa = fields(before, level), fields(after, level)
    if t['header'] == 'same':
        check('%s: the header is byte for byte the same (only the directory entry changes)' % tag, before, after)
    else:
        if t['stored'] and PLAIN.match(t['stored'][1]):
            check('%s: the stored name is %s' % (tag, t['stored'][1]), t['stored'][1], fa['name'])
        check('%s: revision number +%d' % (tag, t['revision']), fb['revision'] + t['revision'], fa['revision'])
        extra = []
        if t.get('nametype') and t['nametype'][0] != t['nametype'][1]:
            extra.append((before[0] * 2, before[0] * 2 + 1))                     # the name type is in the control byte
        if t.get('mpoffset'):
            extra.append((1, 2))                                                  # MPOFFSET
            if t['mpoffset'][1] > t['mpoffset'][0]:
                extra.append((before[0] * 2, t['mpoffset'][1] * 2 + before[58] * 2))   # the ident area grows over the old map; the map moves up
        check('%s: no byte outside name, revision, the date field and the checksum changed%s' % (tag, ' (and the back link, which is set)' if t.get('backlink') in ('adopted', 'moved') else ''), [],
              other_bytes_same(before, after, level, t.get('backlink') in ('adopted', 'moved'), extra))
        if t.get('backlink') == 'moved':
            check('%s: the back link is now the directory %s' % (tag, t['dir2']), E.D.fnum(img, '[000000]%s.DIR' % t['dir2'].strip('[]')), backlink_num(after))
        if t.get('mpoffset'):
            mp0, mp1 = t['mpoffset']
            mb = before[58] * 2
            ident = before[0] * 2
            check('%s: MPOFFSET goes %d to %d words (a longer name moves the map up, a shorter one leaves it: CORPUS 67.5)' % (tag, mp0, mp1), (mp0, mp1), (before[1], after[1]))
            check('%s: the map area (%d bytes) is intact, at its new place' % (tag, mb), bytes(before[mp0 * 2:mp0 * 2 + mb]), bytes(after[mp1 * 2:mp1 * 2 + mb]))
            nl = fa['namelen']
            check('%s: nothing is left after the name in the ident area (%d bytes of slack, all zero)' % (tag, mp1 * 2 - ident - 76 - nl), True, not any(after[ident + 76 + nl:mp1 * 2]))
        if t.get('backlink') == 'adopted':
            check('%s: the header named no entry (back link 0); its back link is now the directory of the renamed entry' % tag, (bytes(6), E.D.fnum(img, '[000000]L.DIR')), (bytes(before[66:72]), struct.unpack_from('<H', after, 66)[0]))
        check('%s: the header checksum is right' % tag, True, cksum_ok(after))
        if level == 2:
            check('%s: ODS-2: the revision date is set to the time of the RENAME' % tag, (True, True), (fa['revdate'] > fb['revdate'], abs(fa['revdate'] - vms_now()) < 864000000000))
        else:
            check('%s: ODS-5: the last attribute change date is set, the revision date is not changed' % tag,
                  (True, True, fb['revdate']), (fa['attdate'] > fb['attdate'], abs(fa['attdate'] - vms_now()) < 864000000000, fa['revdate']))
    if t.get('nametype'):
        check('%s: the name type is %s and stays, the length goes %d to %d' % (tag, t['nametype'][0], t['namelen'][0], t['namelen'][1]),
              (NAMETYPE[t['nametype'][0]], NAMETYPE[t['nametype'][1]], t['namelen'][0], t['namelen'][1]), (fb['nametype'], fa['nametype'], fb['namelen'], fa['namelen']))
    if t.get('linkcount') is not None:
        check('%s: the link count is %d before and after' % (tag, t['linkcount']), (t['linkcount'],) * 2, (fb['linkcount'], fa['linkcount']))
    for u, h in others.items():
        check('%s: the header of %s is untouched' % (tag, u), h, hdr(img, t['dir'] + u))
    if t['removed'] is not None and all(PLAIN.match(x) for x in t['removed'] + t['added']):
        if t.get('dir2'):
            lb, la = set(list_before), set(listing(img, t['dir']))
            lb2, la2 = set(list_before2), set(listing(img, t['dir2']))
            check('%s: %s loses %s and %s gains %s' % (tag, t['dir'], t['removed'], t['dir2'], t['added']), (sorted(t['removed']), [], [], sorted(t['added'])), (sorted(lb - la), sorted(la - lb), sorted(lb2 - la2), sorted(la2 - lb2)))
        else:
            lb, la = set(list_before), set(listing(img, t['dir']))
            check('%s: the directory loses %s and gains %s' % (tag, t['removed'], t['added']), (sorted(t['removed']), sorted(t['added'])), (sorted(lb - la), sorted(la - lb)))
    check('%s: the checker finds nothing wrong' % tag, [], checker_errors(img))
    if cid == 'g2':
        check('%s: the alias entry stays as it was and reads the same data' % tag, (list_alias_before(base), 'content of FILE'),
              (listing(img, '[Y]'), ods2(img, 'TYPE %s' % t['link'][0])[1].strip().splitlines()[-1]))
    if cid == 'g1':
        check('%s: the data reads back under the alias' % tag, 'content of FILE', ods2(img, 'TYPE %s' % t['new'])[1].strip().splitlines()[-1])
    return img


def efblk(img, num):
    rc, out = run(['./ods2_dump_header', img, str(num)])
    m = re.search(r'efblk\(swapped\)=(\d+)', out)
    return int(m.group(1)) if m else None


def hiblk(img, num):
    rc, out = run(['./ods2_dump_header', img, str(num)])
    m = re.search(r'hiblk\(swapped\)=(\d+)', out)
    return int(m.group(1)) if m else None


def full_volume(work, level):
    """a full volume (review 65): a directory whose next insert must grow it, and no free block: the RENAME that needs the growth is refused and the image is byte for byte the same"""
    tag = 'ODS-%d full volume' % level
    img = os.path.join(work, 'full%d.img' % level)
    rc, out = run([INIT, img, '8192', '--structure=%d' % level, '--label=FULL'])
    assert rc == 0, out
    small = os.path.join(work, 'small.txt')
    open(small, 'w').write('one block of text\n')
    assert ods2(img, 'CREATE/DIRECTORY [F]')[0] == 0
    fid = E.D.fnum(img, '[000000]F.DIR')
    nxt = None
    for i in range(400):
        name = 'N%05d.TXT' % i
        probe = img + '.probe'
        shutil.copyfile(img, probe)
        e = efblk(img, fid)
        ods2(probe, 'COPY %s [F]%s' % (small, name))
        grows = efblk(probe, fid) > e
        os.remove(probe)
        if grows and e >= 3 and e - 1 == hiblk(img, fid):       # the directory is full to its allocation: the growth needs a new, larger area
            nxt = name
            break
        assert ods2(img, 'COPY %s [F]%s' % (small, name))[0] == 0
    assert nxt, 'the directory never grew'
    # take every free block: a file of the size that is left, smaller until it fits
    out = ods2(img, 'SHOW VOLUME')[1]
    free = int(re.search(r'Free blocks:\s+(\d+)', out).group(1))
    for n in range(free - 1, 0, -1):
        big = os.path.join(work, 'big.bin')
        with open(big, 'wb') as f:
            f.write(b'x' * (n * 512))
        if ods2(img, 'COPY %s [000000]BIG.BIN' % big)[0] == 0 and 'ODS2-E' not in ods2(img, 'DIR [000000]BIG.BIN')[1]:
            break
    left = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    check('%s: the volume has no room for the growth of the directory (%d blocks free)' % (tag, left), True, left < 2)
    probe = img + '.probe'
    shutil.copyfile(img, probe)
    rcp, outp = ods2(probe, 'COPY %s [F]%s' % (small, nxt))
    check('%s: (precondition) a COPY of a new name into the directory is refused: the volume cannot hold the growth' % tag, True, '%ODS2-E' in outp or rcp != 0)
    os.remove(probe)
    h0 = sha(img)
    rc, out = ods2(img, 'RENAME [F]N00000.TXT [F]%s' % nxt)
    check('%s: RENAME that needs the directory to grow is refused' % tag, (True, False), ('ILLCMD' not in out and ('-E-' in out or 'ODS2-' in out), '%RENAME-S' in out))
    check('%s: the image is byte for byte the same' % tag, h0, sha(img))


def ext_headers_case(work, level):
    """a file in a primary header and three extension headers (tests/setfile_ext_check.py's volume): RENAME changes the primary header only; the extension headers keep every byte, the old name in them
    included (CORPUS 66.2: VMS leaves them so; ods2v2 writes the first 20 bytes of the name into them at ODS-2 since follow-up d, and a RENAME does not rewrite them)"""
    import setfile_ext_check
    tag = 'ODS-%d file with extension headers' % level
    img = setfile_ext_check.build(level, work)
    before = E.chain(img, '[F]A.DAT')
    rc, out = ods2(img, 'RENAME [F]A.DAT [F]A2.DAT')
    check('%s: RENAME succeeds' % tag, (0, True), (rc, '%ODS2-E' not in out and 'ILLCMD' not in out))
    after = E.chain(img, '[F]A2.DAT')
    check('%s: the file has the same number of headers (%d)' % (tag, len(before)), len(before), len(after))
    if len(before) == len(after) and len(before) >= 2:
        check('%s: every extension header is byte for byte the same' % tag, [], [i for i in range(1, len(before)) if before[i][1] != after[i][1]])
        check('%s: the primary header changed only in its name, revision, date and checksum' % tag, [], other_bytes_same(before[0][1], after[0][1], level))
        check('%s: and its name is A2.DAT;1' % tag, 'A2.DAT;1', fields(after[0][1], level)['name'])
    check('%s: the checker finds nothing wrong' % tag, [], checker_errors(img))


def review67_cases(work, level):
    """review 67: F1 a name held by a directory or a reserved name, F2 another version of the same name under a version limit, F3 an ODS-5 name that needs another ident area, S3 and N2 the words"""
    kind = 'ODS-%d' % level
    img = os.path.join(work, 'r67_%d.img' % level)
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=R67'])[0] == 0
    src = os.path.join(work, 'r67.txt')
    open(src, 'w').write('x\n')
    cmds = ['CREATE/DIRECTORY [D]', 'CREATE/DIRECTORY [D.SUB]', 'COPY %s [D]A.TXT' % src, 'COPY %s [000000]M.TXT' % src, 'COPY %s [D]AL0.TXT' % src]
    for v in (1, 2, 3):
        cmds.append('COPY %s [D]B.TXT' % src)
    cmds += ['SET FILE/VERSION_LIMIT=3 [D]B.TXT', 'LINK [D]AL.TXT [D]AL0.TXT;1']
    if level == 5:
        cmds += ['COPY %s [D]%s' % (src, 'L' * 60 + '.TXT'), 'COPY %s [D]Q.TXT' % src]
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    base = os.path.join(work, 'r67_base.img')
    shutil.copyfile(img, base)

    def case(desc, cmd, msg=None):
        shutil.copyfile(base, img)
        h0 = sha(img)
        rc, out = ods2(img, cmd)
        check('%s: %s is refused%s and the image is unchanged' % (kind, cmd, ' (%s)' % msg if msg else ''), (True, h0), ('ILLCMD' not in out and '%ODS2-E' in out and (msg is None or msg in out), sha(img)))
    # F1: the name is held by a directory: no ;2 above its entry; the reserved names are refused by name
    case('F1', 'RENAME [D]A.TXT [D]SUB.DIR', 'a directory of this name exists')
    case('F1', 'RENAME [000000]M.TXT [000000]INDEXF.SYS', 'reserved')
    case('F1', 'RENAME [000000]M.TXT [000000]BITMAP.SYS', 'reserved')
    # F2 is measured now (CORPUS 67.4: the m cases of the table): another version of the same name prunes nothing while the count is within the limit, and the lowest after the rename when
    # the file's limit is below the count. F3 is measured (67.5: the n cases): a longer name moves the map up, a shorter one leaves MPOFFSET and zero-fills the ident area.
    # An ODS-5 name of an ODD number of bytes over 44 (the area is rounded up to an even size, review 67 recheck): same length, case only, one byte longer, two bytes longer
    if level == 5:
        odd = 'O' * 43 + '.TXT'                    # 47 characters, 49 bytes with ;1: an area of 126
        shutil.copyfile(base, img)
        assert ods2(img, 'COPY %s [D]%s' % (src, odd))[0] == 0
        for nm_new, what in (('P' * 43 + '.TXT', 'the same length'), ('o' * 43 + '.txt', 'the case only (in place)'), ('P' * 44 + '.TXT', 'one byte longer (the area is 126 already)'),
                             ('P' * 45 + '.TXT', 'two bytes longer (the map moves up by one word)')):
            shutil.copyfile(base, img)
            ods2(img, 'COPY %s [D]%s' % (src, odd))
            h0 = hdr(img, '[D]%s;1' % odd)
            rc, out = ods2(img, 'RENAME [D]%s [D]%s' % (odd, nm_new))
            h1 = hdr(img, '[D]%s;1' % nm_new)
            check('%s: a name of an odd number of bytes over 44 renamed to %s works, MPOFFSET %s and the checker is clean' % (kind, what, '%d to %d' % (h0[1], h1[1]) if h1 is not None else '?'),
                  (0, [], True), (rc, checker_errors(img), h1 is not None and h1[1] >= h0[1] and h1[1] * 2 >= h1[0] * 2 + 76 + fields(h1, 5)['namelen']))
        # no room to move the map: a file whose primary header's map is full (tests/setfile_ext_check.py's volume): refused, the image unchanged
        import setfile_ext_check
        ework = os.path.join(work, 'e67')
        os.makedirs(ework, exist_ok=True)
        eimg = setfile_ext_check.build(5, ework)
        h0 = sha(eimg)
        rc, out = ods2(eimg, 'RENAME [F]A.DAT [F]%s' % ('Z' * 60 + '.DAT'))
        check('%s: a rename to a name that needs the map to move, with no room above it (the primary header\'s map is full), is refused and the image is unchanged' % kind, (True, h0), ('map' in out and '%ODS2-E' in out, sha(eimg)))
    # H1 (review 66 F1) in the one function for COPY and RENAME: see tests/versorder_check.py
    # S3: a target entry that already names the file (a LINK, not an interrupted RENAME): said as it is
    shutil.copyfile(base, img)
    rc, out = ods2(img, 'RENAME [D]AL0.TXT [D]AL.TXT')
    check('%s: RENAME onto an alias of the same file says the target already named the file (not "an interrupted RENAME finished")' % kind, (0, True, False, ['AL.TXT;1']),
          (rc, 'already named this file' in out, 'interrupted' in out, [x for x in listing(img, '[D]') if x.startswith('AL.')]))
    # N2: a fault of the target name is the target's
    shutil.copyfile(base, img)
    rc, out = ods2(img, 'RENAME [D]A.TXT [D]A`B.TXT' if level == 5 else 'RENAME [D]A.TXT [D]A@B.TXT')
    check('%s: a fault of the new name is reported as the new name\'s' % kind, True, 'the new name is not valid' in out)


def header_first_states(work, level, hl):
    """the order proposal (review 67 recheck): HEADER FIRST. After the first write the header names the new name and the directory has the old entry only. In that state ods2v2's DELETE of the old name
    removes the ENTRY only (VMS: an alias; the file stays allocated in no directory, CORPUS 69), RENAME to a third name leaves the header alone, and the same RENAME run again finishes it; ods5check says
    exactly one warning (the header name differs from its directory entry). On a HARDLINKS volume the link count was raised in the same write, so a DELETE in that state removes the entry and the
    file stays (the known leak of the README: a lost file for the checker)."""
    kind = 'ODS-%d%s' % (level, ' HARDLINKS' if hl else '')
    img = os.path.join(work, 'hf_%d%s.img' % (level, 'h' if hl else ''))
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=HF'] + (['--hardlinks'] if hl else []))[0] == 0
    src = os.path.join(work, 'hf.txt')
    open(src, 'w').write('header first\n')
    for c in ('CREATE/DIRECTORY [D]', 'COPY %s [D]P.TXT' % src):
        assert ods2(img, c)[0] == 0
    time.sleep(1.1)
    base = os.path.join(work, 'hf_base.img')
    shutil.copyfile(img, base)

    def state():
        shutil.copyfile(base, img)
        rc, out = run(['./ods2_fault', img, 'RENAME [D]P.TXT [D]Q.TXT'] if False else ['env', 'ODS2_FAULT_AFTER_WRITES=1', './ods2_fault', img, 'RENAME [D]P.TXT [D]Q.TXT'])
        return img
    img = state()
    ls = listing(img, '[D]')
    chk = run(['python3', '-I', 'tests/ods5check.py', img])[1]
    warns = [l for l in chk.splitlines() if l.startswith('WARN') and 'link count' not in l]
    h = hdr(img, '[D]P.TXT;1')
    check('%s: after the first write the directory has P.TXT only and the header names Q.TXT;1' % kind, (['P.TXT;1'], 'Q.TXT;1'), (ls, fields(h, level)['name'] if h is not None else None))
    check('%s: ods5check says exactly one warning, the header name that differs from its entry' % kind, (1, True), (len(warns), bool(warns) and "header name 'Q.TXT;1' differs from its directory entry" in warns[0]))
    free0 = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    h0 = fields(h, level)
    # CORPUS 69 (V8.4 both levels, VAX V7.3): VMS reads that entry as an ALIAS (the header names Q1, not P1). DELETE P removes the ENTRY only: the file stays allocated in no directory (ANALYZE: LOSTHEADER;
    # /REPAIR puts it in SYSLOST); ods2v2 says so (not a plain "deleted") and ods5check reports the lost file
    rc, out = ods2(img, 'DELETE [D]P.TXT')
    free1 = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    errs = checker_errors(img)
    check('%s: DELETE P.TXT removes the ENTRY only (VMS: an alias): the directory is empty, no blocks came back, the file is allocated in no directory (the checker: a lost file)' % kind,
          ([], True, True), (listing(img, '[D]'), free1 == free0, any('lost file' in e for e in errs)))
    check('%s: DELETE P.TXT does not print a plain "deleted": it says only the entry was removed and the file stays in no directory (ANALYZE/REPAIR on VMS)' % kind,
          (False, True, True), ('%ODS2-I-DELETED' in out, 'no entry left in its directory' in out, 'REPAIR' in out))
    # RENAME P R in that state: an alias (VMS): the entry only, the header is untouched
    img = state()
    rc, out = ods2(img, 'RENAME [D]P.TXT [D]R.TXT')
    h = hdr(img, '[D]R.TXT;1')
    hr = fields(h, level) if h is not None else {}
    check('%s: RENAME P.TXT R.TXT in that state treats P as an alias (VMS): one entry R.TXT, the header untouched (still Q.TXT;1, the same revision, no error)' % kind,
          (0, ['R.TXT;1'], 'Q.TXT;1', h0['revision'], []), (rc, listing(img, '[D]'), hr.get('name'), hr.get('revision'), checker_errors(img)))
    # and the same RENAME run again finishes it, through the header that already names Q: no header write, no revision bump (VMS: RENAME P1 Q1 finishes cleanly)
    img = state()
    rc, out = ods2(img, 'RENAME [D]P.TXT [D]Q.TXT')
    h = hdr(img, '[D]Q.TXT;1')
    count = fields(h, level)['linkcount'] if (h is not None and hl) else None
    rv = fields(h, level)['revision'] if h is not None else None
    check('%s: RENAME P.TXT Q.TXT in that state finishes it with no header write: the revision is as it was (%s)' % (kind, h0['revision']), h0['revision'], rv)
    warns2 = [w for w in run(['python3', '-I', 'tests/ods5check.py', img])[1].splitlines() if w.startswith('WARN')]
    # third recheck (item 2): a HARDLINKS re-run does not count again when the header already names the target and there is no target entry (the count was already taken): the count ends at 1
    check('%s: the same RENAME run again finishes it: Q.TXT;1 only, the header named once, the checker clean%s' % (kind, ', the link count back to 1 (not counted twice)' if hl else ''),
          (0, ['Q.TXT;1'], 'Q.TXT;1', [], [], 1 if hl else None),
          (rc, listing(img, '[D]'), fields(h, level)['name'] if h is not None else None, checker_errors(img), warns2, count))
    # item 3: the message of that re-run does not call the entry an alias (the header was not changed by THIS run: it was already right)
    check('%s: the re-run of the stopped RENAME does not say "an alias entry"' % kind, (True, True), ('alias entry' not in out, 'renamed to' in out))


def g1_aliases(work, level, hl):
    """third recheck G1: in the state after a stopped RENAME's header write (entry P, the header names N), a second entry X made with LINK makes P and X both aliases (none is named by the header): DELETE of
    either removes the entry only, the file stays (the other name still enters it); PURGE and the version limit use the same rule. Before: DELETE X freed the file and left P dangling."""
    kind = 'ODS-%d%s' % (level, ' HARDLINKS' if hl else '')
    img = os.path.join(work, 'g1_%d%s.img' % (level, 'h' if hl else ''))
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=G1'] + (['--hardlinks'] if hl else []))[0] == 0
    src = os.path.join(work, 'g1.txt')
    open(src, 'w').write('header first\n')
    for c in ('CREATE/DIRECTORY [D]', 'COPY %s [D]P.TXT' % src):
        assert ods2(img, c)[0] == 0
    time.sleep(1.1)
    base = os.path.join(work, 'g1_base.img')
    shutil.copyfile(img, base)

    def state(extra=(), link=True):
        shutil.copyfile(base, img)
        run(['env', 'ODS2_FAULT_AFTER_WRITES=1', './ods2_fault', img, 'RENAME [D]P.TXT [D]N.TXT'])
        for c in (('LINK [D]X.TXT [D]P.TXT',) if link else ()) + tuple(extra):
            assert ods2(img, c)[0] == 0, c
        return img

    def alive(img, name):
        out = ods2(img, 'TYPE [D]' + name)[1]
        return 'header first' in out

    img = state()
    check('%s: after the stopped RENAME and a LINK the directory has P.TXT and X.TXT' % kind, ['P.TXT;1', 'X.TXT;1'], listing(img, '[D]'))
    free0 = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    for victim, other in (('X.TXT', 'P.TXT'), ('P.TXT', 'X.TXT')):
        img = state()
        rc, out = ods2(img, 'DELETE [D]%s' % victim)
        free1 = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
        check('%s: DELETE %s (an alias: the header names N.TXT, which is not there) removes the entry only: %s still enters the file, no blocks freed' % (kind, victim, other),
              ([other + ';1'], True, free1 == free0), (listing(img, '[D]'), alive(img, other), free1 == free0))
    img = state(('COPY %s [D]P.TXT' % src,))
    rc, out = ods2(img, 'PURGE [D]P.TXT')
    check('%s: PURGE P.TXT drops P.TXT;1 as an entry only: X.TXT still enters the file' % kind, (['P.TXT;2', 'X.TXT;1'], True), (listing(img, '[D]'), alive(img, 'X.TXT')))
    shutil.copyfile(base, img)
    assert ods2(img, 'SET FILE/VERSION_LIMIT=1 [D]P.TXT')[0] == 0
    run(['env', 'ODS2_FAULT_AFTER_WRITES=1', './ods2_fault', img, 'RENAME [D]P.TXT [D]N.TXT'])
    assert ods2(img, 'LINK [D]X.TXT [D]P.TXT')[0] == 0
    rc, out = ods2(img, 'COPY %s [D]P.TXT' % src)
    check('%s: COPY over the version limit drops P.TXT;1 as an entry only: X.TXT still enters the file' % kind, (['P.TXT;2', 'X.TXT;1'], True), (listing(img, '[D]'), alive(img, 'X.TXT')))
    # the same rule with NO second entry (VMS, CORPUS 69: the entry is an alias, its file is in no directory afterwards): PURGE and the version limit take the entry out and leave the file allocated
    img = state(('COPY %s [D]P.TXT' % src,), link=False)
    free0b = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    rc, out = ods2(img, 'PURGE [D]P.TXT')
    check('%s: PURGE P.TXT in that state (no LINK) takes P.TXT;1 out as an entry only; its file stays allocated (a lost file for the checker), nothing is freed' % kind,
          (['P.TXT;2'], True, True), (listing(img, '[D]'), int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1)) == free0b, any('lost file' in e for e in checker_errors(img))))
    shutil.copyfile(base, img)
    assert ods2(img, 'SET FILE/VERSION_LIMIT=1 [D]P.TXT')[0] == 0
    run(['env', 'ODS2_FAULT_AFTER_WRITES=1', './ods2_fault', img, 'RENAME [D]P.TXT [D]N.TXT'])
    free0c = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    rc, out = ods2(img, 'COPY %s [D]P.TXT' % src)
    free1c = int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))
    check('%s: COPY over the version limit in that state (no LINK) takes P.TXT;1 out as an entry only; the old file stays allocated (the new copy took its blocks, none came back)' % kind,
          (['P.TXT;2'], True), (listing(img, '[D]'), any('lost file' in e for e in checker_errors(img))))
    # the single-entry state: the checker finds no error after the alias deletes
    img = state()
    ods2(img, 'DELETE [D]X.TXT')
    check('%s: the checker finds no error after DELETE X.TXT in that state' % kind, [], [e for e in checker_errors(img) if 'lost file' not in e or not hl])


def free_blocks(img):
    return int(re.search(r'Free blocks:\s+(\d+)', ods2(img, 'SHOW VOLUME')[1]).group(1))


def backlink_num(h):
    return struct.unpack_from('<H', h, 66)[0]


def move_cases(work, level, hl):
    """RENAME step 2 (review 67 / orch 21:47): a file into ANOTHER directory (CORPUS 65.1 b1 b2, 65.2, 65.5, 66.1, 66.2 k2, 68.3): the header's back link changes (and the name if it changes), revision +1, the
    date; the directories' own headers are not touched; the other versions, the other entries, the bystanders of both directories stay; an alias entry moves with its header untouched."""
    kind = 'ODS-%d%s' % (level, ' HARDLINKS' if hl else '')
    img = os.path.join(work, 'mv_%d%s.img' % (level, 'h' if hl else ''))
    assert run([INIT, img, '16384', '--structure=%d' % level, '--label=MOVE'] + (['--hardlinks'] if hl else []))[0] == 0
    n = [0]

    def put(text):
        n[0] += 1
        p = os.path.join(work, 'mv%d.txt' % n[0])
        open(p, 'w').write(text + '\n')
        return p
    cmds = ['CREATE/DIRECTORY [X]', 'CREATE/DIRECTORY [Y]', 'CREATE/DIRECTORY [Z]', 'CREATE/DIRECTORY [Y.SUBD]']
    files = [('[X]C.TXT', 'content of C'), ('[X]C2.TXT', 'content of C2'), ('[X]M.TXT', 'content of M'), ('[X]M2.TXT', 'content of M2'), ('[X]N.TXT', 'content of N'), ('[X]V.TXT', 'content of V'),
             ('[X]P.TXT', 'content of P'), ('[X]Q.TXT', 'content of Q'), ('[X]R.TXT', 'content of R'), ('[X]RS.TXT', 'content of RS'),
             ('[X]MF.TXT', 'content of MF'), ('[X]BY1.TXT', 'bystander 1'), ('[X]BY2.TXT', 'bystander 2'), ('[Y]BY3.TXT', 'bystander 3'), ('[Y]BY4.TXT', 'bystander 4'), ('[Z]BY5.TXT', 'bystander 5'),
             ('[Y]T.TXT', 'Y T1'), ('[Y]T.TXT', 'Y T2'), ('[Y]T.TXT', 'Y T3'), ('[Y]L.TXT', 'Y L1'), ('[Y]L.TXT', 'Y L2'), ('[Y]L.TXT', 'Y L3')]
    for i, (dst, text) in enumerate(files):
        cmds.append('COPY %s %s' % (put(text), dst))
    cmds += ['SET FILE/VERSION_LIMIT=3 [Y]L.TXT;3', 'LINK [Z]AL.TXT [X]P.TXT', 'LINK [X]QAL.TXT [X]Q.TXT', 'LINK [Z]ALR.TXT [X]R.TXT', 'LINK [Y]RS.TXT [X]RS.TXT']
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
    # the header of R gets back link (0,0,0): no primary entry (what VMS's SET FILE/REMOVE leaves)
    v = O.Vol(img)
    with open(img, 'r+b') as f:
        lbn = E.D.lbn(v, E.D.fnum(img, '[X]R.TXT'))
        f.seek(lbn * 512)
        hb = bytearray(f.read(512))
        hb[66:72] = bytes(6)
        struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
        f.seek(lbn * 512)
        f.write(bytes(hb))
    time.sleep(1.1)
    base = os.path.join(work, 'mv_base_%d%s.img' % (level, 'h' if hl else ''))
    shutil.copyfile(img, base)
    xnum, ynum, znum = (E.D.fnum(base, '[000000]%s.DIR' % d) for d in ('X', 'Y', 'Z'))
    xdir0, ydir0 = hdr(base, '[000000]X.DIR;1'), hdr(base, '[000000]Y.DIR;1')

    def fresh_img(tag):
        shutil.copyfile(base, img)
        return img

    def moved(tag, cmd, old, new, new_name_expect, rev, expect_bl, header_same=False):
        """one successful move: the entry goes from the directory of `old` to that of `new`; the header as VMS makes it"""
        im = fresh_img(tag)
        before = hdr(im, old)
        lx, ly, lz = listing(im, '[X]'), listing(im, '[Y]'), listing(im, '[Z]')
        by = {s: hdr(im, s) for s in ('[X]BY1.TXT;1', '[X]BY2.TXT;1', '[Y]BY3.TXT;1', '[Y]BY4.TXT;1', '[Z]BY5.TXT;1')}
        rc, out = ods2(im, cmd)
        check('%s: %s succeeds' % (tag, cmd), (0, True), (rc, '%ODS2-E' not in out and '%RENAME-E' not in out and 'ILLCMD' not in out))
        after = hdr(im, new)
        if after is None:
            check('%s: the file is found under %s' % (tag, new), True, False)
            return im, out
        fb, fa = fields(before, level), fields(after, level)
        if header_same:
            check('%s: an alias entry moved: the header is byte for byte the same (CORPUS 66.2 k2)' % tag, before, after)
        else:
            check('%s: the stored name is %s' % (tag, new_name_expect), new_name_expect, fa['name'])
            check('%s: revision number +%d' % (tag, rev), fb['revision'] + rev, fa['revision'])
            check('%s: the back link is now the directory of the new entry (%s)' % (tag, expect_bl), expect_bl, backlink_num(after))
            check('%s: no byte outside the back link, the name, the revision, the date field and the checksum changed' % tag, [], other_bytes_same(before, after, level, True, ()))
            check('%s: the header checksum is right' % tag, True, cksum_ok(after))
            if level == 2:
                check('%s: ODS-2: the revision date is set to the time of the RENAME' % tag, (True, True), (fa['revdate'] > fb['revdate'], abs(fa['revdate'] - vms_now()) < 864000000000))
            else:
                check('%s: ODS-5: the last attribute change date is set, the revision date is not changed' % tag, (True, True, fb['revdate']), (fa['attdate'] > fb['attdate'], abs(fa['attdate'] - vms_now()) < 864000000000, fa['revdate']))
        if hl:
            check('%s: the link count is as it was (%d)' % (tag, fb['linkcount']), fb['linkcount'], fa['linkcount'])
        check('%s: the directories own headers are not touched (CORPUS 65.1)' % tag, (xdir0, ydir0), (hdr(im, '[000000]X.DIR;1'), hdr(im, '[000000]Y.DIR;1')))
        check('%s: the bystanders of both directories keep their headers' % tag, by, {s: hdr(im, s) for s in by})
        check('%s: the checker finds nothing wrong' % tag, [], [e for e in checker_errors(im) if tag == 'ma' or 'back link 0 is not a directory' not in e])      # (the base keeps R.TXT with back link 0 until it is moved)
        return im, out

    # b1: the same name into another directory (the target with no file name)
    im, out = moved('mb1', 'RENAME [X]C.TXT;1 [Y]', '[X]C.TXT;1', '[Y]C.TXT;1', 'C.TXT;1', 1, ynum)
    lx, ly = listing(im, '[X]'), listing(im, '[Y]')
    check('mb1 %s: [X] loses C.TXT;1, [Y] gains it, nothing else moves' % kind, (True, True, True),
          ('C.TXT;1' not in lx and len(lx) == len(listing(base, '[X]')) - 1, 'C.TXT;1' in ly and len(ly) == len(listing(base, '[Y]')) + 1, set(lx) | {'C.TXT;1'} == set(listing(base, '[X]'))))
    check('mb1 %s: the data reads back under the new directory' % kind, 'content of C', ods2(im, 'TYPE [Y]C.TXT')[1].strip().splitlines()[-1])
    # the file now belongs to [Y]: its DELETE deletes the file (the entry is in the back link directory and names the header's name)
    f0 = free_blocks(im)
    rc, out = ods2(im, 'DELETE [Y]C.TXT')
    check('mb1 %s: DELETE [Y]C.TXT deletes the file (blocks come back, no error from the checker)' % kind, (True, []), (free_blocks(im) > f0, [e for e in checker_errors(im) if 'back link 0 is not' not in e]))
    # b2: moved and renamed at once
    moved('mb2', 'RENAME [X]C2.TXT [Y]C3.TXT', '[X]C2.TXT;1', '[Y]C3.TXT;1', 'C3.TXT;1', 1, ynum)
    # the target name exists: the next version in the TARGET directory (CORPUS 65.2)
    im, out = moved('mb3', 'RENAME [X]M.TXT [Y]T.TXT', '[X]M.TXT;1', '[Y]T.TXT;4', 'T.TXT;4', 1, ynum)
    check('mb3 %s: [Y]T.TXT keeps its three versions and gains ;4' % kind, ['T.TXT;1', 'T.TXT;2', 'T.TXT;3', 'T.TXT;4'], [x for x in listing(im, '[Y]') if x.startswith('T.TXT')])
    # an explicit version that exists in the target: refused, the volume unchanged (VMS bumps the source header: ods2v2 does not, an intended difference)
    im = fresh_img('mb4')
    h0 = sha(im)
    rc, out = ods2(im, 'RENAME [X]M2.TXT [Y]T.TXT;2')
    check('mb4 %s: an explicit version that exists in the target directory is refused (DUPFILENAME) and the image is unchanged' % kind, (True, h0), ('DUPFILENAME' in out, sha(im)))
    moved('mb5', 'RENAME [X]N.TXT [Y]N.TXT;7', '[X]N.TXT;1', '[Y]N.TXT;7', 'N.TXT;7', 1, ynum)
    # the version limit of the target name (CORPUS 66.1: the lowest goes)
    im, out = moved('mb6', 'RENAME [X]V.TXT [Y]L.TXT', '[X]V.TXT;1', '[Y]L.TXT;4', 'L.TXT;4', 1, ynum)
    check('mb6 %s: the target name limit of 3 removes its lowest version: [Y]L.TXT is ;2 ;3 ;4' % kind, ['L.TXT;2', 'L.TXT;3', 'L.TXT;4'], [x for x in listing(im, '[Y]') if x.startswith('L.TXT')])
    ims = im
    assert ods2(ims, 'COPY %s [Y]L.TXT' % put('one more L'))[0] == 0
    check('mb6 %s: the moved file carries the target name\'s limit (CORPUS 70): one more version of [Y]L.TXT still leaves three' % kind, ['L.TXT;3', 'L.TXT;4', 'L.TXT;5'], [x for x in listing(ims, '[Y]') if x.startswith('L.TXT')])
    # an alias entry (here in the primary's own directory): the entry moves, the header is untouched (CORPUS 66.2 k2)
    im, out = moved('mk2', 'RENAME [X]QAL.TXT [Y]', '[X]Q.TXT;1', '[Y]QAL.TXT;1', None, 0, None, True)
    check('mk2 %s: the alias is in [Y] now and the primary [X]Q.TXT is as it was' % kind, ('QAL.TXT;1' in listing(im, '[Y]'), 'Q.TXT;1' in listing(im, '[X]'), 'QAL.TXT;1' in listing(im, '[X]')),
          (True, True, False))
    im2 = im
    f0 = free_blocks(im2)
    ods2(im2, 'DELETE [Y]QAL.TXT')
    check('mk2 %s: DELETE of the moved alias removes the entry only (the back link directory is [X]): the file stays under [X]Q.TXT, no blocks freed' % kind, (['Q.TXT;1'], True, f0),
          ([x for x in listing(im2, '[X]') if x.startswith('Q.')], 'content of Q' in ods2(im2, 'TYPE [X]Q.TXT')[1], free_blocks(im2)))
    # an alias in a third directory moved and renamed: the header is untouched, the primary stays
    moved('mk2b', 'RENAME [Z]AL.TXT [Y]AL2.TXT', '[X]P.TXT;1', '[Y]AL2.TXT;1', None, 0, None, True)
    # the primary entry that has an alias elsewhere: the header follows the entry, the alias entry is not touched (CORPUS 65.5)
    im, out = moved('mp', 'RENAME [X]P.TXT [Y]', '[X]P.TXT;1', '[Y]P.TXT;1', 'P.TXT;1', 1, ynum)
    check('mp %s: the alias [Z]AL.TXT is as it was and now points at a file whose back link is [Y]; DELETE of it removes the entry only' % kind, (True, True), ('AL.TXT;1' in listing(im, '[Z]'), 'content of P' in ods2(im, 'TYPE [Z]AL.TXT')[1]))
    ods2(im, 'DELETE [Z]AL.TXT')
    check('mp %s: ... the file stays under [Y]P.TXT' % kind, ('content of P', []), (ods2(im, 'TYPE [Y]P.TXT')[1].strip().splitlines()[-1], [e for e in checker_errors(im) if 'back link 0 is not' not in e]))
    # a header with no primary entry (back link 0): the entry moved is adopted (name, back link set), as the same-directory rename does (CORPUS 66.2)
    im, out = moved('ma', 'RENAME [Z]ALR.TXT [Y]ALR2.TXT', '[X]R.TXT;1', '[Y]ALR2.TXT;1', 'ALR2.TXT;1', 1, ynum)
    # the target entry already names the file (a LINK, or a move that was stopped after its new entry): finished, no new version
    im = fresh_img('mresume')
    rc, out = ods2(im, 'RENAME [X]RS.TXT [Y]RS.TXT')
    h = hdr(im, '[Y]RS.TXT;1')
    check('mresume %s: the target entry [Y]RS.TXT already names the file: the move finishes it (one entry [Y]RS.TXT;1, [X] has none, the back link is [Y], the checker is clean)' % kind,
          (0, ['RS.TXT;1'], [], ynum, []), (rc, [x for x in listing(im, '[Y]') if x.startswith('RS.')], [x for x in listing(im, '[X]') if x.startswith('RS.')], backlink_num(h) if h is not None else None, [e for e in checker_errors(im) if 'back link 0 is not' not in e]))
    # errors: the target directory does not exist, another device (VMS: DNF, NOTSAMEDEV; nothing changes)
    im = fresh_img('mdnf')
    h0 = sha(im)
    rc, out = ods2(im, 'RENAME [X]C.TXT [NOSUCH]')
    check('mdnf %s: the target directory does not exist: DNF, the image is unchanged' % kind, (True, h0), ('directory not found' in out and '%RENAME-E-OPENOUT' in out, sha(im)))
    rc, out = ods2(im, 'RENAME [X]C.TXT OTHER:[Y]')
    check('mdev %s: another device: NOTSAMEDEV, the image is unchanged' % kind, (True, h0), ('NOTSAMEDEV' in out, sha(im)))
    rc, out = ods2(im, 'RENAME DKA0:[X]C.TXT DKB0:[Y]')
    check('mdev %s: two different devices typed: NOTSAMEDEV, the image is unchanged' % kind, (True, h0), ('NOTSAMEDEV' in out, sha(im)))
    rc, out = ods2(im, 'RENAME DKA0:[X]C.TXT DKA0:[Y]')
    check('mdev %s: the same device typed twice is fine' % kind, (0, True), (rc, 'C.TXT;1' in listing(im, '[Y]')))
    im = fresh_img('mdev2')
    rc, out = ods2(im, 'RENAME [X]C.TXT MOVE:[Y]')
    check('mdev %s: the volume own label as the device is fine' % kind, (0, True), (rc, 'C.TXT;1' in listing(im, '[Y]')))
    # a target name held by a directory, and a reserved file: refused, unchanged (the same two refusals as in one directory)
    im = fresh_img('mdir')
    h0 = sha(im)
    rc, out = ods2(im, 'RENAME [X]C.TXT [Y]SUBD.DIR')
    check('mdir %s: the target name is held by a directory: refused, the image is unchanged' % kind, (True, h0), ('RENAME' in out and 'ILLCMD' not in out and '%ODS2-E' in out, sha(im)))
    rc, out = ods2(im, 'RENAME [000000]BITMAP.SYS [X]')
    check('mres %s: a reserved file is not moved: refused, the image is unchanged' % kind, (True, h0), ('reserved' in out.lower() or 'NOPRIV' in out, sha(im)))
    # into the master directory and out of it
    im, out = moved('mmfd', 'RENAME [X]MF.TXT [000000]', '[X]MF.TXT;1', '[000000]MF.TXT;1', 'MF.TXT;1', 1, 4)
    # a target directory that must split its block: a name in the front, the middle, the end (the entries stay in order, the checker is clean)
    for where, name in (('front', 'AAAAAA.TXT'), ('middle', 'M000020.TXT'), ('end', 'ZZZZZZ.TXT')):
        im = fresh_img('msplit')
        assert ods2(im, 'CREATE/DIRECTORY [T]')[0] == 0
        for i in range(45):
            assert ods2(im, 'COPY %s [T]M%06d_LONGNAME_FILL_BLOCKS.TXT' % (put('fill'), i * 2))[0] == 0
        before_t = listing(im, '[T]')
        tn = name if where != 'middle' else 'M000021_LONGNAME_FILL_BLOCKS.TXT'
        assert ods2(im, 'COPY %s [X]%s' % (put('moved ' + where), tn))[0] == 0
        rc, out = ods2(im, 'RENAME [X]%s [T]' % tn)
        lt = listing(im, '[T]')
        check('msplit %s %s: the move into a full directory (%d names): all names listed in order, the new one among them, the checker clean' % (kind, where, len(before_t)),
              (0, sorted(before_t + ['%s;1' % tn]), []), (rc, lt, [e for e in checker_errors(im) if 'back link 0 is not' not in e]))
        check('msplit %s %s: the data reads back' % (kind, where), 'moved ' + where, ods2(im, 'TYPE [T]%s' % tn)[1].strip().splitlines()[-1])
    return None


def review69_cases(work, level, hl):
    """review 69 (FIX FIRST), tests first: F1 the version limit's prune must never remove another entry of the file being renamed (step 1 had it too); S1 an over-long device name is refused; S2 a device with no
    directory is parsed off, not made part of the name; S3 a target with only a name, only a type or only a version takes the missing parts from the source (DCL)"""
    kind = 'ODS-%d%s' % (level, ' HARDLINKS' if hl else '')
    img = os.path.join(work, 'r69_%d%s.img' % (level, 'h' if hl else ''))
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=R69'] + (['--hardlinks'] if hl else []))[0] == 0
    src = os.path.join(work, 'r69.txt')
    open(src, 'w').write('content of the file\n')
    for c in ('CREATE/DIRECTORY [X]', 'CREATE/DIRECTORY [Y]', 'COPY %s [X]A.TXT' % src, 'COPY %s [X]A2.TXT' % src, 'COPY %s [X]B' % src):
        assert ods2(img, c)[0] == 0, c
    time.sleep(1.1)
    base = os.path.join(work, 'r69_base_%d%s.img' % (level, 'h' if hl else ''))
    shutil.copyfile(base if False else img, base)

    def fresh():
        shutil.copyfile(base, img)
        return img

    def reads(im, spec):
        return 'content of the file' in ods2(im, 'TYPE ' + spec)[1]

    # F1 (a): the second stopped state of a move (two entries), the target name's limit set to 1, then the rename onto the next version
    for where, setup in (('across (the second stopped state of a move)', 'stop'), ('across, an alias made with LINK', 'linkY'), ('in one directory, an alias made with LINK', 'linkX')):
        im = fresh()
        if setup == 'stop':
            run(['env', 'ODS2_FAULT_AFTER_WRITES=2', './ods2_fault', im, 'RENAME [X]A.TXT [Y]M.TXT'])
            tgt, name_old = '[Y]M.TXT', '[X]A.TXT'
            cmd = 'RENAME [X]A.TXT [Y]M.TXT;2'
        elif setup == 'linkY':
            assert ods2(im, 'LINK [Y]C.TXT [X]A.TXT')[0] == 0
            tgt, name_old = '[Y]C.TXT', '[X]A.TXT'
            cmd = 'RENAME [X]A.TXT [Y]C.TXT;5'
        else:
            assert ods2(im, 'LINK [X]C.TXT [X]A.TXT')[0] == 0
            tgt, name_old = '[X]C.TXT', '[X]A.TXT'
            cmd = 'RENAME [X]A.TXT C.TXT;5'
        assert ods2(im, 'SET FILE/VERSION_LIMIT=1 %s' % tgt)[0] == 0
        h0 = sha(im)
        rc, out = ods2(im, cmd)
        if hl:
            # a HARDLINKS volume with a link count of 2 or more: the prune only takes the entry out and lowers the count (VMS too, CORPUS 70): the file stays and the rename goes on
            newname = tgt + ';' + cmd.rsplit(';', 1)[1]
            check('%s F1 %s: %s goes through (the prune of the other entry of the file lowers the link count only), the file reads under %s, the checker finds nothing wrong' % (kind, where, cmd, newname),
                  (0, True, True, []), (rc, '%ODS2-I-RENAMED' in out and 'the version limit' in out, reads(im, newname), [e for e in checker_errors(im)]))
            continue
        check('%s F1 %s: %s is refused (the version the limit would remove is another entry of this same file), the image is unchanged' % (kind, where, cmd),
              (True, True, h0), ('another entry of this same file' in out and '%ODS2-I-RENAMED' not in out, '%ODS2-E' in out, sha(im)))
        check('%s F1 %s: the file still reads under its names' % (kind, where), True, reads(im, name_old) and reads(im, tgt + ';1'))
    # the same limit with ANOTHER file's version in the way is still pruned (the earlier tests: mb6 and the table's k8)
    # S1: a device name that does not fit the buffer is refused, not taken as this volume (empty equals empty)
    im = fresh()
    h0 = sha(im)
    long_dev = 'A' * 70
    for cmd in ('RENAME [X]A.TXT %s:[Y]' % long_dev, 'RENAME %s:[X]A.TXT %s:[Y]' % (long_dev, 'B' * 71), 'RENAME %s:[X]A.TXT %s:[Y]' % (long_dev, long_dev)):
        rc, out = ods2(im, cmd)
        check('%s S1: %s... is refused (NOTSAMEDEV), the image is unchanged' % (kind, cmd[:30]), (True, h0), ('NOTSAMEDEV' in out, sha(im)))
    # S2: a device with no directory is parsed off
    im = fresh()
    rc, out = ods2(im, 'RENAME [X]A.TXT R69:N.TXT')
    check('%s S2: RENAME [X]A.TXT R69:N.TXT makes [X]N.TXT;1 (the device is this volume\'s label, parsed off), not a name with a colon' % kind, (0, ['A2.TXT;1', 'B.;1', 'N.TXT;1']), (rc, listing(im, '[X]')))
    im = fresh()
    h0 = sha(im)
    rc, out = ods2(im, 'RENAME [X]A.TXT R69:')
    check('%s S2: RENAME [X]A.TXT R69: (a device and nothing else) is the same name: nothing changes' % kind, (0, True, h0), (rc, 'already named' in out, sha(im)))
    for cmd in ('RENAME [X]A.TXT A:B.TXT', 'RENAME [X]A.TXT R69::[Y]', 'RENAME [X]A.TXT R69:R69:[Y]'):
        rc, out = ods2(im, cmd)
        check('%s S2: %s is refused (NOTSAMEDEV), the image is unchanged' % (kind, cmd), (True, h0), ('NOTSAMEDEV' in out, sha(im)))
    # S3: the missing parts of the target come from the source: [Y]N is N.TXT, [Y].DAT is A.DAT, [Y];7 is A.TXT;7 (the type of B, which has none, stays none)
    for cmd, want in (('RENAME [X]A.TXT [Y]N', 'N.TXT;1'), ('RENAME [X]A.TXT [Y].DAT', 'A.DAT;1'), ('RENAME [X]A.TXT [Y];7', 'A.TXT;7'), ('RENAME [X]A.TXT [X]N', 'N.TXT;1'), ('RENAME [X]A.TXT [X].DAT', 'A.DAT;1'),
                      ('RENAME [X]A.TXT [Y]N.', 'N.;1'), ('RENAME [X]A.TXT [Y]N.DAT', 'N.DAT;1'), ('RENAME [X]B [Y]N', 'N.;1'), ('RENAME [X]B [Y].DAT', 'B.DAT;1')):
        im = fresh()
        rc, out = ods2(im, cmd)
        d = '[Y]' if '[Y]' in cmd else '[X]'
        got = [x for x in listing(im, d) if x in (want,)]
        check('%s S3: %s makes %s (DCL fills the missing part from the input)' % (kind, cmd, want), (0, [want], []), (rc, got, checker_errors(im)))


def review69b_cases(work, level, hl):
    """the review 69 recheck (R1 to R3 and the notes), tests first: R1 an over-long specification is refused, not cut; R2 (ODS-5) the part of the new name filled in from the source is the STORED spelling,
    not the typed one; R3 text between the device and the directory is refused; the F1 refusal says what happens to a pure alias rename; the HARDLINKS exemption needs a level-5 header"""
    kind = 'ODS-%d%s' % (level, ' HARDLINKS' if hl else '')
    img = os.path.join(work, 'r69b_%d%s.img' % (level, 'h' if hl else ''))
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=R69B'] + (['--hardlinks'] if hl else []))[0] == 0
    src = os.path.join(work, 'r69b.txt')
    open(src, 'w').write('content of the file\n')
    cmds = ['CREATE/DIRECTORY [X]', 'CREATE/DIRECTORY [Y]', 'CREATE/DIRECTORY [Y.KID]', 'COPY %s [X]B.TXT' % src, 'COPY %s [X]B.TXT' % src, 'COPY %s [X]A.TXT' % src]
    if level == 5:
        cmds += ['COPY %s [X]lower.txt' % src, 'COPY %s [X]UP.TXT' % src, 'COPY %s [X]NOTYPE.' % src, 'COPY %s [X]a^.b.c' % src]
    for c in cmds:
        assert ods2(img, c)[0] == 0, c
    time.sleep(1.1)
    base = os.path.join(work, 'r69b_base_%d%s.img' % (level, 'h' if hl else ''))
    shutil.copyfile(img, base)

    def fresh():
        shutil.copyfile(base, img)
        return img

    # R1: a specification of ODS2_PATH_MAX characters or more is refused before it is split: the 2048-character source ending in B.TXT;12 renamed B.TXT;1
    im = fresh()
    h0 = sha(im)
    for cmd in ('RENAME [X]%sB.TXT;12 [Y]' % ('a' * 2040), 'RENAME [X]B.TXT;1 [Y]%sB.TXT' % ('a' * 2040), 'RENAME [X]%sB.TXT;12 MOV:[Y]' % ('a' * 2040), 'RENAME MOV:[X]B.TXT;1 %s:[Y]' % ('a' * 2045)):
        rc, out = ods2(im, cmd)
        check('%s R1: %s... is refused, the image is unchanged' % (kind, cmd[:34]), (True, h0), ('too long' in out and '%ODS2-I-RENAMED' not in out, sha(im)))
    # R3: text between the device and the directory is not dropped
    for cmd in ('RENAME [X]B.TXT;1 MOV:junk[Y]N.TXT', 'RENAME [X]B.TXT;1 R69B:junk[Y]', 'RENAME R69B:junk[X]B.TXT;1 [Y]'):
        rc, out = ods2(im, cmd)
        check('%s R3: %s is refused, the image is unchanged' % (kind, cmd), (True, h0), ('%ODS2-I-RENAMED' not in out and '%ODS2-E' in out + '%RENAME-E', sha(im)))
    if level == 5:
        # R2: the missing parts of the new name come from the STORED name (lower.txt, UP.TXT), not from what was typed
        for cmd, d, want in (('RENAME [X]LOWER.TXT [Y]n', '[Y]', 'n.txt;1'), ('RENAME [X]LOWER.TXT [Y].dat', '[Y]', 'lower.dat;1'), ('RENAME [X]lower.TXT [Y]', '[Y]', 'lower.txt;1'),
                             ('RENAME [X]up.txt [Y]n', '[Y]', 'n.TXT;1'), ('RENAME [X]up.txt [Y].dat', '[Y]', 'UP.dat;1'), ('RENAME [X]up.txt [Y]', '[Y]', 'UP.TXT;1'), ('RENAME [X]up.txt [Y.KID]', '[Y.KID]', 'UP.TXT;1')):
            im2 = fresh()
            rc, out = ods2(im2, cmd)
            check('%s R2: %s makes %s (the stored spelling fills in what is missing)' % (kind, cmd, want), (0, [want]), (rc, [x for x in listing(im2, d) if x == want]))
        # review 70 F1: an escaped dot and no type: the source's type is added to the name WITHOUT the literal's no-type dot (c5df84a stored n^.m.TXT; c5d458e stored n^.m^..TXT)
        for cmd, want in (('RENAME [X]A.TXT [Y]n^.m', 'n^.m.TXT;1'), ('RENAME [X]A.TXT [Y]^.m', '^.m.TXT;1'), ('RENAME [X]NOTYPE. [Y]n^.m', 'n^.m.;1'), ('RENAME [X]a^.b.c [Y]n^.m', 'n^.m.c;1'),
                          ('RENAME [X]A.TXT [Y]n^.m.', 'n^.m.;1'), ('RENAME [X]A.TXT [Y]n^.m.dat', 'n^.m.dat;1'), ('RENAME [X]a^.b.c [Y].DAT', 'a^.b.DAT;1'), ('RENAME [X]NOTYPE. [Y].DAT', 'NOTYPE.DAT;1')):
            im2 = fresh()
            rc, out = ods2(im2, cmd)
            got = [x for x in listing(im2, '[Y]')]
            check('%s review 70 F1: %s makes %s' % (kind, cmd, want), (0, [want]), (rc, [x for x in got if x == want]))
        im2 = fresh()
        h1 = sha(im2)
        rc, out = ods2(im2, 'RENAME [X]up.txt R69B:')
        check('%s R2: RENAME [X]up.txt R69B: leaves the stored UP.TXT alone: nothing is written' % kind, (0, True, h1), (rc, 'already named' in out, sha(im2)))
    # review 70 recheck (G1, G2): the broad rule: a pruned entry that is another entry of the same file is refused WHATEVER it is (an alias too: a header, a link count or a name may not tell the
    # truth), except on a HARDLINKS volume with a level-5 header and a link count of 2 or more
    if not hl:
        im = fresh()
        for c in ('LINK [Y]D.TXT [X]A.TXT', 'LINK [Y]C.TXT [X]A.TXT', 'SET FILE/VERSION_LIMIT=1 [Y]C.TXT'):
            assert ods2(im, c)[0] == 0, c
        h4 = sha(im)
        rc, out = ods2(im, 'RENAME [Y]D.TXT [Y]C.TXT;5')
        check('%s review 70 recheck: RENAME [Y]D.TXT [Y]C.TXT;5 with the pruned [Y]C.TXT;1 an alias is refused too (the broad rule), the image is unchanged, the file reads' % kind,
              (True, h4, True), ('another entry of this same file' in out and '%ODS2-I-RENAMED' not in out, sha(im), 'content of the file' in ods2(im, 'TYPE [X]A.TXT;1')[1]))
    # the F1 refusal on a pure alias rename: only an entry would move, the PRIMARY entry of the target name is what the prune would take out
    im = fresh()
    assert ods2(im, 'LINK [X]AL.TXT [X]B.TXT;2') [0] == 0
    assert ods2(im, 'COPY %s [Y]C.TXT' % src)[0] == 0
    assert ods2(im, 'LINK [X]ALC.TXT [Y]C.TXT')[0] == 0
    assert ods2(im, 'SET FILE/VERSION_LIMIT=1 [Y]C.TXT')[0] == 0
    h2 = sha(im)
    rc, out = ods2(im, 'RENAME [X]ALC.TXT [Y]C.TXT;5')
    if hl:
        check('%s F1 alias: an alias of a file with a link count of 2: the prune only lowers the count: the rename goes on' % kind, (0, True), (rc, '%ODS2-I-RENAMED' in out))
    else:
        check('%s F1 alias: a refused PURE ALIAS rename says the prune can delete the file and that it is refused to keep it safe' % kind,
              (True, True, h2), ('another entry of this same file' in out and 'keep the file safe' in out, '%ODS2-I-RENAMED' not in out, sha(im)))

    def patched(tag, patch, expect_refused_all=True):
        """a HARDLINKS-volume file whose header does not tell the truth (review 70 recheck G1: the link count 1 or 0 with two entries; G2: a header that is not level 5, whose name cannot be read): the
        rename onto the name whose limit prunes the file's other entry is refused and the image is unchanged"""
        im2 = fresh()
        for c in ('COPY %s [Y]C.TXT' % src, 'LINK [X]ALC.TXT [Y]C.TXT', 'SET FILE/VERSION_LIMIT=1 [Y]C.TXT'):
            assert ods2(im2, c)[0] == 0, c
        v = O.Vol(im2)
        with open(im2, 'r+b') as f:
            lbn = E.D.lbn(v, E.D.fnum(im2, '[Y]C.TXT'))
            f.seek(lbn * 512)
            hb = bytearray(f.read(512))
            patch(hb)
            struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
            f.seek(lbn * 512)
            f.write(bytes(hb))
        h5 = sha(im2)
        rc2, out2 = ods2(im2, 'RENAME [X]ALC.TXT [Y]C.TXT;5')
        check('%s review 70 recheck %s: refused (the file may be lost otherwise), the image is unchanged' % (kind, tag), (True, h5), ('another entry of this same file' in out2 and '%ODS2-I-RENAMED' not in out2, sha(im2)))
        im3 = fresh()
        return im3
    if hl:
        # T1 (review 70 second recheck): the direction that lost the file under the narrowed rule: the PRIMARY entry is renamed onto the name of its own alias (LINK [Y]C.TXT [X]A.TXT, the limit 1 on
        # [Y]C.TXT) while the header's link count is 1 or 0 with two entries
        for cnt in (1, 0):
            im4 = fresh()
            for c in ('LINK [Y]C.TXT [X]A.TXT', 'SET FILE/VERSION_LIMIT=1 [Y]C.TXT'):
                assert ods2(im4, c)[0] == 0, c
            v4 = O.Vol(im4)
            with open(im4, 'r+b') as f:
                lbn = E.D.lbn(v4, E.D.fnum(im4, '[X]A.TXT'))
                f.seek(lbn * 512)
                hb = bytearray(f.read(512))
                struct.pack_into('<H', hb, 74, cnt)
                struct.pack_into('<H', hb, 510, sum(struct.unpack_from('<255H', hb, 0)) & 0xffff)
                f.seek(lbn * 512)
                f.write(bytes(hb))
            h6 = sha(im4)
            rc4, out4 = ods2(im4, 'RENAME [X]A.TXT [Y]C.TXT;5')
            check('%s T1 the primary [X]A.TXT onto its alias [Y]C.TXT;5 (limit 1), link count %d with two entries: refused, the image is unchanged, the file reads' % (kind, cnt), (True, h6, True),
                  ('another entry of this same file' in out4 and '%ODS2-I-RENAMED' not in out4, sha(im4), 'content of the file' in ods2(im4, 'TYPE [X]A.TXT;1')[1]))
    if level == 5:
        patched('G2 a header that is not level 5 (its name cannot be read)', lambda hb: struct.pack_into('<H', hb, 6, 0x0201))
        if not hl:      # (a level-5 header with a link count of 2 on a HARDLINKS volume is the exemption: the prune only lowers the count)
            patched('G2 a header with the name area damaged (name length 0)', lambda hb: hb.__setitem__(hb[0] * 2 + 1, 0))
    if hl:
        patched('G1 link count 1 with two entries', lambda hb: struct.pack_into('<H', hb, 74, 1))
        patched('G1 link count 0 with two entries', lambda hb: struct.pack_into('<H', hb, 74, 0))


def r1_unusable_number(work, level):
    """review 67 recheck R1: a name with a version number that is not usable (0) is not pruned: E.TXT stored ;2 ;1 ;0 with the limit 2: COPY and RENAME delete nothing (version 0 was taken for \"nothing removed\")"""
    kind = 'ODS-%d' % level
    img = os.path.join(work, 'r1_%d.img' % level)
    assert run([INIT, img, '8192', '--structure=%d' % level, '--label=R1'])[0] == 0
    src = os.path.join(work, 'r1.txt')
    open(src, 'w').write('x\n')
    cmds = ['CREATE/DIRECTORY [D]'] + ['COPY %s [D]E.TXT' % src] * 3 + ['COPY %s [D]M.TXT' % src, 'SET FILE/VERSION_LIMIT=2 [D]E.TXT']
    for c in cmds:
        assert ods2(img, c)[0] == 0
    dn = E.D.fnum(img, '[000000]D.DIR')
    for idx, val in ((0, 2), (1, 1), (2, 0)):
        rc, out = run(['python3', '-I', 'tests/swap_versions.py', img, str(dn), 'E.TXT', 'setver', str(idx), str(val)])
        assert rc == 0, out
    base = os.path.join(work, 'r1_base.img')
    shutil.copyfile(img, base)
    before = sorted(x for x in listing(img, '[D]') if x.startswith('E.TXT'))
    for cmd in ('COPY %s [D]E.TXT' % src, 'RENAME [D]M.TXT [D]E.TXT'):
        shutil.copyfile(base, img)
        rc, out = ods2(img, cmd)
        after = sorted(x for x in listing(img, '[D]') if x.startswith('E.TXT'))
        check('%s: %s on E.TXT stored ;2 ;1 ;0 (limit 2) deletes no version (an unusable number: no pruning)' % (kind, cmd), True, all(x in after for x in before))


def list_alias_before(base):
    return listing(base, '[Y]')


def main():
    keep = '--keep' in sys.argv
    work = tempfile.mkdtemp(prefix='rename_check_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        for kind, level, hl, kname in KINDS:
            print('=== %s' % kname)
            bases, done = {}, {}
            for cid in sorted(T.CASES, key=lambda c: (c[0], int(re.sub(r'\D', '', c) or 0))):
                t = T.CASES[cid]
                if kind not in t['vols']:
                    continue
                alias = (t.get('link'), bool(t.get('noprimary')))
                if alias not in bases:
                    bases[alias] = build(work, level, hl, alias[0], alias[1])
                    time.sleep(1.1)           # the dates have a resolution of a second here: the RENAME must come later than the creation for its date to be seen
                done[cid] = run_case(cid, t, kind, level, hl, kname, bases[alias], work, done.get(t.get('follows')))
        for level in (2, 5):
            full_volume(work, level)
            ext_headers_case(work, level)
            review67_cases(work, level)
            r1_unusable_number(work, level)
            header_first_states(work, level, False)
            g1_aliases(work, level, False)
            move_cases(work, level, False)
            review69_cases(work, level, False)
            review69b_cases(work, level, False)
        header_first_states(work, 5, True)
        g1_aliases(work, 5, True)
        move_cases(work, 5, True)
        review69_cases(work, 5, True)
        review69b_cases(work, 5, True)
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)
    print('rename_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
