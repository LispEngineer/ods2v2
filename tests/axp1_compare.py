#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""axp1_compare.py - ods2v2's read of a VMS-made ODS-5 system disk against the listings VMS made of it (R2).

VMS side (CORPUS.md section 11): <manifest>/AXP1_<tree>_{FULL,IDS,MD5}.LST - DIRECTORY/FULL, DIRECTORY/FILE_ID and
CHECKSUM/ALGORITHM=MD5 for every tree of the disk. ods2v2 side: `ods2 IMAGE "DIR/FULL [tree...]*.*;*"` (names, FIDs,
every attribute DIR/FULL shows) and tools/ods2_filedump (every primary header's record attributes and its bytes as
read from the file: the MD5 of those). Usage:

    axp1_compare.py MANIFEST_DIR IMAGE REPORT [--ods2 ./ods2] [--filedump ./ods2_filedump] [--scratch DIR]

REPORT gets one line per difference and the summary; the summary is also printed. Exit 1 if anything that is not an
explained, known difference is found (a name or FID missing either way, an attribute differing)."""
import argparse, collections, glob, hashlib, os, pickle, re, subprocess, sys

ENT = re.compile(r'^(\S.*?;\d+)\s+File ID:\s+\((\d+),(\d+),(\d+)\)')
VKEYS = (('Created:', 'created'), ('Modified:', 'modified'), ('Expires:', 'expires'), ('Backup:', 'backup'),
         ('Effective:', 'effective'), ('Recording:', 'recording'), ('Accessed:', 'accessed'), ('Attr Mod:', 'attrmod'),
         ('Data Mod:', 'datamod'), ('Linkcount:', 'linkcount'), ('File organization:', 'org'), ('Record format:', 'rfm'),
         ('Record attributes:', 'rat'), ('File protection:', 'prot'), ('Access Cntrl List:', 'acl'))


def parse_vms(path):
    out, cur, d, last = [], None, None, None
    for line in open(path, encoding='latin-1'):
        line = line.rstrip('\n')
        if line.startswith('Directory '):
            d, cur = line[10:].strip(), None
            continue
        m = ENT.match(line)
        if m:
            cur = {'dir': re.sub(r'^[A-Z0-9$_]+:\[(.*)\]$', r'\1', d), 'name': m.group(1),
                   'fid': (int(m.group(2)), int(m.group(3)), int(m.group(4)))}
            out.append(cur)
            continue
        if cur is None or not line.strip():
            continue
        if line.startswith('Size:'):
            m = re.match(r'Size:\s+(\d+)/(\d+)\s+Owner:\s+(.*)$', line)
            if m:
                cur['used'], cur['alloc'], cur['owner'] = int(m.group(1)), int(m.group(2)), m.group(3).strip()
            continue
        for key, f in VKEYS:
            if line.startswith(key):
                cur[f] = line[len(key):].strip()
                last = f
                break
        else:
            if line.startswith(' ') and last == 'acl' and 'acl' in cur:
                cur['acl'] += ' ' + line.strip()
    return out


def parse_ours(text):
    out, cur, d = [], None, None
    for line in text.split('\n'):
        if line.startswith('Directory '):
            d, cur = line[10:].strip().strip('[]'), None
            continue
        if line and not line.startswith((' ', '%', 'Total', 'Grand')):
            cur = {'dir': d, 'name': line.strip()}
            out.append(cur)
            continue
        if cur is None:
            continue
        if re.match(r'^\s{10,}\(', line) and 'acl' in cur:
            cur['acl'] += ' ' + line.strip()
            continue
        m = re.match(r'\s+([A-Za-z ]+?):\s+(.*)$', line)
        if not m:
            continue
        k, v = m.group(1), m.group(2).strip()
        if k == 'File ID':
            cur['fid'] = tuple(int(x) for x in re.match(r'\((\d+),(\d+),(\d+)\)', v).groups())
        elif k == 'Size':
            mm = re.match(r'(\d+)/(\d+) blocks', v)
            if mm:
                cur['used'], cur['alloc'] = int(mm.group(1)), int(mm.group(2))
        else:
            f = {'Owner': 'owner', 'Protection': 'prot', 'Created': 'created', 'Revised': 'modified', 'Expires': 'expires',
                 'Backup': 'backup', 'Accessed': 'accessed', 'Attributes changed': 'attrmod', 'Data modified': 'datamod',
                 'Link count': 'linkcount', 'File organization': 'org', 'Record format': 'rfm',
                 'Record attributes': 'rat', 'Access Cntrl List': 'acl'}.get(k)
            if f:
                cur[f] = v
    return out


def day(v):          # VMS prints a one-digit day without the zero
    return re.sub(r'^0(\d-)', r'\1', v or '<absent>')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('manifest'); ap.add_argument('image'); ap.add_argument('report')
    ap.add_argument('--ods2', default='./ods2'); ap.add_argument('--filedump', default='./ods2_filedump')
    ap.add_argument('--scratch', default=os.environ.get('TMPDIR', '.'))
    a = ap.parse_args()
    rep = open(a.report, 'w')

    def log(s):
        rep.write(s + '\n')

    trees = sorted(re.match(r'AXP1_(.*)_FULL\.LST$', os.path.basename(p)).group(1) for p in glob.glob(a.manifest + '/AXP1_*_FULL.LST'))
    vms, ours = {}, {}
    for t in trees:
        vms[t] = parse_vms('%s/AXP1_%s_FULL.LST' % (a.manifest, t))
        spec = '[000000]*.*;*' if t == 'ROOT' else '[%s...]*.*;*' % t
        r = subprocess.run([a.ods2, a.image, 'DIR/FULL ' + spec], capture_output=True, text=True, errors='replace')
        ours[t] = parse_ours(r.stdout)
    S = collections.Counter(); problems = 0
    owner = collections.defaultdict(set)
    # (a) names and FIDs, both ways, and (b) the attributes
    for t in trees:
        vk = {(e['dir'], e['name']): e for e in vms[t]}
        ok = {(e['dir'], e['name']): e for e in ours[t]}
        S['names VMS'] += len(vk); S['names ods2v2'] += len(ok)
        for k in vk.keys() - ok.keys():
            log('ONLY VMS   %s %s' % (t, k)); S['only VMS'] += 1; problems += 1
        for k in ok.keys() - vk.keys():
            log('ONLY ODS2V2 %s %s' % (t, k)); S['only ods2v2'] += 1; problems += 1
        for k in vk.keys() & ok.keys():
            v, o = vk[k], ok[k]
            diffs = []
            def chk(f, x, y):
                if x != y:
                    diffs.append((f, x, y))
            chk('fid', v['fid'], o.get('fid'))
            chk('used', v.get('used'), o.get('used')); chk('alloc', v.get('alloc'), o.get('alloc'))
            owner[v.get('owner')].add(o.get('owner'))
            chk('prot', '(' + v['prot'].replace('System:', 'S:').replace('Owner:', 'O:').replace('Group:', 'G:').replace('World:', 'W:') + ')', o.get('prot'))
            for f in ('created', 'expires', 'backup', 'accessed', 'attrmod', 'datamod'):
                chk(f, v.get(f, ''), day(o.get(f)))
            mv = re.match(r'(.*?)\s*\((\d+)\)$', v.get('modified', '')); mo = re.match(r'(.*?)\s*\((\d+)\)$', o.get('modified', ''))
            chk('modified', mv and mv.group(1), mo and day(mo.group(1))); chk('revisions', mv and mv.group(2), mo and mo.group(2))
            chk('linkcount', v.get('linkcount'), o.get('linkcount'))
            chk('org', v.get('org'), o.get('org')); chk('rfm', v.get('rfm'), o.get('rfm')); chk('rat', v.get('rat'), o.get('rat'))
            va = re.sub(r'\s+', '', v.get('acl', 'None')); oa = re.sub(r'\s+', '', o.get('acl', 'None'))
            if va != oa:
                # explained: rights-database names ods2v2 has no database for ([SCA$SERVER] = [370,355], DFS$SERVICE =
                # %X80010002), and the hidden APPLICATION ACE VMS shows only with SECURITY
                idx = r'IDENTIFIER=(?:\[[^\]]*\]|[^,)]*)'
                sv = re.sub(idx, 'ID', va); so = re.sub(idx, 'ID', oa)
                if sv == so:
                    S['ACL: same ACEs, an identifier named by VMS (rights database)'] += 1
                elif v['name'].startswith('DEFSTYLE.DDIF') and 'APPLICATION' in oa and va == 'None':
                    S['ACL: the hidden APPLICATION ACE (VMS shows it only with SECURITY)'] += 1
                else:
                    diffs.append(('acl', va, oa))
            for f, x, y in diffs:
                if f == 'org' and re.match(r'(Indexed|Relative)', x or '') and (y or '') == (x or '').split(',')[0]:
                    S['org: VMS adds the prolog / key count of an Indexed or Relative file (RMS area, not read)'] += 1
                    continue
                S['differences ' + f] += 1; problems += 1
                log('DIFF %-9s %s %s: VMS %r  ods2v2 %r' % (f, t, k, x, y))
    S['owner names VMS'] = len(owner)
    S['owner names that stand for more than one UIC'] = sum(1 for v in owner.values() if len(v) > 1)
    # (c) content: what VMS's MD5 hashes, and ods2v2's bytes
    spec_fid = {}
    for p in glob.glob(a.manifest + '/AXP1_*_IDS.LST'):
        L = open(p, encoding='latin-1').read().split('\n')
        for i, l in enumerate(L):
            if l.startswith('DKA200:'):
                m = re.match(r'\s+\((\d+),(\d+),(\d+)\)', L[i + 1])
                if m:
                    spec_fid[l.strip()] = tuple(int(x) for x in m.groups())
    vmd5 = collections.defaultdict(set); verr = []
    for p in glob.glob(a.manifest + '/AXP1_*_MD5.LST'):
        for l in open(p, encoding='latin-1'):
            l = l.rstrip('\n')
            if l.startswith('MD5 '):
                h, spec = l[4:].split(' ', 1)
                vmd5[spec_fid[spec]].add(h)
            elif l.startswith('ERROR'):
                verr.append(l)
    S['VMS MD5 errors'] = len(verr)
    for l in verr:
        log('VMS CHECKSUM ERROR ' + l)
    p = subprocess.Popen([a.filedump, a.image], stdout=subprocess.PIPE, bufsize=1 << 20, stderr=subprocess.DEVNULL)
    f = p.stdout; headers = {}; match = collections.Counter()
    while True:
        line = f.readline()
        if not line:
            break
        parts = line.split()
        if parts[0] == b'E':
            S['files ods2v2 could not read whole'] += 1
            continue
        num, seq, lev, fc, rtype, rattr, rsize, maxrec, vfc, hiblk, efblk, ffbyte, length = [int(x, 16) if i == 3 else int(x) for i, x in enumerate(parts[1:14])]
        status = parts[14].decode(); fid = (num, seq, 0)
        data = b''
        if status == 'OK':
            data = f.read(length); f.readline()
        headers[fid] = status
        if fid in vmd5:
            org = rtype >> 4
            mine = hashlib.md5(data).hexdigest().upper()
            if mine in vmd5[fid]:
                match['sequential, raw bytes' if org == 0 else 'non-sequential, raw bytes'] += 1
            elif org in (1, 2):
                match['Indexed/Relative: VMS hashes the records in key order'] += 1
                log('CONTENT Indexed/Relative file %s (organisation %d): the MD5 of its bytes differs, as expected' % (fid, org))
            else:
                match['MISMATCH'] += 1; problems += 1
                log('CONTENT MISMATCH %s (rtype %d)' % (fid, rtype))
    p.wait()
    for k, v in match.items():
        S['content: ' + k] = v
    listed = {e['fid'] for t in trees for e in vms[t]}
    S['headers ods2v2 reads (valid primary)'] = len(headers)
    S['FIDs VMS lists'] = len(listed)
    for fid in listed - headers.keys():
        log('FID listed by VMS, no header in ods2v2: %s' % (fid,)); problems += 1
    for fid in headers.keys() - listed:
        S['headers ods2v2 reads that VMS lists nowhere (%s)' % headers[fid]] += 1
        log('header not listed by VMS: %s (%s)' % (fid, headers[fid]))
    rep.write('\n--- summary ---\n')
    for k in sorted(S):
        rep.write('%-90s %d\n' % (k, S[k])); print('%-90s %d' % (k, S[k]))
    print('problems (unexplained):', problems)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
