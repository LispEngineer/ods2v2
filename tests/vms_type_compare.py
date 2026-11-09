#!/usr/bin/env python3
"""Compare ods2 TYPE with the records VMS TYPE wrote (m2 binary batch log): usage vms_type_compare.py ODS2 IMAGE LOG"""
import re, struct, subprocess, sys
ods2, img, log = sys.argv[1:4]
d = open(log, 'rb').read()
recs = []; o = 0
while o + 2 <= len(d):
    n = struct.unpack_from('<H', d, o)[0]
    if n == 0xFFFF: o = (o // 512 + 1) * 512; continue
    recs.append(d[o + 2:o + 2 + n]); o += 2 + n + (n & 1)
secs = {}; cur = None
for r in recs:
    m = re.match(rb'..=== BEGIN (\S+)', r, re.S)
    if m: cur = m.group(1).decode(); secs[cur] = []; continue
    if r[2:].startswith(b'=== END'): cur = None; continue
    if cur: secs[cur].append((r[0], r[1], r[2:]))
def text(trip):
    out = bytearray(); st = 0   # 0 none, 1 eol, 2 mid
    for p, q, data in trip:
        if p & 0x80:
            if st == 1: out += b'\n'
            out.append(p & 0x7f)
        elif p == 0:
            if st == 1: out += b'\r'
        else:
            out += b'\n' * (p if st else p - 1)
        out += data
        if q == 0x8D: st = 1
        else:
            st = 2
            if q & 0x80: out.append(q & 0x7f)
            else: out += b'\n' * q
    if st == 1: out += b'\n'
    return bytes(out)
bad = 0
for name, trip in sorted(secs.items()):
    if any(r[2].startswith(b'%TYPE-') for r in trip):
        run = subprocess.run([ods2, img, 'TYPE [000000]' + name], capture_output=True)
        ok = run.returncode != 0 and run.stdout == b''
        bad += 0 if ok else 1
        print('%-4s %-14s VMS TYPE fails (%s); ods2 exit %d, %d bytes of output' % ('OK' if ok else 'DIFF', name, trip[1][2].decode(errors='replace')[:40], run.returncode, len(run.stdout)))
        continue
    want = text(trip)
    got = subprocess.run([ods2, img, 'TYPE [000000]' + name], capture_output=True).stdout
    ok = got == want
    if not ok: bad += 1
    print('%-4s %-14s %6d records  %d bytes' % ('OK' if ok else 'DIFF', name, len(trip), len(want)))
    if not ok and len(want) < 400:
        print('   vms ', want); print('   ours', got)
print('differences:', bad)
