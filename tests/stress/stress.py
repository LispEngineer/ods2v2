#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""stress.py - seeded, randomized, differential stress harness for ods2v2 (task S1).

Keeps a MODEL of the tree and the bytes the volume must hold, applies random sequences of ods2 commands to a
volume (fresh ODS-2 / ODS-5 made by ods2_init, or a COPY of a VMS-made image) and after every operation checks:

  1. tests/ods5check.py reports 0 errors, 0 warnings;
  2. tests/stress/ods5read.py (an independent reader: it shares no code with ods2v2) finds exactly the model's
     directories, names, versions, sizes, MD5 sums, symbolic-link targets and link groups;
  3. ods2's own EXPORT/LINKS/VERSIONS gives the same bytes (every few steps when the data is large);
  4. SHOW VOLUME's free blocks = the bitmap's (read independently);
  5. a command that failed left the model's world unchanged (the checks above prove it), and a command that
     the model says must succeed (or must be refused) did.

Expected values come from the model (its own rules written from the VMS behaviour: case-blind names, 39.39 /
236 / 118 name limits, version rules) and from VMS-made images, never from ods2v2's output.  Every run is
reproducible from its seed:  stress.py --seed N [--profile P] [--config C].

  stress.py --seeds 1-50 --jobs 8 --ops 60          # many seeds in parallel
  stress.py --seed 17 --profile mix --config f5     # one seed, verbose, work files kept on failure
"""
import argparse, hashlib, json, os, random, re, shutil, subprocess, sys, time, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import ods5read   # noqa: E402

ODS2 = os.environ.get('ODS2', os.path.join(ROOT, 'ods2'))
ODS2_INIT = os.environ.get('ODS2_INIT', os.path.join(ROOT, 'ods2_init'))
CHECKER = os.path.join(ROOT, 'tests', 'ods5check.py')
CORPUS = os.path.expanduser('~/tmp/ods5-cli')
WORK = os.environ.get('STRESS_WORK', os.path.expanduser('~/tmp/ods5-st/work'))

ENV = dict(os.environ)
ENV['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=0:exitcode=77'
ENV['UBSAN_OPTIONS'] = 'print_stacktrace=1:halt_on_error=1:exitcode=78'
ENV['TZ'] = 'UTC'

VMS_BASELINES = ['asim12-ods5t-20261108', 'asim12-ods2t-20261108', 'asim6-ods5t6-20261108', 'asim6-ods2t6-20261108',
                 'w1c-f5-after-asim12', 'w1c-f2-after-asim12', 'w1-ods5t-after-asim12', 'w1-ods2t-after-asim12',
                 'm1-hl-after-asim12', 'm1-nohl-after-asim12', 'm1-ods2-after-asim12', 'm23-after-asim12',
                 'm2b-after-asim12', 'phvs2-ods2v-20261108', 'w1c-f5-after-asim6', 'w1-ods5t6-after-asim6']


class EscRng:
    def choice(self, seq):
        return seq[0]

    def random(self):
        return 1.0


class Failure(Exception):
    pass


# ---------------------------------------------------------------- names

def fold(c):
    u = c.upper()
    return u if len(u) == 1 else c


def fold_str(s):
    return ''.join(fold(c) for c in s)


def complete(display):
    return display if '.' in display else display + '.'


def key_of(display):
    return fold_str(complete(display))


ISL1_POOL = 'àáâãäåæçèéêëìíîïðñòóôõöøùúûüýþÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖØÙÚÛÜÝÞ¡¢£¤¥¦§¨©ª«¬®¯°±²³´¶·¸¹º»¼½¾¿'
UCS2_POOL = 'αβγδεζηθικλμνξοπρστυφχψωΑΒΓΔΩБВГДЖЗИЙКЛМНПРСТУФЦЧШЩЭЮЯабвгдежзийклмнопрстуфцчшщэюя一二三四五六七八九十あいうえおアイウエオ한글日本語'
HARD_SPECIAL = " ~!@&()+=,'"
SOFT_SPECIAL = "[];^%"
WORDS = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta', 'eta', 'theta', 'iota', 'kappa', 'lambda', 'mu',
         'The', 'quick', 'brown', 'fox', 'jumps', 'over', 'lazy', 'dog', 'VMS', 'Files-11', 'ODS-5', '12345', '$HOME']
ODS2_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$_-'


class NameGen:
    def __init__(self, rng, ods5):
        self.rng = rng
        self.ods5 = ods5
        self.n = 0

    def _part(self, lo, hi, chars):
        n = self.rng.randint(lo, hi)
        return ''.join(self.rng.choice(chars) for _ in range(n))

    def file_name(self, kind=None):
        """-> (display, strict) display name as it will be stored (a dot completed). strict: must be accepted"""
        r = self.rng
        self.n += 1
        if not self.ods5:
            stem = self._part(1, 12, ODS2_CHARS)
            ty = self._part(0, 5, ODS2_CHARS) if r.random() < 0.9 else ''
            if r.random() < 0.05:
                stem = self._part(36, 39, ODS2_CHARS)
                ty = self._part(36, 39, ODS2_CHARS)
            if ty == 'DIR':
                ty = 'DAT'
            return stem + '.' + ty, True
        kind = kind or r.choices(['plain', 'mixed', 'special', 'isl1', 'ucs2', 'dots', 'soft', 'long'],
                                 [30, 25, 10, 12, 8, 6, 3, 2])[0]
        alnum = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$_-'
        strict = True
        if kind == 'plain':
            stem, ty = self._part(1, 14, ODS2_CHARS), self._part(0, 4, ODS2_CHARS)
        elif kind == 'mixed':
            stem, ty = self._part(1, 14, alnum), self._part(0, 4, alnum)
        elif kind == 'special':
            stem = self._part(1, 6, alnum) + self._part(1, 3, HARD_SPECIAL) + self._part(0, 6, alnum)
            ty = self._part(0, 4, alnum)
        elif kind == 'isl1':
            stem = self._part(0, 4, alnum) + self._part(1, 4, ISL1_POOL) + self._part(0, 4, alnum)
            ty = self._part(0, 3, alnum + ISL1_POOL)
        elif kind == 'ucs2':
            stem = self._part(0, 4, alnum) + self._part(1, 6, UCS2_POOL) + self._part(0, 3, alnum)
            ty = self._part(0, 3, alnum + UCS2_POOL)
        elif kind == 'dots':
            stem = self._part(1, 5, alnum) + '.' + self._part(1, 5, alnum) + '.' + self._part(1, 5, alnum)
            ty = self._part(0, 3, alnum)
        elif kind == 'soft':
            stem = self._part(1, 4, alnum) + self._part(1, 2, SOFT_SPECIAL) + self._part(0, 4, alnum)
            ty = self._part(0, 3, alnum)
            strict = False
        else:  # long: up to the limit
            pool = alnum if r.random() < 0.5 else alnum + UCS2_POOL
            ucs = any(ord(c) > 255 for c in pool)
            limit = 118 if ucs else 236
            total = r.choice([limit, limit - 1, limit - 2, limit - 30])
            tlen = r.randint(0, 6)
            stem = self._part(max(total - tlen - 1, 1), max(total - tlen - 1, 1), pool)
            ty = self._part(tlen, tlen, pool)
            if ucs and not any(ord(c) > 255 for c in stem + ty):
                stem = stem[:-1] + 'ж'
        if ty == 'DIR' or ty.upper() == 'DIR':
            ty = 'DAT'
        if stem == '' and ty == '':
            stem = 'X'
        d = stem + '.' + ty
        # name must not look like a directory's
        return d, strict

    def dir_name(self):
        if not self.ods5:
            return self._part(1, 12, ODS2_CHARS)
        r = self.rng
        kind = r.choices(['plain', 'mixed', 'special', 'isl1', 'ucs2', 'dots'], [35, 25, 8, 12, 8, 6])[0]
        alnum = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$_-'
        if kind == 'plain':
            return self._part(1, 10, ODS2_CHARS)
        if kind == 'mixed':
            return self._part(1, 10, alnum)
        if kind == 'special':
            return self._part(1, 4, alnum) + self._part(1, 2, HARD_SPECIAL) + self._part(1, 4, alnum)
        if kind == 'isl1':
            return self._part(1, 3, alnum) + self._part(1, 3, ISL1_POOL)
        if kind == 'ucs2':
            return self._part(0, 3, alnum) + self._part(1, 4, UCS2_POOL)
        return self._part(1, 4, alnum) + '.' + self._part(1, 4, alnum)


def esc_char(c, rng, lowercase_ok=False):
    o = ord(c)
    if c.isascii() and (c.isalnum() or c in '$_-'):
        if lowercase_ok and rng.random() < 0.3:
            return c.lower()
        return c
    if c == ' ':
        return '^_'
    if o < 128:
        return '^' + c
    if o <= 0xFF:
        return rng.choice(['^%02X' % o, c])
    return rng.choice(['^U%04X' % o, c])


def spec_dir_name(name, rng, ods5):
    out = ''.join(('^.' if c == '.' else esc_char(c, rng, not ods5)) for c in name)
    return '^-' + out[1:] if out.startswith('-') else out   # a leading '-' in [..] means "up one level" 


def spec_file_name(display, rng, ods5):
    stem, _, ty = display.rpartition('.')
    return (''.join(('^.' if c == '.' else esc_char(c, rng, not ods5)) for c in stem) + '.' +
            ''.join(esc_char(c, rng, not ods5) for c in ty))


def spec_dir(path, rng, ods5):
    if not path:
        return '[000000]'
    return '[' + '.'.join(spec_dir_name(n, rng, ods5) for n in path) + ']'


_HEX = set('0123456789abcdefABCDEF')


def _special(cp):
    return cp == 0x2F or cp < 0x20 or cp == 0x7F or 0x80 <= cp <= 0x9F


def linux_escape(name):
    # EXPORT's rule (README, "Linux names"): '/', C0 controls, DEL, C1 controls -> %XX; a '%' that is followed by
    # two hex digits that would read back as one of those escapes -> %25
    out = []
    for i, c in enumerate(name):
        o = ord(c)
        if _special(o):
            out.append('%%%02X' % o)
        elif c == '%' and i + 2 < len(name) + 0 and name[i + 1] in _HEX and name[i + 2] in _HEX and _special(int(name[i + 1:i + 3], 16)):
            out.append('%25')
        else:
            out.append(c)
    return ''.join(out)


def linux_name(display):
    # EXPORT / IMPORT: a lone trailing dot (no other dot in the name) is dropped; "a.b." keeps its dot
    return display[:-1] if display.endswith('.') and '.' not in display[:-1] else display


# ---------------------------------------------------------------- content

def text_lines(rng, nbytes):
    out = []
    n = 0
    while n < nbytes:
        line = ' '.join(rng.choice(WORDS) for _ in range(rng.randint(0, 14)))
        out.append(line)
        n += len(line) + 1
    return out


def gen_content(rng, size, kind):
    """-> bytes of exactly `size` bytes (kind says the line-end style)"""
    if size == 0:
        return b''
    if kind in ('lf', 'crlf', 'cr', 'mixed', 'nonl'):
        lines = text_lines(rng, size + 16)
        if kind == 'lf' or kind == 'nonl':
            data = '\n'.join(lines).encode()
        elif kind == 'crlf':
            data = '\r\n'.join(lines).encode()
        elif kind == 'cr':
            data = '\r'.join(lines).encode()
        else:
            data = ''.join(l + rng.choice(['\n', '\r\n']) for l in lines).encode()
        data = (data * (size // max(len(data), 1) + 1))[:size]
        if kind != 'nonl' and kind != 'cr' and size > 0 and data[-1:] not in (b'\n', b'\r'):
            data = data[:-1] + b'\n' if kind != 'crlf' else data[:-1] + b'\n'
        return data
    if kind == 'zero':
        return bytes(size)
    if kind == 'ff':
        return b'\xff' * size
    if kind == 'tagged':   # every 512-byte block carries its own number: a misplaced block shows
        blocks = []
        for i in range((size + 511) // 512):
            blocks.append(('%08X' % i).encode() + rng.randbytes(504))
        return b''.join(blocks)[:size]
    return rng.randbytes(size)   # 'bin'


SIZES_SMALL = [0, 1, 2, 3, 127, 128, 255, 256, 509, 510, 511, 512, 513, 514, 1000, 1023, 1024, 1025, 1535, 1536,
               1537, 2047, 2048, 4095, 4096, 4097, 8191, 8192, 8193, 32767, 32768, 32769, 65535, 65536, 65537]
KINDS = ['lf', 'lf', 'crlf', 'cr', 'mixed', 'nonl', 'bin', 'bin', 'zero', 'ff', 'tagged']


def gen_size(rng, big_ok):
    r = rng.random()
    if r < 0.55:
        return rng.choice(SIZES_SMALL)
    if r < 0.85 or not big_ok:
        return rng.randint(1, 20000)
    if r < 0.97:
        return rng.randint(20000, 400000)
    return rng.randint(1_000_000, 6_000_000)


def max_line(data, sep):
    return max((len(x) for x in data.split(sep)), default=0)


def var_records(data):
    """what COPY/CONVERT stores: one record per line (LF ends it, a CR just before the LF is dropped; a last line
    with no LF keeps whatever it ends with)"""
    if not data:
        return b''
    lines = data.split(b'\n')
    last = lines.pop()
    out = bytearray()
    for i, ln in enumerate(lines + ([last] if last else [])):
        if i < len(lines) and ln.endswith(b'\r'):
            ln = ln[:-1]
        out += len(ln).to_bytes(2, 'little') + ln
        if len(ln) % 2:
            out += b'\0'
    return bytes(out)


# ---------------------------------------------------------------- model

class Obj:
    __slots__ = ('kind', 'md5', 'size', 'target', 'volatile', 'name')

    def __init__(self, kind='file', md5=None, size=0, target=None, volatile=False):
        self.kind, self.md5, self.size, self.target, self.volatile = kind, md5, size, target, volatile


class Model:
    def __init__(self, ods5, hardlinks):
        self.ods5 = ods5
        self.hardlinks = hardlinks
        self.dirs = {(): ()}          # folded path -> display path
        self.ents = {}                # (folded dir path, key, ver) -> (display, Obj)
        self.frozen = []              # display path prefixes the harness must not touch
        self.secondary = set()        # entry keys that are non-primary aliases made by us

    # lookups
    def fkpath(self, path):
        return tuple(fold_str(p) for p in path)

    def has_dir(self, path):
        return self.fkpath(path) in self.dirs

    def find_dir(self, path):
        return self.dirs.get(self.fkpath(path))

    def entries_in(self, path):
        fp = self.fkpath(path)
        return [(k, v) for k, v in self.ents.items() if k[0] == fp]

    def is_frozen(self, path):
        p = tuple(path)
        return any(p[:len(f)] == f for f in self.frozen)

    def name_taken(self, path, display):
        """a file entry with this name (any version), or a directory of the same name, in dir `path`"""
        fp = self.fkpath(path)
        k = key_of(display)
        for (d, key, ver) in self.ents:
            if d == fp and key == k:
                return True
        if k.endswith('.') and (fp + (k[:-1],)) in self.dirs:
            return True     # file "A." and directory "A" export to the same Linux name
        if (fp + (fold_str(display.rstrip('.')),)) in self.dirs and '.' not in display.rstrip('.'):
            return True
        if k.endswith('.DIR'):
            return True
        return False

    def dir_taken(self, path, name):
        fp = self.fkpath(path)
        fk = fold_str(name)
        if fp + (fk,) in self.dirs:
            return True
        for (d, key, ver) in self.ents:
            if d == fp and (key == fk + '.' or key == fk + '.DIR'):
                return True
        return False

    def add_file(self, path, display, obj):
        self.ents[(self.fkpath(path), key_of(display), 1)] = (display, obj)

    def add_dir(self, path, name):
        full = tuple(path) + (name,)
        self.dirs[self.fkpath(full)] = full

    def remove_dir(self, path):
        del self.dirs[self.fkpath(path)]

    def expected_entries(self):
        out = {}
        for (fp, key, ver), (display, obj) in self.ents.items():
            out[(self.dirs[fp], display, ver)] = obj
        return out

    def objects(self):
        return {id(o): o for (_, o) in self.ents.values()}


def model_from_volume(vol, ods5, hardlinks, skip_dangling=False):
    m = Model(ods5, hardlinks)
    for dp in sorted(vol.dirs, key=len):
        if dp:
            m.dirs[m.fkpath(dp)] = dp
    fobj = {}
    for (dpath, nm, ver), fid in vol.files.items():
        if fid is None:
            if skip_dangling:
                continue
            raise Failure('baseline has a dangling entry %r' % ((dpath, nm, ver),))
        fi = vol.info[fid]
        if fid not in fobj:
            if fi.special == 5:
                fobj[fid] = Obj('symlink', target=fi.target, volatile=False)
            else:
                fobj[fid] = Obj('file', md5=fi.md5, size=fi.size, volatile=fi.volatile)
        m.ents[(m.fkpath(dpath), key_of(nm), ver)] = (nm, fobj[fid])
    for dp in vol.dir_aliases:
        m.frozen.append(dp)
    # a directory that is the target of an alias is reachable by 2 paths: freeze both
    byfid = {}
    for dp, fid in vol.dirs.items():
        byfid.setdefault(fid, []).append(dp)
    for fid, ps in byfid.items():
        if len(ps) > 1:
            m.frozen.extend(ps)
    # entries whose file has several names on a volume that is not hard-linked: do not delete (primary unknown)
    return m


# ---------------------------------------------------------------- running ods2

class Res:
    def __init__(self, rc, out, err):
        self.rc, self.out, self.err = rc, out, err
        self.text = (out + err).decode('utf-8', 'replace') if isinstance(out, bytes) else out + err
        self.crash = (rc not in (0, 1)) or ('Sanitizer' in self.text) or ('runtime error' in self.text)
        self.ok = rc == 0 and '%ODS2-E-' not in self.text and '%ODS2-F-' not in self.text


def run_ods2(img, *cmd, timeout=600, stdin=None):
    r = subprocess.run([ODS2, img] + list(cmd), capture_output=True, env=ENV, timeout=timeout, input=stdin)
    return Res(r.returncode, r.stdout, r.stderr)


def run_batch(img, cmds, workdir, timeout=1800):
    """run a script of commands in one ods2 process. stdout and stderr go to one pty so that their order is the
    order the program wrote them in (a pipe would make stdout block-buffered and reorder it behind stderr)"""
    import pty, select
    path = os.path.join(workdir, 'batch.cmd')
    with open(path, 'w', encoding='utf-8') as f:
        for c in cmds:
            f.write(c + '\n')
    master, slave = pty.openpty()
    p = subprocess.Popen([ODS2, img, '@' + path], stdout=slave, stderr=slave, stdin=subprocess.DEVNULL, env=ENV, close_fds=True)
    os.close(slave)
    chunks = []
    t0 = time.time()
    while True:
        r, _, _ = select.select([master], [], [], 1.0)
        if r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            chunks.append(d)
        elif p.poll() is not None:
            break
        if time.time() - t0 > timeout:
            p.kill()
            break
    os.close(master)
    rc = p.wait()
    text = b''.join(chunks).decode('utf-8', 'replace').replace('\r\n', '\n')
    res = []
    cur = None
    for line in text.split('\n'):
        if line.startswith('ODS2> '):
            cur = [line[6:], []]
            res.append(cur)
        elif cur is not None:
            cur[1].append(line)
    crash = (rc not in (0, 1)) or 'Sanitizer' in text or 'runtime error' in text
    outs = []
    for c, lines in res:
        t = '\n'.join(lines)
        outs.append((c, '%ODS2-E-' not in t and '%ODS2-F-' not in t, t))
    return outs, crash, text, rc


def run_checker(img):
    r = subprocess.run([sys.executable, '-I', CHECKER, img], capture_output=True, text=True, env=ENV)
    out = r.stdout + r.stderr
    m = re.search(r'(\d+) error\(s\), (\d+) warning\(s\)', out)
    free = re.search(r'storage: (\d+) clusters, (\d+) free', out)
    return (m.group(0) if m else 'CHECKER CRASHED: ' + out[-400:]), out, (int(free.group(2)) if free else None)


# ---------------------------------------------------------------- the harness

class Harness:
    def __init__(self, seed, profile, config, nops, workdir, verbose=False, check_every=1, big=True):
        self.seed, self.profile, self.config = seed, profile, config
        self.rng = random.Random(seed * 1000003 + 17)
        self.nops = nops
        self.dir = workdir
        self.verbose = verbose
        self.check_every = check_every
        self.big = big
        self.log = []
        self.opno = 0
        self.src = os.path.join(workdir, 'src')
        os.makedirs(self.src, exist_ok=True)
        self.img = os.path.join(workdir, 'vol.img')
        self.ncopy = 0
        self.free_blocks = None
        self.cluster = None
        self.last_ok = True
        self.stop = None
        self.opts = {}

    # ---- logging
    def note(self, s):
        self.log.append(s)
        if self.verbose:
            print('  ' + s[:300], flush=True)

    def fail(self, msg):
        self.note('FAIL: ' + msg)
        raise Failure(msg)

    # ---- volume setup
    def setup(self):
        r = self.rng
        cfg = self.config
        if cfg.startswith('vms:'):
            name = cfg[4:]
            src = os.path.join(CORPUS, name + '.img')
            subprocess.check_call(['cp', '--reflink=auto', src, self.img])
            os.chmod(self.img, 0o644)
            self.ods5 = ods5read.C.Vol(self.img).level == 5
            self.note('baseline copy of %s' % name)
        else:
            self.ods5 = cfg.startswith('f5')
            sizes = {'fill': [8192, 8200, 9973, 16384, 20000], 'frag': [8192, 9000, 16384], 'bigdir': [65536, 131072],
                     'deep': [16384, 32768], 'indexf': [32768, 65536], 'bigfile': [131072, 262144, 400000], 'shatter': [8192, 9000],
                     }.get(self.profile, [8192, 8200, 9973, 16384, 20000, 32768, 65536, 131072, 262144])
            size = r.choice(sizes)
            size = self.opts.get('size', size)
            args = [ODS2_INIT, self.img, str(size), '--structure=%d' % (5 if self.ods5 else 2),
                    '--label=STRESS%d' % (self.seed % 1000)]
            cl = r.choice([1, 2, 3, 4, 8, 16]) if self.profile not in ('frag', 'shatter') else r.choice([1, 1, 1, 2, 3])
            use_cl = (r.random() < 0.6) or self.profile in ('frag', 'shatter')
            if 'cluster' in self.opts:
                cl, use_cl = self.opts['cluster'], True
            if use_cl:
                args.append('--cluster=%d' % cl)
            hl = r.random() < 0.5
            hl = self.opts.get('hl', hl)
            if self.ods5 and hl:
                args.append('--hardlinks')
            if r.random() < 0.2:
                args.append('--nohighwater')
            if self.ods5 and r.random() < 0.2:
                args.append('--access-dates')
            if self.profile == 'indexf':
                args.append('--maxfiles=%d' % r.choice([300, 600, 1500]))
            self.note('init: ' + ' '.join(args[1:]))
            res = subprocess.run(args, capture_output=True, env=ENV, text=True)
            if res.returncode != 0:
                self.fail('ods2_init failed: ' + res.stdout + res.stderr)
        vol = ods5read.read_volume(self.img)
        self.hardlinks = bool(vol.volchar & 0x40)
        self.cluster = vol.cluster
        self.model = model_from_volume(vol, self.ods5, self.hardlinks)
        self.baseline_keys = set(self.model.ents)
        self.baseline_dirs = set(self.model.dirs.values())
        self.names = NameGen(r, self.ods5)
        self.note('volume: ODS-%d cluster %d blocks %d hardlinks %s files %d dirs %d'
                  % (5 if self.ods5 else 2, vol.cluster, vol.size_blocks, self.hardlinks, len(vol.files), len(vol.dirs)))
        self.verify('baseline')

    # ---- sources
    def write_src(self, data, name=None):
        self.ncopy += 1
        p = os.path.join(self.src, name or ('s%05d.dat' % self.ncopy))
        with open(p, 'wb') as f:
            f.write(data)
        return p

    # ---- verification
    def verify(self, tag, export=None):
        t0 = time.time()
        summary, out, free = run_checker(self.img)
        if summary != '0 error(s), 0 warning(s)':
            self.fail('%s: checker: %s\n%s' % (tag, summary, '\n'.join(l for l in out.split('\n') if l.startswith(('ERROR', 'WARN')))[:3000]))
        vol = ods5read.read_volume(self.img)
        if vol.problems:
            self.fail('%s: independent reader: %s' % (tag, vol.problems[:5]))
        self.compare_model(tag, vol)
        sv = run_ods2(self.img, 'SHOW VOLUME')
        m = re.search(r'Free blocks:\s+(\d+)', sv.text)
        if sv.crash or not m:
            self.fail('%s: SHOW VOLUME: %s' % (tag, sv.text[:500]))
        self.free_blocks = int(m.group(1))
        if vol.free_clusters is not None:
            exp = vol.free_clusters * vol.cluster
            if self.free_blocks != exp or free != vol.free_clusters:
                self.fail('%s: free blocks: SHOW VOLUME %d, bitmap %d (checker %s clusters of %d)'
                          % (tag, self.free_blocks, exp, free, vol.cluster))
        if export is None:
            export = True
        if export and not getattr(self, 'noexport', False):
            self.compare_export(tag)
        self.t_verify = time.time() - t0

    def compare_model(self, tag, vol):
        want = self.model.expected_entries()
        got = vol.files
        for k in sorted(set(want) - set(got), key=repr):
            self.fail('%s: model has %r, the volume does not' % (tag, k))
        for k in sorted(set(got) - set(want), key=repr):
            self.fail('%s: the volume has %r, the model does not' % (tag, k))
        mdirs = set(self.model.dirs.values())
        vdirs = set(vol.dirs)
        if mdirs != vdirs:
            self.fail('%s: directories differ: model-only %r, volume-only %r' % (tag, sorted(mdirs - vdirs)[:5], sorted(vdirs - mdirs)[:5]))
        fid_of_obj = {}
        obj_of_fid = {}
        for k, obj in want.items():
            fid = got[k]
            fi = vol.info[fid]
            if id(obj) in fid_of_obj and fid_of_obj[id(obj)] != fid:
                self.fail('%s: %r: the model says it is the same file as another entry, the volume says not' % (tag, k))
            fid_of_obj[id(obj)] = fid
            if fid in obj_of_fid and obj_of_fid[fid] != id(obj):
                self.fail('%s: %r: the volume says it is the same file as another entry, the model says not' % (tag, k))
            obj_of_fid[fid] = id(obj)
            if obj.volatile:
                continue
            if obj.kind == 'symlink':
                if fi.special != 5 or fi.target != obj.target:
                    self.fail('%s: %r: symbolic link: volume special=%d target=%r, model %r' % (tag, k, fi.special, fi.target, obj.target))
                continue
            if fi.special == 5:
                self.fail('%s: %r is a symbolic link on the volume only' % (tag, k))
            if fi.size != obj.size or fi.md5 != obj.md5:
                self.fail('%s: %r: size/md5 volume %d/%s, model %d/%s' % (tag, k, fi.size, fi.md5, obj.size, obj.md5))

    def compare_export(self, tag):
        out = os.path.join(self.dir, 'export')
        shutil.rmtree(out, ignore_errors=True)
        r = run_ods2(self.img, 'EXPORT/LINKS/VERSIONS [000000...]*.* ' + out)
        errs = [l for l in r.text.split('\n') if '%ODS2-E-' in l or '%ODS2-F-' in l]
        # KNOWN, not a bug: BADBLK.SYS of the VAX-made PHVS2 image maps 3 blocks of which the last lies 1 block past the end of the image file
        errs = [l for l in errs if 'BADBLK.SYS' not in l]
        if r.crash or errs:
            self.fail('%s: EXPORT failed: %s' % (tag, r.text[-600:]))
        got = {}
        gdirs = set()
        for root, dirs, files in os.walk(out):
            rel = os.path.relpath(root, out)
            for d in dirs:
                gdirs.add(os.path.normpath(os.path.join(rel, d)))
            for f in files:
                p = os.path.join(root, f)
                relp = os.path.normpath(os.path.join(rel, f))
                if os.path.islink(p):
                    got[relp] = ('l', os.readlink(p).encode('latin-1', 'replace'), os.lstat(p).st_ino)
                else:
                    with open(p, 'rb') as fh:
                        data = fh.read()
                    got[relp] = ('f', hashlib.md5(data).hexdigest(), os.lstat(p).st_ino)
        want = {}
        for (dpath, display, ver), obj in self.model.expected_entries().items():
            relp = os.path.normpath(os.path.join('/'.join(linux_escape(x) for x in dpath) if dpath else '.',
                                                 '%s;%d' % (linux_escape(linux_name(display)), ver)))
            want[relp] = obj
        for k in sorted(set(want) - set(got), key=repr):
            if want[k].volatile:
                continue
            self.fail('%s: EXPORT lacks %r' % (tag, k))
        for k in sorted(set(got) - set(want), key=repr):
            self.fail('%s: EXPORT has extra %r' % (tag, k))
        for k, obj in want.items():
            if obj.volatile:
                continue
            g = got[k]
            if obj.kind == 'symlink':
                if g[0] != 'l' or g[1] != obj.target:
                    self.fail('%s: EXPORT %r: symlink %r, model %r' % (tag, k, g, obj.target))
            elif g[0] != 'f' or g[1] != obj.md5:
                self.fail('%s: EXPORT %r: %s, model md5 %s' % (tag, k, g[:2], obj.md5))
        shutil.rmtree(out, ignore_errors=True)

    def write_manifests(self, base):
        """MANIFEST_LINUX (md5 size kind path, as the earlier batches) and MANIFEST_VMS (md5 size DCL spec) from the MODEL"""
        m = self.model
        lines, vms = [], []
        r0 = EscRng()    # ASCII only on the VMS side: every non-ASCII character as ^xx / ^Uxxxx
        for (dpath, display, ver), obj in sorted(m.expected_entries().items(), key=lambda kv: repr(kv[0])):
            if obj.volatile:
                continue
            lp = '/'.join([linux_escape(x) for x in dpath] + ['%s;%d' % (linux_escape(linux_name(display)), ver)])
            spec = spec_dir(dpath, r0, self.ods5) + spec_file_name(display, r0, self.ods5) + ';%d' % ver
            if obj.kind == 'symlink':
                lines.append('%s %d %s %s\t%s' % ('-' * 32, len(obj.target), 'symlink', lp, obj.target.decode('latin-1')))
                vms.append('symlink %s' % spec)
            else:
                lines.append('%s %d %s %s' % (obj.md5, obj.size, 'file', lp))
                vms.append('%s %d %s' % (obj.md5, obj.size, spec))
        for dp in sorted(m.dirs.values()):
            if dp:
                lines.append('%s %d %s %s' % ('-' * 32, 0, 'dir', '/'.join(linux_escape(x) for x in dp)))
        open(base + '.MANIFEST_LINUX.TXT', 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
        open(base + '.MANIFEST_VMS.TXT', 'w', encoding='utf-8').write('\n'.join(vms) + '\n')

    # ---- operations (each applies itself to the volume and the model)
    def pick_dir(self, writable=True):
        m = self.model
        cands = [p for p in m.dirs.values() if not (writable and m.is_frozen(p))]
        # prefer shallow, populated directories a little
        return self.rng.choice(cands)

    def pick_entry(self, deletable=True):
        m = self.model
        cands = []
        for (fp, key, ver), (display, obj) in m.ents.items():
            dp = m.dirs[fp]
            if obj.volatile or m.is_frozen(dp):
                continue
            if deletable and self.group_size(obj) > 1 and not self.hardlinks and (fp, key, ver) not in m.secondary:
                continue
            cands.append(((fp, key, ver), display, dp, obj))
        return self.rng.choice(cands) if cands else None

    def group_size(self, obj):
        return sum(1 for (_, o) in self.model.ents.values() if o is obj)

    def check_result(self, what, res, expect_ok, strict, need_blocks=0, nospace_ok=False):
        """compare the outcome with the model's expectation. returns ok"""
        if res.crash:
            self.fail('%s: CRASH rc=%s: %s' % (what, res.rc, res.text[-800:]))
        if expect_ok is None:
            return res.ok
        if res.ok == expect_ok:
            return res.ok
        if res.ok and not expect_ok:
            self.fail('%s: succeeded but the model says it must be refused: %s' % (what, res.text[-300:]))
        # failed but the model expected success
        if strict:
            free = self.free_blocks or 0
            if nospace_ok and (need_blocks + 4 * (self.cluster or 1) + 64 > free or re.search(r'space|full|allocat|INDEXF|header', res.text, re.I)):
                return False
            self.fail('%s: refused but the model says it is valid (free %s blocks): %s' % (what, free, res.text[-400:]))
        return False

    def op_mkdir(self):
        m, r = self.model, self.rng
        parent = self.pick_dir()
        if len(parent) >= (7 if not self.ods5 else 40):
            parent = ()
        name = self.names.dir_name()
        spec_parent = tuple(parent) + (name,)
        taken = m.dir_taken(parent, name)
        depth_bad = (not self.ods5) and len(spec_parent) > 8
        expect = not taken and not depth_bad
        cmd = 'CREATE/DIRECTORY ' + spec_dir(spec_parent, r, self.ods5)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd, res.rc))
        ok = self.check_result('mkdir ' + repr(spec_parent), res, expect, True, need_blocks=2 * self.cluster, nospace_ok=True)
        if ok:
            stored = name if self.ods5 else name.upper()
            m.add_dir(parent, stored)

    def src_for(self, size, kind):
        data = gen_content(self.rng, size, kind)
        return data, self.write_src(data)

    def op_copy(self, dirpath=None, name_kind=None, fmt=None, forced=None):
        m, r = self.model, self.rng
        dirpath = dirpath if dirpath is not None else self.pick_dir()
        display, strict = self.names.file_name(name_kind) if forced is None else forced
        if not self.ods5:
            display = display.upper()
        size = gen_size(r, self.big and self.profile != 'names')
        kind = r.choice(KINDS)
        data, src = self.src_for(size, kind)
        fmt = fmt or r.choices([None, 'STMLF', 'STM', 'STMCR', 'FIX512', 'UDF', 'VAR', 'ATTR_FIX', 'ATTR_STM'],
                               [60, 5, 5, 3, 6, 4, 6, 4, 4])[0]
        q = ''
        expect_data = data
        if fmt == 'VAR':
            q = '/CONVERT'
            expect_data = var_records(data) if max_line(data, b'\n') <= 32767 else b''
        elif fmt == 'ATTR_FIX':
            mrs = r.choice([2, 10, 80, 512, 1024])
            if size == 0 or size % mrs:
                size = mrs * r.randint(1, 20)
                data, src = self.src_for(size, 'bin')
                expect_data = data
            q = '/ATTRIBUTES=(RFM:FIX,MRS:%d)' % mrs
        elif fmt == 'ATTR_STM':
            q = '/ATTRIBUTES=(RFM:STMLF,RAT:CR)'
        elif fmt:
            q = '/FORMAT=' + fmt
        taken = m.name_taken(dirpath, display)
        longline = False
        if fmt in ('STMLF', 'STM', 'ATTR_STM', 'VAR'):
            longline = max_line(data, b'\n') > 32767
        elif fmt == 'STMCR':
            longline = max_line(data, b'\r') > 32767
        via_linux_name = self.ods5 and r.random() < 0.15 and not display.endswith('.DIR')
        if via_linux_name:
            lname = linux_name(display) if self.ods5 else display.lower()
            if '.' not in lname and False:
                pass
            src = os.path.join(self.src, 'ln%05d' % self.ncopy)
            os.makedirs(src, exist_ok=True)
            p = os.path.join(src, lname)
            with open(p, 'wb') as f:
                f.write(data)
            cmd = 'COPY%s "%s" %s' % (q, p, spec_dir(dirpath, r, self.ods5))
        else:
            cmd = 'COPY%s %s %s%s' % (q, src, spec_dir(dirpath, r, self.ods5), spec_file_name(display, r, self.ods5))
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        need = (len(expect_data) + 511) // 512 + 2 * self.cluster
        refused_soft = not strict
        expect = (not taken) if not longline else (False if taken else None)
        ok = self.check_result('copy %r' % (display,), res, expect, strict or taken, need_blocks=need, nospace_ok=True)
        if not ok and not taken and not strict:
            return
        if ok:
            m.add_file(dirpath, display, Obj('file', hashlib.md5(expect_data).hexdigest(), len(expect_data)))

    def op_bad_name(self):
        """names VMS refuses must be refused and leave nothing behind"""
        r = self.rng
        dirpath = self.pick_dir()
        if self.ods5:
            bad = r.choice(['bad^#name.txt', 'br^{ace.dat', 'x^}.y', 'tick^`.z', 'a' * 240 + '.t',
                            'b' * 119 + '^U0416.ж' if False else 'c' * 237 + '.'])
        else:
            bad = r.choice(['A.B.C', 'x' * 40 + '.TXT', 'NAME.' + 'y' * 40, 'a^#b.c', 'sp^_ace.txt',
                            'Q^&Z.DAT', 'caf^E9.txt'])
        src = self.write_src(b'bad name test')
        res = run_ods2(self.img, 'COPY %s %s%s' % (src, spec_dir(dirpath, r, self.ods5), bad))
        self.note('bad-name copy %r -> rc=%d' % (bad[:30], res.rc))
        if res.crash:
            self.fail('bad name %r crashed: %s' % (bad[:40], res.text[-500:]))
        if res.ok:
            self.fail('bad name %r was accepted: %s' % (bad[:40], res.text[-300:]))

    def op_case_collision(self):
        m, r = self.model, self.rng
        c = [(k, d, dp, o) for (k, d, dp, o) in [e for e in [self.pick_entry(False)] if e] if o.kind == 'file']
        if not c:
            return
        k, display, dp, obj = c[0]
        variant = ''.join(ch.swapcase() for ch in display) if self.ods5 else display.lower()
        if self.ods5 and fold_str(variant) != fold_str(display):
            return
        src = self.write_src(b'collision')
        res = run_ods2(self.img, 'COPY %s %s%s' % (src, spec_dir(dp, r, self.ods5), spec_file_name(variant, r, self.ods5)))
        self.note('case-collision copy %r -> rc=%d' % (variant[:30], res.rc))
        self.check_result('case collision %r' % (variant,), res, False, True)

    def op_delete(self):
        m, r = self.model, self.rng
        e = self.pick_entry()
        if e is None:
            return
        (fp, key, ver), display, dp, obj = e
        mode = r.choice(['exact', 'noversion', 'all'])
        vers = sorted(v for (f2, k2, v) in m.ents if f2 == fp and k2 == key)
        if mode == 'exact':
            vs = ';%d' % ver
            gone = [ver]
        elif mode == 'noversion':
            vs = ''
            ver = vers[-1]
            gone = [ver]
        else:
            vs = ';*'
            gone = vers
        # a primary link on a hard-linked volume may go; on others only entries that are not shared
        cmd = 'DELETE %s%s%s' % (spec_dir(dp, r, self.ods5), spec_file_name(display, r, self.ods5), vs)
        # noversion deletes the highest version: pick that entry's object
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        ok = self.check_result('delete %r%s' % (display, vs), res, True, True)
        if ok:
            for v in gone:
                m.ents.pop((fp, key, v), None)
                m.secondary.discard((fp, key, v))

    def op_wild_delete(self):
        """DELETE with a wildcard: refused with nothing changed, or exactly the matching entries gone"""
        m, r = self.model, self.rng
        e = self.pick_entry()
        if e is None:
            return
        (fp, key, ver), display, dp, obj = e
        stem, _, ty = display.rpartition('.')
        if len(stem) < 2 or not (stem[:2].isascii() and stem[:2].isalnum()) or not ty or not (ty.isascii() and ty.isalnum()) or ty.upper() == 'DIR':
            return
        pre = stem[:2]
        hits = []
        for (f2, k2, v2), (d2, o2) in m.ents.items():
            if f2 != fp or o2.volatile:
                continue
            s2, _, t2 = d2.rpartition('.')
            if fold_str(s2).startswith(fold_str(pre)) and fold_str(t2) == fold_str(ty):
                hits.append((f2, k2, v2, o2))
        # entries that are one of several names of a file, on a volume that does not count links, are left alone by the model:
        # only run when every hit is a plain single-name file
        if any(self.group_size(o2) > 1 for (_, _, _, o2) in hits):
            return
        cmd = 'DELETE %s%s*.%s;*' % (spec_dir(dp, r, self.ods5), pre, ty)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d (%d model hits)' % (cmd[:200], res.rc, len(hits)))
        ok = self.check_result('wildcard delete', res, None, False)
        if ok:
            for (f2, k2, v2, o2) in hits:
                m.ents.pop((f2, k2, v2), None)

    def op_rmdir(self):
        m, r = self.model, self.rng
        cands = [p for p in m.dirs.values() if p and not m.is_frozen(p)]
        if not cands:
            return
        p = r.choice(cands)
        empty = not any(True for k in m.ents if k[0] == m.fkpath(p)) and not any(
            len(d) == len(p) + 1 and d[:len(p)] == m.fkpath(p) for d in m.dirs)
        cmd = 'DELETE %s%s.DIR' % (spec_dir(p[:-1], r, self.ods5), spec_dir_name(p[-1], r, self.ods5))
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        ok = self.check_result('rmdir %r' % (p,), res, empty, True)
        if ok:
            m.remove_dir(p)

    def op_link(self):
        m, r = self.model, self.rng
        e = self.pick_entry(False)
        if e is None:
            return
        (fp, key, ver), display, dp, obj = e
        if obj.kind != 'file' or ver != 1:
            return
        ndp = self.pick_dir()
        ndisp, strict = self.names.file_name('plain' if r.random() < 0.7 else None)
        if not self.ods5:
            ndisp = ndisp.upper()
        taken = m.name_taken(ndp, ndisp)
        cmd = 'LINK %s%s %s%s;%d' % (spec_dir(ndp, r, self.ods5), spec_file_name(ndisp, r, self.ods5),
                                     spec_dir(dp, r, self.ods5), spec_file_name(display, r, self.ods5), ver)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        ok = self.check_result('link %r' % (ndisp,), res, not taken, strict or taken, nospace_ok=True)
        if ok:
            m.add_file(ndp, ndisp, obj)
            m.secondary.add((m.fkpath(ndp), key_of(ndisp), 1))

    def op_symlink(self):
        m, r = self.model, self.rng
        if not self.ods5:
            return
        dp = self.pick_dir()
        display, strict = self.names.file_name('plain' if r.random() < 0.7 else None)
        target = r.choice(['x', 'some/relative/path.txt', '../up/file', '/abs/path/to/thing', 'a' * 200, 'dir/' * 20 + 'f'])
        taken = m.name_taken(dp, display)
        cmd = 'SYMLINK %s%s %s' % (spec_dir(dp, r, self.ods5), spec_file_name(display, r, self.ods5), target)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        ok = self.check_result('symlink %r' % (display,), res, not taken, strict or taken, nospace_ok=True)
        if ok:
            m.add_file(dp, display, Obj('symlink', target=target.encode()))

    def op_label(self):
        r = self.rng
        lab = ''.join(r.choice(ODS2_CHARS) for _ in range(r.randint(1, 12)))
        res = run_ods2(self.img, 'SET VOLUME/LABEL=' + lab)
        self.note('SET VOLUME/LABEL=%s -> rc=%d' % (lab, res.rc))
        self.check_result('label', res, True, True)
        vol = ods5read.C.Vol(self.img)
        if vol.label != lab:
            self.fail('label on the volume is %r, expected %r' % (vol.label, lab))
        self.label_copies_check(lab)

    def label_copies_check(self, lab):
        v = ods5read.C.Vol(self.img)
        for name, lbn in (('home', 1), ('alternate home', ods5read.u32(v.home, 4))):
            b = v.blk(lbn)
            lab2 = b[472:484].decode('latin-1').rstrip()
            if lab2 != lab:
                self.fail('%s block label %r, expected %r' % (name, lab2, lab))
            s1 = sum(ods5read.u16(b, 2 * i) for i in range(29)) & 0xFFFF
            if s1 != ods5read.u16(b, 58) or ods5read.C.checksum(b) != ods5read.u16(b, 510):
                self.fail('%s block checksum wrong after relabel' % name)

    def op_import(self):
        m, r = self.model, self.rng
        parent = self.pick_dir()
        if len(parent) > (4 if not self.ods5 else 30):
            parent = ()
        dname = self.names.dir_name()
        if m.dir_taken(parent, dname) or (not self.ods5 and len(parent) + 2 > 8):
            return
        # make the target directory first
        cmd = 'CREATE/DIRECTORY ' + spec_dir(tuple(parent) + (dname,), r, self.ods5)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        if not self.check_result('mkdir for import', res, True, True, nospace_ok=True):
            return
        stored = dname if self.ods5 else dname.upper()
        m.add_dir(parent, stored)
        base = tuple(parent) + (stored,)
        tree = os.path.join(self.src, 'imp%05d' % self.ncopy)
        self.ncopy += 1
        ng = NameGen(r, self.ods5)
        planned = []            # (relpath dirs tuple, display, obj, data)
        dirs_planned = []
        nfiles = r.randint(1, 12)
        sub = [()]
        for _ in range(r.randint(0, 3)):
            sd = r.choice(sub)
            if not self.ods5 and len(base) + len(sd) + 1 > 8:
                continue
            dn = ng.dir_name()
            if any(fold_str(dn) == fold_str(x[-1]) for x in sub if x and x[:-1] == sd):
                continue
            sub.append(sd + (dn,))
            dirs_planned.append(sd + (dn,))
        os.makedirs(tree, exist_ok=True)
        for d in dirs_planned:
            os.makedirs(os.path.join(tree, *[x.upper() if not self.ods5 else x for x in d]), exist_ok=True)
        used = set()
        for _ in range(nfiles):
            sd = r.choice(sub)
            disp, strict = ng.file_name('plain' if not self.ods5 else None)
            if not strict:
                continue
            if not self.ods5:
                disp = disp.upper()
            k = (tuple(fold_str(x) for x in sd), key_of(disp))
            if k in used:
                continue
            if any(fold_str(x[-1]) + '.' == key_of(disp) for x in sub if x and x[:-1] == sd):
                continue
            if key_of(disp) == 'X.' and False:
                continue
            used.add(k)
            size = gen_size(r, False)
            data = gen_content(r, size, r.choice(KINDS))
            p = os.path.join(tree, *[(x.upper() if not self.ods5 else x) for x in sd], linux_name(disp))
            with open(p, 'wb') as f:
                f.write(data)
            planned.append((sd, disp, data))
        links = []
        if self.ods5 and r.random() < 0.5:
            for i in range(r.randint(1, 3)):
                sd = r.choice(sub)
                disp, strict = ng.file_name('plain')
                k = (tuple(fold_str(x) for x in sd), key_of(disp))
                if k in used or not strict:
                    continue
                if any(fold_str(x[-1]) + '.' == key_of(disp) for x in sub if x and x[:-1] == sd):
                    continue
                used.add(k)
                tgt = r.choice(['target.txt', '../x/y', 'a b/c'])
                os.symlink(tgt, os.path.join(tree, *sd, linux_name(disp)))
                links.append((sd, disp, tgt))
        cmd = 'IMPORT %s %s' % (tree, spec_dir(base, r, self.ods5))
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        free = self.free_blocks or 0
        need = sum((len(d) + 511) // 512 for _, _, d in planned) + 4 * self.cluster * (len(planned) + 2)
        if res.crash:
            self.fail('IMPORT crashed: ' + res.text[-800:])
        if not res.ok:
            if need + 64 > free:
                # out of room: whatever was imported stays; resync the model from the volume below
                self.resync_after_partial(base)
                return
            self.fail('IMPORT refused a valid tree: ' + res.text[-600:])
        for d in dirs_planned:
            m.add_dir(base, d[0] if len(d) == 1 else None) if False else None
        for d in sorted(dirs_planned, key=len):
            m.add_dir(base + tuple((x.upper() if not self.ods5 else x) for x in d[:-1]), d[-1].upper() if not self.ods5 else d[-1])
        for sd, disp, data in planned:
            m.add_file(base + tuple((x.upper() if not self.ods5 else x) for x in sd), disp,
                       Obj('file', hashlib.md5(data).hexdigest(), len(data)))
        for sd, disp, tgt in links:
            m.add_file(base + tuple(sd), disp, Obj('symlink', target=tgt.encode()))

    def resync_after_partial(self, base):
        """a command ran out of room part way: keep what the volume has under `base`, must be a consistent prefix"""
        vol = ods5read.read_volume(self.img)
        m = self.model
        pre = len(base)
        for dp in sorted(vol.dirs, key=len):
            if dp[:pre] == base and len(dp) > pre and not m.has_dir(dp):
                m.add_dir(dp[:-1], dp[-1])
        for (dpath, nm, ver), fid in vol.files.items():
            if dpath[:pre] == base:
                fi = vol.info[fid]
                if not m.has_dir(dpath):
                    self.fail('partial import left an entry in an unknown directory %r' % (dpath,))
                if (m.fkpath(dpath), key_of(nm), ver) not in m.ents:
                    if fi.special == 5:
                        m.add_file(dpath, nm, Obj('symlink', target=fi.target))
                    else:
                        m.add_file(dpath, nm, Obj('file', fi.md5, fi.size))
        self.note('partial import: resynchronised under %r' % (base,))

    def op_type(self):
        """TYPE/RAW a file and compare with the model"""
        e = self.pick_entry(False)
        if e is None:
            return
        (fp, key, ver), display, dp, obj = e
        if obj.kind != 'file' or obj.volatile or obj.size > 3_000_000:
            return
        cmd = 'TYPE/RAW %s%s;%d' % (spec_dir(dp, self.rng, self.ods5), spec_file_name(display, self.rng, self.ods5), ver)
        res = run_ods2(self.img, cmd)
        self.note('%s -> rc=%d' % (cmd[:200], res.rc))
        if res.crash or res.rc != 0:
            self.fail('TYPE/RAW %r failed: %s' % (display, res.text[-300:]))
        if hashlib.md5(res.out).hexdigest() != obj.md5 or len(res.out) != obj.size:
            self.fail('TYPE/RAW %r: %d bytes md5 %s, model %d %s' % (display, len(res.out), hashlib.md5(res.out).hexdigest(), obj.size, obj.md5))

    def op_set_default(self):
        """a script that sets the default directory and copies with a relative path"""
        m, r = self.model, self.rng
        dp = self.pick_dir()
        display, strict = self.names.file_name('plain')
        if not self.ods5:
            display = display.upper()
        if m.name_taken(dp, display):
            return
        data, src = self.src_for(gen_size(r, False), r.choice(KINDS))
        cmds = ['SET DEFAULT ' + spec_dir(dp, r, self.ods5), 'COPY %s %s' % (src, spec_file_name(display, r, self.ods5)),
                'SHOW DEFAULT']
        outs, crash, text, rc = run_batch(self.img, cmds, self.dir)
        self.note('script SET DEFAULT/COPY %r -> %s' % (display, [o[1] for o in outs]))
        if not all(o[1] for o in outs):
            self.note('script output: ' + text[-700:].replace('\n', ' | '))
        if crash:
            self.fail('script crashed: ' + text[-600:])
        if len(outs) == 3 and outs[0][1] and outs[1][1]:
            m.add_file(dp, display, Obj('file', hashlib.md5(data).hexdigest(), len(data)))
        elif strict and (self.free_blocks or 0) > 4000:
            self.fail('script copy refused: ' + text[-400:])

    OPS = {
        'names': [('copy', 45), ('mkdir', 12), ('delete', 10), ('rmdir', 3), ('bad_name', 10), ('case', 8), ('import', 10),
                  ('link', 4), ('symlink', 4), ('type', 2)],
        'links': [('copy', 15), ('link', 30), ('delete', 30), ('symlink', 10), ('mkdir', 8), ('rmdir', 6), ('type', 3)],
        'bigfile': [('copy', 50), ('delete', 20), ('type', 10), ('mkdir', 5), ('link', 5), ('import', 5)],
        'mix': [('copy', 40), ('mkdir', 8), ('delete', 14), ('rmdir', 4), ('link', 4), ('symlink', 3), ('label', 1),
                ('import', 5), ('bad_name', 4), ('case', 3), ('type', 6), ('set_default', 3), ('wild_delete', 3)],
    }

    def do_op(self, name):
        getattr(self, 'op_' + {'case': 'case_collision'}.get(name, name))()

    # ---- scenarios
    def batch_copy(self, items, fmt=''):
        """items: [(dirpath, display, data)] copied in one ods2 process; returns the list of results (True/False)"""
        r = self.rng
        cmds = []
        want = []
        for dp, display, data in items:
            src = self.write_src(data)
            cmds.append('COPY%s %s %s%s' % (fmt, src, spec_dir(dp, r, self.ods5), spec_file_name(display, r, self.ods5)))
        outs, crash, text, rc = run_batch(self.img, cmds, self.dir)
        self.note('batch of %d copies: %d ok' % (len(items), sum(1 for o in outs if o[1])))
        if crash:
            self.fail('batch crashed: ' + text[-800:])
        if len(outs) != len(items):
            self.fail('batch ran %d of %d commands: %s' % (len(outs), len(items), text[-400:]))
        for (dp, display, data), (c, ok, t) in zip(items, outs):
            if not ok and len(items) < 5:
                self.note('copy refused (%s, %d bytes): %s' % (display[:30], len(data), t.strip().replace('\n', ' | ')[:300]))
            if ok:
                self.model.add_file(dp, display, Obj('file', hashlib.md5(data).hexdigest(), len(data)))
        return [o[1] for o in outs], [o[2] for o in outs]

    def batch_delete(self, specs):
        """specs: [(dirpath, display, ver)] deleted in one process; model updated for the successful ones"""
        r, m = self.rng, self.model
        cmds = ['DELETE %s%s;%d' % (spec_dir(dp, r, self.ods5), spec_file_name(d, r, self.ods5), v) for dp, d, v in specs]
        outs, crash, text, rc = run_batch(self.img, cmds, self.dir)
        self.note('batch of %d deletes: %d ok' % (len(specs), sum(1 for o in outs if o[1])))
        if crash:
            self.fail('delete batch crashed: ' + text[-800:])
        if len(outs) != len(specs):
            self.fail('delete batch ran %d of %d: %s' % (len(outs), len(specs), text[-400:]))
        for (dp, d, v), (c, ok, t) in zip(specs, outs):
            if ok:
                m.ents.pop((m.fkpath(dp), key_of(d), v), None)
        return [o[1] for o in outs]

    def vol_stats(self):
        return ods5read.read_volume(self.img, want_md5=False)

    def scenario_fill(self):
        r, m = self.rng, self.model
        base = ods5read.read_volume(self.img, want_md5=False)
        free0 = self.free_blocks
        idx0 = base.indexf_blocks
        d0 = ods5read.read_volume(self.img, want_md5=False)
        dp = ()
        made = []
        phase = 0
        # 1. big files until one is refused; each refusal must leave everything as it was
        refusals = 0
        k = 0
        while refusals < 6 and k < 400:
            k += 1
            free = self.free_blocks
            if free < 8 * self.cluster:
                bsz = r.choice([1, 100, 511, 512, 513, 2000])
            else:
                bsz = r.choice([free * 512 // 3, free * 512 // 2, free * 512 // 8, free * 512 - r.randint(0, 5000), free * 512 + r.randint(1, 5000), 100000, 3000])
            nm, strict = self.names.file_name('plain' if k % 2 else None)
            if not self.ods5:
                nm = nm.upper()
            if m.name_taken(dp, nm):
                continue
            data = gen_content(r, max(bsz, 1), r.choice(['bin', 'tagged', 'lf', 'zero']))
            cmd_before = free
            ok = self.batch_copy([(dp, nm, data)])[0][0]
            if not ok:
                refusals += 1
            self.verify('fill step %d (%s, %d bytes, free %d->)' % (k, 'ok' if ok else 'refused', len(data), cmd_before), export=(k % 4 == 0 or not ok))
            if self.free_blocks == 0:
                break
        # 2. 1-byte files until nothing more fits (the directory or header space runs out)
        for j in range(60):
            nm, _ = self.names.file_name('plain')
            nm = nm.upper() if not self.ods5 else nm
            if m.name_taken(dp, nm):
                continue
            ok = self.batch_copy([(dp, nm, b'x' * r.randint(1, 3))])[0][0]
            if not ok:
                break
        self.verify('volume full', export=True)
        if self.stop == 'full':
            return
        # other commands on a full volume must fail cleanly (or succeed) and leave it consistent
        sfx = self.names.dir_name()
        for what in range(3):
            if what == 0:
                # KNOWN (review 6 #6, ods5-wr2 is fixing): CREATE/DIRECTORY with no room leaks a file number.
                # Run it on a throwaway copy, report what the checker says, keep the main volume clean.
                cp = self.img + '.fullmkdir'
                shutil.copyfile(self.img, cp)
                res = run_ods2(cp, 'CREATE/DIRECTORY [%s]' % spec_dir_name(sfx, r, self.ods5))
                if res.crash:
                    self.fail('mkdir on a full volume crashed: ' + res.text[-500:])
                summ = run_checker(cp)[0]
                self.note('XFAIL-KNOWN review6#6: mkdir on full volume ok=%s, checker on the copy: %s' % (res.ok, summ))
                os.unlink(cp)
            elif what == 1 and self.ods5:
                res = run_ods2(self.img, 'SYMLINK [000000]full^_link. target')
                if res.crash:
                    self.fail('symlink on a full volume crashed: ' + res.text[-500:])
                if res.ok:
                    m.add_file((), 'full link.', Obj('symlink', target=b'target'))
            elif what == 2:
                res = run_ods2(self.img, 'SET VOLUME/LABEL=FULLVOL')
                if res.crash or not res.ok:
                    self.fail('label on a full volume: ' + res.text[-300:])
            self.verify('full volume op %d' % what)
        # 3. delete a random half, verify, refill, delete all, check nothing leaked
        ents = [(m.dirs[fp], d, v) for (fp, key, v), (d, o) in m.ents.items() if not o.volatile and o.kind == 'file' and self.group_size(o) == 1 and (fp, key, v) not in self.baseline_keys]
        r.shuffle(ents)
        self.batch_delete(ents[:len(ents) // 2])
        self.verify('half deleted')
        items = []
        for j in range(20):
            nm, _ = self.names.file_name('plain')
            nm = nm.upper() if not self.ods5 else nm
            if m.name_taken(dp, nm) or any(nm == i[1] for i in items):
                continue
            items.append((dp, nm, gen_content(r, r.randint(1, 20000), 'tagged')))
        self.batch_copy(items)
        self.verify('refilled')
        ents = [(m.dirs[fp], d, v) for (fp, key, v), (d, o) in m.ents.items() if not o.volatile and o.kind == 'file' and self.group_size(o) == 1 and (fp, key, v) not in self.baseline_keys]
        self.batch_delete(ents)
        for (fp, key, v), (d, o) in list(m.ents.items()):
            if o.kind == 'symlink' and (fp, key, v) not in self.baseline_keys:
                self.batch_delete([(m.dirs[fp], d, v)])
        for p in sorted([p for p in m.dirs.values() if p and p not in self.baseline_dirs], key=len, reverse=True):
            res = run_ods2(self.img, 'DELETE %s%s.DIR' % (spec_dir(p[:-1], r, self.ods5), spec_dir_name(p[-1], r, self.ods5)))
            if res.crash or not res.ok:
                self.fail('rmdir %r at the end: %s' % (p, res.text[-300:]))
            m.remove_dir(p)
        self.verify('all deleted')
        end = self.vol_stats()
        # free space must come back, but for what INDEXF.SYS and the root directory grew by
        lost = free0 - self.free_blocks
        grown = (end.indexf_blocks - idx0)
        rootext = sum(c for _, c in end.info[4].extents) - sum(c for _, c in d0.info[4].extents)
        self.note('fill: free at start %d, at end %d; INDEXF grew %d, root dir grew %d' % (free0, self.free_blocks, grown, rootext))
        if lost != grown + rootext:
            self.fail('space leak: %d blocks free at the start, %d after deleting everything (INDEXF grew %d, root %d): %d unaccounted'
                      % (free0, self.free_blocks, grown, rootext, lost - grown - rootext))

    def scenario_frag(self):
        r, m = self.rng, self.model
        nsmall = r.choice([300, 600, 1000])
        free0 = self.free_blocks
        self.dname = 'FRAG'
        res = run_ods2(self.img, 'CREATE/DIRECTORY [FRAG]')
        if not res.ok:
            self.fail('mkdir FRAG: ' + res.text[-300:])
        m.add_dir((), 'FRAG')
        dp = ('FRAG',)
        items = []
        for i in range(nsmall):
            nm = 'F%05d.D' % i
            sz = r.choice([1, 100, 512, 512, 512, 1000, 1024, 1500])
            items.append((dp, nm, gen_content(r, sz, 'tagged')))
        oks, _ = self.batch_copy(items)
        if not all(oks):
            self.note('only %d of %d small files fit' % (sum(oks), nsmall))
        self.verify('small files')
        # delete a pattern: every other, or random 50%
        mode = r.choice(['alternate', 'random', 'runs'])
        keep = [(it[1]) for it, ok in zip(items, oks) if ok]
        if mode == 'alternate':
            gone = keep[::2]
        elif mode == 'random':
            gone = [k for k in keep if r.random() < 0.5]
        else:
            gone = []
            i = 0
            while i < len(keep):
                n = r.randint(1, 4)
                if r.random() < 0.5:
                    gone += keep[i:i + n]
                i += n + r.randint(1, 3)
        self.batch_delete([(dp, g, 1) for g in gone])
        self.verify('holes made (%s, %d)' % (mode, len(gone)))
        # big files into the holes: each must fragment
        vol = self.vol_stats()
        total_free = self.free_blocks
        for j in range(3):
            if j == 0:
                size_blocks = max(total_free // 2, 2)
            elif j == 1:
                size_blocks = max(total_free - 1, 1)
            else:
                size_blocks = max(total_free // 8, 2)
            kind = r.choice(['tagged', 'bin'])
            data = gen_content(r, size_blocks * 512 - r.randint(0, 511), kind)
            ok = self.batch_copy([((), 'BIG%d.DAT' % j, data)])[0][0]
            vol = self.vol_stats()
            self.note('frag: BIG%d %d blocks ok=%s; INDEXF extension headers %d, max extents of a file %d' % (j, size_blocks, ok, vol.nheaders_ext, vol.max_extents))
            self.verify('big file %d (%s)' % (j, 'ok' if ok else 'refused'))
            total_free = self.free_blocks
        self.coverage = {'ext_headers': vol.nheaders_ext, 'max_extents': vol.max_extents}
        if self.stop == 'frag':
            return
        # delete the big ones, then the rest
        rest = [(dp, k, 1) for k in keep if k not in gone]
        self.batch_delete([((), 'BIG%d.DAT' % j, 1) for j in range(3) if m.ents.get((m.fkpath(()), key_of('BIG%d.DAT' % j), 1))])
        self.verify('big files deleted')
        self.batch_delete(rest)
        res = run_ods2(self.img, 'DELETE [000000]FRAG.DIR')
        if res.ok:
            m.remove_dir(('FRAG',))
        self.verify('everything deleted')

    def scenario_shatter(self):
        """every cluster of a small volume a file of its own, every other one deleted, then a file needing more pieces
        than the 1024 extents ods2v2 can map: it must succeed or be refused cleanly, never corrupt"""
        r, m = self.rng, self.model
        res = run_ods2(self.img, 'CREATE/DIRECTORY [SH]')
        m.add_dir((), 'SH')
        dp = ('SH',)
        n = 0
        while True:
            items = [(dp, 'S%05d.D' % (n + i), gen_content(r, 1 + (n + i) % 400, 'tagged')) for i in range(500)]
            oks, texts = self.batch_copy(items)
            n += 500
            if not all(oks) or n >= 9000:
                break
        self.verify('shattered volume full of small files (%d)' % sum(1 for k in m.ents if k[0] == m.fkpath(dp)), export=False)
        keep = sorted(d for (fp, key, v), (d, o) in m.ents.items() if fp == m.fkpath(dp))
        gone = keep[::2]
        self.batch_delete([(dp, g, 1) for g in gone])
        self.verify('every other file deleted (%d holes)' % len(gone), export=False)
        free = self.free_blocks
        for want in (free // self.cluster * self.cluster - 40 * self.cluster, free // self.cluster * self.cluster, 1100 * self.cluster, 1500 * self.cluster):
            data = gen_content(r, max(want, 1) * 512 - r.randint(0, 100), 'tagged')
            ok = self.batch_copy([((), 'SHATTER%d.DAT' % want, data)])[0][0]
            vol = self.vol_stats()
            self.note('shatter: %d blocks wanted, ok=%s, max extents %d, extension headers %d' % (want, ok, vol.max_extents, vol.nheaders_ext))
            self.verify('shatter file %d (%s)' % (want, 'ok' if ok else 'refused'), export=True)
            self.coverage = {'max_extents': vol.max_extents}
        rest = [(dp, k, 1) for k in keep if k not in gone]
        self.batch_delete([((), d, 1) for (fp, key, v), (d, o) in list(m.ents.items()) if fp == m.fkpath(()) and d.startswith('SHATTER')])
        self.verify('shatter files deleted', export=False)
        self.batch_delete(rest)
        self.verify('all small files deleted', export=False)

    def scenario_bigdir(self):
        r, m = self.rng, self.model
        n = r.choice([400, 1200, 3000, 5000])
        res = run_ods2(self.img, 'CREATE/DIRECTORY [BIG]')
        m.add_dir((), 'BIG')
        dp = ('BIG',)
        names = set()
        items = []
        longnames = r.random() < 0.5
        while len(items) < n:
            nm, strict = self.names.file_name('long' if (longnames and r.random() < 0.3 and self.ods5) else (None if self.ods5 else None))
            if not self.ods5:
                nm = nm.upper()
            if not strict or key_of(nm) in names or key_of(nm).endswith('.DIR'):
                continue
            names.add(key_of(nm))
            items.append((dp, nm, gen_content(r, r.choice([0, 0, 1, 100, 511, 513]), 'bin')))
        for i in range(0, n, 500):
            self.batch_copy(items[i:i + 500])
            self.verify('bigdir fill %d' % min(i + 500, n), export=False)
        self.verify('bigdir full', export=True)
        if self.stop == 'bigdir':
            return
        vol = self.vol_stats()
        self.coverage = {'dir_blocks': sum(c for _, c in vol.info[vol.dirs[dp]].extents), 'entries': n}
        # delete most, in random order, then add again
        ents = [(dp, d, v) for (fp, key, v), (d, o) in m.ents.items() if m.dirs[fp] == dp]
        r.shuffle(ents)
        cut = int(len(ents) * r.choice([0.3, 0.7, 0.95]))
        self.batch_delete(ents[:cut])
        self.verify('bigdir shrunk to %d' % (len(ents) - cut), export=False)
        items2 = []
        while len(items2) < min(300, n // 2):
            nm, strict = self.names.file_name(None)
            nm = nm.upper() if not self.ods5 else nm
            if not strict or key_of(nm) in names or key_of(nm).endswith('.DIR'):
                continue
            names.add(key_of(nm))
            items2.append((dp, nm, gen_content(r, r.choice([0, 1, 100]), 'bin')))
        self.batch_copy(items2)
        self.verify('bigdir regrown', export=True)
        ents = [(dp, d, v) for (fp, key, v), (d, o) in m.ents.items() if m.dirs[fp] == dp]
        self.batch_delete(ents)
        self.verify('bigdir emptied', export=True)
        res = run_ods2(self.img, 'DELETE [000000]BIG.DIR')
        if not res.ok:
            self.fail('rmdir BIG after emptying it: ' + res.text[-300:])
        m.remove_dir(dp)
        self.verify('bigdir removed')

    def scenario_deep(self):
        r, m = self.rng, self.model
        limit = 8 if not self.ods5 else r.choice([30, 100, 255, 255, 300])
        self.noexport = False
        names = []
        cmds = ['SET DEFAULT [000000]']
        path = ()
        made = 0
        for lvl in range(1, limit + 1):
            nm = 'D%d' % lvl if r.random() < 0.7 else self.names.dir_name()
            if not self.ods5:
                nm = nm.upper()
            if any(fold_str(nm) == fold_str(n) for n in names[-1:]):
                nm = 'Z%d' % lvl
            cmds.append('CREATE/DIRECTORY [.%s]' % spec_dir_name(nm, r, self.ods5))
            cmds.append('SET DEFAULT [.%s]' % spec_dir_name(nm, r, self.ods5))
            names.append(nm)
        outs, crash, text, rc = run_batch(self.img, cmds, self.dir)
        if crash:
            self.fail('deep tree crashed: ' + text[-800:])
        depth = 0
        for i in range(1, len(outs), 2):
            c, ok, t = outs[i]
            if not ok:
                break
            depth += 1
        self.note('deep: requested %d levels, %d made (%s)' % (limit, depth, outs[2 * depth + 1][2][-200:].replace('\n', ' ') if 2 * depth + 1 < len(outs) else ''))
        for i in range(depth):
            m.add_dir(tuple(names[:i]), names[i])
        want = limit if limit <= (8 if not self.ods5 else 255) else 255
        if depth < want:
            self.fail('deep tree: only %d of %d levels were made: %s' % (depth, want, text[-400:]))
        if depth > want:
            self.note('deep: %d levels were made, beyond the 255 the ods2 tools can traverse (KNOWN-LIMIT: CREATE/DIRECTORY has no depth limit, DIR [...] and EXPORT stop at 255 with %%ODS2-E-TOODEEP); verified with the independent reader only' % depth)
            self.noexport = True
        self.verify('deep tree', export=True)
        # files at several depths, written by relative path in a script
        cmds = ['SET DEFAULT [000000]']
        items = []
        leaf_levels = set(range(1, depth + 1, max(depth // 6, 1)))
        for k in range(1, depth + 1):
            cmds.append('SET DEFAULT [.%s]' % spec_dir_name(names[k - 1], r, self.ods5))
            if k in leaf_levels:
                data = gen_content(r, r.choice([1, 513, 5000]), 'tagged')
                src = self.write_src(data)
                cmds.append('COPY %s LEAF%d.TXT' % (src, k))
                items.append((tuple(names[:k]), 'LEAF%d.TXT' % k, data))
        outs, crash, text, rc = run_batch(self.img, cmds, self.dir)
        if crash:
            self.fail('deep copy crashed: ' + text[-500:])
        okc = [o for o in outs if o[0].startswith('COPY')]
        for (dp, d, data), o in zip(items, okc):
            if o[1]:
                m.add_file(dp, d, Obj('file', hashlib.md5(data).hexdigest(), len(data)))
            else:
                self.fail('deep copy at depth %d refused: %s' % (len(dp), o[2][-300:]))
        self.verify('deep files', export=True)
        if self.stop == 'deep':
            return
        # remove from the bottom
        for k in range(depth, 0, -1):
            dp = tuple(names[:k])
            leaf = [(dp, d, 1) for (fp, key, v), (d, o) in list(m.ents.items()) if m.dirs[fp] == dp]
            if leaf:
                self.batch_delete(leaf)
            parent = ('[000000]' if k == 1 else '[' + '.'.join(spec_dir_name(n, r, self.ods5) for n in names[:k - 1]) + ']')
            if len(parent) > 900:
                break
            res = run_ods2(self.img, 'DELETE %s%s.DIR' % (parent, spec_dir_name(names[k - 1], r, self.ods5)))
            if not res.ok:
                self.fail('rmdir at depth %d: %s' % (k, res.text[-300:]))
            m.remove_dir(dp)
            if k % 40 == 0:
                self.verify('deep teardown %d' % k, export=False)
        self.verify('deep removed', export=True)

    def scenario_reserved(self):
        """review 6 #1: DELETE / LINK of the reserved files. Run on throwaway copies. Expected today: DELETE is not
        refused (KNOWN, ods5-wr2 is fixing on ods5-int); once fixed, every one of these must be refused and leave the
        volume exactly as it was (image md5 before = after)."""
        names = ['INDEXF.SYS', 'BITMAP.SYS', 'BADBLK.SYS', 'CORIMG.SYS', 'VOLSET.SYS', 'CONTIN.SYS', 'BACKUP.SYS',
                 'BADLOG.SYS', 'SECURITY.SYS', '000000.DIR']
        fixed = os.environ.get('STRESS_FIXED_RESERVED') == '1'
        for nm in names:
            for how in ('DELETE [000000]%s;1', 'DELETE [000000]%s;*', 'DELETE [000000]%s', 'LINK [000000]ALIAS.SYS [000000]%s'):
                cp = self.img + '.rsv'
                shutil.copyfile(self.img, cp)
                before = hashlib.md5(open(cp, 'rb').read()).hexdigest()
                res = run_ods2(cp, how % nm)
                if res.crash:
                    self.fail('%s crashed: %s' % (how % nm, res.text[-300:]))
                after = hashlib.md5(open(cp, 'rb').read()).hexdigest()
                summ = run_checker(cp)[0]
                clean = summ == '0 error(s), 0 warning(s)' and after == before
                tag = 'XFAIL-KNOWN review6#1' if not clean or res.ok else 'ok'
                self.note('%s -> ok=%s image-unchanged=%s checker=%s [%s]' % (how % nm, res.ok, after == before, summ, tag))
                if fixed and (res.ok or not clean) and not (how.startswith('LINK') and not res.ok):
                    self.fail('reserved file %s not protected: %s -> ok=%s unchanged=%s checker=%s' % (nm, how, res.ok, after == before, summ))
                os.unlink(cp)

    def scenario_indexf(self):
        r, m = self.rng, self.model
        # fill with tiny files until the index file cannot hold more: no corruption allowed, the limit is reported
        res = run_ods2(self.img, 'CREATE/DIRECTORY [IDX]')
        m.add_dir((), 'IDX')
        dp = ('IDX',)
        total = 0
        refused = False
        vol0 = self.vol_stats()
        self.coverage = {}
        for rnd in range(40):
            items = [(dp, 'I%06d.X' % (total + i), gen_content(r, r.choice([0, 1, 10, 600]), 'bin')) for i in range(200)]
            oks, texts = self.batch_copy(items)
            total += len(items)
            self.verify('indexf round %d' % rnd, export=(rnd % 5 == 4))
            if not all(oks):
                refused = True
                self.note('indexf: refusal after %d files: %s' % (total, texts[oks.index(False)][-200:].replace('\n', ' ')))
                break
        vol = self.vol_stats()
        self.coverage = {'files': len(vol.files), 'indexf_blocks': vol.indexf_blocks, 'maxfiles_refused': refused}
        # delete half, add again (header reuse)
        ents = [(dp, d, v) for (fp, key, v), (d, o) in m.ents.items() if m.dirs[fp] == dp]
        r.shuffle(ents)
        self.batch_delete(ents[:len(ents) // 2])
        self.verify('indexf half deleted')
        items = [(dp, 'J%06d.X' % i, gen_content(r, 5, 'bin')) for i in range(len(ents) // 4)]
        self.batch_copy(items)
        self.verify('indexf reuse', export=True)

    def run(self):
        self.setup()
        ops = self.OPS.get(self.profile, self.OPS['mix'])
        for i in range(self.nops):
            self.opno = i + 1
            if self.profile in self.OPS or self.profile == 'mix':
                names = [n for n, _ in ops]
                name = self.rng.choices(names, [w for _, w in ops])[0]
                self.note('#%d %s' % (self.opno, name))
                self.do_op(name)
            else:
                getattr(self, 'scenario_' + self.profile)()
                break
            if self.opno % self.check_every == 0:
                self.verify('after op %d' % self.opno, export=(self.opno % 3 == 0 or self.nops <= 40))
        self.verify('final')
        self.final_digest = None
        if os.environ.get('STRESS_DIGEST'):
            v = ods5read.read_volume(self.img)
            blob = repr((sorted((k, (v.info[f].size, v.info[f].md5, v.info[f].special) if f else None) for k, f in v.files.items()),
                         sorted(v.dirs), v.free_clusters, v.indexf_blocks))
            self.final_digest = hashlib.md5(blob.encode()).hexdigest()
        return True


# ---------------------------------------------------------------- driver

def pick_config(seed, only=None, profile='mix'):
    rng = random.Random(seed)
    if only:
        return only
    if profile in ('fill', 'frag', 'bigdir', 'deep', 'indexf', 'reserved', 'shatter'):
        return 'f5' if rng.random() < 0.6 else 'f2'    # scenarios need a volume of their own size
    r = rng.random()
    if r < 0.4:
        return 'f5'
    if r < 0.65:
        return 'f2'
    return 'vms:' + rng.choice(VMS_BASELINES)


def run_seed(args):
    seed, profile, config, nops, keep, verbose, check_every = args
    config = pick_config(seed, config, profile)
    wd = os.path.join(WORK, 'stress', 's%d-%s' % (seed, profile))
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    h = Harness(seed, profile, config, nops, wd, verbose=verbose, check_every=check_every)
    t0 = time.time()
    try:
        h.run()
        res = ('PASS', 'digest=%s' % h.final_digest if h.final_digest else '')
    except Failure as e:
        res = ('FAIL', str(e))
    except Exception:
        res = ('ERROR', traceback.format_exc())
    dt = time.time() - t0
    with open(os.path.join(wd, 'ops.log'), 'w') as f:
        f.write('\n'.join(h.log) + '\n')
    if res[0] == 'PASS' and not keep:
        shutil.rmtree(wd, ignore_errors=True)
    else:
        for d in ('export',):
            shutil.rmtree(os.path.join(wd, d), ignore_errors=True)
    return seed, profile, config, res[0], res[1], dt, h.opno


CAP = ['systemd-run', '--user', '--scope', '--quiet', '-p', 'MemoryMax=4G', '-p', 'MemorySwapMax=0']


def run_child(a, seed):
    """one seed as its own process inside a 4 GB systemd scope (the operator's rule: never run uncapped)"""
    cmd = ([] if os.environ.get('STRESS_UNCAPPED') == '1' else CAP) + [
        sys.executable, '-I', os.path.abspath(__file__), '--child', '--seed', str(seed), '--profile', a.profile,
        '--ops', str(a.ops), '--check-every', str(a.check_every)]
    if a.config:
        cmd += ['--config', a.config]
    if a.keep:
        cmd.append('--keep')
    if a.verbose:
        cmd.append('-v')
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
    for line in r.stdout.split('\n'):
        if line.startswith('RESULT '):
            return tuple(json.loads(line[7:]))
    why = 'child died (rc %s, 4 GB scope: out of memory?): %s' % (r.returncode, (r.stdout + r.stderr)[-600:])
    return (seed, a.profile, a.config or '?', 'ERROR', why, 0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', type=int)
    ap.add_argument('--seeds')
    ap.add_argument('--profile', default='mix')
    ap.add_argument('--config', help='f5, f2 or vms:<image name without .img>')
    ap.add_argument('--ops', type=int, default=60)
    ap.add_argument('--jobs', type=int, default=4, help='parallel seeds (at most 6)')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--check-every', type=int, default=1)
    ap.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    a.jobs = min(a.jobs, 6)
    if a.child:
        res = run_seed((a.seed, a.profile, a.config, a.ops, a.keep, a.verbose, a.check_every))
        print('RESULT ' + json.dumps(list(res)))
        return 0
    if a.seed is not None:
        seeds = [a.seed]
    else:
        lo, _, hi = (a.seeds or '1-10').partition('-')
        seeds = range(int(lo), int(hi or lo) + 1)
    from concurrent.futures import ThreadPoolExecutor
    results = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for res in report(ex.map(lambda sd: run_child(a, sd), seeds)):
            results.append(res)
    nfail = sum(1 for r in results if r[3] != 'PASS')
    print('%d seeds, %d failed' % (len(results), nfail))
    return 1 if nfail else 0


def report(it):
    for seed, profile, config, status, msg, dt, nops in it:
        print('seed %d %s %s: %s (%d ops, %.0fs) load %s %s' % (seed, profile, config, status, nops, dt, round(os.getloadavg()[0], 1), msg if status == 'PASS' else ''), flush=True)
        if status != 'PASS':
            print('    ' + msg.replace('\n', '\n    ')[:2500], flush=True)
        yield seed, profile, config, status, msg, dt, nops


if __name__ == '__main__':
    sys.exit(main())
