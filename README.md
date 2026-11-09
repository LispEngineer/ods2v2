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
to get a VAX8600 running VMS 7.1 on a Ubuntu 24 Linux system.

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
| `COPY <local-file> <path>` | `COPY readme.txt [DECUS]README.TXT` |
| `COPY/ATTRIBUTES=(...) <local-file> <path>` | `COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) kit.pcsi [KITS]KIT.PCSI` |
| `COPY/CONVERT <local-file> <path>` | `COPY/CONVERT notes.txt [DOC]NOTES.TXT` — text as VAR records |
| `TYPE[/RAW\|/LINES] path` | `TYPE [DECUS]README.TXT;2` — sequential files as text the way VMS `TYPE` shows them (carriage control applied: a file with none shows its records run together); `/LINES` one line per record always; `/RAW` the stored bytes |
| `DELETE path` | `DELETE [DECUS]OLDFILE.TXT;1` |
| `SET DEFAULT path` | `SET DEFAULT [DECUS.NETLIB020]` |
| `SHOW DEFAULT` | |
| `SET VOLUME/LABEL=name` | `SET VOLUME/LABEL=ITANKITS` — relabel the volume |
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
`DELETE NAME;*` removes them all. `COPY` onto the disk always creates
version 1 and refuses a name that already exists.

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
A name equal to an existing one but for case is refused (it would be a new
version, which ods2v2 does not make; it creates version 1 only). Also:

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
  image file must not exist. `--geometry=SECTORS,TRACKS` sets the disk geometry
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
  geometry of the LD driver and of the RA types depends on the size; a size that
  was not measured is said to be a prediction (check `SHOW DEVICE/FULL`).

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
are shown in their stored case. Directories nest to 255 levels.
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
version 1 of a new name only.

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
`ANALYZE/DISK_STRUCTURE/READ_CHECK` on OpenVMS Alpha V8.4-2L2 and OpenVMS Alpha V7.3-2, and VMS wrote into them there
(files, directories, aliases, deletes) with a clean ANALYZE afterwards; on OpenVMS VAX V7.3 the ODS-2 volumes mount, read
and take VMS writes and the ODS-5 ones mount and read (names with characters outside ASCII appear there, and on V7.3-2,
as `\pISO_LATIN\.???` placeholders). Reading VMS-made volumes matches `DIRECTORY/FULL` and the file checksums of the test
volumes of those three systems. Physical VAX and Alpha machines (through the LD driver) are still to be tested.

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

`make test` builds and runs 20 test suites and five scripts that check
the `ods2` command line, with `-fsanitize=address,undefined` on by default for development
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

## Known limitations

- `COPY` takes any size of file the volume has room for. Its content
  goes in as few pieces as the free space allows (one, on a volume
  with a long enough free run), described by Format 1, 2 or 3
  retrieval pointers, continuing into extension headers on fragmented
  free space - up to 1,024 pieces. `COPY` and `TYPE` stream the
  content in 1 MiB chunks, so memory use stays flat however large the
  file. Input with no length to go by (a pipe, `/dev/stdin`) is first
  copied to a temporary file (in `$TMPDIR`, or `/tmp`), so that needs
  room for it. Directories still grow only within one header.
- No INITIALIZE command (formatting a *new* volume from scratch) - see
  `samples/README.md` for why that's a deliberate choice (it's complicated)
  and how to get a real, ready-to-use volume instead.
- `COPY` can't add a new version of an existing file - it creates
  version 1 of a new name only.
- `TYPE` and `DELETE` don't take wildcards yet, in file names or
  directories (VMS's do); only `DIR` does.
- INDEXF.SYS is extended as new files need it, anywhere on a volume of any size (Format 1 retrieval pointers
  below LBN 4,194,304, Format 2 above it): by 256 blocks at a time while it is under 4,096 blocks, then by half
  its size, at most 16,384 blocks (one Format 2 pointer). Only the pointers its own header's map area holds are
  available (about 51 Format 2 ones: roughly 510,000 files, more if the early steps were small); past that new
  files are refused ("no room for another extent") and nothing else is affected. INDEXF.SYS extension headers
  are read (a VMS volume that has them mounts) but not yet written.
- File numbers are 24 bits (up to 16,777,215, FID_NMX << 16 | FID_NUM), bounded by the volume's MAXIMUM_FILES
  and its index bitmap; an entry naming a file by a relative volume number (a volume set) is refused.
- A directory is limited to 65,536 blocks (32 MiB of records), the most one
  contiguous extent is allowed here (it is held in memory while it changes), and directories nest at most 255
  levels (a VMS volume can hold more: V7.3-2 made 400). Reading has no such limit: `DIR [...]`, `EXPORT` and the
  other readers follow a tree down to 2,048 levels (five times what VMS lists) (a bound on the C stack; a deeper tree is reported with the
  directory's name and entry count, never cut silently). `EXPORT` copies what the host's 4,096-character path limit
  allows and names every file or directory it could not copy (and counts the rest); the exit status is 1. On an
  ODS-2 volume `CREATE/DIRECTORY` and `IMPORT` warn from the 8th level below [000000], which OpenVMS VAX V7.3
  cannot reach (`%RMS-F-DIR`; Alpha V7.3-2 and later can). A file with more than 1,024 extents, or a header chain
  with more than 64 segments, is refused (read and delete) rather than handled partly.
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

