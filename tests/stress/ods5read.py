# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""ods5read.py - an independent reader of an ODS-2 / ODS-5 disk image for the stress harness.

It shares no code with ods2v2: it reads the home block, INDEXF.SYS (with its extension headers), every file
header, every directory and every file's data straight from the image bytes, using only the structure
facts that tests/ods5check.py also uses (VSI definitions, measured on VMS-made images).  The harness
compares what it finds with its model of what the volume should hold; ods2v2's own reader is a second,
separate comparison.

read_volume(path) -> Volume with
    .dirs   {path tuple of display names: fid}           ([] = the root, fid 4)
    .files  {(dirpath, display name, version): fid}      (display name: as stored, a dot completed)
    .info   {fid: FileInfo}                               (size, md5, special type, link target, extents ...)
    .problems [str]                                        (things this reader could not make sense of)
    .free_clusters, .cluster, .size_blocks, .label, .nheaders_valid, .nheaders_ext
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import ods5check as C   # noqa: E402  (helpers only: Vol, decode_map, vbn2lbn, checksum)

u16, u32, swap32 = C.u16, C.u32, C.swap32


class BoundedVol(C.Vol):
    """C.Vol with every read bounded by the image: a damaged header can claim extents of 2^30 blocks
    (this once made the fuzz harness grow to 139 GB)"""
    def blk(self, lbn, n=1):
        if lbn < 0 or lbn >= self.size_blocks:
            return b'\0' * 512
        n = max(1, min(n, self.size_blocks - lbn))
        self.f.seek(lbn * 512)
        b = self.f.read(512 * n)
        return b + b'\0' * (512 * n - len(b))


MAX_STRUCT_BYTES = 32 * 1024 * 1024    # a directory / bitmap / index file larger than this is not read whole


class _F:
    def err(self, m):
        raise ValueError(m)


class FileInfo:
    def __init__(self):
        self.size = 0
        self.md5 = None
        self.special = 0        # FAT special type (5 = symbolic link) when file organisation is 4
        self.target = None      # symbolic link target (bytes)
        self.hiblk = self.efblk = self.ffbyte = 0
        self.extents = []
        self.directory = False
        self.volatile = False   # reserved files: contents change with the volume
        self.linkcount = 0
        self.level = 0


class Volume:
    pass


def _decode_name(raw, ntype):
    if ntype == 3:
        return raw.decode('utf-16-le', 'replace')
    return raw.decode('latin-1')


def read_volume(path, want_md5=True):
    v = BoundedVol(path)
    out = Volume()
    out.problems = []
    out.cluster = v.cluster
    out.size_blocks = v.size_blocks
    out.label = v.label
    out.level = v.level
    out.volchar = v.volchar
    out.dirs, out.files, out.info = {}, {}, {}
    out.dir_aliases = []
    out.free_clusters = None
    out.indexf_blocks = 0
    out.ext_files = 0
    out.max_extents = 0
    fnd = _F()

    base = v.ibmaplbn + v.ibmapsize
    h1 = v.blk(base)
    idx = C.decode_map(h1, fnd, 'INDEXF.SYS')
    hb = h1
    for _ in range(64):
        efn = u16(hb, 14) | (hb[19] << 16)
        if efn == 0:
            break
        lbn = C.vbn2lbn(idx, 4 * v.cluster + v.ibmapsize + efn)
        if lbn is None:
            out.problems.append('INDEXF.SYS extension header %d not mapped' % efn)
            break
        hb = v.blk(lbn)
        idx += C.decode_map(hb, fnd, 'INDEXF.SYS ext')
    hdr_vbn0 = 4 * v.cluster + v.ibmapsize
    nheaders = swap32(u32(h1, 28)) - 1 - hdr_vbn0
    nheaders = max(0, min(nheaders, sum(c for _, c in idx) - hdr_vbn0, 1 << 20))   # a damaged EOF must not make this loop 2^32 times

    def data_blocks(ext, first_vbn, nblk):
        """bytes of nblk blocks from file VBN first_vbn"""
        got = []
        vb = 1
        nblk = min(nblk, MAX_STRUCT_BYTES // 512)
        need_end = first_vbn + nblk
        for l, c in ext:
            lo, hi = max(vb, first_vbn), min(vb + c, need_end)
            if lo < hi:
                got.append(v.blk(l + lo - vb, hi - lo))
            vb += c
            if vb >= need_end:
                break
        return b''.join(got)

    heads = {}
    for n in range(1, nheaders + 1):
        lbn = C.vbn2lbn(idx, hdr_vbn0 + n)
        if lbn is None:
            continue
        b = v.blk(lbn)
        fn = u16(b, 8) | (b[13] << 16)
        if b[7] in (2, 5) and fn == n and C.checksum(b) == u16(b, 510):
            heads[n] = b
    out.nheaders_valid = len(heads)
    out.nheaders_ext = 0

    prim = {}
    for n, b in heads.items():
        if u16(b, 4) != 0:
            out.nheaders_ext += 1
            continue
        ext = C.decode_map(b, fnd, 'file %d' % n)
        cur, segs = b, [n]
        while True:
            efn = u16(cur, 14) | (cur[19] << 16)
            if efn == 0:
                break
            nb = heads.get(efn)
            if nb is None or efn in segs or len(segs) > 100:
                out.problems.append('file %d: extension chain broken at %d' % (n, efn))
                break
            ext += C.decode_map(nb, fnd, 'file %d ext' % n)
            segs.append(efn)
            cur = nb
        fi = FileInfo()
        fi.level = b[7]
        fi.extents = ext
        fi.hiblk = swap32(u32(b, 24))
        fi.efblk = swap32(u32(b, 28))
        fi.ffbyte = u16(b, 32)
        fi.directory = bool(u32(b, 52) & 0x2000)
        fi.linkcount = u16(b, 74)
        fi.rtype = b[20]
        fi.rattrib = b[21]
        fi.rsize = u16(b, 22)
        fi.maxrec = u16(b, 36)
        fi.fileorg = b[20] >> 4
        if fi.fileorg == 4:
            fi.special = b[21]
        # size by the end-of-file mark: blocks before EFBLK, plus FFBYTE bytes of block EFBLK
        full = max(fi.efblk - 1, 0)
        fi.size = full * 512 + (fi.ffbyte if fi.efblk >= 1 else 0)
        if fi.efblk == 0 and fi.hiblk > 0:
            fi.size = fi.hiblk * 512   # indexed files carry no EOF mark (EFBLK 0): all allocated blocks, as stored
        if fi.efblk > fi.hiblk + 1:
            out.problems.append('file %d: EFBLK beyond HIBLK+1' % n)
        fi.volatile = n <= 10
        out.info[n] = fi
        prim[n] = (b, fi)

    def file_md5(fi):
        h = hashlib.md5()
        left, vb = fi.size, 1
        for l, c in fi.extents:
            while c > 0 and left > 0:
                n = min(c, 2048)
                chunk = v.blk(l, n)
                take = min(len(chunk), left)
                h.update(chunk[:take])
                left -= take
                l += n
                c -= n
            if left <= 0:
                break
        return h.hexdigest(), (None if left > 0 else True)

    def file_bytes(fi):
        nb = (min(fi.size, 1 << 20) + 511) // 512
        return data_blocks(fi.extents, 1, nb)[:min(fi.size, 1 << 20)]

    # directories
    entries = {}
    for n, (b, fi) in prim.items():
        if not fi.directory:
            continue
        used = fi.efblk - 1 if fi.ffbyte == 0 else fi.efblk
        data = data_blocks(fi.extents, 1, max(used, 0))
        lst = []
        for bi in range(max(used, 0)):
            blk = data[bi * 512:(bi + 1) * 512]
            pos = 0
            while pos + 6 <= 512:
                size = u16(blk, pos)
                if size == 0xFFFF or size == 0:
                    break
                if pos + size + 2 > 512:
                    out.problems.append('dir %d block %d: record past the block' % (n, bi + 1))
                    break
                flags = blk[pos + 4]
                nc = blk[pos + 5]
                nm = _decode_name(blk[pos + 6:pos + 6 + nc], (flags >> 3) & 7)
                ent = pos + 6 + nc
                ent += ent % 2
                if (flags & 7) == 0:
                    while ent + 8 <= pos + size + 2:
                        fid = u16(blk, ent + 2) | (blk[ent + 7] << 16)
                        lst.append((nm, u16(blk, ent), fid, bool(flags & 0x40)))
                        ent += 8
                pos += size + 2
        entries[n] = lst

    # walk from the root (fid 4); a directory reachable by two names (VMS-made corpus has one) is walked under both
    if 4 not in entries:
        out.problems.append('no root directory')
        return out
    out.dirs[()] = 4
    todo = [((), 4, (4,))]
    nvisit = 0
    while todo:
        dpath, dfid, anc = todo.pop()
        nvisit += 1
        if nvisit > 200000:
            out.problems.append('directory walk too long')
            break
        seen_names = set()
        for nm, ver, fid, special in entries.get(dfid, []):
            if (nm.upper(), ver) in seen_names:
                out.problems.append('dir %r: name %r;%d listed twice' % (dpath, nm, ver))
            seen_names.add((nm.upper(), ver))
            fi = out.info.get(fid)
            if fi is None:
                out.files[(dpath, nm, ver)] = None   # dangling entry
                continue
            if fi.directory and nm.upper().endswith('.DIR') and ver == 1:
                if fid == 4 and dfid == 4 and nm.upper() == '000000.DIR':
                    continue   # the root's own entry
                sub = dpath + (nm[:-4],)
                if fid in anc:
                    out.problems.append('directory loop at %r' % (sub,))
                    continue
                if fid in out.dirs.values():
                    out.dir_aliases.append(sub)
                out.dirs[sub] = fid
                todo.append((sub, fid, anc + (fid,)))
                continue
            if (dpath, nm, ver) in out.files:
                out.problems.append('duplicate directory entry %r' % ((dpath, nm, ver),))
            out.files[(dpath, nm, ver)] = fid
    for fid, fi in out.info.items():
        if fi.directory or fid in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10):
            continue
        if fi.md5 is None and want_md5 and fid in set(f for f in out.files.values() if f):
            fi.md5, _ = file_md5(fi)
            if fi.special == 5:
                fi.target = file_bytes(fi)
    out.free_clusters = None
    out.indexf_blocks = sum(c for _, c in idx)
    out.ext_files = sum(1 for n, (b, fi) in prim.items() if len(fi.extents) > 0 and (u16(b, 14) | (b[19] << 16)) != 0)
    out.max_extents = max((len(fi.extents) for (b, fi) in prim.values()), default=0)
    # storage bitmap free count (independent of the checker's loop)
    bm = prim.get(2)
    if bm:
        bd = data_blocks(bm[1].extents, 1, min(sum(c for _, c in bm[1].extents), MAX_STRUCT_BYTES // 512))
        nclusters = min(v.size_blocks // v.cluster, (len(bd) - 512) * 8)
        free = 0
        bits = bd[512:]
        for i in range(nclusters):
            if bits[i // 8] >> (i % 8) & 1:
                free += 1
        out.free_clusters = free
    return out
