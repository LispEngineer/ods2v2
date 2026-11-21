#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""newver_vms_compare.py - COPY to a new version against what VMS itself wrote (PLAN_WRITE_OPS item 1; CORPUS 49 and 49.8).

usage: newver_vms_compare.py LEVEL VMS_IMAGE   (run from the project root; needs ./ods2 and ./ods2_init)

The VMS containers (ods5-vms3, step 0: ~/tmp/ods5-vms3/wo/a5/img/asim5-copy-{2,5}.img, wo/img/phvs2-copy-2.img) hold the directories
[C] (A.TXT;1 to ;4, then ;7 ;8 ;9), [L] (the version limit 3, six creations of B.TXT), [M] (C.TXT with the limit 3, D.TXT with none),
[N] (Z.TXT;32766, ;32767, ;1) and, on level 5, [K] (Abc.txt, ABC.TXT, abc.TXT). The same commands are run by ods2v2 on a fresh
volume and the DIRECTORY FILES are compared record by record: the name as stored, the versions of every record (a name's versions in
one record, newest first), the version limit word (DIR$W_VERLIMIT) and the flags byte (DIR$B_FLAGS: the name type at level 5). The
header's ident name ("NAME;version") of each version of [C] and [K] is compared too.

The block each record is in is compared. The file ids of [L] are compared relative to its first version (they depend on what else the volume
held), which pins VMS's order of create and delete at the version limit. Allowed to differ, by design: file ids elsewhere (the numbers; [M]'s D.TXT;4
takes slot 25, sequence 2 on VMS, the slot the previous create freed, where ods2v2 takes the lowest free header number), dates, sizes, the files'
record attributes and contents, the volume's other directories, and the bytes after a directory's end-of-records marker. [V] (90
versions of one name between two neighbours, CORPUS 49 C7: VMS keeps 58 in the older record and starts a new record in front of it
for the next, 32 in the newest) is compared like the others.
"""
import os
import atexit
import shutil
import tempfile
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import ods5check as O   # noqa: E402

ODS2, INIT = './ods2', './ods2_init'
_TMPBASE = os.environ.get('TMPDIR', 'samples')
os.makedirs(_TMPBASE, exist_ok=True)
TMP = tempfile.mkdtemp(prefix='newver_vms_compare_', dir=_TMPBASE)      # a directory of this run's own: two runs at once (parallel gates sharing a TMPDIR) never meet
atexit.register(shutil.rmtree, TMP, ignore_errors=True)
fails = 0


def check(ok, text, detail=''):
    global fails
    print('%s: %s' % ('PASS' if ok else 'FAIL', text))
    if not ok:
        fails += 1
        if detail:
            print('      ' + detail.replace('\n', '\n      '))


def run(args):
    return subprocess.run(args, capture_output=True, text=True, errors='replace').stdout


def file_number(img, spec):
    out = run([ODS2, img, 'DIR/FULL ' + spec])
    m = re.search(r'File ID:\s+\((\d+),', out)
    return int(m.group(1)) if m else None


def header(v, n):
    """file header n: its block is found through INDEXF.SYS's own map (the index file is not always one extent)"""
    if not hasattr(v, 'ix'):
        h1 = v.blk(v.ibmaplbn + v.ibmapsize)
        v.ix = O.decode_map(h1, O.Findings(False), 'indexf')
        v.ixfirst = (O.u16(v.home, 22) + O.u16(v.home, 32))    # IBMAPVBN + IBMAPSIZE: the VBN of header 1
    vbn = v.ixfirst + (n - 1)                                    # 1-based virtual block of INDEXF.SYS
    left = vbn - 1
    for lbn, cnt in v.ix:
        if left < cnt:
            return v.blk(lbn + left)
        left -= cnt
    raise ValueError('header %d is past the index file map' % n)


def file_blocks(v, n):
    h = header(v, n)
    fnd = O.Findings(False)
    ext = O.decode_map(h, fnd, 'f%d' % n)
    efblk = ((h[24] << 8 | h[25]) if False else O.u32(h, 24))   # recattr.efblk is word swapped
    return h, ext


def dir_layout(img, spec, level):
    """[(name, [versions], verlimit, flags)] of a directory file, in stored order"""
    n = file_number(img, spec)
    if n is None:
        return None
    v = O.Vol(img)
    h = header(v, n)
    fnd = O.Findings(False)
    ext = O.decode_map(h, fnd, 'dir')
    efblk = O.swap32(O.u32(h, 24 + 4)) if False else None
    data = b''
    for lbn, cnt in ext:
        data += v.blk(lbn, cnt)
    # blocks in use: up to the end of file, taken from the listing's size "used/allocated"
    out = run([ODS2, img, 'DIR/FULL ' + spec])
    m = re.search(r'Size:\s+(\d+)/', out)
    used = int(m.group(1)) if m else len(data) // 512
    recs = []
    for b in range(used):
        blk = data[b * 512:(b + 1) * 512]
        pos = 0
        while pos + 6 <= 512:
            size = O.u16(blk, pos)
            if size == 0xffff or size == 0:
                break
            total = size + 2
            verlimit, flags, namecount = O.u16(blk, pos + 2), blk[pos + 4], blk[pos + 5]
            raw = blk[pos + 6:pos + 6 + namecount]
            nt = (flags >> 3) & 7 if level == 5 else 0
            name = raw.decode('utf-16-le', 'replace') if (level == 5 and (flags & 0x03) == 3) else raw.decode('latin-1')
            first = pos + 6 + namecount + (namecount % 2)
            vers = [O.u16(blk, e) for e in range(first, pos + total, 8)]
            fids = [(O.u16(blk, e + 2), O.u16(blk, e + 4)) for e in range(first, pos + total, 8)]
            recs.append((name, vers, verlimit, flags, b + 1, fids))
            pos += total
    return recs, used


def ident_names(img, spec_dir, name, versions, level):
    out = {}
    v = O.Vol(img)
    for ver in versions:
        n = file_number(img, '%s%s;%d' % (spec_dir, name, ver))
        if n is None:
            out[ver] = None
            continue
        d = O.parse_ident(header(v, n), O.Findings(False), 'x')
        out[ver] = d['name'] if d else None
    return out


def build(level, label):
    img = '%s/ours%d.img' % (TMP, level)
    if os.path.exists(img):
        os.remove(img)
    subprocess.run([INIT, img, '20000', '--structure=%d' % level, '--label=' + label], capture_output=True)
    src = TMP + '/src.txt'
    open(src, 'w').write('source text\n')
    cmds = ['CREATE/DIRECTORY [C]', 'CREATE/DIRECTORY [L]', 'CREATE/DIRECTORY [M]', 'CREATE/DIRECTORY [N]', 'CREATE/DIRECTORY [V]']
    for _ in range(3):
        cmds.append('COPY %s [C]A.TXT' % src)
    cmds += ['COPY %s [C]SRC.TXT' % src, 'COPY %s [C]A.TXT' % src, 'COPY %s [C]A.TXT;7' % src, 'COPY %s [C]A.TXT' % src, 'COPY %s [C]A.TXT;0' % src]
    cmds += ['COPY %s [L]B.TXT' % src, 'SET FILE/VERSION_LIMIT=3 [L]B.TXT'] + ['COPY %s [L]B.TXT' % src] * 5
    cmds += ['COPY %s [M]C.TXT' % src, 'COPY %s [M]D.TXT' % src, 'SET FILE/VERSION_LIMIT=3 [M]C.TXT']
    for _ in range(5):
        cmds += ['COPY %s [M]C.TXT' % src, 'COPY %s [M]D.TXT' % src]
    cmds += ['COPY %s [M]SRC.TXT' % src] + ['COPY %s [M]C.TXT' % src] * 3
    cmds += ['COPY %s [N]Z.TXT;32766' % src, 'COPY %s [N]Z.TXT' % src, 'COPY %s [N]Z.TXT;1' % src]
    cmds += ['COPY %s [V]AAFIRST.TXT' % src, 'COPY %s [V]ZZLAST.TXT' % src] + ['COPY %s [V]VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT' % src] * 90
    if level == 5:
        cmds += ['CREATE/DIRECTORY [K]', 'COPY %s [K]Abc.txt' % src, 'COPY %s [K]ABC.TXT' % src, 'COPY %s [K]abc.TXT' % src]
    sc = TMP + '/ours%d.cmd' % level
    open(sc, 'w').write('\n'.join(cmds) + '\n')
    subprocess.run([ODS2, img, '@' + sc], capture_output=True)
    return img


def main():
    level = int(sys.argv[1])
    vms = sys.argv[2]
    os.makedirs(TMP, exist_ok=True)
    ours = build(level, 'NVC%d' % level)
    spots = [('[000000]C.DIR', '[C]'), ('[000000]L.DIR', '[L]'), ('[000000]M.DIR', '[M]'), ('[000000]N.DIR', '[N]'), ('[000000]V.DIR', '[V]')]
    if level == 5:
        spots.append(('[000000]K.DIR', '[K]'))
    fid_pairs = []
    for dirspec, tag in spots:
        la = dir_layout(vms, dirspec, level)
        lb = dir_layout(ours, dirspec, level)
        if la is None:
            print('SKIP: %s is not on the VMS container' % tag)
            continue
        (a, ua), (b, ub) = la, lb
        check(ua == ub, 'level %d %s: the directory has the same number of blocks in use as VMS\'s (%d)' % (level, tag, ua), 'VMS %d, ours %d' % (ua, ub))
        strip = lambda recs: [(n_, v_, vl_, fl_, blk_) for n_, v_, vl_, fl_, blk_, fids_ in recs]
        check(strip(a) == strip(b), 'level %d %s: the directory records (name, versions of the record, DIR$W_VERLIMIT, DIR$B_FLAGS, the BLOCK each record is in) equal what VMS wrote' % (level, tag),
              'VMS:  %s\nours: %s' % (strip(a), strip(b)))
        fa = {(n_, v_): f_ for n_, vs_, vl_, fl_, blk_, fids_ in a for v_, f_ in zip(vs_, fids_)}
        fb = {(n_, v_): f_ for n_, vs_, vl_, fl_, blk_, fids_ in b for v_, f_ in zip(vs_, fids_)}
        print('INFO: level %d %s file ids (number, sequence), VMS | ours: %s' % (level, tag, sorted((k, fa[k], fb.get(k)) for k in fa)[:12]))
        fid_pairs.append((tag, fa, fb))
    # the ident names of the versions of [C] (and [K] at level 5)
    for spec_dir, name, vers in (('[C]', 'A.TXT', [9, 8, 7, 4, 3, 2, 1]),) + ((('[K]', 'Abc.txt', [3, 2, 1]),) if level == 5 else ()):
        ia = ident_names(vms, spec_dir, name, vers, level)
        ib = ident_names(ours, spec_dir, name, vers, level)
        check(ia == ib, 'level %d %s%s: the ident name of every version (name;version) equals VMS\'s' % (level, spec_dir, name), 'VMS:  %s\nours: %s' % (ia, ib))
    # the file ids of [L] (the limit 3, B.TXT;1 to ;6): VMS gives ;4, the create that removed ;1, a NEW header, and the freed slots go to the next creates
    # (;5 is (21,2), ;6 is (22,2): the numbers of ;1 and ;2 with the sequence + 1). The numbers differ by what else the volume held, so they are compared
    # relative to ;4's header: the same difference and the same sequence for every version (review 40 F4: this pins the order of create and delete)
    for tag, fa, fb in fid_pairs:
        if tag != '[L]':
            continue
        keys = sorted(fa)
        base_off = fb[keys[0]][0] - fa[keys[0]][0]
        ok_ = all(fb[k][0] - fa[k][0] == base_off and fb[k][1] == fa[k][1] for k in keys)
        check(ok_ and fa[keys[0]][1] == 1 and all(fa[k][1] == 2 for k in keys[1:]),
              'level %d [L]: the file ids of the versions made at the limit follow VMS\'s order (a new header for the first, the freed slots, sequence + 1, for the next two): offset %+d' % (level, base_off),
              'VMS: %s\nours: %s' % ({k: fa[k] for k in keys}, {k: fb[k] for k in keys}))
    print('newver_vms_compare level %d: %s' % (level, 'FAILED' if fails else 'all compared directories equal'))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
