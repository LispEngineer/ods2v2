#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""commaesc_check.py - an escaped comma in an ODS-5 file name is a character, not a list separator (found by ods5-rev2 in review 66: the real V8.4 battery runs a8-vmsinit-ods5t and
ods5t6 hold CORPUS/NAMES/sp,c.txt, which the battery TYPEs as `TYPE [CORPUS.NAMES]sp^,c.txt`). Until now `TYPE [D]sp^,c.txt` was split by split_list (tools/ods2.c) at the ESCAPED comma:
SEARCHFAIL for `[D]sp^`, then it typed `[D]c.txt` if that exists: the wrong file, with no error. A ^ escapes the character after it (^, a comma, ^^ a caret, ^[ a bracket), in the spec of
every command that takes a list or splits a specification.
The volume (ODS-5) holds [D]c.txt ("DECOY"), [D]sp^,c.txt = the file `sp,c.txt` ("COMMA"), [D]y^^ = the file `y^.` ("CARET"), [D]b^[1^].txt = the file `b[1].txt` ("BRACKET"), [D]b^[x.txt = `b[x.txt` ("BRACKET2": an escaped bracket that is never closed), [D]a.txt ("A").
Usage: commaesc_check.py   (from the project root, with ./ods2 and ./ods2_init built; TMPDIR holds the scratch image)."""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ODS2 = './ods2'
INIT = './ods2_init'
fails = passes = 0


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


def listing(img, spec):
    """the 'name;version' entries of a DIR (names as DIR spells them, ^ escapes included)"""
    out = ods2(img, 'DIR ' + spec)[1]
    return sorted('%s;%s' % (m.group(1), m.group(2)) for m in (re.match(r'^(\S+?)\s*;(\d+)\s*$', l) for l in out.splitlines()) if m)


def words(out):
    """the text lines of a TYPE (no message lines, no blank lines)"""
    return [l.strip() for l in out.splitlines() if l.strip() and not l.startswith('%') and not l.startswith('-')]


def main():
    work = tempfile.mkdtemp(prefix='commaesc_check_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        img = os.path.join(work, 'v.img')
        rc, out = run([INIT, img, '8192', '--structure=5', '--label=COMMAESC'])
        assert rc == 0, out
        assert ods2(img, 'CREATE/DIRECTORY [D]')[0] == 0
        for dst, text in (('[D]c.txt', 'DECOY'), ('[D]sp^,c.txt', 'COMMA'), ('[D]y^^', 'CARET'), ('[D]b^[1^].txt', 'BRACKET'), ('[D]b^[x.txt', 'BRACKET2'), ('[D]v^;x.txt', 'SEMI'), ('[D]q^%r.txt', 'PCT'), ('[D]a.txt', 'A')):
            p = os.path.join(work, 'f.txt')
            open(p, 'w').write(text + '\n')
            rc, out = ods2(img, 'COPY %s %s' % (p, dst))
            assert rc == 0 and '%ODS2-E' not in out, (dst, out)
        # a second version of the comma file, for PURGE
        open(os.path.join(work, 'f.txt'), 'w').write('COMMA2\n')
        assert ods2(img, 'COPY %s [D]sp^,c.txt' % os.path.join(work, 'f.txt'))[0] == 0
        base = os.path.join(work, 'base.img')
        shutil.copyfile(img, base)

        def fresh():
            shutil.copyfile(base, img)
            return img

        # TYPE: a list is split at the commas that are not escaped
        for spec, want in (('[D]sp^,c.txt', ['COMMA2']), ('[D]a.txt,sp^,c.txt', ['A', 'COMMA2']), ('[D]y^^,c.txt', ['CARET', 'DECOY']), ('[D]b^[1^].txt,c.txt', ['BRACKET', 'DECOY']), ('[D]b^[x.txt,c.txt', ['BRACKET2', 'DECOY']), ('[D]v^;x.txt,c.txt', ['SEMI', 'DECOY']), ('[D]q^%r.txt,c.txt', ['PCT', 'DECOY']),
                           ('[D]sp^,c.txt,c.txt', ['COMMA2', 'DECOY']), ('[D]a.txt,y^^,sp^,c.txt', ['A', 'CARET', 'COMMA2'])):
            rc, out = ods2(fresh(), 'TYPE ' + spec)
            ls = [l for l in words(out) if l in ('A', 'COMMA', 'COMMA2', 'CARET', 'BRACKET', 'BRACKET2', 'SEMI', 'PCT', 'DECOY')]
            check('TYPE %s shows %s, in that order, and reports nothing missing' % (spec, want), (want, False), (ls, 'SEARCHFAIL' in out or 'not found' in out.lower()))
        # the other commands that take or split a specification read the escaped comma as a character too
        img = fresh()
        check('DIR [D]sp^,c.txt lists the comma file only (both versions)', ['sp^,c.txt;1', 'sp^,c.txt;2'], listing(img, '[D]sp^,c.txt;*'))
        img = fresh()
        rc, out = ods2(img, 'PURGE/LOG [D]sp^,c.txt')
        check('PURGE [D]sp^,c.txt is one specification (no TOOMANY) and keeps the highest version of the comma file only', (False, ['a.txt;1', 'b^[1^].txt;1', 'b^[x.txt;1', 'c.txt;1', 'q^%r.txt;1', 'sp^,c.txt;2', 'v^;x.txt;1', 'y^^.;1']),
              ('TOOMANY' in out, listing(img, '[D]*.*;*')))
        img = fresh()
        rc, out = ods2(img, 'DELETE [D]sp^,c.txt;1')
        check('DELETE [D]sp^,c.txt;1 deletes that version of the comma file only', ['a.txt;1', 'b^[1^].txt;1', 'b^[x.txt;1', 'c.txt;1', 'q^%r.txt;1', 'sp^,c.txt;2', 'v^;x.txt;1', 'y^^.;1'], listing(img, '[D]*.*;*'))
        img = fresh()
        rc, out = ods2(img, 'SET FILE/PROTECTION=(W:R) [D]sp^,c.txt')
        pc = re.findall(r'Protection:\s+(\(.*\))', ods2(img, 'DIR/FULL [D]sp^,c.txt')[1])
        po = re.findall(r'Protection:\s+(\(.*\))', ods2(img, 'DIR/FULL [D]c.txt')[1])
        check('SET FILE/PROTECTION [D]sp^,c.txt changes the comma file only', (True, False), (bool(pc) and 'W:R' in pc[0], bool(po) and 'W:R' in po[0]))
        img = fresh()
        rc, out = ods2(img, 'LINK [D]sp^,link.txt [D]sp^,c.txt')
        check('LINK makes a second entry with a comma in its name for the comma file', ['sp^,link.txt;1'], listing(img, '[D]sp^,link.txt'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('commaesc_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
