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
    return {'name': name, 'nametype': 0, 'namelen': len(name), 'revision': u16(ident + 20), 'revdate': u64(ident + 30), 'attdate': 0, 'linkcount': 0}


def linkcount(img, spec):
    h = hdr(img, spec)
    return fields(h, 5)['linkcount'] if h is not None else None


def may_differ(h, level):
    """the byte ranges of a header a rename may change"""
    ident = h[0] * 2
    if level == 5:
        return [(ident + 1, ident + 2), (ident + 2, ident + 4), (ident + 44, ident + 52), (ident + 76, ident + 76 + 248), (510, 512)]
    return [(ident, ident + 20), (ident + 20, ident + 22), (ident + 30, ident + 38), (510, 512)]


def other_bytes_same(a, b, level):
    ok = [(x, y) for x, y in may_differ(a, level)]
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


def build(work, level, hl, alias):
    """the base volume of one kind: the files of the cases of the table, with the alias entry [Y]<alias> for FILE.TXT (None: none)"""
    img = os.path.join(work, 'base_%d%s_%s.img' % (level, 'h' if hl else '', alias or 'none'))
    a = [INIT, img, '16384', '--structure=%d' % level, '--label=RENAME'] + (['--hardlinks'] if hl else [])
    rc, out = run(a)
    assert rc == 0, out

    def put(name, text):
        p = os.path.join(work, name)
        open(p, 'w').write(text)
        return p
    cmds = ['CREATE/DIRECTORY [X]', 'CREATE/DIRECTORY [X2]', 'CREATE/DIRECTORY [Y]']
    files = [('[X]A.TXT', 'content of A'), ('[X2]M.TXT', 'content of M'), ('[X2]M2.TXT', 'content of M2'),
             ('[X2]T.TXT', 'T version 1'), ('[X2]T.TXT', 'T version 2'), ('[X2]T.TXT', 'T version 3'), ('[X2]FILE.TXT', 'content of FILE'), ('[X2]H3.TXT', 'content of H3')]
    if level == 5:
        files += [('[X2]abc.txt', 'lower case'), ('[X2]sp^_ace.txt', 'a name with a space'), ('[X2]eur^U20ACo.txt', 'a name with a euro sign')]
    for i, (dst, text) in enumerate(files):
        cmds.append('COPY %s %s' % (put('f%d.txt' % i, text + '\n'), dst))
    if alias:
        cmds.append('LINK [Y]%s [X2]FILE.TXT' % alias)
    for c in cmds:
        rc, out = ods2(img, c)
        assert rc == 0 and '%ODS2-E' not in out, (c, out)
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
    if cid == 'h3':
        os.chmod(img, 0o444)
    h0 = sha(img)
    hspec = t.get('file') or t['old']
    before = hdr(img, hspec)
    others = {u: hdr(img, t['dir'] + u) for u in t.get('untouched', []) + t.get('untouched_extra', [])}
    list_before = listing(img, t['dir'])
    rc, out = ods2(img, 'RENAME %s %s' % (t['old'], t['new']))
    if t['status'] == 'refused':
        msgs = t.get('ods2v2_msgs', t.get('msgs', []))
        check('%s: RENAME %s %s is refused with %s' % (tag, t['old'], t['new'], msgs), (True, True), (all(m in out.upper() or m in out for m in msgs), 'ILLCMD' not in out))
        check('%s: the image is byte for byte the same%s' % (tag, ' (VMS bumps the source header: ods2v2 does not, an intended difference)' if t.get('vms_bumps_refused') else ''), h0, sha(img))
        return img
    check('%s: RENAME %s %s succeeds' % (tag, t['old'], t['new']), (0, True), (rc, '%ODS2-E' not in out and '%RENAME-E' not in out and 'ILLCMD' not in out))
    new_spec = t.get('file') or t.get('after') or (t['new'] if ';' in t['new'] else t['new'] + ';1')
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
        check('%s: no byte outside name, revision, the date field and the checksum changed' % tag, [], other_bytes_same(before, after, level))
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
        lb, la = set(list_before), set(listing(img, t['dir']))
        check('%s: the directory loses %s and gains %s' % (tag, t['removed'], t['added']), (sorted(t['removed']), sorted(t['added'])), (sorted(lb - la), sorted(la - lb)))
    check('%s: the checker finds nothing wrong' % tag, [], checker_errors(img))
    if cid == 'g2':
        check('%s: the alias entry stays as it was and reads the same data' % tag, (list_alias_before(base), 'content of FILE'),
              (listing(img, '[Y]'), ods2(img, 'TYPE [Y]%s' % t['alias'])[1].strip().splitlines()[-1]))
    if cid == 'g1':
        check('%s: the data reads back under the alias' % tag, 'content of FILE', ods2(img, 'TYPE %s' % t['new'])[1].strip().splitlines()[-1])
    return img


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
                alias = t.get('alias')
                if alias not in bases:
                    bases[alias] = build(work, level, hl, alias)
                done[cid] = run_case(cid, t, kind, level, hl, kname, bases[alias], work, done.get(t.get('follows')))
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)
    print('rename_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
