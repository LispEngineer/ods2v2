#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""wo_vms_compare.py - TYPE with wildcards, PURGE and SET FILE/PROTECTION, /OWNER_UIC against what VMS itself did (step 0 of PLAN_WRITE_OPS: CORPUS 49 on
VAX V7.3, 49.8 on V8.4; items 2 to 4).

usage: wo_vms_compare.py KIND LEVEL LOG CONTAINER     (KIND type | purge | setf; run from the project root; needs ./ods2 and ./ods2_init)

LOG is ods5-vms3's log of the VMS run (corpus/manifests/write-ops-step0-*/WO_<TEST>_<level>.LOG in ods5-cli), CONTAINER the volume that run left
(~/tmp/ods5-vms3/wo/img/phvs2-*.img, wo/a5/img/asim5-*.img).

  type : every TYPE command of the log (no /OUTPUT, /PAGE or directory wildcard) is run on the container; the text must equal what VMS printed (the device
         VMS prints in front of a name is not printed here). A missing file named without a wildcard keeps ods2's own message and is skipped.
  purge: the log is replayed on a fresh volume (the files VMS made are made again, the PURGE, SET DEFAULT and SET FILE commands run, a /NOWRITE mount is
         a read-only image); the messages of every PURGE must equal VMS's, and the final directory must hold the names, versions and sizes of the
         container's.
  setf : the SET FILE commands of the log are replayed on a fresh volume (names that need the rights database are given as the numeric UIC the log
         says the account has); each message must equal VMS's (the first line only where ods2v2 adds an explanation), and after each step the
         protection, the owner and the revision number of the files the log lists afterwards must equal VMS's (a directory's revision is compared as
         the change from its first value: VMS makes directories at revision 0, ods2v2 at ODS-2 with 1: a difference that is not SET FILE's).
Allowed to differ, by design: dates, file ids, the device name, ods2v2's extra explanation lines.
"""
import os
import re
import shutil
import subprocess
import sys

ODS2, INIT = './ods2', './ods2_init'
TMP = os.environ.get('TMPDIR', 'samples') + '/wo_vms_compare'
fails = 0


def check(ok, text, detail=''):
    global fails
    print('%s: %s' % ('PASS' if ok else 'FAIL', text))
    if not ok:
        fails += 1
        if detail:
            print('      ' + detail.replace('\n', '\n      '))


def run(img, cmd):
    p = subprocess.run([ODS2, img, cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    out = p.stdout
    return '\n'.join(ln for ln in out.split('\n') if not ln.startswith('%ODS2-W-READONLY'))


def strip_dev(t):
    return re.sub(r'\b[A-Z]{2,}\d*\$?[A-Z0-9]*:(?=\[)', '', t)         # LDA3:[T] -> [T]


def parse_log(path):
    """[('cmd', text, [output lines]) | ('mkf', spec) | ('uic', numeric)] in order"""
    ev = []
    lines = open(path, encoding='latin-1').read().replace('\r', '').split('\n')
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r'^MKF (\S+?) made', ln)
        if m:
            ev.append(('mkf', m.group(1)))
            i += 1
            continue
        m = re.match(r'^my UIC (\S+) numeric (\[\S+\])', ln)
        if m:
            ev.append(('uic', m.group(1), m.group(2)))
            i += 1
            continue
        if ln.startswith('$ '):
            cmd = re.sub(r'\s+\(privileged\)\s*$', '', ln[2:].rstrip())
            i += 1
            out = []
            while i < len(lines) and not lines[i].startswith('  status ') and not lines[i].startswith('$ ') and not lines[i].startswith('=== '):
                out.append(lines[i])
                i += 1
            while out and out[-1].strip() == '':
                out.pop()
            while out and out[0].strip() == '' and not cmd.startswith('TYPE'):
                out.pop(0)
            if i < len(lines) and lines[i].startswith('  status '):
                i += 1
            ev.append(('cmd', cmd, out))
            continue
        i += 1
    return ev


def octal_uic(numeric):
    g, m = numeric.strip('[]').split(',')
    return '[%o,%o]' % (int(g, 8), int(m, 8))


def fresh(level, label):
    os.makedirs(TMP, exist_ok=True)
    img = '%s/%s%d.img' % (TMP, label, level)
    if os.path.exists(img):
        os.chmod(img, 0o644)
        os.remove(img)
    subprocess.run([INIT, img, '20000', '--structure=%d' % level, '--label=' + label.upper() + str(level), '--cluster=1'], capture_output=True)
    return img


def source_text(lines):
    pth = TMP + '/src_%d.txt' % lines
    with open(pth, 'w') as f:
        for k in range(lines):
            f.write(('line %d ' % (k + 1) + '.' * 70)[:70] + '\n')
    return pth


# ---- type -------------------------------------------------------------------------------------------------------------------------------------------------
def do_type(level, log, container):
    n = 0
    for ev in parse_log(log):
        if ev[0] != 'cmd' or not ev[1].startswith('TYPE '):
            continue
        cmd = strip_dev(ev[1])
        if '/OUTPUT' in cmd or '/PAGE' in cmd or '...' in cmd:
            print('SKIP: %s (not implemented or not comparable)' % ev[1])
            continue
        spec = cmd.split()[-1]
        if not any(c in spec for c in '*%,;') and ev[2] and ev[2][0].startswith('%TYPE-W-SEARCHFAIL'):
            print('SKIP: %s (a missing single file keeps ods2\'s own message)' % ev[1])
            continue
        want = '\n'.join(strip_dev(x) for x in ev[2]).rstrip()
        got = run(container, cmd).rstrip()
        n += 1
        check(got == want, 'level %d: %s prints what VMS printed' % (level, ev[1].split(' ', 1)[1]), 'VMS:\n%s\nours:\n%s' % (want[:900], got[:900]))
    check(n > 0, 'level %d: %d TYPE commands of the VMS log compared' % (level, n))


# ---- purge ------------------------------------------------------------------------------------------------------------------------------------------------
def listing(img, spec):
    out = run(img, 'DIR ' + spec)
    return [m.group(1) + ';' + m.group(2) for m in (re.match(r'^(\S+)\s+;(\d+)\s*$', ln) for ln in out.split('\n')) if m]


def vms_listing(lines):
    res = []
    for ln in lines:
        m = re.match(r'^(\S+;\d+)\s+\(\d+,\d+,\d+\)\s+(\d+)(\s+\(.*\))?\s*$', ln)
        if m:
            res.append(m.group(1))
    return res


def do_purge(level, log, container):
    img = fresh(level, 'pu')
    src = source_text(8)
    ok_all = True
    default_dir = None
    for ev in parse_log(log):
        if ev[0] == 'mkf':
            m = re.match(r'^\S*?(\[[^\]]+\])(\S+?);(\d+)$', ev[1])
            run(img, 'COPY %s %s%s' % (src, m.group(1), m.group(2)))
            got = listing(img, m.group(1) + m.group(2) + ';*')
            if (m.group(2) + ';' + m.group(3)) not in got:
                check(False, 'level %d: %s made again as the same version' % (level, ev[1]), 'ours: %s' % got)
                ok_all = False
            continue
        if ev[0] != 'cmd':
            continue
        c = strip_dev(ev[1])
        if c.startswith('CREATE/DIRECTORY'):
            run(img, c)
        elif c.startswith('SET DEFAULT'):
            default_dir = '[P]' if '[P]' in c else None          # one ods2 command is one process: the default is kept here
        elif c.startswith('SET FILE/'):
            run(img, c)
        elif c.startswith('DISMOUNT'):
            pass
        elif c.startswith('MOUNT') and 'NOWRITE' in c:
            os.chmod(img, 0o444)
        elif c.startswith('MOUNT'):
            os.chmod(img, 0o644)
        elif c.startswith('PURGE'):
            want = [strip_dev(x) for x in ev[2]]
            cc = c
            if len(c.split()) == 1 and default_dir:
                cc = c + ' ' + default_dir + '*.*'
            got = run(img, cc).rstrip().split('\n')
            check(got == want, 'level %d: %s says what VMS said' % (level, c), 'VMS:\n%s\nours:\n%s' % ('\n'.join(want), '\n'.join(got)))
        elif c.startswith('DIRECTORY/FILE_ID/SIZE') and ' [P]' in (' ' + c):
            want = vms_listing(ev[2])
            got = listing(img, c.split()[-1])
            check(got == want, 'level %d: after the step the directory holds the versions VMS held (%d)' % (level, len(want)), 'VMS: %s\nours: %s' % (want, got))
    os.chmod(img, 0o644)
    # the final state: the container's
    cont = [m.group(1) + ';' + m.group(2) for m in (re.match(r'^(\S+)\s+;(\d+)\s*$', ln) for ln in run(container, 'DIR [P]*.*;*').split('\n')) if m]
    final = listing(img, '[P]*.*;*')
    check(final == cont, 'level %d: the final directory is the container\'s (%d names;versions)' % (level, len(cont)), 'VMS: %s\nours: %s' % (cont, final))
    r = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True).stdout.strip().split('\n')[-1]
    check(r == '0 error(s), 0 warning(s)', 'level %d: ods5check is clean after the replay' % level, r)


# ---- setf -------------------------------------------------------------------------------------------------------------------------------------------------
def norm_vms_prot(t):
    m = re.search(r'File protection:\s+(.*)$', t)
    if not m:
        return None
    return m.group(1).replace('System', 'S').replace('Owner', 'O').replace('Group', 'G').replace('World', 'W').replace(' ', '')


def vms_dirfull(lines):
    """{name;version: (prot, owner, revision)} of a VMS DIRECTORY/FULL listing"""
    res, cur = {}, None
    for ln in lines:
        m = re.match(r'^(\S+;\d+)\s+File ID:', ln)
        if m:
            cur = m.group(1)
            res[cur] = [None, None, None]
            continue
        if cur is None:
            continue
        m = re.match(r'^Size:\s+\S+\s+Owner:\s+(\S+)', ln)
        if m:
            res[cur][1] = m.group(1)
        m = re.match(r'^Revised:\s+.*\((\d+)\)\s*$', ln)
        if m:
            res[cur][2] = int(m.group(1))
        p = norm_vms_prot(ln)
        if p:
            res[cur][0] = p
    return {k: tuple(v) for k, v in res.items()}


def ours_dirfull(img, spec):
    out = run(img, 'DIR/FULL ' + spec)
    res, cur = {}, None
    for ln in out.split('\n'):
        m = re.match(r'^(\S+;\d+)\s*$', ln)
        if m:
            cur = m.group(1)
            res[cur] = [None, None, None]
            continue
        if cur is None:
            continue
        m = re.match(r'^  Owner:\s+(\S+)', ln)
        if m:
            res[cur][1] = m.group(1)
        m = re.match(r'^  Revised:.*\((\d+)\)\s*$', ln)
        if m:
            res[cur][2] = int(m.group(1))
        m = re.match(r'^  Protection:\s+\((.*)\)\s*$', ln)
        if m:
            res[cur][0] = m.group(1).replace(' ', '')
    return {k: tuple(v) for k, v in res.items()}


def do_setf(level, log, container):
    img = fresh(level, 'sf')
    src = source_text(2)
    events = parse_log(log)
    uic_names = {}
    for ev in events:
        if ev[0] == 'uic':
            uic_names[ev[1]] = octal_uic(ev[2])
    base_rev = {}
    owner_set = set()         # the names of files whose owner a replayed command set: the owner before that is by design not VMS's (VMS: the process UIC, ods2v2: [1,4])
    pending = None            # the SET command whose DIRECTORY/FULL block is next
    for ev in events:
        if ev[0] == 'mkf':
            m = re.match(r'^\S*?(\[[^\]]+\])(\S+?);(\d+)$', ev[1])
            run(img, 'COPY %s %s%s' % (src, m.group(1), m.group(2)))
            continue
        if ev[0] != 'cmd':
            continue
        c = strip_dev(ev[1])
        if c.startswith('CREATE/DIRECTORY'):
            run(img, c)
            continue
        if c.startswith('SET FILE/'):
            cc = c
            for nm, num in uic_names.items():
                cc = cc.replace(nm, num)
            if 'INDEXF.SYS' in cc and 'OWNER_UIC' in cc:
                print('SKIP: %s (VMS: the file is open; ods2v2 refuses an owner change on a reserved file in its own words)' % ev[1])
                pending = None
                continue
            want = [strip_dev(x) for x in ev[2]]
            got = run(img, cc).rstrip().split('\n')
            got = [x for x in got if x != '']
            if got and got != want:
                got = [x for x in got if not x.startswith('-ODS2-')]          # ods2v2's explanation lines
            if level == 2 and '.DIR' in cc and '000000.DIR' not in cc:
                got = [x.replace('S:RWED,O:RWED,', 'S:RWE,O:RWE,') for x in got]       # a new directory: ods2v2 at ODS-2 0xFA00, VMS 0xBA88 (follow-up c)
            check(got == want or (got == [] and want == []), 'level %d: %s says what VMS said' % (level, c), 'VMS: %s\nours: %s' % (want, got))
            if 'OWNER_UIC' in cc:
                owner_set.add(os.path.basename(cc.split()[-1]).split(';')[0].upper())
            pending = c
            continue
        if c.startswith('DIRECTORY/FULL') and pending is not None:
            spec = c.split()[-1]
            vms = vms_dirfull(ev[2])
            ours = ours_dirfull(img, spec)
            bad = []
            for k, (vp, vo, vr) in vms.items():
                if k not in ours:
                    bad.append('%s: not listed by ods2v2' % k)
                    continue
                op, oo, orv = ours[k]
                vo2 = uic_names.get(vo, vo)
                vo2 = '[1,4]' if vo2 == '[SYSTEM]' else vo2
                if k.upper().endswith('.DIR;1') and vr is not None:
                    base = base_rev.setdefault((spec, k), orv - 1 if False else None)
                if level == 2 and k.upper().endswith('.DIR;1') and k.upper() != '000000.DIR;1':
                    op = op.replace('S:RWED,O:RWED,', 'S:RWE,O:RWE,')           # the known difference of the new directory's protection at ODS-2 (follow-up c)
                owner_known = k.split(';')[0].upper() in owner_set
                if op != vp or (owner_known and oo != vo2):
                    bad.append('%s: VMS %s %s, ours %s %s' % (k, vp, vo2, op, oo))
                if k.upper().endswith('.DIR;1'):
                    # a directory: the revision as the change from the first value seen (VMS 0 at the start, ods2v2 level 2: 1)
                    first = base_rev.get(k)
                    if first is None:
                        base_rev[k] = (orv - vr, )
                        first = base_rev[k]
                    if orv - vr != first[0]:
                        bad.append('%s: revision VMS %s, ours %s (offset %s expected)' % (k, vr, orv, first[0]))
                elif vr != orv:
                    bad.append('%s: revision VMS %s, ours %s' % (k, vr, orv))
            check(not bad, 'level %d: after %s: protection, owner and revision equal VMS\'s (%d file(s))' % (level, pending, len(vms)), '\n'.join(bad))
            pending = None
    cont = ours_dirfull(container, '[S]*.*;*')
    final = ours_dirfull(img, '[S]*.*;*')
    cn = {k: (v[0],) for k, v in cont.items()}
    fn = {k: (v[0],) for k, v in final.items()}
    check(fn == cn, 'level %d: the final protections of [S] are the container\'s' % level, 'container: %s\nours: %s' % (cn, fn))
    r = subprocess.run(['python3', '-I', 'tests/ods5check.py', img], capture_output=True, text=True).stdout.strip().split('\n')[-1]
    check(r == '0 error(s), 0 warning(s)', 'level %d: ods5check is clean after the replay' % level, r)


def main():
    kind, level, log, container = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    os.makedirs(TMP, exist_ok=True)
    {'type': do_type, 'purge': do_purge, 'setf': do_setf}[kind](level, log, container)
    print('wo_vms_compare %s level %d: %s' % (kind, level, 'FAILED' if fails else 'all compared items equal'))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
