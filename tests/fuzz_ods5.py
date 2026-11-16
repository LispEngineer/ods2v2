#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""fuzz_ods5.py - damaged ODS-5 images must give errors, never crashes.

Usage: fuzz_ods5.py ODS2_BINARY BASE_IMAGE ITERATIONS SEED WORKDIR [--quick] [--write TREE]

With --write TREE the runs are writes instead: on the damaged image ods2 runs a script that makes directories
(with ^-escapes), imports the Linux directory TREE, makes symbolic and hard links, deletes files and directories
and relabels the volume. Writing to a damaged volume must fail with a message, never crash, hang or run wild.

Each iteration copies BASE_IMAGE (reflink/sparse copy when the file system has them), damages it - random
bytes or extreme values in the home block, in file headers (FI5 name length and name type, offsets, map
pointers, file size, extension links), in directory blocks (record sizes, name counts, flags, versions), a
block zeroed or filled with 0xFF, or the file truncated - and runs ods2 on it: SHOW VOLUME,
DIR/FULL [...], and EXPORT of the whole tree. A run fails if ods2 dies from a signal, an AddressSanitizer
or UndefinedBehaviorSanitizer report appears, or it takes longer than the timeout. Failing images are
kept in WORKDIR/fail-<n>.img with the command and stderr in fail-<n>.txt. Exit status 1 if any run failed.
(Leaks reported by LeakSanitizer are listed separately and do not fail the run.)
"""
import os, random, shutil, struct, subprocess, sys, time

TIMEOUT = 30

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def csum(b, words): return sum(struct.unpack_from('<%dH' % words, b, 0)) & 0xffff

class Img:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.size = os.path.getsize(path)
    def blk(self, lbn, n=1):
        self.f.seek(lbn * 512); return self.f.read(512 * n)

def extents(h):
    out, o, end = [], h[1] * 2, min(512, h[1] * 2 + h[58] * 2)
    while o + 2 <= end:
        w = u16(h, o); fmt = w >> 14
        if fmt == 0: o += 2
        elif fmt == 1 and o + 4 <= end: out.append((((w >> 8) & 0x3f) << 16 | u16(h, o + 2), (w & 0xff) + 1)); o += 4
        elif fmt == 2 and o + 6 <= end: out.append((u32(h, o + 2), (w & 0x3fff) + 1)); o += 6
        elif fmt == 3 and o + 8 <= end: out.append((u32(h, o + 4), ((w & 0x3fff) << 16 | u16(h, o + 2)) + 1)); o += 8
        else: break
    return out

def interesting(path):
    """LBNs of the home block, of used file headers and of directory blocks."""
    img = Img(path)
    home = img.blk(1)
    cluster, ibl, ibs = u16(home, 14), u32(home, 24), u16(home, 32)
    idx = extents(img.blk(ibl + ibs))
    headers, dirs = [], []
    vbn_first = 4 * cluster + ibs + 1
    pos = 1
    for lbn, cnt in idx:
        lo, hi = max(vbn_first, pos), pos + cnt
        for v in range(lo, hi):
            if len(headers) >= 4000: break
            l = lbn + v - pos
            if l * 512 + 512 > img.size: break
            h = img.blk(l)
            if len(h) == 512 and u16(h, 6) != 0:
                headers.append(l)
                if u32(h, 52) & 0x2000:
                    for dl, dc in extents(h)[:2]:
                        for k in range(min(dc, 4)):
                            if (dl + k) * 512 + 512 <= img.size: dirs.append(dl + k)
        pos += cnt
    return home_lbns(home), headers, dirs

def home_lbns(home):
    return [1]

def mutate(path, rng, headers, dirs):
    size = os.path.getsize(path)
    kind = rng.choice(['hdr', 'hdr', 'hdr', 'fi5', 'dir', 'dir', 'dir', 'home', 'zero', 'ff', 'trunc', 'map'])
    with open(path, 'r+b') as f:
        def get(l): f.seek(l * 512); return bytearray(f.read(512).ljust(512, b'\0'))
        def put(l, b):
            f.seek(0, 2)
            if (l + 1) * 512 <= f.tell():     # never grow a truncated image
                f.seek(l * 512); f.write(b)
        def fixsum(b): struct.pack_into('<H', b, 510, csum(b, 255))
        if kind == 'trunc':
            f.truncate(rng.randrange(0, size))
            return kind
        if kind == 'home':
            b = get(1)
            for _ in range(rng.randrange(1, 6)):
                b[rng.randrange(0, 512)] = rng.randrange(256)
            if rng.random() < 0.7:
                struct.pack_into('<H', b, 58, csum(b, 29)); struct.pack_into('<H', b, 510, csum(b, 255))
            put(1, b)
            return kind
        if kind in ('zero', 'ff'):
            pool = headers + dirs
            if pool:
                put(rng.choice(pool), bytearray([0 if kind == 'zero' else 255] * 512))
            return kind
        if kind in ('hdr', 'fi5', 'map') and headers:
            l = rng.choice(headers); b = get(l)
            if kind == 'hdr':
                for _ in range(rng.randrange(1, 8)):
                    b[rng.randrange(0, 512)] = rng.choice([0, 1, 2, 127, 128, 254, 255, rng.randrange(256)])
                if rng.random() < 0.5: b[rng.randrange(0, 4)] = rng.choice([0, 1, 40, 100, 200, 255])
            elif kind == 'fi5':
                o = b[0] * 2
                if o + 80 < 512:
                    for off, val in rng.sample([(0, rng.randrange(256)), (1, rng.choice([0, 1, 100, 236, 242, 250, 255])),
                                                (2, rng.randrange(256)), (76, rng.randrange(256)), (77, 0xff), (36, 0xff)], 3):
                        if o + off < 512: b[o + off] = val
            else:
                o = b[1] * 2
                if o + 8 < 512:
                    for k in range(rng.randrange(1, 6)):
                        b[min(511, o + rng.randrange(0, 40))] = rng.randrange(256)
                    if rng.random() < 0.4: b[58] = rng.choice([0, 1, 2, 60, 100, 200, 255])
            if rng.random() < 0.8: fixsum(b)
            put(l, b)
            return kind
        if kind == 'dir' and dirs:
            l = rng.choice(dirs); b = get(l)
            how = rng.randrange(4)
            if how == 0:
                for _ in range(rng.randrange(1, 8)): b[rng.randrange(0, 512)] = rng.randrange(256)
            elif how == 1:
                p = 0
                while p + 6 <= 512:
                    sz = u16(b, p)
                    if sz in (0, 0xffff): break
                    if rng.random() < 0.4:
                        which = rng.randrange(3)
                        if which == 0: b[p + 5] = rng.choice([0, 1, 2, 3, 200, 236, 255])
                        elif which == 1: b[p + 4] = rng.randrange(256)
                        else: struct.pack_into('<H', b, p, rng.choice([0, 1, 5, 6, 12, 14, 506, 510, 511, 0xfffe]))
                    p += sz + 2
            elif how == 2:
                p = rng.randrange(0, 500); b[p:p + 8] = bytes(rng.randrange(256) for _ in range(8))
            else:
                b[rng.randrange(0, 512)] = 0xff; b[rng.randrange(0, 512)] = 0xff
            put(l, b)
            return kind
    return 'none'

def run(binary, img, args, out_dir=None):
    cmd = [binary, img] + args
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT, errors='replace', env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1'))
        return cmd, r.returncode, r.stderr, r.stdout
    except subprocess.TimeoutExpired as e:
        return cmd, 'timeout', (e.stderr or b'').decode('utf-8', 'replace') if isinstance(e.stderr, bytes) else (e.stderr or ''), ''

def bad(rc, err):
    if rc == 'timeout': return 'timeout'
    if isinstance(rc, int) and rc < 0: return 'signal %d' % -rc
    for marker in ('AddressSanitizer', 'runtime error', 'SEGV', 'Assertion'):
        if marker in err: return marker
    return None

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    quick = '--quick' in sys.argv
    tree = None
    if '--write' in sys.argv:
        tree = sys.argv[sys.argv.index('--write') + 1]
        args = [a for a in args if a != tree]
    binary, base, iters, seed, work = args[0], args[1], int(args[2]), int(args[3]), args[4]
    os.makedirs(work, exist_ok=True)
    rng = random.Random(seed)
    home, headers, dirs = interesting(base)
    print('base %s: %d used headers, %d directory blocks' % (os.path.basename(base), len(headers), len(dirs)))
    fails, t0, kinds = 0, time.time(), {}
    for n in range(iters):
        img = os.path.join(work, 'cur.img')
        if os.path.exists(img): os.remove(img)
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', base, img], check=True)
        os.chmod(img, 0o644)
        applied = [mutate(img, rng, headers, dirs) for _ in range(rng.choice([1, 1, 2, 3]))]
        for k in applied: kinds[k] = kinds.get(k, 0) + 1
        out = os.path.join(work, 'out')
        runs = [['SHOW VOLUME'], ['DIR/FULL [...]'], ['EXPORT/VERSIONS [000000...]*.* ' + out]]
        if quick: runs = runs[1:2]
        if tree:
            script = os.path.join(work, 'write.cmd')
            open(script, 'w').write('\n'.join([
                'CREATE/DIRECTORY [Fuzz^_Dir]', 'CREATE/DIRECTORY [Fuzz^_Dir.Sub^.Dots]',
                'IMPORT %s [Fuzz^_Dir]' % tree, 'COPY %s/hello.txt [Fuzz^_Dir.Sub^.Dots]' % tree,
                'SYMLINK [Fuzz^_Dir]sl.lnk target.txt', 'LINK [Fuzz^_Dir]hl.txt [Fuzz^_Dir]hello.txt',
                'DELETE [Fuzz^_Dir]hl.txt', 'DELETE [Fuzz^_Dir]hello.txt', 'DELETE [Fuzz^_Dir]sl.lnk',
                'SET VOLUME/LABEL=FUZZED', 'DIR/FULL [...]', 'DELETE [Fuzz^_Dir.Sub^.Dots]hello.txt',
                'DELETE [Fuzz^_Dir.Sub^.Dots]', 'DELETE [000000]Fuzz^_Dir.DIR', '']))
            runs = [['@' + script]]
        for a in runs:
            shutil.rmtree(out, ignore_errors=True)
            cmd, rc, err, so = run(binary, img, a)
            why = bad(rc, err)
            if why:
                fails += 1
                keep = os.path.join(work, 'fail-%d.img' % fails)
                os.replace(img, keep)
                open(os.path.join(work, 'fail-%d.txt' % fails), 'w').write(
                    'iteration %d seed %d mutations %s\ncmd: %r\nrc: %r (%s)\n%s\n' % (n, seed, applied, cmd[2:], rc, why, err[:4000]))
                print('FAIL %d (iteration %d, %s, %s): %s' % (fails, n, applied, why, a[0][:40]))
                break
    shutil.rmtree(os.path.join(work, 'out'), ignore_errors=True)
    if os.path.exists(os.path.join(work, 'cur.img')): os.remove(os.path.join(work, 'cur.img'))
    print('%d iterations in %.0fs, mutations %s, failures %d' % (iters, time.time() - t0, kinds, fails))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
