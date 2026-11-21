#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""dirhdr_vms_compare.py VMS_IMAGE : the header of a directory ods2v2 makes on an ODS-2 volume against the header of C.DIR in one of ods5-vms3's step 0 containers
(phvs2-copy-2.img: VAX V7.3; asim5-copy-2.img: V8.4, ODS-2), both made by CREATE/DIRECTORY (follow-up c; CORPUS 49 and 53). Run from the project root; needs ./ods2 and
./ods2_init; the container is read through a copy.

Equal, byte for byte: the offsets, the segment number, the structure level, the file id, the record attribute area (type, attributes, size, HIBLK, EFBLK, FFBYTE, maximum
record, version limit), the characteristics, RECPROT, MAP_INUSE, ACC_MODE, FILEPROT, the back link, HIGHWATER, the file name area, the revision number and the ACL area.
Allowed to differ, by design: the owner (VMS: the process UIC; ods2v2: [1,4]), the dates, the retrieval pointer's LBN, the checksum."""
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ods5check as O   # noqa: E402
from dirhdr_fields import header, fnum   # noqa: E402

ODS2, INIT = './ods2', './ods2_init'
TMP = os.environ.get('TMPDIR', 'samples') + '/dirhdr_vms_compare'
fails = 0


def check(ok, text, detail=''):
    global fails
    print('%s: %s' % ('PASS' if ok else 'FAIL', text))
    if not ok:
        fails += 1
        if detail:
            print('      ' + detail.replace('\n', '\n      '))


def main():
    os.makedirs(TMP, exist_ok=True)
    vms = TMP + '/vms.img'
    shutil.copyfile(sys.argv[1], vms)
    ours = TMP + '/ours.img'
    if os.path.exists(ours):
        os.remove(ours)
    subprocess.run([INIT, ours, '20000', '--structure=2', '--cluster=1', '--label=DHC'], capture_output=True)
    src = TMP + '/f.txt'
    open(src, 'w').write('one\n')
    subprocess.run([ODS2, ours, 'CREATE/DIRECTORY [C]'], capture_output=True)
    subprocess.run([ODS2, ours, 'COPY %s [C]A.TXT' % src], capture_output=True)
    hv = header(O.Vol(vms), fnum(vms, '[000000]C.DIR'))
    ho = header(O.Vol(ours), fnum(ours, '[000000]C.DIR'))
    fields = [(0, 1, 'IDOFFSET'), (1, 1, 'MPOFFSET'), (2, 1, 'ACOFFSET'), (3, 1, 'RSOFFSET'), (4, 2, 'SEG_NUM'), (6, 2, 'STRUCLEV'), (8, 6, 'FID'), (14, 6, 'EXT_FID'),
              (20, 32, 'record attributes'), (52, 4, 'FILECHAR'), (56, 2, 'RECPROT'), (58, 1, 'MAP_INUSE'), (59, 1, 'ACC_MODE'), (64, 2, 'FILEPROT'),
              (66, 6, 'BACKLINK'), (72, 2, 'journal'), (76, 4, 'HIGHWATER')]
    for off, n, nm in fields:
        check(hv[off:off + n] == ho[off:off + n], '%s: %s equals VMS\'s' % (os.path.basename(sys.argv[1]), nm), 'VMS %s, ours %s' % (hv[off:off + n].hex(), ho[off:off + n].hex()))
    iv, io = hv[0] * 2, ho[0] * 2
    check(hv[iv:iv + 20] == ho[io:io + 20], '%s: the file name area equals VMS\'s' % os.path.basename(sys.argv[1]), '%r %r' % (hv[iv:iv + 20], ho[io:io + 20]))
    check(hv[iv + 20:iv + 22] == ho[io + 20:io + 22], '%s: the revision number equals VMS\'s' % os.path.basename(sys.argv[1]), 'VMS %d, ours %d' % (struct.unpack_from('<H', hv, iv + 20)[0], struct.unpack_from('<H', ho, io + 20)[0]))
    check(hv[hv[2] * 2:hv[3] * 2] == ho[ho[2] * 2:ho[3] * 2], '%s: the ACL area equals VMS\'s' % os.path.basename(sys.argv[1]))
    print('dirhdr_vms_compare %s: %s' % (os.path.basename(sys.argv[1]), 'FAILED' if fails else 'all compared fields equal'))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
