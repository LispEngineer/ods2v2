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
         mpoffset  (before, after) FH2$B_MPOFFSET in words (ODS-5 long names); extra_changed more DUMP fields that change; link      (alias entry, file) the volume needs: LINK makes the alias entry for the file (SET FILE/ENTER on VMS); and untouched: other versions' headers that must not change; after the header's spec after the rename where it is not the new
                   name with ;1; file the header of this file is the one compared (an alias entry's file); pruned the versions the version limit deletes in the same command; untouched_extra more versions the test checks that VMS's dumps do not show (only ;3 is dumped); follows the case whose result volume this one starts from (VMS ran a1, a2, a3 one after the other on one volume); ods2v2_msgs the messages ods2v2 prints where they are not VMS's (a read-only image
                   gives the READONLY refusal every ods2v2 write command gives)."""

# The VERSION a renamed file gets (review 65 C1) is measured only in part: e2 shows "the source's version if that name;version is free, else one above the highest"; every single-file case has
# a source of version 1, so only cases both readings agree on are here (a1-a3, d1: ;1 is taken, so ;4). The two settling cases (RENAME M.TXT;5 N.TXT; RENAME M.TXT;5 T.TXT with ;1 ;2 ;3 present)
# are asked of ods5-vms3 (j3, j4); step 4 (wildcards) waits for them. No case here renames a file with extension headers (C2) or a file whose header names no entry (C5).
# f1 is the case-only rename of review 65 C3: the stored case changes and the name does NOT become a new version (the test lists the directory: abc.txt;1 goes, ABC.txt;1 appears).
# the long names of CORPUS 67.5 (ASIM5 V8.4): 60 and 140 characters, with ';1' a header name of 62 and 142 bytes
L60 = 'ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ012345.TXT'
L60B = 'ZYXWVUTSRQ9876543210ZYXWVUTSRQ9876543210ZYXWVUTSRQ987654.TXT'
L140 = '0123456789ABCDEFGHIJ' * 5 + '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.TXT'
L140B = 'Q' + '0123456789ABCDEFGHIJ' * 5 + '0123456789ABCDEFGHIJKLMNOPQRSTUVWXY.TXT'
assert (len(L60), len(L60B), len(L140), len(L140B)) == (60, 60, 140, 140)

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
               link=('[Y]LNK2.TXT', '[X2]FILE.TXT'), linkcount=2),
    'f5': dict(vols=('5h',), old='[Y]LNK2.TXT;1', new='[Y]LNK3.TXT', stored=('FILE2.TXT;1', 'FILE2.TXT;1'), dir='[Y]', removed=['LNK2.TXT;1'], added=['LNK3.TXT;1'], revision=0, header='same', status='ok',
               link=('[Y]LNK2.TXT', '[X2]FILE.TXT'), linkcount=2, file='[X2]FILE2.TXT;1', follows='f4'),
    # the same without the link count (ODS-2 and ODS-5 without HARDLINKS)
    'g1': dict(vols=('2', '5'), old='[Y]ALIAS.TXT;1', new='[Y]ALIAS2.TXT', stored=('FILE.TXT;1', 'FILE.TXT;1'), dir='[Y]', removed=['ALIAS.TXT;1'], added=['ALIAS2.TXT;1'], revision=0, header='same', status='ok',
               link=('[Y]ALIAS.TXT', '[X2]FILE.TXT'), file='[X2]FILE.TXT;1'),
    'g2': dict(vols=('2', '5'), old='[X2]FILE.TXT;1', new='[X2]FILE3.TXT', stored=('FILE.TXT;1', 'FILE3.TXT;1'), dir='[X2]', removed=['FILE.TXT;1'], added=['FILE3.TXT;1'], revision=1, header='changed', status='ok',
               link=('[Y]ALIAS2.TXT', '[X2]FILE.TXT')),
    # --- CORPUS 66 (ods5-vms3's follow-up e, 23-NOV-2026): the version rule, the header that names no entry, an alias in its own directory, names, the version limit, reserved files ---
    # C1 (66.2): with no version in the target the new version is ONE ABOVE THE HIGHEST that exists, or 1 for a name that is new: the source's version number is never kept
    'l1': dict(vols=('2', '5', '5h'), old='[L]M.TXT;5', new='[L]N.TXT', stored=('M.TXT;5', 'N.TXT;1'), dir='[L]', removed=['M.TXT;5'], added=['N.TXT;1'], revision=1, header='changed', status='ok'),
    'l2': dict(vols=('2', '5', '5h'), old='[L]U.TXT;5', new='[L]T.TXT', stored=('U.TXT;5', 'T.TXT;4'), dir='[L]', removed=['U.TXT;5'], added=['T.TXT;4'], revision=1, header='changed', status='ok',
               after='[L]T.TXT;4'),
    # C5 (66.2): an alias made with SET FILE/ENTER, then the primary entry removed (SET FILE/REMOVE): the header still names the removed entry and its back link is (0,0,0); renaming the
    # alias entry ADOPTS it: the header's name and back link are set (the directory of the entry), revision +1, the level's date
    'l3': dict(vols=('2', '5', '5h'), old='[L]F5_AL.TXT;1', new='[L]F5_AL2.TXT', stored=('F5.TXT;1', 'F5_AL2.TXT;1'), dir='[L]', removed=['F5_AL.TXT;1'], added=['F5_AL2.TXT;1'], revision=1,
               header='changed', status='ok', extra_changed=['Back link file identification'], backlink='adopted', link=('[L]F5_AL.TXT', '[L]F5.TXT'), noprimary=True, file='[L]F5_AL.TXT;1',
               after='[L]F5_AL2.TXT;1'),
    # an alias renamed in the directory of its primary entry: the header is not touched at all
    'k1': dict(vols=('2', '5', '5h'), old='[K]P1_AL.TXT;1', new='[K]P1_AL2.TXT', stored=('P1.TXT;1', 'P1.TXT;1'), dir='[K]', removed=['P1_AL.TXT;1'], added=['P1_AL2.TXT;1'], revision=0, header='same',
               status='ok', link=('[K]P1_AL.TXT', '[K]P1.TXT'), file='[K]P1.TXT;1'),
    # nothing checks the type: a file to NAME.DIR
    'k3a': dict(vols=('2', '5', '5h'), old='[K]F3.TXT;1', new='[K]F3.DIR', stored=('F3.TXT;1', 'F3.DIR;1'), dir='[K]', removed=['F3.TXT;1'], added=['F3.DIR;1'], revision=1, header='changed', status='ok'),
    # the reserved files: V8.4 refuses (the VAX allows the volume owner: only the V8.4 evidence is compared)
    'k4a': dict(vols=('2', '5', '5h'), old='[000000]INDEXF.SYS;1', new='[000000]INDEXF2.SYS', stored=('INDEXF.SYS;1', 'INDEXF.SYS;1'), dir='[000000]', removed=[], added=[], revision=0, header='same',
                status='refused', msgs=['RMS-F-RMV', 'NOPRIV'], ods2v2_msgs=['RESERVED'], evidence=('asim5',)),
    'k4c': dict(vols=('2', '5', '5h'), old='[000000]BITMAP.SYS;1', new='[000000]BITMAP2.SYS', stored=('BITMAP.SYS;1', 'BITMAP.SYS;1'), dir='[000000]', removed=[], added=[], revision=0, header='same',
                status='refused', msgs=['RMS-F-RMV', 'NOPRIV'], ods2v2_msgs=['RESERVED'], evidence=('asim5',)),
    # an explicit target version that is free (T.TXT;1 and ;3 exist): works
    'k5': dict(vols=('2', '5', '5h'), old='[K]Q5.TXT;1', new='[K]T.TXT;2', stored=('Q5.TXT;1', 'T.TXT;2'), dir='[K]', removed=['Q5.TXT;1'], added=['T.TXT;2'], revision=1, header='changed', status='ok',
               untouched=['T.TXT;1', 'T.TXT;3'], after='[K]T.TXT;2'),
    # A.TXT to A.TXT: VMS bumps the revision and sets the date (the name is the same); ods2v2 writes NOTHING (an intended difference; the image is byte for byte the same)
    'k6': dict(vols=('2', '5', '5h'), old='[K]A6.TXT;1', new='[K]A6.TXT', stored=('A6.TXT;1', 'A6.TXT;1'), dir='[K]', removed=[], added=[], revision=1, header='changed', status='ok', ods2v2_unchanged=True),
    # a 36-character name (the ident area holds the first 20 bytes at ODS-2, the rest is FILENAMEXT), then at ODS-5 the same name in lower case (a case-only rename of a name over 20 characters)
    'k7a': dict(vols=('2', '5', '5h'), old='[K]S7.TXT;1', new='[K]ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT', stored=('S7.TXT;1', 'ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT;1'), dir='[K]', removed=['S7.TXT;1'],
                added=['ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT;1'], revision=1, header='changed', status='ok'),
    'k7b': dict(vols=('5', '5h'), old='[K]ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT;1', new='[K]abcdefghijklmnopqrstuvwxyz012345.txt', stored=('ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT;1', 'abcdefghijklmnopqrstuvwxyz012345.txt;1'),
                dir='[K]', removed=['ABCDEFGHIJKLMNOPQRSTUVWXYZ012345.TXT;1'], added=['abcdefghijklmnopqrstuvwxyz012345.txt;1'], revision=1, header='changed', status='ok', follows='k7a'),
    # the version limit (66.1): a name whose entry has a limit loses its LOWEST version when a rename makes a new one (exactly one: the count stays at the limit). The limit that counts is the entry's own
    # (SET FILE/VERSION_LIMIT here; the directory default at creation on VMS)
    'k8': dict(vols=('2', '5', '5h'), old='[K]N8.TXT;1', new='[K]L.TXT', stored=('N8.TXT;1', 'L.TXT;4'), dir='[K]', removed=['L.TXT;1', 'N8.TXT;1'], added=['L.TXT;4'], revision=1, header='changed', status='ok',
               after='[K]L.TXT;4', pruned=['L.TXT;1']),
    'i3': dict(vols=('2', '5'), old='[VL]D.TXT;1', new='[VL]A.TXT', stored=('D.TXT;1', 'A.TXT;6'), dir='[VL]', removed=['A.TXT;3', 'D.TXT;1'], added=['A.TXT;6'], revision=1, header='changed', status='ok',
               after='[VL]A.TXT;6', pruned=['A.TXT;3']),
    'i7': dict(vols=('2', '5'), old='[VL]E.TXT;1', new='[VL]C.TXT', stored=('E.TXT;1', 'C.TXT;5'), dir='[VL]', removed=['C.TXT;3', 'E.TXT;1'], added=['C.TXT;5'], revision=1, header='changed', status='ok',
               after='[VL]C.TXT;5', pruned=['C.TXT;3']),
    # --- CORPUS 67.4 (F2): RENAME of one version of a name to another version of the SAME name: nothing is pruned while the count stays within the limit; with a FILE limit below the count the
    # lowest versions AFTER the rename go (exactly one here). m1 m2: no limit; m3 m4: limit 3 (the files carry it); m5 m6: SET FILE/VERSION_LIMIT=2 set after the three versions exist; m7: another
    # name onto B7 with the limit 3 (the version rule and the prune together)
    'm1': dict(vols=('2', '5', '5h'), old='[M]B1.TXT;3', new='[M]B1.TXT;5', stored=('B1.TXT;3', 'B1.TXT;5'), dir='[M]', removed=['B1.TXT;3'], added=['B1.TXT;5'], revision=1, header='changed', status='ok'),
    'm2': dict(vols=('2', '5', '5h'), old='[M]B2.TXT;1', new='[M]B2.TXT;5', stored=('B2.TXT;1', 'B2.TXT;5'), dir='[M]', removed=['B2.TXT;1'], added=['B2.TXT;5'], revision=1, header='changed', status='ok'),
    'm3': dict(vols=('2', '5', '5h'), old='[ML]B3.TXT;3', new='[ML]B3.TXT;5', stored=('B3.TXT;3', 'B3.TXT;5'), dir='[ML]', removed=['B3.TXT;3'], added=['B3.TXT;5'], revision=1, header='changed', status='ok'),
    'm4': dict(vols=('2', '5', '5h'), old='[ML]B4.TXT;1', new='[ML]B4.TXT;5', stored=('B4.TXT;1', 'B4.TXT;5'), dir='[ML]', removed=['B4.TXT;1'], added=['B4.TXT;5'], revision=1, header='changed', status='ok'),
    'm5': dict(vols=('2', '5', '5h'), old='[M]B5.TXT;3', new='[M]B5.TXT;5', stored=('B5.TXT;3', 'B5.TXT;5'), dir='[M]', removed=['B5.TXT;3', 'B5.TXT;1'], added=['B5.TXT;5'], revision=1, header='changed', status='ok',
               pruned=['B5.TXT;1']),
    'm6': dict(vols=('2', '5', '5h'), old='[M]B6.TXT;1', new='[M]B6.TXT;5', stored=('B6.TXT;1', 'B6.TXT;5'), dir='[M]', removed=['B6.TXT;2', 'B6.TXT;1'], added=['B6.TXT;5'], revision=1, header='changed', status='ok',
               pruned=['B6.TXT;2']),
    'm7': dict(vols=('2', '5', '5h'), old='[ML]X7.TXT;1', new='[ML]B7.TXT', stored=('X7.TXT;1', 'B7.TXT;4'), dir='[ML]', removed=['B7.TXT;1', 'X7.TXT;1'], added=['B7.TXT;4'], revision=1, header='changed', status='ok',
               after='[ML]B7.TXT;4', pruned=['B7.TXT;1']),
    # --- CORPUS 67.5 (F3), ODS-5: a long name renamed short keeps MPOFFSET (the ident area keeps its length, the name field is zero-filled to its end); a short name renamed long grows the ident area
    # and moves the map UP (MPOFFSET 100 to 109 for 62 bytes, to 149 for 142; ACOFFSET and RSOFFSET stay). The names are not compared with the evidence (matched by name type and length)
    'n1': dict(vols=('5', '5h'), old='[N]' + L60 + ';1', new='[N]SHORT1.TXT', stored=None, dir='[N]', removed=None, added=None, revision=1, header='changed', status='ok', after='[N]SHORT1.TXT;1',
               nametype=('ISL-1', 'ODS-2'), namelen=(62, 12), extra_changed=['File name type'], mpoffset=(109, 109)),
    'n2': dict(vols=('5', '5h'), old='[N]SHORT2.TXT;1', new='[N]' + L60B, stored=None, dir='[N]', removed=None, added=None, revision=1, header='changed', status='ok', after='[N]' + L60B + ';1',
               nametype=('ODS-2', 'ISL-1'), namelen=(12, 62), extra_changed=['File name type', 'Map area offset'], mpoffset=(100, 109)),
    'n3': dict(vols=('5', '5h'), old='[N]' + L140 + ';1', new='[N]SHORT3.TXT', stored=None, dir='[N]', removed=None, added=None, revision=1, header='changed', status='ok', after='[N]SHORT3.TXT;1',
               nametype=('ISL-1', 'ODS-2'), namelen=(142, 12), extra_changed=['File name type'], mpoffset=(149, 149)),
    'n4': dict(vols=('5', '5h'), old='[N]SHORT4.TXT;1', new='[N]' + L140B, stored=None, dir='[N]', removed=None, added=None, revision=1, header='changed', status='ok', after='[N]' + L140B + ';1',
               nametype=('ODS-2', 'ISL-1'), namelen=(12, 142), extra_changed=['File name type', 'Map area offset'], mpoffset=(100, 149)),
    # a volume mounted /NOWRITE: refused, nothing changes
    'h3': dict(vols=('2', '5', '5h'), old='[X2]H3.TXT;1', new='[X2]H3B.TXT', stored=('H3.TXT;1', 'H3.TXT;1'), dir='[X2]', removed=[], added=[], revision=0, header='same', status='refused',
               msgs=['RMS-F-RMV', 'WRITLCK'], ods2v2_msgs=['READONLY']),
}
