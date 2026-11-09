#!/usr/bin/env python3
"""mkrms_image.py ODS2 ODS2_INIT IMAGE OUTDIR -- build an ODS-5 test image of RMS sequential files with chosen
record formats / attributes (raw bytes copied in with COPY, then the FAT in the file header patched and the
header checksum redone).  Writes OUTDIR/MANIFEST.txt (name, rfm, rat, vfc, bytes) and the expected-text files
for formats whose text is unambiguous.  By ods5-wr2 for Douglas P. Fields, Jr. (symbolics@lisp.engineer), MIT."""
import os, struct, subprocess, sys

RFM = dict(UDF=0, FIX=1, VAR=2, VFC=3, STM=4, STMLF=5, STMCR=6)
FTN, CR, PRN, BLK = 1, 2, 4, 8

def var(recs, vfc=None, blk=False, pad_block=False):
    out = bytearray()
    for r in recs:
        h = vfc.pop(0) if vfc else b''
        body = h + r
        rec = struct.pack('<H', len(body)) + body + (b'\0' if len(body) & 1 else b'')
        if blk and (len(out) % 512) + len(rec) > 512:
            out += b'\xff' * (512 - len(out) % 512)
        out += rec
    return bytes(out)

lines = [b'line %03d of the test' % i for i in range(1, 41)]
lines[4] = b''
lines[6] = b'odd'
lines[9] = b'x' * 130
FILES = []   # name, rfm, rat, vfcsize, rsize, maxrec, bytes
def add(name, rfm, rat, data, vfcsize=0, rsize=0, maxrec=0):
    FILES.append((name, rfm, rat, vfcsize, rsize, maxrec, data))

add('VAR_CR.TXT',  'VAR', CR,  var(lines), rsize=130, maxrec=0)
add('VAR_NONE.TXT','VAR', 0,   var(lines), rsize=130)
add('VAR_BLK.TXT', 'VAR', CR|BLK, var(lines, blk=True), rsize=130)
add('VAR_FTN.TXT', 'VAR', FTN, var([b' first', b'0double', b'-triple', b'1new page', b'+over', b' next', b'$prompt', b' last']), rsize=20)
add('VFC_PRN.TXT', 'VFC', PRN, var([b'alpha', b'beta', b'gamma', b'delta', b'epsilon', b'zeta', b'eta'],
    vfc=[bytes([1,0x8d]), bytes([2,0x8d]), bytes([0,0x8d]), bytes([0x8c,0x8d]), bytes([3,0x8d]), bytes([1,0x8d]), bytes([0x81,0x8d])]), vfcsize=2, rsize=20)
add('VFC_CR.TXT',  'VFC', CR,  var([b'one', b'two', b'three'], vfc=[bytes([1,0x8d])]*3), vfcsize=2, rsize=20)
add('VFC_NONE.TXT','VFC', 0,   var([b'one', b'two', b'three'], vfc=[bytes([1,0x8d])]*3), vfcsize=2, rsize=20)
add('VFC_4.TXT',   'VFC', PRN, var([b'one', b'two'], vfc=[bytes([1,0x8d,9,9])]*2), vfcsize=4, rsize=20)
add('FIX_NONE.TXT','FIX', 0,   b''.join(b'%-20s' % (b'rec %d' % i) for i in range(1, 9)), rsize=20, maxrec=20)
add('FIX_CR.TXT',  'FIX', CR,  b''.join(b'%-20s' % (b'rec %d' % i) for i in range(1, 9)), rsize=20, maxrec=20)
add('FIX_FTN.TXT', 'FIX', FTN, b''.join(b'%-20s' % (c + b'rec') for c in (b' ', b'0', b'1', b'+', b' ')), rsize=20, maxrec=20)
add('FIX_SHORT.TXT','FIX', 0,  b''.join(b'%-20s' % (b'rec %d' % i) for i in range(1, 4)) + b'tail', rsize=20, maxrec=20)
add('STM_CR.TXT',  'STM', CR,  b'one\r\ntwo\r\n\r\nfour has a lone LF \n inside\r\nlast no end')
add('STMLF_CR.TXT','STMLF', CR, b'one\ntwo\n\nfour\nlast no end')
add('STMLF_END.TXT','STMLF', CR, b'one\ntwo\n')
add('STMCR_CR.TXT','STMCR', CR, b'one\rtwo\r\rfour\rlast no end')
add('UDF.BIN',     'UDF', 0,   bytes(range(256)) * 3)
add('BAD_VAR.TXT', 'VAR', CR, b'\x00\x80abc')
add('EMPTY_VAR.TXT','VAR', CR, b'')
add('BIG_VAR.TXT', 'VAR', CR,  var([b'record number %06d ' % i + b'z' * (i % 97) for i in range(1, 4000)]), rsize=200)

# stream and carriage-control cases measured in batch m2b; VMS TYPE refuses VAR_PRN, FIX_PRN, FIX_CRFTN (RMS-F-RAT) and BAD_VAR (RMS-F-IRC)
TYPE_FAILS = ['VAR_PRN.TXT', 'FIX_PRN.TXT', 'FIX_CRFTN.TXT', 'BAD_VAR.TXT']
add('STM_NONE.TXT',  'STM',   0, b'one\r\ntwo\r\nthree')
add('STMLF_NONE.TXT','STMLF', 0, b'one\ntwo\nthree\n')
add('STMCR_NONE.TXT','STMCR', 0, b'one\rtwo\rthree\r')
add('STM_FF.TXT',    'STM',   CR, b'one\x0ctwo\x0bthree\r\nfour')
add('STMLF_FF.TXT',  'STMLF', CR, b'one\x0ctwo\nthree')
add('VAR_PRN.TXT',   'VAR',   PRN, var([b'alpha', b'beta']), rsize=20)
add('VFC_FTN.TXT',   'VFC',   FTN, var([b' abc', b'0def', b'1ghi'], vfc=[bytes([1,0x8d])] * 3), vfcsize=2, rsize=20)
add('FTN_ODD.TXT',   'VAR',   FTN, var([b'\0nul', b'xother', b'', b'2two', b' end', b'$', b'+']), rsize=20)
add('VFC_PRN_ODD.TXT','VFC',  PRN, var([b'a', b'b', b'c', b'd', b'e', b'f'],
    vfc=[bytes([4,0]), bytes([0,0]), bytes([5,0x8a]), bytes([1,1]), bytes([0x80,0x8d]), bytes([2,0x8c])]), vfcsize=2, rsize=20)
add('FIX_PRN.TXT',   'FIX',   PRN, b''.join(b'%-10s' % (b'r%d' % i) for i in range(1, 5)), rsize=10, maxrec=10)
add('FIX_CRFTN.TXT', 'FIX',   CR|FTN, b''.join(b'%-10s' % (b' r%d' % i) for i in range(1, 4)), rsize=10, maxrec=10)

# the text VMS TYPE shows for each file, from the records VMS V8.4-2L2 wrote with TYPE/OUTPUT (batches m2 and m2b),
# as Linux text (a carriage return after the data = a newline, a bare CR = overprint): ods5-wr2 batch m2, 9-NOV-2026
VMS_EXPECT = {
    'FTN_ODD.TXT': b'nul\nother\n\ntwo\nend\n\n',
    'STMCR_NONE.TXT': b'one\ntwo\nthree\n',
    'STMLF_FF.TXT': b'one\x0ctwo\nthree\n',
    'STMLF_NONE.TXT': b'one\ntwo\nthree\n',
    'STM_FF.TXT': b'one\x0c\ntwo\x0b\nthree\nfour\n',
    'STM_NONE.TXT': b'one\ntwo\nthree\n',
    'VFC_FTN.TXT': b'abc\n\ndef\n\x0cghi\n',
    'VFC_PRN_ODD.TXT': b'\n\n\nab\n\n\n\n\nc\n\nd\n\x00e\n\nf\x0c',
    'EMPTY_VAR.TXT': b'',
    'FIX_CR.TXT': b'rec 1               \nrec 2               \nrec 3               \nrec 4               \nrec 5               \nrec 6               \nrec 7               \nrec 8               \n',
    'FIX_FTN.TXT': b'rec                \n\nrec                \n\x0crec                \rrec                \nrec                \n',
    'FIX_NONE.TXT': b'rec 1               rec 2               rec 3               rec 4               rec 5               rec 6               rec 7               rec 8               ',
    'FIX_SHORT.TXT': b'rec 1               rec 2               rec 3               tail\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00',
    'STMCR_CR.TXT': b'one\ntwo\n\nfour\nlast no end\n',
    'STMLF_CR.TXT': b'one\ntwo\n\nfour\nlast no end\n',
    'STMLF_END.TXT': b'one\ntwo\n',
    'STM_CR.TXT': b'one\ntwo\n\nfour has a lone LF \n\n inside\nlast no end\n',
    'VAR_BLK.TXT': b'line 001 of the test\nline 002 of the test\nline 003 of the test\nline 004 of the test\n\nline 006 of the test\nodd\nline 008 of the test\nline 009 of the test\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nline 011 of the test\nline 012 of the test\nline 013 of the test\nline 014 of the test\nline 015 of the test\nline 016 of the test\nline 017 of the test\nline 018 of the test\nline 019 of the test\nline 020 of the test\nline 021 of the test\nline 022 of the test\nline 023 of the test\nline 024 of the test\nline 025 of the test\nline 026 of the test\nline 027 of the test\nline 028 of the test\nline 029 of the test\nline 030 of the test\nline 031 of the test\nline 032 of the test\nline 033 of the test\nline 034 of the test\nline 035 of the test\nline 036 of the test\nline 037 of the test\nline 038 of the test\nline 039 of the test\nline 040 of the test\n',
    'VAR_CR.TXT': b'line 001 of the test\nline 002 of the test\nline 003 of the test\nline 004 of the test\n\nline 006 of the test\nodd\nline 008 of the test\nline 009 of the test\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nline 011 of the test\nline 012 of the test\nline 013 of the test\nline 014 of the test\nline 015 of the test\nline 016 of the test\nline 017 of the test\nline 018 of the test\nline 019 of the test\nline 020 of the test\nline 021 of the test\nline 022 of the test\nline 023 of the test\nline 024 of the test\nline 025 of the test\nline 026 of the test\nline 027 of the test\nline 028 of the test\nline 029 of the test\nline 030 of the test\nline 031 of the test\nline 032 of the test\nline 033 of the test\nline 034 of the test\nline 035 of the test\nline 036 of the test\nline 037 of the test\nline 038 of the test\nline 039 of the test\nline 040 of the test\n',
    'VAR_FTN.TXT': b'first\n\ndouble\ntriple\n\x0cnew page\rover\nnext\nprompt\nlast\n',
    'VAR_NONE.TXT': b'line 001 of the testline 002 of the testline 003 of the testline 004 of the testline 006 of the testoddline 008 of the testline 009 of the testxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxline 011 of the testline 012 of the testline 013 of the testline 014 of the testline 015 of the testline 016 of the testline 017 of the testline 018 of the testline 019 of the testline 020 of the testline 021 of the testline 022 of the testline 023 of the testline 024 of the testline 025 of the testline 026 of the testline 027 of the testline 028 of the testline 029 of the testline 030 of the testline 031 of the testline 032 of the testline 033 of the testline 034 of the testline 035 of the testline 036 of the testline 037 of the testline 038 of the testline 039 of the testline 040 of the test',
    'VFC_4.TXT': b'one\ntwo\n',
    'VFC_CR.TXT': b'one\ntwo\nthree\n',
    'VFC_NONE.TXT': b'onetwothree',
    'VFC_PRN.TXT': b'alpha\n\nbeta\rgamma\n\x0cdelta\n\n\nepsilon\nzeta\n\x01eta\n',
}
EXPECT = dict(VMS_EXPECT)
EXPECT['UDF.BIN'] = bytes(range(256)) * 3
EXPECT['BIG_VAR.TXT'] = b''.join(b'record number %06d ' % i + b'z' * (i % 97) + b'\n' for i in range(1, 4000))

def u16(b, o): return struct.unpack_from('<H', b, o)[0]

def decode_map(b):
    o = b[1] * 2; end = o + b[58] * 2; ext = []
    while o < end:
        w = u16(b, o); fmt = w >> 14
        if fmt == 0: o += 2
        elif fmt == 1: ext.append((((w >> 8) & 0x3F) << 16 | u16(b, o + 2), (w & 0xFF) + 1)); o += 4
        elif fmt == 2: ext.append((struct.unpack_from('<I', b, o + 2)[0], (w & 0x3FFF) + 1)); o += 6
        else: ext.append((struct.unpack_from('<I', b, o + 4)[0], (((w & 0x3FFF) << 16) | u16(b, o + 2)) + 1)); o += 8
    return ext

def hdr_lbn(f, base, n):
    """LBN of the file header of file number n: header 1 is at `base`; INDEXF.SYS may continue in extension headers"""
    f.seek(base * 512); h1 = f.read(512)
    ext = decode_map(h1)
    seen = 0; hb = h1
    while seen < 16:
        efn = u16(hb, 14)
        if efn == 0: break
        # the extension header: INDEXF's own header n=efn, in the first extent
        f.seek((base + efn - 1) * 512); hb = f.read(512)
        ext += decode_map(hb); seen += 1
    v = 1; bvbn = None
    for l, c in ext:
        if l <= base < l + c: bvbn = v + base - l; break
        v += c
    vbn = bvbn + n - 1   # VBN of header n
    v = 1
    for l, c in ext:
        if vbn < v + c: return l + vbn - v
        v += c
    raise SystemExit('header %d beyond INDEXF map' % n)

def main():
    ods2, init, img, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    if os.path.exists(img): os.remove(img)
    subprocess.check_call([init, img, '16M', '--structure=5', '--label=RMSTEST'], stdout=subprocess.DEVNULL)
    man = open(os.path.join(out, 'MANIFEST.txt'), 'w')
    for name, rfm, rat, vfcsize, rsize, maxrec, data in FILES:
        src = os.path.join(out, name)
        open(src, 'wb').write(data)
        subprocess.check_call([ods2, img, 'COPY/ATTRIBUTES=(RFM:UDF) %s [000000]%s' % (src, name)], stdout=subprocess.DEVNULL)
        man.write('%s %s rat=%d vfc=%d rsize=%d maxrec=%d bytes=%d\n' % (name, rfm, rat, vfcsize, rsize, maxrec, len(data)))
    man.close()
    os.makedirs(os.path.join(out, 'expect'), exist_ok=True)
    for name, text in EXPECT.items():
        open(os.path.join(out, 'expect', name), 'wb').write(text)
    # patch the FATs
    f = open(img, 'r+b')
    h = f.read(1024)[512:]
    ibmaplbn = struct.unpack_from('<I', h, 24)[0]; ibmapsize = u16(h, 32)
    base = ibmaplbn + ibmapsize
    listing = subprocess.run([ods2, img, 'DIR/FULL [000000]*.*'], capture_output=True, text=True).stdout
    cur = None; ids = {}
    for ln in listing.splitlines():
        if ln.endswith(';1') and not ln.startswith(' '): cur = ln.split(';')[0]
        if 'File ID:' in ln and cur: ids[cur] = int(ln.split('(')[1].split(',')[0])
    for name, rfm, rat, vfcsize, rsize, maxrec, data in FILES:
        n = ids[name]
        lbn = hdr_lbn(f, base, n)
        f.seek(lbn * 512); hd = bytearray(f.read(512))
        hd[20] = RFM[rfm]; hd[21] = rat
        struct.pack_into('<H', hd, 22, rsize)
        hd[35] = vfcsize
        struct.pack_into('<H', hd, 36, maxrec)
        s = sum(struct.unpack_from('<255H', hd, 0)) & 0xFFFF
        struct.pack_into('<H', hd, 510, s)
        f.seek(lbn * 512); f.write(hd)
    f.close()
    print('ok', len(FILES), 'files')
main()
