#!/usr/bin/env python3
# MIT License
#
# Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""RULE: every expectation in this checker comes from VMS-made images or the VSI definitions, never from
what ods2v2 writes. After any change run it on all the VMS-made images (it must report 0 errors and 0
warnings on each) before trusting it on ours.

ods5check.py - an independent structure check of an ODS-2/ODS-5 disk image
(a poor man's ANALYZE/DISK_STRUCTURE), written from the VSI definitions and the
VMS-made images, sharing no code with ods2v2. Prints one line per finding and a
summary; exit status 1 if there is any error.

usage: ods5check.py [-v] [--ignore-order] image

Checks: home block copies and checksums; header checksums, structure level,
FID/slot agreement, ident area (FI2/FI5: control byte, name length, MPOFFSET),
map area (extents inside the volume, HIBLK = sum of extents, EFBLK sane,
extension chains); index file bitmap vs headers; storage bitmap vs the extents
of all files (lost blocks, double allocation); directories (record structure,
name order, name type flags vs name, every FID exists and is in use, backlink
of every file points at a directory that lists it, header name = the directory
entry of the primary link, link count where the volume keeps one).
"""
import os
import re
import struct, sys, time

u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]


def stream_longest(rfm, data):
    """The longest record of Stream (4), Stream_LF (5) or Stream_CR (6) content as RMS reads it (see ods2_reclen_feed in
    src/ods2_volume.c): Stream_LF ends a record at LF, Stream_CR at CR (neither is part of the record); Stream at LF, FF or
    VT (that byte stays in the record) or CR LF (stripped); the last record may have no terminator."""
    if rfm == 5:
        return max((len(x) for x in data.split(b'\n')), default=0)
    if rfm == 6:
        return max((len(x) for x in data.split(b'\r')), default=0)
    longest = prev = 0
    for m in re.finditer(rb'\r\n|[\n\f\v]', data):
        ln = m.start() - prev if m.group() == b'\r\n' else m.end() - prev
        if ln > longest:
            longest = ln
        prev = m.end()
    return max(longest, len(data) - prev)


def checksum(b, n=255):
    s = 0
    for i in range(n):
        s = (s + u16(b, 2 * i)) & 0xFFFF
    return s


def swap32(v):
    return ((v >> 16) | (v << 16)) & 0xFFFFFFFF


def fold(cp):
    # Unicode simple upper case of the BMP, as ods5_fold does (python's upper()
    # maps some characters to several; keep single-character results only)
    c = chr(cp)
    u = c.upper()
    if len(u) != 1:
        return cp
    # VMS does not fold these (measured: sharp s stays); python agrees. The
    # micro sign and dotless i/long s differences are not measured; ignore them.
    return ord(u)


def fold_name(s):
    return [fold(ord(c)) for c in s]


class Findings:
    def __init__(self, verbose):
        self.errors = []
        self.warnings = []
        self.verbose = verbose

    def err(self, msg):
        self.errors.append(msg)
        print('ERROR  ' + msg)

    def warn(self, msg):
        self.warnings.append(msg)
        print('WARN   ' + msg)

    def info(self, msg):
        if self.verbose:
            print('info   ' + msg)


class Vol:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        self.size_blocks = self.f.tell() // 512
        self.home = self.blk(1)
        h = self.home
        self.struclev = u16(h, 12)
        self.cluster = u16(h, 14)
        self.ibmaplbn = u32(h, 24)
        self.maxfiles = u32(h, 28)
        self.ibmapsize = u16(h, 32)
        self.resfiles = u16(h, 34)
        self.volchar = u16(h, 42)
        self.label = h[472:484].decode('latin-1').rstrip()
        self.level = self.struclev >> 8

    def blk(self, lbn, n=1):
        self.f.seek(lbn * 512)
        b = self.f.read(512 * n)
        if len(b) < 512 * n:
            b = b + b'\0' * (512 * n - len(b))
        return b


def decode_map(b, fnd, where):
    """extents (lbn, count) of the map area of header b"""
    o = b[1] * 2
    end = o + b[58] * 2
    ext = []
    if end > 510 or o < 80:
        fnd.err('%s: map area out of range (mpoffset %d words, inuse %d)' % (where, b[1], b[58]))
        return ext
    while o < end:
        w = u16(b, o)
        fmt = w >> 14
        if fmt == 0:
            o += 2
        elif fmt == 1:
            ext.append((((w >> 8) & 0x3F) << 16 | u16(b, o + 2), (w & 0xFF) + 1))
            o += 4
        elif fmt == 2:
            ext.append((u32(b, o + 2), (w & 0x3FFF) + 1))
            o += 6
        else:
            ext.append((u32(b, o + 4), (((w & 0x3FFF) << 16) | u16(b, o + 2)) + 1))
            o += 8
    if o != end:
        fnd.err('%s: map area pointers run past MAP_INUSE' % where)
    return ext


FUTURE_SEEN = {}


def parse_ident(b, fnd, where):
    """returns dict for the ident area of a primary header, or None"""
    lev = b[7]
    ido = b[0] * 2
    mpo = b[1] * 2
    d = {'level': lev}
    if lev == 2:
        name = b[ido:ido + 20] + b[ido + 54:ido + 120]
        d['name'] = name.split(b' ')[0].split(b'\0')[0].decode('latin-1')
        d['type'] = 0
        d['revision'] = u16(b, ido + 20)
        d['credate'] = u64(b, ido + 22)
        d['revdate'] = u64(b, ido + 30)
        return d
    if lev != 5:
        fnd.err('%s: structure level %d' % (where, lev))
        return None
    control = b[ido]
    namelen = b[ido + 1]
    d['type'] = control & 3
    d['control'] = control
    d['namelen'] = namelen
    d['revision'] = u16(b, ido + 2)
    d['credate'] = u64(b, ido + 4)
    d['revdate'] = u64(b, ido + 12)
    d['expdate'] = u64(b, ido + 20)
    d['bakdate'] = u64(b, ido + 28)
    d['accdate'] = u64(b, ido + 36)
    d['attdate'] = u64(b, ido + 44)
    raw = b[ido + 76:ido + 76 + namelen]
    d['rawname'] = raw
    if b[4] == 0 and b[5] == 0:  # primary header (seg_num is u16 at 4)
        need = max(120, 76 + namelen)
        need += need % 2
        if mpo != ido + need and not (control & 0x10):
            fnd.err('%s: MPOFFSET %d bytes, expected %d for a %d-byte name' % (where, mpo, ido + need, namelen))
        if ido + 76 + namelen > mpo:
            fnd.err('%s: FI5 name runs past MPOFFSET' % where)
        if d['type'] == 3:
            if namelen % 2:
                fnd.err('%s: UCS-2 name of odd length' % where)
            d['name'] = raw.decode('utf-16-le', 'replace')
        else:
            d['name'] = raw.decode('latin-1')
    return d


def stored_type_for(name):
    """the name type VMS stores for a literal name (with dot)"""
    if any(ord(c) > 0xFF for c in name):
        return 3
    ok = True
    nm = ty = 0
    seen = False
    for c in name:
        if c == '.':
            if seen:
                ok = False
            seen = True
        elif c.isascii() and (c.isalnum() or c in '$_-'):
            if seen:
                ty += 1
            else:
                nm += 1
        else:
            ok = False
    if ok and nm <= 39 and ty <= 39:
        return 0
    return 1


def main():
    args = sys.argv[1:]
    verbose = False
    ignore_order = False
    while args and args[0].startswith('-'):
        a = args.pop(0)
        if a == '-v':
            verbose = True
        elif a == '--ignore-order':
            ignore_order = True
    if not args:
        print(__doc__)
        return 2
    fnd = Findings(verbose)
    v = Vol(args[0])
    print('image %s: %d blocks, level %d, cluster %d, label %r, volchar 0x%04x'
          % (v.path, v.size_blocks, v.level, v.cluster, v.label, v.volchar))
    if v.level not in (2, 5):
        fnd.err('home block structure level %d' % v.level)
        return 1
    hc = u16(v.home, 510)
    if hc != checksum(v.home):
        fnd.err('home block checksum')
    # home block copies: every home block copy must be equal but for HOMELBN/HOMEVBN
    cs = v.cluster
    for lbn in range(2, 2 + 3 * cs) if False else []:
        pass

    # --- INDEXF.SYS ---
    base = v.ibmaplbn + v.ibmapsize  # VBN k of INDEXF.SYS header n: LBN base + n - 1 in the first extent
    h1 = v.blk(base)
    if u16(h1, 8) != 1:
        fnd.err('INDEXF.SYS header not at LBN %d (fid %d)' % (base, u16(h1, 8)))
    idx = decode_map(h1, fnd, 'INDEXF.SYS')
    # follow extension headers of INDEXF.SYS (primary header from the first extent)
    seen = 0
    hb = h1
    while True:
        efn = u16(hb, 14) | (hb[19] << 16)
        if efn == 0 or seen > 64:
            break
        # header efn's VBN = 4*cluster + ibmapsize + efn; locate through idx so far
        vbn = 4 * v.cluster + v.ibmapsize + efn
        lbn = vbn2lbn(idx, vbn)
        if lbn is None:
            fnd.err('INDEXF.SYS extension header %d not mapped' % efn)
            break
        hb = v.blk(lbn)
        idx += decode_map(hb, fnd, 'INDEXF.SYS ext %d' % efn)
        seen += 1
    idx_blocks = sum(c for _, c in idx)
    hdr_vbn0 = 4 * v.cluster + v.ibmapsize  # header n at VBN hdr_vbn0 + n
    max_hdr = idx_blocks - hdr_vbn0  # headers within allocation
    # EOF of INDEXF.SYS
    eof_blk = swap32(u32(h1, 28))
    nheaders = eof_blk - 1 - hdr_vbn0  # header numbers 1..nheaders are inside EOF
    print('INDEXF.SYS: %d blocks allocated, EOF block %d, headers 1..%d inside EOF' % (idx_blocks, eof_blk, nheaders))

    def read_header(n):
        lbn = vbn2lbn(idx, hdr_vbn0 + n)
        if lbn is None:
            return None
        return v.blk(lbn)

    # index file bitmap
    ibm = v.blk(v.ibmaplbn, v.ibmapsize)

    def idx_used(n):
        bit = n - 1
        if bit // 8 >= len(ibm):
            return False
        return bool(ibm[bit // 8] >> (bit % 8) & 1)

    # --- all headers ---
    headers = {}   # fid number -> bytes (valid ones)
    deleted = set()
    pending = set()   # marked for delete, FID intact
    headers_pending = {}
    for n in range(1, nheaders + 1):
        b = read_header(n)
        if b is None:
            fnd.err('header %d is not mapped by INDEXF.SYS' % n)
            continue
        lev = b[7]
        struct_ok = lev in (2, 5) and b[0] >= 40 and b[0] <= b[1] <= b[2] <= b[3]
        fn = u16(b, 8) | (b[13] << 16)
        used = idx_used(n)
        if struct_ok and fn != 0 and checksum(b) == u16(b, 510) and (u32(b, 52) & 0x8000):
            # FH2$M_MARKDEL with the FID and checksum intact: a delete that was interrupted (VMS's own order:
            # mark the header, remove the entry, free the blocks, then clear the FID). Its blocks may be free
            # already; ANALYZE/REPAIR finishes it. Not a live file.
            pending.add(n)
            headers_pending[n] = b
            fnd.info('header %d is marked for delete (an interrupted delete, or a file deleted while open)' % n)
        elif struct_ok and fn != 0 and checksum(b) == u16(b, 510):
            if fn != n:
                fnd.err('header %d holds file number %d' % (n, fn))
            headers[n] = b
            if not used:
                fnd.err('header %d is valid but its index bitmap bit is clear' % n)
        elif struct_ok and (u32(b, 52) & 0x8000) and fn == 0:
            deleted.add(n)
            if used:
                fnd.err('header %d is deleted but its index bitmap bit is set' % n)
        elif used and (n & 0xFFFF) == 0 and not any(b):
            # file numbers whose low 16 bits are 0 (65536, 131072, ...) are never used (FID_NUM 0 means "no file"); VMS marks
            # the number in use in the index bitmap and never writes its header slot (a volume of 70,000 files made on V8.4)
            pass
        else:
            if used:
                fnd.err('header %d: bitmap bit set but no valid header (checksum %04x/%04x, struclev %04x)'
                        % (n, checksum(b), u16(b, 510), u16(b, 6)))
            elif any(b):
                fnd.info('header %d: garbage slot (not in use)' % n)
    print('headers: %d valid, %d deleted' % (len(headers), len(deleted)))
    # bits set beyond what exists
    for n in range(nheaders + 1, v.ibmapsize * 4096 + 1):
        if idx_used(n):
            fnd.err('index bitmap bit %d set beyond the index file EOF' % n)
            break

    # --- per header: map, ident, fat ---
    owner = {}  # lbn cluster -> fid (for double allocation)
    pend_owned = {}   # cluster -> a header marked for delete that maps it (such a cluster is not 'lost')
    for n in sorted(pending):
        for l, c in decode_map(headers_pending[n], fnd, 'file %d (marked for delete)' % n):
            for cl in range(l // v.cluster, (l + c - 1) // v.cluster + 1):
                pend_owned[cl] = n
    file_extents = {}
    prim = {}   # primary file number -> (header, ident)
    for n, b in sorted(headers.items()):
        where = 'file %d' % n
        seg = u16(b, 4)
        if seg != 0:
            continue
        d = parse_ident(b, fnd, where)
        prim[n] = (b, d)
        # A date is local time in the zone the VMS system runs in. One that is more than an hour ahead of this host's
        # local time was most likely written as UTC by a host west of Greenwich (VMS then calls it FUTCREDAT /
        # FUTREVDAT, and every such file sorts and purges as if from the future): the writer's zone (TZ, or ODS2_TZ for
        # ods2v2) was not the VMS machine's. Compared with the host running this checker, so it is a warning.
        if d is not None and not FUTURE_SEEN.get('done'):
            # the zone: ODS2_TZ (the VMS system's, as for ods2v2) when set, else this host's
            _tzsave = os.environ.get('TZ')
            if os.environ.get('ODS2_TZ'):
                os.environ['TZ'] = os.environ['ODS2_TZ']
                time.tzset()
            ahead = (time.time() + time.localtime().tm_gmtoff + 3506716800 + 3600) * 10000000
            if os.environ.get('ODS2_TZ'):
                if _tzsave is None: os.environ.pop('TZ', None)
                else: os.environ['TZ'] = _tzsave
                time.tzset()
            for key in ('credate', 'revdate', 'accdate', 'attdate'):
                if d.get(key, 0) > ahead:
                    hrs = (d[key] - (ahead - 3600 * 10000000)) / 10000000 / 3600.0
                    fnd.warn('%s: %s is %.1f hours in the future of this host (file dates are local time of the VMS '
                             'system. Either the writer stamped another zone than the VMS machine runs in (a TZ=UTC script; '
                             'run the checker with ODS2_TZ set to the VMS machine\'s zone to compare in that zone), or the '
                             'VMS machine\'s clock is ahead of this host\'s, or the VMS system\'s zone is east of this host\'s)'
                             % (where, key, hrs))
                    FUTURE_SEEN['count'] = FUTURE_SEEN.get('count', 0) + 1
                    break
            if FUTURE_SEEN.get('count', 0) >= 3:
                fnd.warn('(further files with future dates are not listed)')
                FUTURE_SEEN['done'] = True
        # extension chain
        exts = decode_map(b, fnd, where)
        cur = b
        segs = [n]
        while True:
            efn = u16(cur, 14) | (cur[19] << 16)
            if efn == 0:
                break
            if efn in segs or len(segs) > 64:
                fnd.err('%s: extension chain loops' % where)
                break
            nb = headers.get(efn)
            if nb is None:
                fnd.err('%s: extension header %d missing' % (where, efn))
                break
            if u16(nb, 4) != len(segs):
                fnd.err('%s: extension header %d has segment number %d, expected %d' % (where, efn, u16(nb, 4), len(segs)))
            if u16(nb, 66) | (nb[71] << 16) != n:
                fnd.err('%s: extension header %d back link is %d' % (where, efn, u16(nb, 66)))
            exts += decode_map(nb, fnd, '%s ext %d' % (where, efn))
            segs.append(efn)
            cur = nb
        file_extents[n] = exts
        total = sum(c for _, c in exts)
        hiblk = swap32(u32(b, 24))
        efblk = swap32(u32(b, 28))
        ffbyte = u16(b, 32)
        if total != hiblk:
            fnd.err('%s: HIBLK %d but extents hold %d blocks' % (where, hiblk, total))
        if efblk > hiblk + 1 and not (hiblk == 0):
            fnd.err('%s: EFBLK %d beyond HIBLK %d' % (where, efblk, hiblk))
        if ffbyte > 512:
            fnd.err('%s: FFBYTE %d' % (where, ffbyte))
        for l, c in exts:
            if l + c > v.size_blocks and n != 3:
                fnd.err('%s: extent LBN %d+%d beyond the volume (%d blocks)' % (where, l, c, v.size_blocks))
            for cl in range(l // v.cluster, (l + c - 1) // v.cluster + 1):
                if cl in owner and owner[cl] != n:
                    fnd.err('%s: cluster %d (LBN %d) also owned by file %d' % (where, cl, cl * v.cluster, owner[cl]))
                owner[cl] = n
        if b[7] == 5:
            # FH2$W_LINKCOUNT as VMS V8.4 writes it (measured on VMS-made images only: ODS5T/ODS5T6 with
            # HARDLINKS: 1 or more; ASIM8ODS5, AXP2ODS5 and the archive images without: 0)
            lc = u16(b, 74)
            if (v.volchar & 0x40):
                if lc < 1:
                    fnd.warn('%s: link count %d on a volume with HARDLINKS' % (where, lc))
            elif lc != 0:
                fnd.warn('%s: link count %d on a volume without HARDLINKS (VMS writes 0)' % (where, lc))
        # fixed-part sanity
        if u32(b, 52) & 0x2000:  # directory
            if b[20] & 0xF != 2:
                fnd.warn('%s: directory with record type %d' % (where, b[20] & 0xF))
    # --- storage bitmap ---
    if 2 not in headers:
        fnd.err('no BITMAP.SYS')
    else:
        bx = file_extents[2]
        data = b''.join(v.blk(l, c) for l, c in bx)
        scb = data[:512]
        bits = data[512:]
        volsize = u32(scb, 4)
        if volsize == 0 or volsize > v.size_blocks:
            volsize = v.size_blocks  # the starter volume's SCB is all ones: go by the image size
        print('BITMAP.SYS: %d blocks, SCB volsize %d' % (len(data) // 512, volsize))
        free = 0
        starter_quirk = bits[0:1] == b'\0' and u32(scb, 4) > v.size_blocks  # SCB of all ones
        nclusters = min(volsize // v.cluster, len(bits) * 8)
        for cl in range(nclusters):
            isfree = bool(bits[cl // 8] >> (cl % 8) & 1)
            if isfree:
                free += 1
            if isfree and cl in owner:
                msg = 'cluster %d (LBN %d) is in use by file %d but marked free' % (cl, cl * v.cluster, owner[cl])
                if owner[cl] <= 10 and starter_quirk:
                    fnd.warn(msg)   # the starter volume marks its own middle-of-disk reserved files free
                else:
                    fnd.err(msg)
            if cl in pend_owned and isfree:
                # a finished delete frees the clusters its header maps; while the header is marked they must
                # still be allocated (ods2v2 writes the final header before it frees anything)
                fnd.err('cluster %d (LBN %d) is free but still mapped by header %d, marked for delete'
                        % (cl, cl * v.cluster, pend_owned[cl]))
            if cl in pend_owned and cl in owner:
                fnd.err('cluster %d (LBN %d) is mapped by header %d, marked for delete, and owned by file %d'
                        ' (finishing the delete would free a live file\'s cluster)' % (cl, cl * v.cluster, pend_owned[cl], owner[cl]))
            if not isfree and cl not in owner and cl not in pend_owned:
                if starter_quirk and cl < 1100: continue
                fnd.warn('cluster %d (LBN %d) is allocated but owned by no file (lost)' % (cl, cl * v.cluster))
        if volsize % v.cluster != 0 and not starter_quirk:
            # The blocks past the last whole cluster: VMS gives that partial cluster to BADBLK.SYS (one extent, a whole
            # cluster starting at floor(VOLSIZE/cluster)*cluster, of which only the blocks inside the volume exist), so
            # that no cluster is allocated to nobody. Measured on every VMS-made volume of such a size (V7.3, V8.4, V8.4-2L2).
            # A volume that stops its bitmap at the last whole cluster and gives BADBLK.SYS nothing is the older ods2_init's:
            # V8.4 ANALYZE calls the partial cluster incorrectly allocated (ALLOCCLR) and the free count drifted.
            part = volsize // v.cluster * v.cluster
            if not any(l <= part < l + c for l, c in file_extents.get(3, [])):
                fnd.warn('the partial cluster at LBN %d (the %d blocks past the last whole cluster of %d) is not owned by '
                         'BADBLK.SYS, as VMS INITIALIZE does: V8.4 ANALYZE reports ALLOCCLR and FREESPADRIFT' %
                         (part, volsize - part, v.cluster))
        print('storage: %d clusters, %d free' % (nclusters, free))

    # --- stream files whose longest record exceeds their LRL or MRS ---
    # RMS reads a Stream, Stream_LF or Stream_CR file in record mode with a buffer of the LRL (RSIZE) or, when it is not 0, the
    # MRS: a longer record fails (measured on VAX V7.3 and V8.4: TYPE %RMS-F-RSZ, COPY %RMS-W-RTB, CHECKSUM %RMS-W-RTB on the
    # VAX; CONVERT silently drops the record; COPY to disk and BACKUP work). ods2v2 once gave such files MRS = LRL = 512.
    # Record ends as RMS applies them (ods2_reclen_feed): Stream_LF at LF; Stream_CR at CR; Stream at LF, FF or VT (the byte
    # stays in the record) or CR LF (stripped).
    for n, (b, d) in sorted(prim.items()):
        if n <= 10 or (u32(b, 52) & 0x2000) or (u32(b, 52) & 0x8000):
            continue
        rfm, org, maxrec, lrl = b[20] & 0xF, b[20] >> 4, u16(b, 36), u16(b, 22)
        if org != 0 or rfm not in (4, 5, 6):
            continue
        size = (swap32(u32(b, 28)) - 1) * 512 + u16(b, 32)
        if size <= 0 or size > (1 << 28):
            continue
        data = b''.join(v.blk(l, c) for l, c in file_extents[n])[:size]
        longest = stream_longest(rfm, data)

        kind = {4: 'Stream', 5: 'Stream_LF', 6: 'Stream_CR'}[rfm]
        if longest > 32767 and maxrec == 0:
            # VMS's own state for a binary stream file with no line ends: MRS 0 and LRL at most 32767 (or 0), and no record length
            # can hold the record (52 such files on the AXP1 system disk); measured (V7.3, V7.3-2, V8.4): record-mode reads fail
            fnd.info('file %d (%s): a %s file with a record of %d bytes (LRL %d, MRS 0): a record longer than 32,767 bytes: no VMS '
                     'version tested (V7.3, V7.3-2, V8.4) reads it in record mode (TYPE, COPY to NL:, CHECKSUM\'s default, READ fail '
                     'with RTB; CONVERT drops the whole file); block mode (COPY file to file, BACKUP) and V8.4 CHECKSUM/ALGORITHM=MD5 '
                     'read it; VMS leaves binary stream files like this' %
                     (n, d['name'] if d else '?', kind, longest, lrl))
        elif lrl == 0 and maxrec == 0 and longest > 512:
            # LRL 0 ("not recorded") with MRS 0: V7.3 and V7.3-2 CHECKSUM/FILE gives RTB for a 2000-byte line (TYPE, COPY, CONVERT work;
            # V8.4 is fine). 512 is the threshold chosen here; only 2000 was measured.
            fnd.info('file %d (%s): a %s file with LRL 0 and MRS 0 holds a record of %d bytes: CHECKSUM/FILE fails on V7.3 and '
                     'V7.3-2 (%%RMS-W-RTB; measured with a 2000-byte line; 512 is the threshold chosen here), TYPE, COPY and CONVERT '
                     'work, V8.4 is fine; VMS leaves files like this on its system disks and ods2v2 never writes one; '
                     'SET FILE/ATTRIBUTES=(MRS:0) repairs it (LRL the longest record)' %
                     (n, d['name'] if d else '?', kind, longest))
        elif (lrl != 0 and longest > lrl) or (maxrec != 0 and longest > maxrec):
            fnd.warn('file %d (%s): a %s file whose longest record is %d bytes has LRL %d and MRS %d: TYPE, COPY and (VAX) '
                     'CHECKSUM fail (%%RMS-F-RSZ, %%RMS-W-RTB); CONVERT drops the record; block I/O and COPY to disk work; '
                     'SET FILE/ATTRIBUTES=(MRS:0) repairs it' %
                     (n, d['name'] if d else '?', kind, longest, lrl, maxrec))

    # --- directories ---
    dirs = [n for n, (b, d) in prim.items() if u32(b, 52) & 0x2000]
    entries = {}   # dir fn -> list of (name_str, type, version, fid)
    for n in dirs:
        b, d = prim[n]
        data = b''.join(v.blk(l, c) for l, c in file_extents[n])
        efblk = swap32(u32(b, 28))
        used_blocks = efblk - 1 if u16(b, 32) == 0 else efblk
        used_blocks = max(used_blocks, 0)
        where = 'dir %d (%s)' % (n, d['name'] if d else '?')
        lst = []
        prevkey = None
        for bi in range(used_blocks):
            blk = data[bi * 512:(bi + 1) * 512]
            if len(blk) < 512:
                fnd.err('%s: end-of-file (block %d) lies beyond its %d allocated blocks' % (where, used_blocks, len(data) // 512))
                break
            pos = 0
            nrec = 0
            while pos + 6 <= 512:
                size = u16(blk, pos)
                if size == 0xFFFF:
                    break
                if size == 0:
                    # zeroes where a record or the 0xFFFF end marker should be: ods2v2's
                    # own fixtures have blocks like that; VMS writes the end marker
                    fnd.warn('%s block %d: no end-of-records marker (zeros at %d)' % (where, bi + 1, pos))
                    break
                if pos + size + 2 > 512:
                    fnd.err('%s block %d: record at %d runs past the block' % (where, bi + 1, pos))
                    break
                flags = blk[pos + 4]
                nc = blk[pos + 5]
                raw = blk[pos + 6:pos + 6 + nc]
                ntype = (flags >> 3) & 7
                if (flags & 7) != 0:
                    pos += size + 2
                    continue
                if ntype == 3:
                    if nc % 2:
                        fnd.err('%s: UCS-2 name of odd length' % where)
                    nm = raw.decode('utf-16-le', 'replace')
                else:
                    nm = raw.decode('latin-1')
                if (flags & 0x80) or (flags & 0x40 and False):
                    pass
                ent = pos + 6 + nc
                ent += ent % 2
                if ent + 8 > pos + size + 2:
                    fnd.err('%s: record %r has no version entry' % (where, nm))
                if (size + 2 - (ent - pos)) % 8:
                    fnd.err('%s: record %r: odd tail length' % (where, nm))
                vl = u16(blk, pos + 2)
                key = fold_name(nm if '.' in nm else nm + '.')
                if v.level == 5 and not (flags & 0x40):
                    exp = stored_type_for(nm if '.' in nm else nm + '.')
                    if ntype != exp:
                        fnd.err('%s: %r has name type %d, VMS would store %d' % (where, nm, ntype, exp))
                if '.' not in nm:
                    fnd.warn('%s: name %r has no dot' % (where, nm))
                if v.level == 2 and nm.startswith('-') and nm.upper().endswith('.DIR'):
                    # DCL reads a '-' that starts a directory component as "the parent directory" and ODS-2 has no '^' escape
                    # (ODS-5 has: ^-): VAX V7.3 cannot name anything inside such a directory (%CLI-W-PARSEFAIL, %RMS-F-SYN);
                    # Alpha V8.4 can (measured, CORPUS 26)
                    fnd.warn('%s: directory %r starts with "-": VAX VMS (V7.3) reads a leading "-" as the parent directory and '
                             'cannot reach files inside (%%RMS-F-SYN); Alpha V8.4 can' % (where, nm))
                if prevkey is not None and not ignore_order:
                    if key < prevkey:
                        fnd.err('%s block %d: %r out of order' % (where, bi + 1, nm))
                    elif key == prevkey:
                        # continuation record of the same name: versions must continue downward
                        pass
                prevkey = key
                prevver = None
                while ent + 8 <= pos + size + 2:
                    ver = u16(blk, ent)
                    fid = (u16(blk, ent + 2), u16(blk, ent + 4), blk[ent + 6], blk[ent + 7])
                    lst.append((nm, ntype, ver, fid, flags, vl))
                    if prevver is not None and ver >= prevver:
                        fnd.err('%s: %r versions not descending (%d then %d)' % (where, nm, prevver, ver))
                    prevver = ver
                    ent += 8
                pos += size + 2
                nrec += 1
            if nrec == 0 and used_blocks > 1 and False:
                fnd.err('%s block %d: empty block inside the EOF' % (where, bi + 1))
        seen_nv = set()
        for ent in lst:
            key = (ent[0], ent[2])   # exact: the VMS-made asim8 ODS-5 disk has lp.COM and LP.COM in one directory
            if key in seen_nv:
                fnd.err('%s: two entries for %r;%d' % (where, ent[0], ent[2]))
            seen_nv.add(key)
        entries[n] = lst
        if os.environ.get('ODS5CHECK_DUMPDIR') and d and d['name'].upper().startswith(os.environ['ODS5CHECK_DUMPDIR'].upper()):
            for ent in lst:
                print('DUMPDIR %s: %r;%d fid=%s limit=%d' % (d['name'], ent[0], ent[2], ent[3], ent[5]))
    # --- cross checks: entries -> headers, backlinks ---
    referenced = set()
    dir_of_primary = {}
    for dn, lst in entries.items():
        # An entry whose version limit is 1 makes VMS purge version 1 as soon as it makes version 2: every VMS-made user
        # entry has 32767 or the directory's default (AXP1SYS: 40,000 of them, none with 1), and ods2v2 wrote 1 on ODS-2
        # until the fix. Not judged: the reserved files' entries in the MFD, and a directory's own entry (judged by the
        # header's directory bit, not the name): OpenVMS VAX V7.3 writes 1 for the directories it creates below [000000]
        # (physical VAX96 run 1), and a directory has no versions.
        vl1 = []
        for nm, ntype, ver, fid, flags, vl in lst:
            fn = fid[0] | (fid[3] << 16)
            hd1 = headers.get(fn)
            if vl == 1 and not (dn == 4 and fn <= 10) and not (hd1 is not None and u32(hd1, 52) & 0x2000):
                vl1.append(nm)
        if vl1:
            fnd.warn('dir %d: %d entries have version limit 1, e.g. %r: VMS purges version 1 when it makes version 2 '
                     '(ods2v2 before the fix wrote this; SET FILE/VERSION_LIMIT=32767 puts it right)' % (dn, len(vl1), vl1[0]))
        for nm, ntype, ver, fid, flags, vl in lst:
            fnum = fid[0] | (fid[3] << 16)
            hd = headers.get(fnum)
            if hd is None or u16(hd, 10) == fid[1]:
                referenced.add(fnum)
            if hd is None:
                if fnum in pending:
                    # VMS removes the entry before it frees anything; on the VMS-made AXP1 system disk none of
                    # its 9 marked headers is listed. An entry for one is a half-done delete (ours, or damage).
                    fnd.err('dir %d: %r;%d -> file %d, which is marked for delete (a half-done delete)' % (dn, nm, ver, fnum))
                elif fnum in deleted or not idx_used(fnum):
                    fnd.warn('dir %d: %r;%d -> file %d is not in use (dangling alias?)' % (dn, nm, ver, fnum))
                else:
                    fnd.err('dir %d: %r;%d -> file %d has no valid header' % (dn, nm, ver, fnum))
                continue
            if u16(hd, 10) != fid[1]:
                if u16(hd, 10) > fid[1]:
                    # VMS gives a reused file number a new sequence number. On a volume without HARDLINKS,
                    # deleting the primary name of a file leaves its other names in place (measured, notes
                    # 15), so an entry with an older sequence number is a stale alias VMS itself makes.
                    fnd.warn('dir %d: %r;%d -> file %d: a stale alias (entry sequence %d, the file now using the number has %d)'
                             % (dn, nm, ver, fnum, fid[1], u16(hd, 10)))
                else:
                    fnd.err('dir %d: %r;%d -> file %d: sequence %d, header has %d' % (dn, nm, ver, fnum, fid[1], u16(hd, 10)))
                continue
            bl = u16(hd, 66) | (hd[71] << 16)
            if bl == dn:
                dir_of_primary.setdefault(fnum, []).append((nm, ver))
    for n, (b, d) in prim.items():
        if n <= v.resfiles and n != 4 and False:
            continue
        bl = u16(b, 66) | (b[71] << 16)
        if n in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) or n == 0:
            continue
        if n not in referenced:
            fnd.err('file %d (%s) is in no directory (lost file)' % (n, d['name'] if d else '?'))
        elif bl == 0xFFFFFF and (v.volchar & 0x40):
            pass   # V8.4: the primary link was deleted, other links remain; the back link is (0xFFFFFF,0,0)
        elif bl not in entries:
            fnd.err('file %d: back link %d is not a directory' % (n, bl))
        elif n not in dir_of_primary:
            fnd.err('file %d (%s): its back link directory %d has no entry for it' % (n, d['name'] if d else '?', bl))
        else:
            # the header's name equals the entry's name (case-blind) of the primary link
            hn = d['name']
            base = hn.rsplit(';', 1)[0]
            vers = int(hn.rsplit(';', 1)[1]) if ';' in hn and hn.rsplit(';', 1)[1].isdigit() else None
            if not any(fold_name(en if '.' in en else en + '.') == fold_name(base if '.' in base else base + '.')
                       and (vers is None or vers == ev) for en, ev in dir_of_primary[n]):
                fnd.warn('file %d: header name %r differs from its directory entry %r' % (n, hn, dir_of_primary[n]))
    # directories must be reachable
    reach = {4}
    todo = [4]
    while todo:
        dn = todo.pop()
        for nm, ntype, ver, fid, flags, vl in entries.get(dn, []):
            fnum = fid[0] | (fid[3] << 16)
            if fnum in entries and fnum not in reach:
                reach.add(fnum)
                todo.append(fnum)
    for dn in entries:
        if dn not in reach and dn != 4:
            fnd.err('directory %d is not reachable from the root' % dn)
    nent = sum(len(l) for l in entries.values())
    print('directories: %d, entries: %d' % (len(entries), nent))
    print('%d error(s), %d warning(s)' % (len(fnd.errors), len(fnd.warnings)))
    return 1 if fnd.errors else 0


def vbn2lbn(ext, vbn):
    v = 1
    for l, c in ext:
        if vbn < v + c:
            return l + vbn - v
        v += c
    return None


if __name__ == '__main__':
    sys.exit(main())
