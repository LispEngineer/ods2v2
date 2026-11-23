#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""rename_expect.py - the table of what OpenVMS does for RENAME, case by case (CORPUS 65; ods5-vms3's RENCASE.COM cases), as data. Two readers:
  * tests/rename_check.py builds the same relations on an ods2v2 volume and checks that ods2 RENAME makes the same changes;
  * ods5-cli's corpus/tools/rename_expect_check.py checks THIS TABLE against VMS's own DUMP/HEADER and DUMP/BLOCK text of the cases (corpus/manifests/rename-65), so a wrong
    number here is caught against the evidence.
No imports: it is read by path. Every entry is keyed by the RENCASE case id.
Fields:  vols      the volume kinds the case was measured on: '2' ODS-2, '5' ODS-5, '5h' ODS-5 with HARDLINKS (the tags RNR2/RNV2, RNR5, RNRH of the evidence)
         old       the file as the command names it (device left out); new the target as typed; stored the header's File name before and after (as DUMP/HEADER prints it; None where the
                   name is UCS-2 and DUMP prints it in 16-bit pieces); dir the directory whose entries change; removed / added its entries (name;version) that disappear / appear
         revision  the change of the Revision number of the header of the file the command names: 1, or 0 when the header is not touched (an alias entry); vms_bumps_refused: VMS bumps it
                   although the command is refused (d3: an intended difference, ods2v2 leaves the image unchanged)
         header    'changed' (name area / revision / date / checksum) or 'same' (not one field of the header changes)
         nametype  the File name type VMS prints before and after (it stays with the name); namelen the File name length before and after (bytes, ';version' included)
         linkcount the File entry linkcount of the file (HARDLINKS volumes); status 'ok' or 'refused'; msgs the substrings of the messages of a refused command
         alias     the name of the alias entry the case needs ([Y]...), and untouched: other versions' headers that must not change; after the header's spec after the rename where it is not the new
                   name with ;1; file the header of this file is the one compared (an alias entry's file); untouched_extra more versions the test checks that VMS's dumps do not show (only ;3 is dumped); follows the case whose result volume this one starts from (VMS ran a1, a2, a3 one after the other on one volume); ods2v2_msgs the messages ods2v2 prints where they are not VMS's (a read-only image
                   gives the READONLY refusal every ods2v2 write command gives)."""

# The VERSION a renamed file gets (review 65 C1) is measured only in part: e2 shows "the source's version if that name;version is free, else one above the highest"; every single-file case has
# a source of version 1, so only cases both readings agree on are here (a1-a3, d1: ;1 is taken, so ;4). The two settling cases (RENAME M.TXT;5 N.TXT; RENAME M.TXT;5 T.TXT with ;1 ;2 ;3 present)
# are asked of ods5-vms3 (j3, j4); step 4 (wildcards) waits for them. No case here renames a file with extension headers (C2) or a file whose header names no entry (C5).
# f1 is the case-only rename of review 65 C3: the stored case changes and the name does NOT become a new version (the test lists the directory: abc.txt;1 goes, ABC.txt;1 appears).
CASES = {
    'a1': dict(vols=('2', '5', '5h'), old='[X]A.TXT;1', new='[X]B.TXT', stored=('A.TXT;1', 'B.TXT;1'), dir='[X]', removed=['A.TXT;1'], added=['B.TXT;1'], revision=1, header='changed', status='ok'),
    'a2': dict(vols=('2', '5', '5h'), old='[X]B.TXT;1', new='[X]B.DAT', stored=('B.TXT;1', 'B.DAT;1'), dir='[X]', removed=['B.TXT;1'], added=['B.DAT;1'], revision=1, header='changed', status='ok', follows='a1'),
    'a3': dict(vols=('2', '5', '5h'), old='[X]B.DAT;1', new='[X]B.DAT;5', stored=('B.DAT;1', 'B.DAT;5'), dir='[X]', removed=['B.DAT;1'], added=['B.DAT;5'], revision=1, header='changed', status='ok', follows='a2'),
    # the target name exists (T.TXT;1 ;2 ;3 in [X2]): the file becomes T.TXT;4; the three headers of the other versions do not change
    'd1': dict(vols=('2', '5', '5h'), old='[X2]M.TXT;1', new='[X2]T.TXT', stored=('M.TXT;1', 'T.TXT;4'), dir='[X2]', removed=['M.TXT;1'], added=['T.TXT;4'], revision=1, header='changed', status='ok',
               untouched=['T.TXT;3'], untouched_extra=['T.TXT;1', 'T.TXT;2'], after='[X2]T.TXT;4'),
    # an explicit version that exists: refused (the three messages); the directory does not change; VMS bumps the SOURCE header (revision +1), ods2v2 does not: the whole image stays as it was
    'd3': dict(vols=('2', '5', '5h'), old='[X2]M2.TXT;1', new='[X2]T.TXT;2', stored=('M2.TXT;1', 'M2.TXT;1'), dir='[X2]', removed=[], added=[], revision=0, vms_bumps_refused=1, header='same', status='refused',
               msgs=['%RENAME-E-OPENOUT', 'RMS-E-ENT', 'DUPFILENAME']),
    # ODS-5 names: the name type stays with the name, the stored case changes, the length is in bytes
    'f1': dict(vols=('5', '5h'), old='[X2]abc.txt;1', new='[X2]ABC.txt', stored=('abc.txt;1', 'ABC.txt;1'), dir='[X2]', removed=['abc.txt;1'], added=['ABC.txt;1'], revision=1, header='changed', status='ok',
               nametype=('ODS-2', 'ODS-2'), namelen=(9, 9)),
    'f2': dict(vols=('5', '5h'), old='[X2]sp^_ace.txt;1', new='[X2]sp^_ace2.txt', stored=('sp ace.txt;1', 'sp ace2.txt;1'), dir='[X2]', removed=['sp ace.txt;1'], added=['sp ace2.txt;1'], revision=1, header='changed',
               status='ok', nametype=('ISL-1', 'ISL-1'), namelen=(12, 13)),
    'f3': dict(vols=('5', '5h'), old='[X2]eur^U20ACo.txt;1', new='[X2]eur^U20ACo2.txt', stored=None, dir='[X2]', removed=None, added=None, revision=1, header='changed', status='ok',
               nametype=('UCS-2', 'UCS-2'), namelen=(22, 24)),
    # an alias entry (SET FILE/ENTER, here LINK): HARDLINKS volume: the primary entry renamed changes the header (the link count 2 stays), the alias entry renamed changes nothing in the header
    'f4': dict(vols=('5h',), old='[X2]FILE.TXT;1', new='[X2]FILE2.TXT', stored=('FILE.TXT;1', 'FILE2.TXT;1'), dir='[X2]', removed=['FILE.TXT;1'], added=['FILE2.TXT;1'], revision=1, header='changed', status='ok',
               alias='LNK2.TXT', linkcount=2),
    'f5': dict(vols=('5h',), old='[Y]LNK2.TXT;1', new='[Y]LNK3.TXT', stored=('FILE2.TXT;1', 'FILE2.TXT;1'), dir='[Y]', removed=['LNK2.TXT;1'], added=['LNK3.TXT;1'], revision=0, header='same', status='ok',
               alias='LNK2.TXT', linkcount=2, file='[X2]FILE2.TXT;1'),
    # the same without the link count (ODS-2 and ODS-5 without HARDLINKS)
    'g1': dict(vols=('2', '5'), old='[Y]ALIAS.TXT;1', new='[Y]ALIAS2.TXT', stored=('FILE.TXT;1', 'FILE.TXT;1'), dir='[Y]', removed=['ALIAS.TXT;1'], added=['ALIAS2.TXT;1'], revision=0, header='same', status='ok',
               alias='ALIAS.TXT', file='[X2]FILE.TXT;1'),
    'g2': dict(vols=('2', '5'), old='[X2]FILE.TXT;1', new='[X2]FILE3.TXT', stored=('FILE.TXT;1', 'FILE3.TXT;1'), dir='[X2]', removed=['FILE.TXT;1'], added=['FILE3.TXT;1'], revision=1, header='changed', status='ok',
               alias='ALIAS2.TXT'),
    # a volume mounted /NOWRITE: refused, nothing changes
    'h3': dict(vols=('2', '5', '5h'), old='[X2]H3.TXT;1', new='[X2]H3B.TXT', stored=('H3.TXT;1', 'H3.TXT;1'), dir='[X2]', removed=[], added=[], revision=0, header='same', status='refused',
               msgs=['RMS-F-RMV', 'WRITLCK'], ods2v2_msgs=['READONLY']),
}
