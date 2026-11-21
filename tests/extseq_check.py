#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""extseq_check.py : an extension header must name its primary header by FILE NUMBER AND SEQUENCE NUMBER (review 54 item 1). extension_header_ok compared the back link's file
number only, so an extension header whose back link had the right number but another sequence (a header of an older file that had the number, or a crafted one) was taken as
this file's next segment. In VMS-made volumes the back link of every extension header carries the primary header's sequence number (248 extension headers of 159 corpus images, 199 of
them in files whose sequence number is above 1: none differs). For both levels: a file of several extension headers (tests/setfile_ext_check.py's build) reads back as before
(control); with the back-link sequence of its first, or of its last, extension header made wrong (header checksum kept right) the file is NOT read: EXPORT fails and SET FILE/OWNER_UIC
refuses with nothing written; the other file is read as before. Run from the project root. Exit 1 on any FAIL. Written before the change (tests first): on ods5 a2ac9be a wrong
back-link sequence was not noticed."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext_headers as E   # noqa: E402
import setfile_ext_check as S   # noqa: E402

fails = 0


def check(desc, expected, got):
    global fails
    if expected == got:
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %s' % (expected,))
        print('      got:      %s' % (got,))


def ods2(img, *cmds):
    p = subprocess.run(['./ods2', img] + list(cmds), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def export_md5(img, spec, work, tag):
    out = os.path.join(work, 'ex_' + tag)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    rc, o = ods2(img, 'EXPORT %s %s' % (spec, out))
    names = os.listdir(out)
    return rc, o, (hashlib.md5(open(os.path.join(out, names[0]), 'rb').read()).hexdigest() if names else None)


def main():
    base = os.environ.get('TMPDIR', 'samples')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='extseq_', dir=base)
    try:
        for level in (2, 5):
            tag = 'ODS-%d' % level
            img0 = S.build(level, work)
            ch = E.chain(img0, '[F]A.DAT')
            check('%s: the file has a primary header and at least 3 extension headers (setup)' % tag, True, len(ch) >= 4)
            rc, o, md = export_md5(img0, '[F]A.DAT', work, 'a0')
            rcb, ob, mdb = export_md5(img0, '[F]B.DAT', work, 'b0')
            check('%s: control: the file reads back (EXPORT succeeds)' % tag, (0, True), (rc, md is not None))
            v = E.O.Vol(img0)
            for which, idx in (('first', 1), ('last', len(ch) - 1)):
                img = os.path.join(work, 'x%d%s.img' % (level, which))
                shutil.copyfile(img0, img)
                lbn = E.D.lbn(E.O.Vol(img), ch[idx][0])
                with open(img, 'r+b') as f:
                    f.seek(lbn * 512)
                    b = bytearray(f.read(512))
                    struct.pack_into('<H', b, 68, (struct.unpack_from('<H', b, 68)[0] + 1) & 0xffff)      # the back link's sequence number
                    struct.pack_into('<H', b, 510, sum(struct.unpack_from('<255H', b, 0)) & 0xffff)
                    f.seek(lbn * 512)
                    f.write(b)
                rc, o, md2 = export_md5(img, '[F]A.DAT', work, 'a_' + which)
                # review 59 S2: the refusal names the one cause and both file IDs (the primary's number and sequence as they are, and the sequence the back link carries)
                prim = ch[0][0]
                pseq = struct.unpack_from('<H', ch[0][1], 10)[0]
                rcx, ox = ods2(img, 'TYPE [F]A.DAT')
                check('%s: the refusal (%s extension header) names the cause: the back link\'s sequence, with both sequence numbers and the file number' % (tag, which),
                      True, 'back link' in ox and 'sequence' in ox and ('file %d' % prim) in ox and ('sequence %d' % pseq) in ox and ('sequence %d' % (pseq + 1)) in ox)
                # review 59 S1: ods5check reports it too, as ods2 refuses it
                pr = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True)
                errs = [ln for ln in pr.stdout.split('\n') if ln.startswith('ERROR') and 'back link' in ln and 'sequence' in ln]
                check('%s: ods5check reports the wrong back-link sequence of the %s extension header as an error' % (tag, which), (1, True), (pr.returncode, bool(errs)))
                check('%s: the back-link sequence of the %s extension header is wrong: the file is NOT read (EXPORT fails or gives nothing)' % (tag, which), True, rc != 0 or md2 is None)
                chain_before = [E.describe(n, h) for n, h in ch]
                rc, o = ods2(img, 'SET FILE/OWNER_UIC=[5,5] [F]A.DAT')
                after = [E.describe(n, h) for n, h in E.chain(img, '[F]A.DAT')]
                check('%s: SET FILE/OWNER_UIC refuses it (%s extension header) and writes nothing' % (tag, which), True, rc != 0 and [x['owner'] for x in after] == [x['owner'] for x in chain_before])
                rcb2, ob2, mdb2 = export_md5(img, '[F]B.DAT', work, 'b_' + which)
                if which == 'first':
                    # review 61 S3: the primary header's EXT_FID names the extension header by number and SEQUENCE; one whose sequence differs from the header's own (EXT_FID says
                    # N+1, the header has N) is refused by ods2 and reported by ods5check
                    img3 = os.path.join(work, 'f%d.img' % level)
                    shutil.copyfile(img0, img3)
                    lbn3 = E.D.lbn(E.O.Vol(img3), ch[0][0])
                    with open(img3, 'r+b') as f:
                        f.seek(lbn3 * 512)
                        b3 = bytearray(f.read(512))
                        struct.pack_into('<H', b3, 16, (struct.unpack_from('<H', b3, 16)[0] + 1) & 0xffff)      # EXT_FID's sequence number
                        struct.pack_into('<H', b3, 510, sum(struct.unpack_from('<255H', b3, 0)) & 0xffff)
                        f.seek(lbn3 * 512)
                        f.write(b3)
                    rc3, o3, md3 = export_md5(img3, '[F]A.DAT', work, 'ef_' + str(level))
                    check('%s: an EXT_FID whose sequence differs from the extension header\'s: the file is NOT read' % tag, True, rc3 != 0 or md3 is None)
                    pr3 = subprocess.run(['python3', '-I', 'tests/ods5check.py', img3], capture_output=True, text=True)
                    errs3 = [ln for ln in pr3.stdout.split('\n') if ln.startswith('ERROR') and 'EXT_FID' in ln and 'sequence' in ln]
                    check('%s: ods5check reports the EXT_FID sequence that does not match the extension header as an error' % tag, (1, True), (pr3.returncode, bool(errs3)))
                check('%s: the other file is read as before' % tag, (0, mdb), (rcb2, mdb2))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('extseq_check: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    sys.exit(1 if fails else 0)


main()
