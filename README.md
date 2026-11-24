# ods2v2

This is a from-scratch implementation of DEC Files-11 (ODS-2) utility
to copy local files from a Linux (likely a Mac too) system to an
ODS-2 disk volume that is usable by SIMH simulator running a
VAX/VMS (vax8600) OpenVMS 7.1 system.

This utility can run a minimal set of VMS DCL type commands on the
disk file, with the intent of transferring files into or out of the
VMS system.  Reads and writes real ODS-2 disk images - the same format
real VMS's own `MOUNT`, `INITIALIZE`, `COPY`, and `DIRECTORY` commands
use.  No VMS installation, SIMH emulator, or license required to use it.

Built directly from the Files-11 On-Disk Structure Specification, not
from copying an existing implementation, and validated repeatedly
against real OpenVMS 7.1 (via SIMH) throughout development with
real  MOUNT / DIR / TYPE / COPY and ANALYZE/DISK commands on a
vax8600 running OpenVMS 7.1 in simh. Every significant bug this
project found (a word-swapped 32-bit field, an inverted bitmap
convention, directory records needing to be storage-sorted) was found
with analysis on an OpenVMS system.

I have followed the excellent SIMH processor simulator package
(https://github.com/simh/simh.git)
implementation guide here 
(https://www.openvmshobby.com/vax-vms/openvms-on-vax-simh/)
along with an OpenVMS 7.1 "ISO" image from here
(http://vaxhaven.com/cd-image/AG-QSBWB-BE.iso.zip)
to get a VAX8600 running VMS 7.1 on a Rocky Linux 10.2 system.
Will also work on Ubuntu 24 system.

I also have hosted a copy of that ISO on my website here
(https://pomeroy.us/vms-software/VAXVMS071.iso)

## Quick start

```bash
tar xzf ods2v2.tgz && cd ods2v2  # expand and cd into ods2v2 cwd
make clean && make ods2          # only builds ods2
make starter-volume              # creates an empty ODS2 RA92 volume
                                 # by decompressing a real VMS
                                 # INITIALIZEd disk file: transfer.dsk
./ods2 transfer.dsk DIR          # starts ods2v2 and executes a DIR command
```

## Full test suite

```bash
tar xzf ods2v2.tgz && cd ods2v2  # expand and cd into ods2v2 cwd
make clean && make all           # builds everything, runs the full test suite
make starter-volume              # creates an empty ODS2 RA92 volume
                                 # by decompressing a real VMS
                                 # INITIALIZEd disk file: transfer.dsk
./ods2 transfer.dsk DIR          # starts ods2v2 and executes a DIR command
```

No external dependencies beyond a C compiler and standard C dev tools.
Development was on Rocky Linux 10.2 (same system as the simh vax8600 is
running on). There is external code included for the interactive shell
command history (see Dependencies below).

## What ods2v2 (ods5 branch) can and cannot do

A short summary for users of this branch; the sections above and "Known limitations" below have the detail. "Verified" means
that OpenVMS itself mounted, listed, read and analysed (`ANALYZE/DISK_STRUCTURE`) volumes this tool wrote, not that every
case was tried; the project's record of what was tried is `VMS_MEASURED_BEHAVIOUR.md` and `CORPUS.md` in
[ods5-cli](https://github.com/LispEngineer/ods5-cli).

**Works today (ODS-2 and ODS-5, read and write):** `DIR` (wildcards, `[...]`, `/FULL`), `TYPE` (records as text, `/RAW`,
`/LINES`, wildcards and comma lists), `COPY`, `IMPORT` and `EXPORT` (record format chosen per file, `/FORMAT=`, `/ATTRIBUTES=`, `/CONVERT`),
`CREATE/DIRECTORY`, `DELETE` (wildcards in one directory), `PURGE`, `RENAME` (one file in its directory), `SYMLINK` and `LINK` (ODS-5), `SET VOLUME/LABEL`,
`SET FILE/PROTECTION` and `/OWNER_UIC`, `SET FILE/VERSION_LIMIT` and the `SET FILE/ATTRIBUTES=(MRS:0)` repair, ACLs shown as `DIRECTORY/SECURITY` shows them,
long ISO Latin-1 and UCS-2 names with `^` escapes, version numbers and limits, deep directories, hard links and aliases;
`ods2_init` makes new ODS-2 and ODS-5 volumes from 8,192 blocks to 2^32 - 16 blocks; `tests/ods5check.py` checks a volume.
OpenVMS accepted volumes written by this branch on VAX V7.3, Alpha V7.3-2, V8.4 and V8.4-2L2 (simulators), on a physical
VAX and a physical Alpha V8.4. Structure-level-0 volumes (the OpenVMS Alpha V8.4 CD) are read-only.

**Operations not provided** (the plans are drafts awaiting decisions: [PLAN_WRITE_OPS.md](https://github.com/LispEngineer/ods5-cli/blob/main/PLAN_WRITE_OPS.md) in ods5-cli): `RENAME` of a directory and with wildcards (steps 3 and 4); appending to or overwriting a file in place; ACL editing; `/PROTECTION` and `/OWNER` on `COPY` and
`CREATE/DIRECTORY`; wildcards in the directory part of `TYPE` (VMS allows `[T...]`) and of `DELETE`.

**Files and volumes not handled:** indexed and relative files as records (they are given as stored); volume sets (refused);
ODS-1 and structure levels 3, 4 and 6 (refused); disk quotas and journaling; UCS-2 beyond the Basic Multilingual Plane (a
name above U+FFFF is refused); the rights database (identifiers print as numbers, `[200,1]`, not `[USERS,USER1]`).

**Size limits** (details in "Large volumes and image files"):
- `INDEXF.SYS` grows in pieces of at most 16,384 blocks, inside the retrieval pointers of its primary header, and this
  branch writes no `INDEXF.SYS` extension headers (the OpenVMS versions tried crash or refuse them: see Known limitations).
  About 51 pointers fit, so roughly **510,000 files** is the ceiling of a volume that grows by this tool's steps; past it
  new files are refused and nothing is written. That figure is arithmetic from the code, **not tested at that size** (the
  largest volumes tried hold 70,000 files made by VMS and 75,000 made by the tests).
- A directory is at most 65,536 blocks, in one extent and under one file header; directories and `BITMAP.SYS` are read
  with at most 1,024 pieces.

**Not yet verified on VMS** (so nothing here should be relied on): the byte layout of SUBSYSTEM and resource ACEs;
`TYPE` of stream files with print carriage control on V8.4-2L2; what VMS itself does for a wildcard `DELETE` (the rules
here are modelled on `DIRECTORY` matching); how VMS grows `INDEXF.SYS` past its primary header's map, and how big a
directory VMS allows (`SS$_DIRFULL`): **open questions, not measured on any version**; a 2 TiB volume written and read
on VMS (the manual test); OpenVMS x86-64 V9.2-3 beyond the `INDEXF.SYS` crash runs (there, two control volumes this tool wrote without extension
headers mounted, took 60 files made by VMS on one of them and analysed clean, but the full set of test volumes was not tried); OpenVMS Itanium (never tried). The checking plan is
[PLAN_VMS_VERIFY.md](https://github.com/LispEngineer/ods5-cli/blob/main/PLAN_VMS_VERIFY.md) in ods5-cli (a draft).

## Using `ods2`

`ods2` is the actual, intended interface - one binary, DCL-style
commands, VMS bracket-notation paths throughout. Interactive mode
implements basic command recall to ease repetitive command sequences.

```bash
./ods2 transfer.dsk                          # interactive shell
./ods2 transfer.dsk DIR [DECUS.NETLIB020]    # single-shot
./ods2 transfer.dsk @script.cmd              # run commands from a file
```

### Commands

| Command | Example |
|---|---|
| `DIR [path] [wildcard]` | `DIR [DECUS] *.TXT` |
| `DIR [path...] [wildcard]` | `DIR [...]*.*` — recursive, VMS's `...` notation |
| `DIR [*...] [wildcard]` | `DIR [*...]*.COM` — wildcards in directory names (see below) |
| `CREATE/DIRECTORY path` | `CREATE/DIRECTORY [DECUS.NETLIB020]` |
| `COPY <local-file> <path>[;n]` | `COPY readme.txt [DECUS]README.TXT` — a name that exists gets a new version (`;n`: that version) |
| `COPY/ATTRIBUTES=(...) <local-file> <path>` | `COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) kit.pcsi [KITS]KIT.PCSI` |
| `COPY/CONVERT <local-file> <path>` | `COPY/CONVERT notes.txt [DOC]NOTES.TXT` — text as VAR records |
| `TYPE[/RAW\|/LINES] path` | `TYPE [DECUS]README.TXT;2` — sequential files as text the way VMS `TYPE` shows them (carriage control applied: a file with none shows its records run together); `/LINES` one line per record always; `/RAW` the stored bytes  **Wildcards and comma lists** (VMS's, CORPUS 49.3): for each file a wildcard finds, and for every file of a list but a first one named without a wildcard, a line with one space, the full name (`[T]A1.TXT;1`, without the device VMS prints in front), a line with one space, then the records; a wildcard without a version shows the highest version of each name in name order, `;*` every version; nothing found is `%TYPE-W-SEARCHFAIL, error searching for [T]*.NOSUCH;` with `-RMS-E-FNF` (a missing directory: `-RMS-E-DNF`, `-SYSTEM-W-NOSUCHFILE`), after what was typed; a wildcard in the directory part is still refused (`%ODS2-E-WILDDIR`). A single missing file named without a wildcard keeps `%ODS2-E-FNF`. A list element without a type means every type (`TYPE [T]A2.TXT,A3` types `A3.LIS` and `A3.TXT`; `TYPE [T]A3` alone is not found: VMS takes missing parts from the element before, not measured), and a list element with a directory of its own is `%ODS2-E-BADPATH` (an element with no directory uses the previous element's) |
| `PURGE[/KEEP=n][/LOG] [spec]` | `PURGE/KEEP=2 [DECUS]*.TXT` — keeps the `n` highest versions (default 1) of every name the spec matches (wildcards in the name, also in `[dir...]`; no spec: the default directory) and deletes the others through the `DELETE` path, highest of the deleted first. `/LOG` prints `%PURGE-I-FILPURG, [dir]NAME;v deleted (n blocks)` and, for more than one, `%PURGE-I-TOTAL`; nothing purged is `%PURGE-I-NOFILPURG`; `/KEEP=0` is `%PURGE-F-BADVALUE`; a name that does not exist is `%PURGE-W-SEARCHFAIL` and `-RMS-E-FNF` (`-RMS-E-DNF` for a missing directory); on a read-only image each version to delete is `%PURGE-W-FILNOTPUR` with `-RMS-E-MKD` and `-SYSTEM-F-WRITLCK`. As VMS prints them but without the device in front of the name. ods2v2 acts as a privileged process: a version whose protection forbids its owner to delete it is purged too (VMS refuses that, `-SYSTEM-F-NOPRIV`, to an unprivileged process that is not the volume owner: CORPUS 49.7); a version in the spec is refused (not measured). A name's versions are kept by version number, never by the order the directory stores them in (a directory written by another tool or damaged may hold them out of order: they are sorted, highest first, before the first `n` are kept). The alias of a purged version in another directory of an ODS-2 volume is left dangling, as with `DELETE` (ods5check warns once); on a HARDLINKS volume a purged version that has another link loses its entry and one off the link count, the file stays, and the line says `%ODS2-I-DELETED` as `DELETE` does, with no blocks in the total. Qualifiers must be typed in full (`/KEEP=`, `/LOG`; `PURGE/KE=2` is an unrecognized qualifier); a second spec or a comma list is refused (`%PURGE-F-TOOMANY`, nothing purged: run PURGE once for each) A name whose versions are stored out of order is refused (`BAD_VERSORDER`, H1 below): VMS keeps the first stored entry, ods2v2 changes nothing. |
| `SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) spec` | `SET FILE/LOG/PROTECTION=(G:R,W) [DECUS]*.TXT;*` — only the fields named change; a field without a colon clears it; a repeated field takes the last; a bad letter is `%SET-F-SYNTAX, error parsing 'X'` and nothing changes. Every success adds 1 to the revision number (even for the same value); the revision date (level 2 header) or the attribute change date (level 5; the data modification and access dates stay) is the time of the command. Without `;*` or a version only the highest version of each name is set. Directories and the reserved files (INDEXF.SYS, BITMAP.SYS, the master directory: measured) take it; `/LOG` prints `%SET-I-PROTECTED, ... file protection changed to S:RWED,O:R,G:R,W:R` |
| `SET FILE/OWNER_UIC=[g,m] spec` | `[group,member]` in octal (also the 11-digit form `[00000000200,00000000003]`; `[128,3]` is a syntax error), or `[SYSTEM]` (= `[1,4]`); a name that needs the rights database (`[USERS,USER1]`) is refused (`%SET-E-SYNTAX` plus an explanation). Revision +1 per success; `/LOG` prints `%SET-I-MODIFIED, ... modified`. The owner of a reserved file is refused: that is **ods2v2's choice**, VMS was asked once, for INDEXF.SYS, and refused it only because the file is open. SET FILE on INDEXF.SYS writes both copies of its header, where VMS changes only the first (its alternate copy keeps the old protection and revision); no VMS run has yet read a volume where ods2v2 changed both copies, so that this is harmless is **not measured**. `/OWNER_UIC` writes the owner into every extension header of a file as well, as VMS does (CORPUS 56.2, V8.4, both levels: a file of 3 extension headers), each checked as the next segment of the file first, the extension headers before the primary header, all or nothing; `/PROTECTION` changes the primary header only (VMS leaves the extension headers' FILEPROT alone); the revision and dates of the extension headers are not touched (not measured). Qualifiers must be typed in full (`/PROTECTION=`, `/OWNER_UIC=`; `/PROT=` and `/OWNER=` are not recognized). ods2v2 acts as a privileged process: there is no `%SET-E-NOTSET ... -SYSTEM-F-NOPRIV` as VMS gives an unprivileged process that is not the volume owner (CORPUS 49.7); one of `/PROTECTION` and `/OWNER_UIC` per command (both at once was not measured) |
| `DELETE path` | `DELETE [DECUS]OLDFILE.TXT;1`; with `*`, `%` or `?` in the name every match in that one directory: `DELETE [DECUS]*.LIS` (the highest version of each), `DELETE [DECUS]OLD*.*;*` (every version). The name and the type are matched apart (`FOO.*` does not take `FOO^.TAR.GZ`), and no type given is the null type, as for one file (`DELETE [DECUS]READ*;*` is `READ*.;*`). A `%` or `?` in an ODS-5 name is a wildcard here, as in DCL; write it `^%` to delete such a name |
| `DIR/FULL`, `DIR/UTF8`, `SET NAMES UTF8\|ESCAPED` | per-file detail; how names are shown (see "ODS-5 volumes") |
| `EXPORT[/VERSIONS][/OVERWRITE][/LINKS][/RECORDS\|/LINES] <path> <linux-path>`, `IMPORT[/NEW_VERSION] <linux-dir> <path>` | copy files or trees off the volume and onto it (see "ODS-5 volumes") |
| `RENAME <old> <new>` | `RENAME [DECUS]A.TXT B.TXT` (the new name without a directory is in the old one's), `RENAME [D]A.TXT [D]A.TXT;5`, **`RENAME [X]C.TXT [Y]`, `RENAME [X]C.TXT [Y]C3.TXT`: one file, in one directory or into another directory of the volume** (steps 1 and 2 of PLAN_WRITE_OPS item 5; CORPUS 65, 66 and 68; a directory file and wildcards are refused saying so). A target with a directory and no name keeps the name (CORPUS 65.1 b1); a target on another device is refused as VMS does (`NOTSAMEDEV`; the device parts typed must agree, or name this volume by its label), a target directory that does not exist is `DNF`, nothing changes. **Into another directory the header's back link changes with the name in the same write** (CORPUS 65.1: the back link, the name if it changes, revision +1, the date), the directories' own headers are not touched, the version of the new name is the one the TARGET directory's rules give (1 for a name that is new there, one above the highest for a name that exists, a version typed is kept, the version limit of the target name removes its lowest), and an alias entry moves with its header untouched (CORPUS 66.2 k2). The header gets the new name (the version is part of it), revision +1 and the date the level sets (ODS-2 the revision date; ODS-5 the last attribute change date, the revision date stays), the checksum; the file ID, map, protection and the other dates stay, and **the extension headers are never touched** (VMS leaves them with the old name, CORPUS 66.2). **The version of the new name:** none typed gives 1 for a name that is new and one above the highest for a name that exists (the source's version is never kept); a version typed is kept, and one that exists is refused (`%RENAME-E-OPENOUT`, `-RMS-E-ENT`, `-SYSTEM-W-DUPFILENAME`) with the volume **byte for byte unchanged** (VMS also bumps the revision of the source: a deliberate difference). The version limit of the target name removes its lowest version, as `COPY` does (CORPUS 66.1). **Aliases:** an entry that is not the header's own (its back link is another directory or its name is another) is an alias: only the directory entry changes and the header is not touched; a header whose back link is (0,0,0) (the primary entry was removed) is adopted: its name and back link are set. A new name that differs only in case (ODS-5) replaces the record's name bytes in place. `A.TXT` to `A.TXT` writes nothing (VMS bumps the revision and the date: a deliberate difference). The reserved files (INDEXF.SYS, BITMAP.SYS ...) and the master directory are refused (V8.4 refuses them; the VAX allows the volume owner). **No access is checked**, as for `DELETE` and `SET FILE` (ods2v2 acts like a privileged process); step 1 refuses every directory file, so the one place VMS and ods2v2 would differ for it, a directory with the default protection, does not arise yet. A name whose versions are stored out of order is refused unless a version is named (H1). **Another version of the same name** (`RENAME B.TXT;3 B.TXT;5`) changes no count: nothing is pruned while the count stays within the version limit, and with a limit below the count the lowest version after the rename goes (CORPUS 67.4: measured on V8.4 and the VAX, plain and HARDLINKS volumes). **Long ODS-5 names (CORPUS 67.5):** a longer name grows the header's ident area and moves the map up (MPOFFSET 100 to 109 for a 62-byte name, 149 for 142 bytes; ACOFFSET and RSOFFSET stay), a shorter one keeps MPOFFSET and zero-fills the name field to the end of the ident area, as VMS does; with no room above the map (a primary header whose map is full, or an ACL) the rename is refused and nothing changes. **Refused, the volume unchanged:** a target name that a directory holds or that is a reserved file (no new version of it: the two refusals `COPY` has, one function for the version rules, and for the version limit the refusal of an out-of-order name too); a case-only change of a name with several versions (the spelling belongs to the whole record); a new name that is not valid (the message says it is the new name's). `RENAME B.TXT;1 B.TXT` (a lower version of the same name, no version typed) is "already named": nothing changes (VMS may make `B.TXT;4`: not measured). A target entry that already names the file (a `LINK` made first, or an interrupted rename) is not a reason for a new version: the old entry is removed, the message says so, and VMS would probably make `AL.TXT;2` beside `AL.TXT;1` (not measured). **Order of the writes: header first** (review 67 recheck; the order of the proposal in `RENAME-ORDER-PROPOSAL.md`): the header (the new name, **the back link when the file goes to another directory**, revision +1, the date, and on a HARDLINKS volume the link count + 1: one write), the new entry, the old entry removed, the link count back (HARDLINKS). Not VMS's own order (CORPUS 67.2 shows VMS rewriting the header's revision before a refused enter, the name unchanged); the reason is what a VMS `DELETE` does to a file with two names (CORPUS 67.2): deleting the entry the header NAMES deletes the file and leaves the other entry dangling, deleting the other one removes only its entry. Entry first would leave a state in which the user's intended action (delete the old name, which the header still names) destroys the file; header first leaves one entry and a header that names the new name, in which a VMS `DELETE` of the entry removes only the entry and the file stays in no directory (a leak that `ANALYZE` reports and `/REPAIR` recovers, nothing destroyed), and then two names with the header naming the new one, in which deleting the old name is safe. **ods2v2 reads that state as VMS does (CORPUS 69: V8.4 at both levels and VAX V7.3):** the file's primary entry is the one in the header's back-link directory whose name (the version too on a HARDLINKS volume) equals the header's name, and nothing else, so in the state after the first write (one entry P, the header naming Q) P is an ALIAS: `ANALYZE` finds nothing wrong; `RENAME P Q` (the same RENAME again) finishes cleanly, through the header that already names Q: the header is not written again and its revision does not move; `RENAME P R` moves the entry and leaves the header untouched; **`DELETE P` removes the entry only: the file stays allocated in no directory** (VMS: `ANALYZE` says LOSTHEADER and `/REPAIR` puts the file in SYSLOST; ods2v2 says "removed the entry only: the file stays allocated with no entry left in its directory" (another directory may still enter it) and not "deleted", and `ods5check` reports the lost file). So a RENAME that was stopped after its first write leaves the file named in its header only; the safe ways out are to run the same RENAME again (before any `ANALYZE/DISK_STRUCTURE/REPAIR` on VMS: `/REPAIR` moves the files of the first state into SYSLOST, keeping their entries, CORPUS 70.3) or `ANALYZE/DISK_STRUCTURE/REPAIR` on VMS, and a `DELETE` of the old name leaks the file, on VMS and in ods2v2 alike (nothing is destroyed). With two or more entries of the file in the directory and none named by the header (a `LINK` in that state), all are aliases as well and a `DELETE`, `PURGE` or version-limit prune of one removes the entry only (review 67 G1). **Deliberate differences from VMS (CORPUS 70):** (1) a RENAME onto a name whose version limit would remove another entry of THE SAME FILE (an alias made with `LINK`, or the entry a stopped move left in the target directory) is refused before any write on a volume without HARDLINKS: VMS itself prunes that entry, which is the file's primary one, deletes the FILE and leaves the new entry dangling (measured on the VAX); ods2v2 keeps the file. On a HARDLINKS volume with a link count of 2 or more VMS keeps the file (the prune only takes the entry out and lowers the count) and so does ods2v2, which lets the rename go on there. (2) An explicit `;1` onto a name that has it is DUPFILENAME on VMS and in ods2v2, and a rename of the old entry of a stopped move to a THIRD directory is an alias move on VMS as well (the header untouched). The same RENAME run again in the second stopped state of a move (two entries) finishes it, as it does in one directory: VMS's `RENAME [MA2]P [MB2]` does not finish the move, it makes `[MB2]P.TXT;2` beside `;1` (one file, two entries). (3) The missing parts of a new name follow VMS (CORPUS 70): a new name that leaves out the name or the type takes it from the file being renamed (`[Y]N` is `N.TXT`, `[Y].DAT` is `A.DAT`, `[Y];7` is the same name with version 7; the version is never the source's); wildcards in the new name (`[Y]N2.*`) are step 4. A device typed in front of a name with no directory (`MOV:N.TXT`) is parsed off: it must be the other name's device or this volume's label; a device name that does not fit, a node (`MOV::`) or a doubled device is refused (NOTSAMEDEV).

**The same order for a move (step 2), and the same rule decides every stopped state** (VMS deletes the FILE only through the entry that is in the header's back link directory AND carries the header's name, CORPUS 68.3 and 69): after the first write the header already names the target directory, so the old entry is an alias (a `DELETE` of it removes the entry only; the file stays allocated with no entry in its back link directory, `ods5check` says so with a WARNING, as VMS's `ANALYZE` does (W-BACKLINK naming the source entry, CORPUS 70): exactly this one warning is allowed in this one stopped state, and the fault test fails any other, or its absence); after the second write both entries stand, the back link names the target, and `DELETE [old dir]name`, the natural way to finish, removes only that entry (deleting the new one would delete the file and leave the old entry dangling: the same trap as VMS's own); the same RENAME run again finishes every state (the header that already names the target directory and the name is not written again). A move of a file with extension headers or of an alias leaves the extension headers and (for an alias) the header alone. `ods5check` warns "header name ... differs from its directory entry" in that state, and the fault test allows that one warning in that one state only. An interruption leaves two names or the old one, never none, and the same RENAME run again finishes it (a target entry that already points at the source's file is an interrupted rename, not a reason for a new version; a header that already names the target is not written again). **The skipped count rests on a reading:** when the header already names the target and the target entry is absent, the re-run takes that for a stopped rename whose first write already counted, and does not count again. From any other origin of that state (a header whose name no entry has, for another reason) the count would end one too low, the unsafe direction; ods2v2 writes that state only as the first write of a rename, and `ods5check` warns of a count that is not the number of entries. **On a HARDLINKS volume two stops leave the link count one too high for good**: after the first write of the rename of an ALIAS entry (the count + 1, the header untouched, no new entry yet: nothing shows the count was taken, so the run again counts once more; a PRIMARY entry's first write also names the header, and the run again sees the header name the target with no target entry, knows the count was taken and does not count again), and after the old entry was removed and before the count came back (the source is gone, so nothing can finish it). The direction is the safe one: with the count written after the entry a stop would leave it too low, and a `DELETE` of one name would free a file another name still enters. The cost: a `DELETE` of the file's last name takes the count down to 1 and removes the entry, and the file stays allocated in no directory (`ods5check`: a lost file). `ods5check` shows the state: a warning when a file's link count on a HARDLINKS volume is not the number of entries that name it (equal for all 4,185 files of five volumes VMS wrote). A name stopped between the entry and the header is left over its version limit by one version until the next change (the version the limit should have removed stays). `tests/rename_check.py` compares every case with VMS's dumps (the table `tests/rename_expect.py`, checked against them by ods5-cli `corpus/tools/rename_expect_check.py`). |
| `SYMLINK <link-path> <target>`, `LINK <new-path> <existing-path>` | a symbolic link; another directory entry for a file (see "ODS-5 volumes") |
| `SET FILE/VERSION_LIMIT=n spec` | the version limit of the entries a spec names (32767 or 0: no limit, what VMS writes) |
| `SET VOLUME/PARTIAL_CLUSTER`, `SET VOLUME/GEOMETRY=sectors,tracks` | `ods2 HELP` explains them; see `ods2_init` below |
| `SET DEFAULT path` | `SET DEFAULT [DECUS.NETLIB020]` |
| `SHOW DEFAULT` | |
| `SET VOLUME/LABEL=name` | `SET VOLUME/LABEL=ITANKITS` — relabel the volume |
| `SET FILE/ATTRIBUTES=(MRS:0) spec` | `SET FILE/ATTRIBUTES=(MRS:0) [DECUS...]*.*` — repairs Stream, Stream_LF and Stream_CR files made with a maximum record size (older ods2v2 gave them 512, and RMS refuses such a file with a longer line: `%RMS-W-RTB`): MRS 0, LRL the longest record; the only attribute change implemented |
| `SHOW VOLUME` | label, owner, cluster size, maximum files, free blocks |
| `HELP` | |
| `EXIT` / `QUIT` | |

You can probably deduce I was trying to get some freeware networking
running (such as CMU066IP) and developed this tool to help with the
initial transfer of savesets onto the OpenVMS system.

Paths follow real VMS conventions: `[.SUBDIR]` and bare filenames with
no brackets are relative to the current default directory (see `SET
DEFAULT`); `[-]` means "up one level" (`[-.-]` for more); other
bracketed paths are absolute from root. Names are automatically
uppercased on creation, matching real VMS storage.

`DIR` accepts VMS directory wildcards, as `DIRECTORY` does on VMS:
`*` and `%` in any directory name, and the ellipsis `...` (any
number of levels, including none) anywhere in the directory spec.

| Spec | Lists |
|---|---|
| `[*]` | every top-level directory |
| `[*...]` | every directory on the disk except root's own files |
| `[000000...]` | everything, root included |
| `[...]` | the current default directory and everything below it |
| `[DECUS.*]` | every directory directly under `[DECUS]` |
| `[...SALES]` | every directory named `SALES` at any depth below the default |
| `[DECUS...SRC]` | every `SRC` anywhere below `[DECUS]` |
| `[TEST%...]` | `[TEST1]`, `[TESTA]` ... and everything below them |

Like VMS, a multi-directory listing shows only directories that hold
something matching, ends with a grand total, and says "no files
found" when nothing matches. A directory reachable under two names
(on a system disk, `SYS1`...`SYSFF` are all `SYS0`) is listed under
each. An entry that points back at a directory it is below is a loop:
it is not followed, and `%ODS2-W-DIRLOOP` names both paths. Quote these specs on the shell command
line (`./ods2 transfer.dsk DIR "[*...]*.*"`), since `[`, `*` and `?`
are shell wildcards too. The other commands act on one directory and
refuse a wildcard one.

File names follow ODS-2's rules: letters, digits, `$`, `_` and `-`,
with one `.` before the type, and up to 39 characters on each side
(names are uppercased for you). A host path won't do as a name -
`COPY src/x.c [TEST1]src/x.c` is refused; create `[TEST1.SRC]` and
copy to `[TEST1.SRC]X.C` instead. A name without a type is stored as
VMS stores it, with the dot (`MAKEFILE.`), and can be typed either
way.

File versions follow VMS rules too. `NAME;3` is version 3; `NAME`,
`NAME;` and `NAME;0` mean the highest version; `NAME;-1` is the next
lower existing version; `NAME;*` means every version (`DIR` and
`DELETE`). `DIR` lists every version of every file unless you give a
version (`DIR [DECUS]*.*;` shows just the newest of each). `DELETE`
removes one version - the one given, or the highest if none is - and
`DELETE NAME;*` removes them all. `COPY` makes a **new version** of a name that exists, as VMS's COPY does (measured on VAX V7.3 and
V8.4: CORPUS 49 and 49.8): `COPY x [D]A.TXT` (or `A.TXT;` or `A.TXT;0`) makes the highest version + 1 (version 1 of a new name); `A.TXT;n` makes version n if
it is free (a gap is fine) and fails with `%RMS-E-FEX` if it exists; ;32767 is the last (a plain COPY after it, or `;32768`, is `%RMS-E-CRE` with
`%SYSTEM-W-BADFILEVER`); `;*` and `;-1` are refused. The entry's version limit (`SET FILE/VERSION_LIMIT`, stored in the directory entry) is enforced as part of the COPY, in VMS's
order (CORPUS 49.1 C4): the new version is made first, from blocks and a file header that were free before the change began and written, and the oldest
version is removed after it, in the same all-or-nothing change (the freed header slot goes to the next create; free blocks are the same
before and after; a full volume refuses the COPY, honestly). An interrupted COPY leaves the old version intact or gone, never changed
(review 40 F1: removing it first let the new file's content be written onto its blocks before the commit). When the limit is reached exactly **one** version goes, the lowest, however far over its limit the name is: VMS makes the new version
and deletes one, so a name whose limit was lowered below its count keeps its count (`SET FILE/VERSION_LIMIT` itself deletes nothing; CORPUS 49.9, VAX
V7.3 and V8.4 ODS-2 and ODS-5). Refused, changing nothing, because VMS's behaviour there is not measured: an explicit version below all the kept ones
when the limit is reached (it would be deleted at once).

**Owner and protection of a new version (CORPUS 49.9).** The new version takes the **protection of the highest version before it**, whatever the
default of ods2v2 or a default-protection ACE of the directory (VMS applies that ACE to a new *name* only). It also takes that version's **owner**:
ods2v2 acts like a privileged VMS process (or the volume owner), and for those VMS leaves the previous version's owner. An *unprivileged* VMS user who
is not the volume owner would instead get their own UIC as the owner; ods2v2 has no such user. A new **name** gets the owner [1,4] and the default protection
of ods2v2. VMS refuses a new version with `%RMS-E-PRV` when the highest version cannot be written by an unprivileged process; ods2v2 is privileged, so it does
**not** refuse (a version whose protection allows nobody is still given a new version). `IMPORT/NEW_VERSION` does the same for every file of a tree that exists (without the qualifier an existing name is skipped).

### Record formats

With no qualifier `COPY` (and `IMPORT`) choose the record format per file: **text** (no NUL byte, hardly
any other control byte, lines of at most 32767 bytes) becomes a Stream_LF file with carriage-return carriage
control, the longest line as its record length and no maximum, as VMS's own text files are (a CR before an LF
stays part of the line, so a file with mixed LF and CR LF ends keeps its bytes as they are; CR LF throughout
makes a Stream file; CR-only line ends make a Stream_CR file); **anything else** becomes fixed 512-byte records with no carriage
control and an exact end-of-file byte, as VMS binary FTP makes it. (Before, every file was Stream_LF with a
512-byte maximum record size; VMS V7.3-2 cannot read a binary file written like that when a "line" of it is
longer than 512 bytes: `%RMS-W-RTB`. V8.4 could.) `/FORMAT=` overrides the choice: `AUTO` (the default),
`STMLF` (or `TEXT`), `STM`, `STMCR`, `FIX512` (or `BINARY`), `UDF` (size 0 maximum, the file's length as record size)
or `VAR` (= `/CONVERT`); it cannot be combined with `/ATTRIBUTES`. Some files must have a
particular RMS record format - OpenVMS PCSI kit files, for one, are
sequential files of fixed 8192-byte records, and `PRODUCT` won't treat
a Stream_LF copy of the same bytes as a kit. Give the attributes with
`/ATTRIBUTES` on `COPY`, in the syntax of VMS's `SET FILE/ATTRIBUTES`:

```
COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) dec-axpvms-foo-v0100--1.pcsi [KITS]DEC-AXPVMS-FOO-V0100--1.PCSI
COPY/ATTRIBUTES=(RFM:FIX) backup.bck [SAVE]BACKUP.BCK          ! fixed 512-byte records
COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) notes.txt [DOC]NOTES.TXT
```

- `RFM:` record format - `FIX`, `STMLF`, `STM`, `STMCR` or `UDF`, or
  `VAR` with `/CONVERT` (below). On disk each `VAR` or `VFC` record
  starts with a byte count a host file doesn't have, so `VAR` needs the
  conversion; `VFC` can't be imported.
- `MRS:` the record size (for `FIX`; `LRL:` means the same there).
  `RFM:FIX` with no size is 512-byte records. The file must be a whole
  number of records, and the size even (2-32766).
- `RAT:` record attributes - `NONE`, `CR`, `FTN`, `PRN`, `BLK`, `NOBLK`;
  repeat `RAT:` for more than one.
- A stream file (`STMLF`, `STM`, `STMCR`) or `UDF` file gets MRS 0 and its
  own longest record as `LRL` (the longest record as RMS reads it: `STMLF` ends at LF, `STMCR` at CR, `STM` at LF, FF or VT - that byte stays in the record - or CR LF; the length for `UDF`), as
  VMS makes them, unless `MRS:` or `LRL:` is given. A line longer than a
  given `MRS:` or `LRL:` is refused (`%ODS2-E-RECTOOLONG`): RMS could not
  read the file back (`%RMS-W-RTB`, record too large for the user's
  buffer). (Before, `/ATTRIBUTES` left MRS and LRL at 512, and `ods2_put`
  and the library's `ods2_create_file()` did too; a file with a longer
  line could not be read on VMS. `ods5check` warns of such a file, and
  `SET FILE/ATTRIBUTES=(MRS:0) spec` repairs it: MRS 0 and LRL the
  longest record, the data untouched.)

`tests/ods5check.py` judges a stream file's record lengths in tiers, by what
VMS then does (measured on V8.4 and VAX V7.3): a **warning** when a record is
longer than a non-zero MRS (`TYPE` fails with RSZ, `COPY` to `NL:` and
`CHECKSUM` with `RTB` on V7.3, `CONVERT` drops the record), and when MRS is 0
but the LRL is below the longest record (`COPY` to `NL:` and `CHECKSUM` fail,
`TYPE` and `CONVERT` work); no message for a non-zero MRS that is enough with a
short LRL; an **info** line (`-v`) for a record over 32,767 bytes with MRS 0 and
for LRL 0 / MRS 0 with a record over 512 bytes. It also warns of a link count of
0 in the reserved files of a HARDLINKS volume (V8.4 ANALYZE: `LINKCOUNT`; what
INITIALIZE followed by SET VOLUME/VOLUME_CHARACTERISTICS=HARDLINKS leaves).

VMS's native text format is variable-length records with carriage-return
carriage control (`RFM:VAR`, `RAT:CR`) - what VMS FTP writes in ASCII
mode, and what some older utilities and kit procedures expect.
`/CONVERT` makes that from a host text file, one record per line:

```
COPY/CONVERT notes.txt [DOC]NOTES.TXT
COPY/ATTRIBUTES=(RFM:VAR,RAT:CR)/CONVERT notes.txt [DOC]NOTES.TXT   ! the same
```

- Lines end at LF; a CR just before the LF is dropped, so CRLF text
  converts the same. A last line with no LF is still a record.
- The longest record length (`LRL`) is set from the file; `MRS` is 0
  (no limit) unless given. A line over 32,767 bytes is refused.
- `RAT:` defaults to `CR`; `RAT:NONE`, `FTN` or `PRN` can be given
  instead. `RAT:BLK` isn't supported (the records run on across
  blocks, as VMS FTP writes them).
- `TYPE` of such a file shows its records as text as VMS `TYPE` does (one
  line each for `RAT:CR`; `/LINES` forces that for any file); `TYPE/RAW` shows the bytes as they are on disk, byte counts
  included.

The qualifier goes on `COPY` itself, not after the file names (a host
path has `/` in it too). `ods2_put` takes the same as numbers:
`ods2_put disk kit.pcsi KITS KIT.PCSI 1 8192` (record format, record
size, record attributes).

### Volume labels

A copy of a blank volume carries the blank's label, and two volumes
with the same label can't both be mounted `/SYSTEM` on one node - they
clash on the `DISK$label` logical name. Relabel a copy on the host, as
VMS's `SET VOLUME/LABEL` would, while VMS doesn't have it mounted:

```
./ods2 kits.dsk SET VOLUME/LABEL=ITANKITS
./ods2 kits.dsk SHOW VOLUME
```

- The label is 1-12 characters - letters, digits, `$`, `_` and `-` -
  and is uppercased.
- Every copy of the home block is rewritten: the home block and its
  cluster fillers, and the backup home block's cluster (8 blocks on
  the starter volume, cluster factor 3). Each keeps its own position
  fields, with both checksums recomputed; the primary is written last.
- Nothing is written unless every one of those copies is first found
  valid and consistent with the primary. A volume whose copies don't
  agree should go through `ANALYZE/DISK` on VMS first.
- A volume set member is refused: relabel a set on VMS.

### Structure version 0 volumes

The spec (5.1.9.4) has HM2$W_STRUCLEV as level 2, version 1, but the
OpenVMS Alpha V8.4 CD is level 2, version 0, and VMS mounts it. Such a
volume is accepted for reading only (`DIR`, `TYPE`, `COPY` out, `SHOW
VOLUME`), with one warning on stderr (`%ODS2-W-STRUCLEV0`). Every
command that would write to it, and `ods2_mount_write()`, is refused
and the image is not touched. Any other structure level is still
refused. The image may be read-only (`chmod 444`).

### ODS-5 volumes (structure level 5): read and write

A structure level 5 volume mounts like an ODS-2 one; `SHOW VOLUME` reports
`ODS-5, version 1` and the volume characteristics (`NOHIGHWATER`,
`ACCESS_DATES`, `HARDLINKS`, ...). An image that cannot be written
(`chmod 444`) is mounted read-only and every command that would write
is refused. Other levels (3, 4, 6) are refused. ODS-5 reading and writing
were checked against volumes made by OpenVMS itself (see "Tested on" below).

**A short tour.** Real output; the image is made here, then used. (`ods2 IMAGE "COMMAND"` runs one
command; `ods2 IMAGE` alone is the interactive prompt.)

```
$ ods2_init demo.img 16M --structure=5 --label=DEMO --hardlinks
%ODS2-I-INITIALIZED, demo.img: 32768 blocks, ODS-5, label DEMO
$ ods2 demo.img "CREATE/DIRECTORY [Docs]"
%ODS2-I-CREATED, created [Docs]
$ ods2 demo.img "COPY hello.txt [Docs]Hello^_World.TXT"
%ODS2-I-COPIED, 29 bytes to Hello^_World.TXT;1
$ ods2 demo.img "CREATE/DIRECTORY [Docs.Tree]"
%ODS2-I-CREATED, created [Docs.Tree]
$ ods2 demo.img "IMPORT tree [Docs.Tree]"
%ODS2-I-IMPORTED, 4 files (1524 bytes), 1 directory, 1 symbolic link imported from tree; 0 skipped, 0 errors
$ ods2 demo.img "DIR [Docs.Tree]"

Directory [Docs.Tree]

español.txt         ;1
README.             ;1
readme.lnk          ;1
sub.DIR             ;1
two^_words.txt      ;1

Total of 5 files.
$ ods2 demo.img "DIR/UTF8 [Docs.Tree]*.TXT"

Directory [Docs.Tree]

español.txt         ;1
two words.txt       ;1

Total of 2 files.
$ ods2 demo.img "TYPE [Docs]Hello^_World.TXT"
Hello from Linux
second line
$ ods2 demo.img "LINK [Docs]Second^_Name.TXT [Docs]Hello^_World.TXT"
%ODS2-I-LINKED, Second^_Name.TXT;1 is a link to file (12,1,0)
$ ods2 demo.img "SYMLINK [Docs]latest.lnk Hello^_World.TXT"
%ODS2-I-SYMLINK, latest.lnk;1 -> Hello^_World.TXT
```

`^_` writes a space in a name (a command line is split at white space); `DIR` shows names with the same
escapes, `DIR/UTF8` as UTF-8. The hard link (this volume has `HARDLINKS`; without it `LINK` makes an alias) and
the record format `COPY` and `IMPORT` chose for each file (text: Stream_LF; the random binary: fixed
512-byte records); then the whole tree out again:

```
$ ods2 demo.img "DIR/FULL [Docs]Second^_Name.TXT"
Directory [Docs]
Second^_Name.TXT;1
  File ID:            (12,1,0)
  Primary link name:  Hello^_World.TXT;1
  Size:               1/1 blocks, 29 bytes
  Protection:         (S:RWED, O:RWED, G:RE, W:)
  Created:            09-NOV-2026 02:13:09.00
  Revised:            09-NOV-2026 02:13:09.00 (1)
  Link count:         2
  File organization:  Sequential
  Record format:      Stream_LF, maximum 0 bytes, longest 16 bytes
  Record attributes:  Carriage return carriage control
Total of 1 file.
$ ods2 demo.img "DIR/FULL [Docs.Tree.SUB]data.bin"
Directory [Docs.Tree.sub]
data.bin;1
  File ID:            (18,1,0)
  Size:               3/3 blocks, 1500 bytes
  Protection:         (S:RWED, O:RWED, G:RE, W:)
  Created:            09-NOV-2026 02:13:09.00
  Revised:            09-NOV-2026 02:13:09.00 (1)
  Link count:         1
  File organization:  Sequential
  Record format:      Fixed length 512 byte records
  Record attributes:  None
Total of 1 file.
$ ods2 demo.img "EXPORT/VERSIONS [Docs...]*.* out"
%ODS2-I-EXPORTED, 6 files, 2 symbolic links copied to out
```

**Writing.** `CREATE/DIRECTORY`, `COPY`, `DELETE`, `SET VOLUME/LABEL`
work as on ODS-2, but a name keeps its case and may hold any character VMS
accepts: write it with the circumflex escapes (`COPY notes.txt
[Made^_By^.Hand]New^_Name.TXT`) or, with `COPY file [dir]` and no name,
the Linux file name is used as it is (UTF-8: ISO Latin-1 when every
character is up to U+00FF, else UCS-2). The new files carry level 5
headers (FI5 ident area, link count, access and attribute dates) built as
VMS V8.4 builds them, and are entered in the directory in VMS's order.
A name equal to an existing one but for case is a new version of that name, and the case first stored stays (as VMS: `Abc.txt`, then `ABC.TXT`: `Abc.txt;2`,
CORPUS 49.8 C9). Also:

* `IMPORT <linux-dir> <path>` copies a Linux tree (directories made,
  `%2F` in a name is a `/`, Linux symbolic links become VMS ones).
* `SYMLINK <link-path> <target>` (or `CREATE/SYMLINK=target link`) makes a
  symbolic link; `LINK <new-path> <existing-path>` (or `SET FILE/ENTER=new
  existing`) makes another directory entry for a file. On a `HARDLINKS`
  volume it is a hard link and counted; deleting one link takes the entry
  and one off the count, the file goes with the last (the primary link of
  a file that has others may go first, as on VMS). On any other volume
  (ODS-2, or ODS-5 without `HARDLINKS`) it is a plain alias, as VMS
  `SET FILE/ENTER` makes there: nothing in the file's header changes, and
  deleting such an entry (one that is not the file's primary link) takes
  only the entry.
* `ods2_init <new-image> <size> [--structure=2|5] [--label=NAME]
  [--hardlinks] [--access-dates] ...` makes an empty ODS-2 or ODS-5 volume
  of any size from 8192 blocks, laid out as VMS V8.4's INITIALIZE does; the
  image file must not exist. `--maxfiles=N` is clipped to the number of blocks with a note (a volume cannot have more files than blocks: ods5check refuses an SCB volume size smaller than the file count) and raised to 64 if less; a count between blocks / (cluster + 1) and the blocks is kept, with a note that VMS INITIALIZE may not allow it (VMS INITIALIZE clips /MAXIMUM_FILES to floor(blocks / (cluster + 1)): 2,048 for 4,096 blocks at cluster 1, CORPUS 38, and measured at clusters 1, 3, 8 and 16 on 8,192 and 16,384 blocks, on Alpha V8.4 (ODS-2 and ODS-5) and VAX V7.3 (ODS-2), ODS-5 on Alpha V8.4 only, CORPUS 62 and 64; but VMS still reads and writes volumes above it, as with the battery's `a6-cluBIG`, cluster 2048, 2,000 files, on five versions); `--maxfiles-over-limit` keeps even a count above the blocks. `--geometry=SECTORS,TRACKS` sets the disk geometry
  the storage control block records (cylinders = ceil(blocks / (sectors x
  tracks)), as VMS writes them; the default is 32 x 16, and 63 x 255 above
  16 GiB so that the cylinder count stays below 65,536). VMS writes the geometry
  of the device it initialized, and OpenVMS VAX V7.3's ANALYZE/DISK_STRUCTURE
  accepts only a storage control block that records the geometry of the device the
  volume is on (measured on an RA82: 57 x 15; the LD driver, 38 x 38; SCSI disks,
  often 63 x 255). The default is 32 x 16 (63 x 255 above 16 GiB), which Alpha
  V8.4 and V7.3-2 accept; `ods2_init` prints the geometry it chose. **For an
  OpenVMS VAX V7.3 machine name the device when you make the volume:
  `--target=ld` (the LD driver), `--target=ra` (SIMH RA82 / RA92) or
  `--target=scsi` (also `axpbox`, `rz`; or `--geometry=SECTORS,TRACKS`), and fit an
  existing image with `SET VOLUME/GEOMETRY=ld|ra|scsi|sectors,tracks`.** The
  geometry of the LD driver and of the RA types depends on the size. For `ld`
  the table holds the 31 sizes measured on VAX V7.3 (LDDRIVER V8.3): sectors x
  tracks is the largest k x k or k x (k+1) not above the square root of the
  size, cylinders = ceil(blocks / that), but the ORDER of the two (11 x 12 or
  12 x 11) follows no rule found, and ANALYZE reports `CHKSCB` for the wrong one
  (it compares sectors and tracks one by one). A size that is not in the table
  is said to be a prediction, and where the order is ambiguous the warning says
  so: check `LD CONNECT` and `SHOW DEVICE/FULL`, then give
  `--geometry=SECTORS,TRACKS` (or `SET VOLUME/GEOMETRY=SECTORS,TRACKS`).

**Names.** ODS-5 keeps the case of names and allows spaces, many dots,
ISO Latin-1 and UCS-2 characters. `DIR` shows them the way DCL does,
with circumflex escapes: `^_` a space, `^.` a dot that is not the type
delimiter, `^&` `^,` `^;` ... the characters DCL escapes, `^xx` an ISO
Latin-1 byte (`^FF`, `^A0`, `^80`-`^9F`), `^Uxxxx` a character above
U+00FF. `DIR/UTF8` (or `SET NAMES UTF8`, or `ODS2_NAMES=utf8` in the
environment) shows the names as UTF-8 instead; `SET NAMES ESCAPED`
switches back. The same escapes are accepted in every specification you
type: `DIR [Lots^.Of^.Periods]two^_words*`, `DIR caf^E9.txt`; a typed
UTF-8 character is itself. A command line is split at white space, so a
space in a name is written `^_`. Lookups and wildcards (`*`, `%`, `?`)
are case blind and work on whole characters, not bytes (the letters of
ISO Latin-1 and UCS-2 fold like VMS folds them, e.g. `É` finds `é`, `^U03B1`
finds Greek capital Alpha); directory names
are shown in their stored case. Directories nest to 2,048 levels. On an ODS-2 volume a directory name cannot start with `-`: VAX VMS (V7.3) reads a leading `-` as the parent directory (`[-]`) and ODS-2 has no `^` escape, so it cannot name anything inside such a directory (`%CLI-W-PARSEFAIL`, `%RMS-F-SYN`); Alpha V8.4 can (measured), but an ODS-2 volume is the one that goes to VAXen, so it is refused; `CREATE/DIRECTORY` and `IMPORT` refuse it, with a message, and `ods5check` warns of one that exists. On ODS-5 it is fine (VMS writes it `^-`).
`DIR/FULL` shows, per file, what the file header says: id, size, owner,
protection, the dates (also the access and attribute-change dates of
ODS-5), the link count where the volume has hard links, the record
format, a symbolic link's target, and the primary link's name when the
entry is an alias. `DIR/PATHS` prints one `dir/dir/NAME.EXT;ver` line
per file, UTF-8, for scripts.

**Copying files out.** `EXPORT <path> <linux-path>` (alias `GET`) copies
files off the volume to Linux files: the bytes of the file up to its
end-of-file mark, exactly what `TYPE/RAW` prints (RMS record structure
is not interpreted), under the file's UTF-8 name. `/RECORDS` writes
sequential files as text instead, as `TYPE` does (`/LINES`: one line per
record; VAR, VFC, FIX, STM, STMLF, STMCR; relative and indexed files are copied
as stored, with a warning). The destination is a
directory (made if it does not exist) when the specification has
wildcards, `;*` or an ellipsis; a specification naming one file may
instead give the new file's name. An ellipsis (`[000000...]*.*`) recreates
the directory tree below the start directory, directories by name
(`Lots.Of.Periods/`, not `Lots^.Of^.Periods.DIR`). `/VERSIONS` appends
`;N` to every name, `/OVERWRITE` replaces existing files (otherwise they
are refused), `/LINKS` makes a repeated file (an alias or hard link, one
file with several names) a Linux hard link instead of a second copy. A
symbolic link (an ODS-5 special file) becomes a Linux symbolic link;
other special files are skipped with a warning; directory files are not
copied. A `"quoted"` argument keeps white space: `EXPORT [A]*.* "/my files"`.

**Versions.** An ODS-5 (and ODS-2) directory may hold many versions of a name: `DIR` lists them all (`;*` or no
version), `TYPE NAME;2`, `EXPORT/VERSIONS` (every version, `NAME;N` on the Linux side) and `DELETE NAME;N` work on
the one you name, and `;0` / no version / `;-1` mean the highest / the next lower one, as in VMS. ods2v2 itself makes
version 1 of a new name, or a new version of one that exists (`COPY`, `IMPORT/NEW_VERSION`).

**Linux names (IMPORT and EXPORT).** A Linux name becomes an ODS-5 name as it is (UTF-8; ISO Latin-1 when every
character is up to U+00FF, else UCS-2); characters VMS refuses are rejected. Where the two worlds disagree a `%XX`
escape (two hex digits) stands for the byte: `%2F` is a `/` in a VMS name, and a control character in a name is
written `%XX` on the way out so that it cannot reach your terminal or steer a path. A name that VMS stores with a
trailing dot and no type (`README.`) is `README` on Linux and back. The directory tree, symbolic links, hard links
(`EXPORT/LINKS`) and empty directories survive the round trip.

**Access control lists.** `DIR/FULL` prints a file's ACL as VMS's `DIRECTORY/FULL` does:

```
$ ods2 vms-made.img "DIR/FULL [CORPUS.ACL]ACL1.TXT"
  ...
  Access Cntrl List:  (IDENTIFIER=NETWORK,ACCESS=NONE)
                      (IDENTIFIER=[1,2],ACCESS=READ)
                      (IDENTIFIER=[200,1],ACCESS=READ+WRITE+EXECUTE+DELETE)
```

Identifier, CREATOR, DEFAULT_PROTECTION, AUDIT, ALARM and APPLICATION ACEs (also those continued in extension headers)
are shown, with the options DEFAULT, PROTECTED and NOPROPAGATE. Identifiers are numbers, `[group,member]` in octal:
ods2v2 has no rights database, so `[USERS,USER1]` of VMS's listing is `[200,1]` here. An ACE of another kind (and
an ACE with unusual flags) is shown as its bytes: `(ACE type N, flags %Xnnnn, data ..)`. An APPLICATION ACE
(the hidden one DDIF files of a system disk carry) is shown as VMS prints it with the SECURITY privilege:
`(APPLICATION,SIZE=%D25,FLAGS=%X0E03,ACCESS=%X00010000,DATA=%X...,%X...)`. VMS hides such an ACE (HIDDEN flag)
from users without SECURITY; ods2 reads the disk with every privilege and shows it. ACLs are read, never written.

**Records.** `TYPE` shows a sequential file the way VMS `TYPE` does (see "Record formats" above); a file with
records, for example:

```
$ ods2 vms-made.img "DIR/FULL [CORPUS.RMS]SEQ_VFC.DAT"
  ...
  Record format:      VFC, 2 byte header, maximum 80 bytes, longest 61 bytes
  Record attributes:  Print file carriage control
```

`TYPE/RAW` and `EXPORT` give the stored bytes, `EXPORT/RECORDS` the text, `TYPE/LINES` one line per record.

**Limits.** No volume sets, no ODS levels 3, 4 or 6 (refused), no quota or journaling, no UCS-2 beyond the Basic
Multilingual Plane in names. File numbers are 24 bits (FID_NMX << 16 | FID_NUM), as on VMS volumes with more
than 65,535 files; an entry naming another volume of a set (a nonzero RVN) is refused. Rights identifiers are numbers (above);
SUBSYSTEM ACEs, and the combinations of FORTRAN or print carriage control with the stream record formats that VMS
accepts, have not been compared with VMS (the stream formats are always shown as lines; print control with a stream
format is refused, as VMS does). A directory whose end-of-file is
wrong (as old ods2v2 versions left them on ODS-2) is put right by the next write to it; other directory damage is not repaired.

**Tested on.** Volumes written by ods2v2 (ODS-2 and ODS-5, also volumes it made with `ods2_init`) mount and pass
`ANALYZE/DISK_STRUCTURE/READ_CHECK` on OpenVMS Alpha V8.4, V8.4-2L2 and V7.3-2, and VMS wrote into them there
(files, directories, aliases, deletes) with a clean ANALYZE afterwards; on OpenVMS VAX V7.3 the ODS-2 volumes mount, read
and take VMS writes and the ODS-5 ones mount and read (names with characters outside ASCII appear there, and on V7.3-2,
as `\pISO_LATIN\.???` placeholders). Reading VMS-made volumes matches `DIRECTORY/FULL` and the file checksums of the test
volumes of those systems. Physical machines were tested through the LD driver: a VAX (V7.3) and an Alpha (V8.4); on a
real Alpha V7.3-2 (PWS 500au) only cluster-size images were mounted and analysed (CORPUS 45). OpenVMS VAX V7.3's
`ANALYZE/DISK_STRUCTURE` compares the volume's recorded geometry with the device's (`CHKSCB`): make the volume with the
right `--target`. (The simulators and machines are named in `CORPUS.md`; the record is in ods5-cli.)

### File dates and time zones

VMS keeps file dates in local time, so `ods2` stamps new files (and
revised directories) with the time in the zone your VMS system runs
on, not in UTC. By default that's the host's own zone, which honours
`TZ`. If the guest's zone differs from the host's, set `ODS2_TZ` to
the guest's zone; it takes precedence over `TZ` and affects only
`ods2`:

```bash
ODS2_TZ=America/New_York ./ods2 transfer.dsk COPY build.com [CONVERT]
export ODS2_TZ=UTC        # for a VMS system that really runs on UTC
```

**The zone must be the VMS machine's**: a volume stamped in a zone behind the VMS machine's (for instance by a script that sets
`TZ=UTC` on a host that is not on UTC) gets dates in the VMS machine's future, which `ANALYZE/DISK_STRUCTURE` reports as
`FUTCREDAT` / `FUTREVDAT` (and, on ODS-5, `FUTACCDAT` / `FUTATTDAT`). `ods2_init` says which zone it used, and
`tests/ods5check.py` warns of dates more than an hour ahead of the host that runs it.

Any value `TZ` accepts works, including POSIX rules such as
`EST5EDT,M3.2.0,M11.1.0`. A zone name the host doesn't recognise
quietly falls back to UTC, so check the spelling if dates still look
off. If they're wrong, `ANALYZE/DISK_STRUCTURE` on VMS reports
`FUTCREDAT` / `FUTREVDAT` for files dated ahead of its clock.

### A simple example

```
$ ./ods2 transfer.dsk
ODS2> CREATE/DIRECTORY [DECUS]
%ODS2-I-CREATED, created [DECUS]
ODS2> SET DEFAULT [DECUS]
%ODS2-I-DEFSET, default set to [DECUS]
ODS2> COPY /tmp/readme.txt README.TXT
%ODS2-I-COPIED, 59 bytes to README.TXT;1
ODS2> DIR
Directory [DECUS]
README.TXT           ;1
ODS2> type readme.txt
Use this to get VMS savesets onto your VAX/VMS sim system.
ODS2> EXIT
```

## Development/diagnostic tools

Ten additional single-purpose tools exist from this project's own
development:

One operation each, no DCL syntax:
`ods2_cat`
`ods2_ls`
`ods2_mkdir`
`ods2_put`
`ods2_rm` 

Raw on-disk structure inspection:
`ods2_check_headers`
`ods2_dump_header`
`ods2_dump_root`
`read_file_header`
`read_home_block`

Fully functional, but not needed for normal use - `ods2` already covers
everything these commands do.

Build them with:

```bash
make dev-tools
or
make all
```

## Project structure

```
include/    Headers for the core library (ods2_volume.h is main)
src/        Core library - reading, writing, validating ODS-2 structures
tools/      ods2.c (the CLI) plus the dev-tools individual source files
tests/      One test suite per src file, run via `make test`
samples/    Real, VMS-written fragments used by the test suite, plus a
            ready-to-use starter volume (see samples/README.md)
third_party/linenoise/  BSD licensed - interactive-mode command history
            with zero external dependencies
```

## Testing

`make test` builds and runs 35 C self-test programs and 40 further sections (scripts that check
the `ods2` command line, and the Python checker and fuzz runs; counts of 18-NOV-2026 at f7a13f9), with `-fsanitize=address,undefined` on by default for development
builds (`make release` builds without them once things are stable).
Most tests run entirely offline against a synthetic disk
image assembled from real, VMS-written byte fragments captured during
development with VMS generated volumes so there was genuine on-disk bytes
in every structural test.

## Changes of behaviour (the ODS-5 / records work)

- **`TYPE` now decodes records by default**, as VMS `TYPE` does (carriage control applied; a Stream_LF file
  without a final line feed gets one). It used to print the stored bytes: scripts that extract bytes with
  `TYPE` must use `TYPE/RAW` (`EXPORT` still copies the stored bytes by default; `EXPORT/RECORDS` gives text).
- `DIR/FULL` shows the ACL, the record format and attributes and `Link count` as VMS does; ODS-2 directories
  are now made with the end-of-file VMS uses; directories' revision count rises by 2 per block gained.
- **`COPY` and `IMPORT` choose the record format per file** (text: Stream_LF, RAT CR, no maximum; binary: fixed
  512-byte records, no RAT), where every file used to be Stream_LF with a 512-byte maximum; `/FORMAT=` and
  `/ATTRIBUTES` override it (see Record formats). An ODS-2 volume is treated the same.
- `LINK` / `SET FILE/ENTER` on a volume without `HARDLINKS` makes a plain alias; `DELETE` of such an entry
  keeps the file and says so.
- **The writer protects the volume.** `DELETE` refuses the volume's own files (`INDEXF.SYS`, `BITMAP.SYS`,
  ... file numbers 1-9, and `SECURITY.SYS`), and deletes in VMS's order (header marked, entry removed, blocks
  freed), so an interrupted `DELETE` leaves a leak that `ANALYZE/DISK_STRUCTURE/REPAIR` cleans up, never a
  listed file on free space. A damaged directory block or file header, or a directory with an extension
  header, is refused ("not changed") instead of being written around. `ods2` locks the image: one process
  writes, or many read; a second writer is refused (a reader finding the lock mounts read-only). The lock
  is advisory (it does not stop a simulator that has the image open), and a file system without `flock`
  gives no lock and no message; `ODS2_NOLOCK=1` turns it off. `ods2` `fdatasync`s a written image when it
  finishes and fails the command if that fails (`ODS2_NOSYNC=1` skips it). A directory entry that names a
  file by a relative volume number or an extended file number is not followed by a write.

## Large volumes and image files

What a volume and a file may be, as far as ods2v2 is concerned (VMS's own limits are noted where they are known; "not measured"
means no VMS version has been tried at that size):

| | VMS structure | ods2v2 |
| :-- | :-- | :-- |
| volume size | `SCB$L_VOLSIZE` is 32 bits: up to 2^32 - 1 blocks (2 TiB) | `ods2_init`: 8,192 to 2^32 - 16 blocks, with a warning above what a VMS version mounts. Measured (ods2v2-made and VMS-made volumes alike): Alpha V7.3-2 mounts up to 2,147,475,456 blocks (2^31 - 8192) and refuses one more (`%MOUNT-F-FILESTRUCT`); Alpha V8.4 mounts up to 4,261,348,350 = 65534 x 255 x 255 and refuses more (CORPUS 35.1; HP's "OpenVMS V8.4 New Features and Documentation Overview", 2.4, gives the same figure); VAX V7.3 mounts 2,147,483,632 blocks (2^31 - 16) and refuses 2,147,483,664 (`%MOUNT-F-UNSUPPORTED`; its limit between them was not searched: CORPUS 40.1) |
| cluster factor | `HM2$W_CLUSTER` is a word, `HM2$W_IBMAPVBN` = 4c + 1 must fit one: up to 16,383. Measured: `INITIALIZE/CLUSTER_SIZE` takes 1 to 16,380 and refuses 16,381 and above (`%INIT-F-BADCLUSTER`) on Alpha V8.4 and V7.3-2 (CORPUS 36.2, 39); both mount and write a volume that has 16,383. VAX V7.3 INITIALIZE takes 16,381 and 16,382 and refuses 16,383 (CORPUS 40.2): it has no 16,380 cap; the VAX mounts the 16,383 volumes too | `ods2_init --cluster=` 1 to 16,380 (the strictest limit: Alpha INITIALIZE), and at least ceil(blocks / (65535 x 4096)) (HP's INITIALIZE formula, applied as written: the storage bitmap, one bit per cluster and without the SCB block in front of it, is limited to 65,535 blocks. VAX V7.3 INITIALIZE of 2^31-16 blocks, of the values tried: 1, 2, 4 and 8 refused, 9-16 accepted (11 not tried), 17 and up refused (CORPUS 40.1); the lower bound, which equals HP's formula there, rests on one disk size. ods2_init enforces the lower bound only, because the upper bound has no explanation and is the VAX's alone: V7.3-2 accepted 16 to 8,192 on that size (CORPUS 35.3), and VMS-initialized volumes with clusters 9-16 and ods2v2's cluster-16 volume of that size mounted on the VAX, while a larger cluster of an ods2v2-made volume there is not measured. The default cluster is raised to the minimum above 4,294,901,760 blocks: 17 there is ods2v2's choice, not VMS's (HP rounds the default up to a multiple of 16, which would be 32; it cannot matter on VMS, whose largest measured volume, V8.4's 4,261,348,350 blocks, is below that size)); `--cluster-over-limit` allows 16,381 to 16,383 (the VAX INITIALIZE takes 16,381 and 16,382) and a cluster below the minimum, with a warning (V8.4, V7.3-2 and VAX V7.3 refuse to mount such a volume, `%MOUNT-F-FILESTRUCT` (CORPUS 41, 43); the same size with the minimum cluster mounts (41.1, 43)); any value is read and written |
| file numbers | 24 bits (FID_NMX << 16 \| FID_NUM): 16,777,215 | all of them; the allocator skips multiples of 65,536 |
| file size | `HIBLK`, `EFBLK` are 32 bits: 2^32 - 1 blocks | the same; lengths are 64-bit, seeks are `fseeko` with a 64-bit `off_t` (the build refuses a 32-bit one) |
| file pieces | one retrieval pointer per piece; extension headers up to 65,535 per file | no limit but memory (reading, deleting, creating); directories and BITMAP.SYS read at most 1,024 pieces |
| INDEXF.SYS | grows in the volume's extend quantity; extension headers when its map is full | grows in big steps, lengthens its last piece; does NOT write extension headers (VMS V8.4 crashes at the first INDEXF.SYS growth through the ones ods2v2 wrote: CORPUS 37/38; VMS makes none itself), refuses when the primary's map is full |
| directory | sorted blocks, contiguous; VMS's own size limit is not measured | up to 65,536 blocks; a name lookup halves the blocks of a directory of 32 or more |
| volume sets | relative volume numbers | not supported: refused on relabel and on writes; an entry naming another volume is refused |

**Image files.** `ods2_init` makes a sparse file: a 2 TiB volume takes a few tens of MB on the host until it is written.
`cp`, `rsync` and `tar` write the holes out in full unless asked not to: copy an image with `cp --sparse=always` (or
`--reflink=auto` on a file system that has it), `rsync -S`, `tar -S`, `dd conv=sparse`, and check with `du` and `ls -l`
(disk use against apparent size). A volume needs host space for what is written into it and for its structures (the bitmap
is 1 bit per cluster, the index file 1 block per file), not for its size. An image file shorter than the volume it holds
(a truncated copy) is mounted read-only with `%ODS2-W-IMAGESHORT`: extend it (`truncate -s`) to the volume's size to write it.
The check applies only to a storage control block that is one (level 0x0201 or 0x0501): a block that is no SCB, such as the
all-ones placeholder of the starter volume, claims no size, is not compared with the image, and the volume stays writable.
A write the host cannot hold (disk full, a file size limit) fails the command and says why (`No space left on device`,
`File too large`); the change is undone and the volume stays sound. The storage bitmap of a write mount is kept in memory
(up to 64 MiB of it) and COPY from a pipe needs room in `$TMPDIR` or the image's directory for the piped data.
`tests/ods5check.py --sample=K` checks a volume with millions of headers in minutes (headers only: see its header).

## Known limitations

- The ODS-5 file-length-hint words of a file header's ident area read "invalidated" on the files ods2v2 writes, where VMS-made files hold real counts (CORPUS 68; no rename or `ods5check` effect found, a later item): not filled in.
- `COPY` takes any size of file the volume has room for. Its content
  goes in as few pieces as the free space allows (one, on a volume
  with a long enough free run), described by Format 1, 2 or 3
  retrieval pointers, continuing into extension headers on fragmented
  free space (no limit but memory and the 65,535 headers of a file). `COPY` and `TYPE` stream the
  content in 1 MiB chunks, so memory use stays flat however large the
  file. Input with no length to go by (a pipe, `/dev/stdin`) is first
  copied to a temporary file (in `$TMPDIR`, else in the image's directory, never
  in `/tmp`; it is unlinked at once), so that needs room for it. Directories still grow only within one header.
- Formatting a *new* volume is not an `ods2` command: it is the separate tool `ods2_init` (this branch only; upstream
  has none, and `samples/README.md` explains upstream's reasons for not having one). See "ODS-5 volumes". An `ods2_init`
  volume is not bit-identical to one VMS INITIALIZE makes (dates, owner UIC, backup index header position, INDEXF.SYS
  preallocation); VMS mounts and analyses it cleanly on the versions listed under "Tested on".
- **File header fields that differ from what VMS writes** (measured on VAX V7.3, V8.4 ODS-2 and ODS-5: CORPUS 52 point 4 and 5, review 44). Neither
  stops VMS from reading, checking (ANALYZE) or copying the files, and ods5check accepts both:
  * **The header of a new file is VMS's own on ODS-2 since follow-up b** (at ODS-5 it already was; both levels now write the same): **record protection 0**
    (`FH2$W_RECPROT`; VMS writes 0xFE00 only in the reserved files and the master directory, which `ods2_init` still writes so), **no `CONTIG` characteristic** (`FH2$M_CONTIG`
    0x80: none of the 2,644 user files of the VMS-made ODS-2 volumes has it, 225 of them in one piece), and **highest block written = HIBLK + 1**. *Highest block written is the
    **stored** value of `FH2$L_HIGHWATER`; `DUMP/HEADER` prints it minus 1* (a file of 8 blocks with stored 9 shows 8). ods2v2 writes or zero-fills every block it allocates, so
    the value is true and no block of its files holds another file's old data. What VMS does with these fields later (ods5-vms3, ASIM5 V8.4, CORPUS 56.4): a file with `CONTIG`
    copied by `COPY` gives `%COPY-W-CBT` and by `BACKUP` `%BACKUP-E-NONCONTIG` where free space is fragmented (the copies are made, in pieces, without the flag), and `APPEND` or
    `OPEN/APPEND` extends a CONTIG file non-contiguously and clears the flag, which is why ods2v2 no longer sets it; `RECPROT` 0xFE00 behaves exactly as 0 (no difference in reading,
    writing or appending as a user who is not the owner); an `APPEND` inside the allocation sets the stored highest block written to HIBLK + 1 (from 0) or to the end-of-file block + 1 (from the
    end-of-file block + 1 of a file `COPY` made), and past the allocation to EFBLK + 1. **VMS does not zero the blocks an `APPEND` adds or those past the new end of file** (CORPUS 58.7, VAX V7.3, on an image whose unwritten blocks and free blocks were filled with 0xAA: after an `APPEND` inside the allocation blocks 4 to 8 were still all 0xAA, and after one past the allocation the last block of the new allocation was still all 0xAA), and it sets the stored highest block written to the allocation + 1 even over such blocks (0 to 9 over blocks holding 0xAA): the stored value is a mark "everything is written", not a promise that VMS wrote it. **So HIBLK + 1 is true for ods2v2 only because ods2v2 writes or zero-fills every block it allocates** (write_extents), and that is what keeps old data of a deleted file from showing through the unwritten tail of a file's allocation when a later VMS `APPEND` extends the end of file into it. The older ods2v2 wrote 0xFE00, `CONTIG` on a file of one piece and stored 0 at ODS-2; files and volumes it made keep those headers (nothing
    rewrites them). `tests/cli_filehdr_check.sh` (both levels, cluster 1 and 8, COPY, IMPORT, a fragmented volume) and `tests/cli_filehdr_vms_check.sh` (the VMS-made volumes of
    the corpus follow the same rule) hold it.
  * **The extension headers of an ODS-2 user file are VMS's own since follow-up d** (the VMS-made ones of ods5-vms3's `asim5-frag-sf-2.img` and the corpus's `phvs2-ods2v-20261108.img`:
    24 of 24; CORPUS 58.6): the map starts at byte 100 (word 50) after an "ident area" of 20 bytes, the file name alone (no revision, dates or `FILENAMEXT`), and holds up to 205 words (410 bytes: 102 four-byte
    pointers fill 204; when pointer formats mix, as in the `FULLMAP.DAT` of the b2-ext images, the 205th word can be used) (ods2v2 wrote the map at word 100 after a full 120-byte ident area: 77 pointers). At ODS-5 ods2v2's extension headers already were VMS's (map at word 50, the FI5 prefix).
    **Files made by older ods2v2 keep the map at word 100, and VMS reads them** (ANALYZE clean and the file's CHECKSUM equal on `b2x-ops-2`, CORPUS 58.5): nothing rewrites them, and
    `SET FILE/OWNER_UIC` changes only the owner of such a header. The INDEXF.SYS extension header, which ods2v2 makes only for a test, keeps the old layout (VMS-made ones have not been
    seen). `tests/exthdr_vms_compare.py` holds the layout against the VMS-made headers. **The record-attribute fields** (item 6, CORPUS 61: ods5-vms3's COPY of a 1,000-block file into holes of mixed sizes, an APPEND, a long name, on V8.4 and VAX V7.3, ODS-2 and ODS-5, the extension headers byte-identical on both): each extension header holds the primary's RTYPE, RATTRIB, RSIZE and MAXREC and **HIBLK 0, EFBLK 1, FFBYTE 0, stored HIGHWATER 1**; the primary header alone holds the file's size; an APPEND grows the last extension header and adds none. ods2v2 makes its headers in one go like COPY, and now writes exactly that (ods2v2 wrote the whole file's size in every header). **Not copied:** the extension headers of a file VMS *grew* by one-block extends (CORPUS 58.6) hold RSIZE 0 and a HIBLK equal to the blocks the primary maps: ods2v2 never grows a file that way. `tests/exthdr_attr_check.py` holds it against the VMS-made F1, F2 and F3 containers.
  * **The header of a new directory** is VMS's own at both levels since follow-up c (CREATE/DIRECTORY on the step 0 containers of VAX V7.3 and V8.4, ODS-2 and ODS-5; CORPUS 49 and
    53): protection (S:RWE,O:RWE,G:RE,W:E) = 0xBA88, record protection 0, revision number 0, "Highest block written" = HIBLK + 1, the rest of the header equal field by field
    (`tests/dirhdr_vms_compare.py`). Before it, a directory made on an **ODS-2** volume had 0xFA00, 0xFE00, revision 1 and no highest block written (an unprivileged user who is not
    the owner could not search it, and the owner could delete it); directories made by older ods2v2 volumes keep that header (`SET FILE/PROTECTION=(S:RWE,O:RWE,G:RE,W:E)` on each
    puts the protection right). The owner is `[1,4]` (VMS: the process's UIC), by design.
- **Disk quotas (open, not changed):** ods2v2 ignores `QUOTA.SYS`: no code reads or writes it (the README's "no quota" is the whole of it). It neither refuses a volume that has one nor keeps it: a `COPY`, `DELETE`, `PURGE` or `SET FILE/OWNER_UIC` leaves it exactly as it was, so its usage figures go stale. **`SET FILE/OWNER_UIC` on a volume with a quota file does not move the usage from the old owner to the new one; what VMS does in that case is not measured here.** The same holds for every write on such a volume. **ods2v2 does not protect `QUOTA.SYS` itself either:** a `COPY` onto it and a `DELETE` of it both work (found by ods5-rev2's review 59). `ods2_init` makes no `QUOTA.SYS`. Whether ods2v2 should refuse writes on a volume with an active quota file is not decided.
- **Directory revision count on VAX V7.3 (open, not changed):** a directory that grows adds 2 to its revision number for each block it gains here (measured on V8.4 and V7.3-2: ODS5_FORMAT_NOTES 15.4), but a directory VMS grew on a VAX adds 1 per block: `G.DIR` of ods5-vms3's `phvs2-copy-2.img` (VAX V7.3) is 42 blocks with revision 40, the same directory of `asim5-copy-2.img` (V8.4) revision 80. ANALYZE does not check the count and ods2v2 cannot know which VMS will read a volume, so there is no rule per version.
- A version of a name whose record is full (58 versions of a 34-character name) starts a new record, as VMS does; an explicit version that belongs inside a full record
  is refused. The version limit of an explicit version below all the kept ones is refused (VMS's behaviour there is not measured).
- `TYPE` takes wildcards in the name and comma lists, not in the directory part (`TYPE [T...]*.TXT` works on VMS and is refused here: `%ODS2-E-WILDDIR`); `DELETE` takes wildcards in the file name, in one directory (since this branch's port of upstream's #24; see the command table), wildcards in the directory part are refused; `PURGE` and `SET FILE/PROTECTION` take both (`DIR` too).
- **Versions of a name stored out of order (H1, review 50; settled by CORPUS 66.4):** on a directory record that stores a name's versions out of order (;1 ;2 ;3: written by another tool or damaged; ods2v2 and VMS never write it) VMS follows the STORED order for every version form: `TYPE X.TXT`, `;0` and `;-1` show the first and second entry as stored, `DIRECTORY` lists them as stored, `PURGE` keeps the first stored entry, `SET FILE` changes it, `DELETE` with no version is refused (`%DELETE-E-DELVER`) and `ANALYZE` says `%ANALDISK-W-BAD_VERSORDER` (`ANALYZE/REPAIR` changes nothing). ods2v2 **reads** the same way (TYPE, COPY from, DIRECTORY, EXPORT keep the stored-order lookup). A command that **changes** something (`DELETE`, `SET FILE/PROTECTION`, `/OWNER_UIC` and `/ATTRIBUTES`, `PURGE`, and a new version by `COPY`) **refuses** such a name (`%ODS2-E-VERSORDER ... BAD_VERSORDER`, the volume unchanged, the other names of a wildcard go on) unless it names an explicit version (`;n`) or all of them (`;*`): then it acts as told. This is a deliberate divergence from VMS for `PURGE` and `SET FILE` (VMS acts on the first stored entry; ods2v2 used to purge by version number: both can lose the file the user wanted on a damaged directory), and an explicit `;3` that is listed is found (VMS says "file not found"). `ods5check` reports each pair of neighbouring versions that is not descending as `BAD_VERSORDER`. A `COPY` onto such a name whose version limit would delete a version is refused even with a version typed (the version to go is chosen by number, VMS follows the stored order). `PURGE` takes no version, so its refusal points to `DELETE name;n`. Not refused, on purpose: `SET FILE/VERSION_LIMIT` (it sets the limit of the name's records and deletes and selects nothing by position) and `LINK` (it makes another entry and changes none of the name's records; without a version it takes the first stored entry, as VMS reads). A name in the proper order is not affected, and is judged once for all its versions (4,000 versions: PURGE or SET FILE ;* in about a tenth of a second).
- INDEXF.SYS is extended as new files need it, anywhere on a volume of any size (Format 1 retrieval pointers
  below LBN 4,194,304, Format 2 above it): by 256 blocks at a time while it is under 4,096 blocks, then by half
  its size, at most 16,384 blocks (one Format 2 pointer). New blocks that follow the last piece lengthen its
  pointer; otherwise they get a pointer of their own, in the primary header's map. **ods2v2 does not make INDEXF.SYS
  extension headers.** When the primary header's map is full it refuses to grow the index file (new files are refused:
  "the index file cannot grow", nothing written), as it always did before it had an extension-header writer, and it does
  not extend an index file that already has extension headers (it reads those, and mounts them; a volume whose chain
  cannot be followed to its end is mounted read-only). The reason is measured (CORPUS 37, 38): OpenVMS V8.4 reads a
  volume whose INDEXF.SYS links an extension header written by ods2v2 (e0bad43 and earlier test builds), but it
  **crashes the first time it has to grow INDEXF.SYS through that header** (BUGCHECK UNXSIGNAL in the ACP: a volume with
  14 free header slots behind the header took 14 file creations, the 15th, which needed INDEXF.SYS to grow, crashed it;
  any creation that needs no growth is fine). Where the header carries pointers and files lie behind it, V8.4 cannot open
  INDEXF.SYS at all (`ANALYZE/DISK_STRUCTURE` gives `%ANALDISK-F-OPENINDEX`, files behind the first extension header are
  "no such file"). Fields that differ from a VMS-made extension header (map at word 50, name-only ident area, FILECHAR 0,
  EFBLK 1) made no difference. VMS itself made no INDEXF.SYS extension header in any run: it extends the index file by a
  new contiguous piece and adds one pointer to the primary's map (28-40 of 155 words in fragmentation runs), and the
  volume reached its maximum files first. So ods2v2 refuses when the primary map is full and writes none. The code that
  makes extension headers stays in the source for the tests, behind the fault-injection build's `ODS2_TEST_INDEXF`
  hook, and its volumes are for ods2v2's own tests only. `tests/ods5check.py` reports an extension header of INDEXF.SYS
  that looks like ods2v2's (map at word 100 on ODS-2; a level 5 header behind a level 2.1 primary on ODS-5) as an error,
  and mount prints `%ODS2-W-INDEXEXT` (a heuristic: no VMS-made one has been seen): mount such a volume on VMS only with
  /NOWRITE; read-only use is safe.
  An interruption of a growth between its writes (only when the last header is an extension
  header, so only on a volume made with the hook) leaves HIBLK and EFBLK behind the map for a moment (the checker says so,
  and mount prints `%ODS2-W-INDEXGROWTH`); the first change of any kind made through a write mount (a relabel as well as a
  new file) repairs them from the map, once per mount; that repair is written outside the change that triggered it, so a
  command that then fails may still have written it. A growth that does not fit the last header's remaining 4 bytes takes
  a short piece (up to 256 blocks, a Format 1 pointer, below LBN 4,194,304) instead of failing.
- File numbers are 24 bits (up to 16,777,215, FID_NMX << 16 | FID_NUM), bounded by the volume's MAXIMUM_FILES
  and its index bitmap; an entry naming a file by a relative volume number (a volume set) is refused.
- A directory is limited to 65,536 blocks (32 MiB of records), the most one
  contiguous extent is allowed here (it is held in memory while it changes). Directories nest at most 2,048 levels
  (VMS V7.3-2, V8.4 and V8.4-2L2 created 10,000, but VMS's own wildcard walk `[...]` stops at 256 directories on V7.3-2 and
  512 on V8.4, with `%RMS-F-DIR`, and BACKUP cannot save files that deep: ods2v2 makes no tree its own `DIR [...]` and
  `EXPORT` could not read back, and `CREATE/DIRECTORY` and `IMPORT` warn past 256 levels). The readers follow a tree down
  to 2,048 levels (a bound on the C stack; a deeper tree, which only VMS can have made, is reported with the directory's
  name and entry count, never cut silently). `EXPORT` copies what the host's 4,096-character path limit allows and names
  every file or directory it could not copy (and counts the rest); the exit status is 1. On an ODS-2 volume
  `CREATE/DIRECTORY` and `IMPORT` also warn from the 8th level below [000000], which OpenVMS VAX V7.3 cannot reach
  (`%RMS-F-DIR`; Alpha V7.3-2 and later can). A file's extents and extension headers are not limited (up to the
  65,535 headers of FH2$W_SEG_NUM; reading, deleting and creating a file in thousands of pieces works); the directories
  and the bitmap file keep arrays of 1,024 extents and refuse, with a message, a file with more rather than use part of
  it. A directory of 32 blocks or more is searched by halving its blocks (a name lookup reads about log2(blocks) blocks);
  a directory is still read whole for each insert or delete, and an insert into one of 32 blocks or more refuses a name and
  version that is in it already (the search finds nothing in a directory that is not in order, so a create would have added
  it a second time).
- `ods2_init` gives BADBLK.SYS the blocks past the last whole cluster when the size is not a multiple of the cluster, as VMS
  INITIALIZE does (V8.4 ANALYZE reports `ALLOCCLR` and `FREESPADRIFT` for a volume that leaves them to nobody);
  `SET VOLUME/PARTIAL_CLUSTER` does the same for a volume made by an older `ods2_init`, and `ods5check.py` warns of one.
- Mounting warns (`%ODS2-W-CHKSCB`, read only; the volume is used as before) when the storage control block is one
  OpenVMS ANALYZE/DISK_STRUCTURE would call invalid: another structure level or cluster factor than the volume's,
  a blocking factor of 0 (OpenVMS VAX V7.3 then fails with INTDIV; a SIMH RQ disk above about 26 GiB makes this),
  or a geometry more than a cylinder from the volume size.
- A disk container footer of the simh/simh simulator (the last 512 bytes, signature `simh`) is not volume data: it
  is noted, never read or written. If it lies inside the volume (a 32-bit overflow on images of 4 GiB or more puts
  it in the last data block) the volume is mounted read-only with a warning. No block at or past the volume size
  (SCB$L_VOLSIZE) is ever written.

## Dependencies

None beyond a C compiler and `make`. The interactive shell's command
history (up/down arrows) uses a BSD licensed copy of linenoise rather
than linking against a system library like GNU Readline - keeps `make`
working identically on every platform with no separate install step.
FYI, the vanilla linenoise doesn't appear to support Ctrl-R reverse search.

## License

MIT - see LICENSE. Applies to this project's own code; the BSD copy of
linenoise (third_party/linenoise) has its own, separate license.

## Acknowledgements

I started this project from scratch after seeing the great work
from various people on "ods2".  I determined the best way to get
a solid working ODS2 utility was to go back to the original 
DEC Files-11 Specification, versus attempting patches or fixes to 
the original ods2 source code.

That ods2 code had these references in it:

 Jul-2003, v1.3hb, some extensions by Hartmut Becker
 Ods2.c v1.3   Mainline ODS2 program

 This is part of ODS2 written by Paul Nankervis,
 email address:  Paulnank@au1.ibm.com

 ODS2 is distributed freely for all members of the
 VMS community to use. However all derived works
 must maintain comments in their source to acknowledge
 the contibution of the original author.

 Modified by:
 31-AUG-2001 01:04 Hunter Goatley <goathunter@goatley.com>
 For VMS, added routine getcmd() to read commands with full
 command recall capabilities.

 This is the top level set of routines. It is fairly
 simple minded asking the user for a command, doing some
 primitive command parsing, and then calling a set of routines
 to perform whatever function is required (for example COPY).
 Some routines are implemented in different ways to test the
 underlying routines - for example TYPE is implemented without
 a NAM block meaning that it cannot support wildcards...
 (sorry! - could be easily fixed though!)

