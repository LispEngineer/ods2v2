#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""filehdr_vms_rule.py VMS_IMAGE : the rule of follow-up b over a VMS-made ODS-2 image (no ods2v2 code): every user file (file number 12 and up, no directory, primary headers) has
RECPROT 0, no CONTIG (FILECHAR bit 0x80 clear) and "Highest block written" = HIBLK + 1 (DCL-written files: the whole allocation; the files of the step 0 containers have no room
after their end of file, so both readings agree). Prints PASS/FAIL lines and the count; exit 1 on any file that breaks the rule. The tests/cli_filehdr_check.sh makes the same rule
true of what ods2v2 writes."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402


def pointers(h):
    out = []
    i = h[1] * 2
    end = i + h[58] * 2
    while i < end:
        w0 = O.u16(h, i)
        fmt = w0 >> 14
        if fmt == 0:
            i += 2
            continue
        if fmt == 1:
            cnt = (w0 & 0xff) + 1
            lbn = ((w0 >> 8) & 0x3f) << 16 | O.u16(h, i + 2)
            i += 4
        elif fmt == 2:
            cnt = (w0 & 0x3fff) + 1
            lbn = struct.unpack_from('<I', h, i + 2)[0]
            i += 6
        else:
            cnt = (((w0 & 0x3fff) << 16) | O.u16(h, i + 2)) + 1
            lbn = struct.unpack_from('<I', h, i + 4)[0]
            i += 8
        out.append((lbn, cnt))
    return out


def main():
    img = sys.argv[1]
    v = O.Vol(img)
    h1 = v.blk(v.ibmaplbn + v.ibmapsize)
    lbns = [l + k for l, c in pointers(h1) for k in range(c)]
    first = O.u16(v.home, 22) + O.u16(v.home, 32) - 1
    n_ok = n_bad = 0
    bad = []
    for n, lbn in enumerate(lbns[first:], 1):
        h = v.blk(lbn)
        if O.u16(h, 8) != (n & 0xffff) or h[0] != 40 or O.u16(h, 4) != 0 or n < 12:
            continue
        if sum(struct.unpack_from('<255H', h, 0)) & 0xffff != O.u16(h, 510):
            continue
        filechar = struct.unpack_from('<I', h, 52)[0]
        if filechar & 0x2000:
            continue
        hiblk = (O.u16(h, 24) << 16) | O.u16(h, 26)
        hw = struct.unpack_from('<I', h, 76)[0]
        rec = O.u16(h, 56)
        if rec == 0 and not (filechar & 0x80) and hw == hiblk + 1:
            n_ok += 1
        else:
            n_bad += 1
            bad.append('file %d: recprot %04x filechar %08x highwater %d hiblk %d' % (n, rec, filechar, hw, hiblk))
    print('%s: %s: %d user files, %d keep the rule' % ('PASS' if n_bad == 0 and n_ok > 0 else 'FAIL', os.path.basename(img), n_ok + n_bad, n_ok))
    for b in bad[:5]:
        print('      ' + b)
    sys.exit(1 if n_bad or n_ok == 0 else 0)


if __name__ == '__main__':
    main()
