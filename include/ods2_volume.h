/*
 * MIT License
 *
 * Copyright (c) 2026 Allen Pomeroy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* ods2_volume.h - the real read path: open a disk image, read its
 * home block, locate any file's header (general VBN-walking approach,
 * not limited to the first 16 files), read file content, and look up
 * paths by walking directory content.
 */
#ifndef ODS2_VOLUME_H
#define ODS2_VOLUME_H

#include "ods2_ondisk.h"
#include "ods2_retrieval.h"
#include "ods2_directory.h"
#include <stdio.h>
#include <stdbool.h>

/* Most extents (retrieval pointers) a file may have across all its
   headers - the size of the extent lists used when reading, writing
   and deleting. ods2_create_file() refuses a file that would need
   more, which happens only on badly fragmented free space. */
#define ODS2_MAX_EXTENTS 1024

typedef struct {
    FILE          *fp;
    bool           writable;
    const ods2_fid_t *create_protect;  /* files a multi-file command (IMPORT) has created: a version-limit prune of one of them by a later create is refused (review 77 O1); see ods2_set_create_protect */
    unsigned       ncreate_protect;
    bool           quota_known;        /* the lookup of [000000]QUOTA.SYS was made (it cannot appear during a session: ods2v2 refuses to make it) */
    unsigned       quota_number;       /* its file number, 0 when there is none */
    char           write_refusal[640]; /* why a read-only mount refuses every write, set by the caller (the CLI: disk quotas are active); empty: the usual reason */
    bool           version0;  /* HM2$W_STRUCLEV version 0: read-only only */
    bool           ods5;      /* HM2$W_STRUCLEV level 5 (ODS-5): read-only
                                 for now, every write path refuses it */
    unsigned       dir_block_errors; /* directory records skipped as malformed (never reset) */
    ods2_home_t    home;
    /* INDEXF.SYS's own extents, decoded once at mount time - needed
       to locate every other file's header, including files beyond
       the first 16 (which can be located directly from home block
       fields alone; anything past that genuinely requires walking
       INDEXF.SYS's own content, which is what this enables). */
    ods2_extent_t *indexf_extents;      /* grown as needed (INDEXF.SYS's primary header and its whole extension chain) */
    size_t         indexf_extent_cap;
    int            indexf_extent_count;
    /* the storage bitmap of a write mount, read once (bitmaps up to 64 MiB) and kept in step by every allocation and free; the
       hint is a cluster index below which every cluster is allocated, so a first-fit search starts there */
    uint8_t       *bm_data;
    size_t         bm_bytes;            /* bytes of bitmap data (the SCB block is not included) */
    size_t         bm_hint;
    bool           bm_valid;
    unsigned       idx_highest;         /* the highest file number marked in use in the index bitmap, when idx_highest_valid: */
    bool           indexf_checked;      /* the first change of a write mount has looked at INDEXF.SYS's last growth (see below) */
    bool           idx_highest_valid;   /* kept by every change to the bitmap, so a create does not read all of it */
    /* A change to the volume (a delete, a create, a link) is made in memory first and written when it is
       known to succeed: every check that can fail runs before the first write to the image, so a refused
       or failed operation leaves the image byte for byte as it was (see txn_begin() in ods2_volume.c).
       Not for callers to touch. */
    struct ods2_txn *txn;
    int            txn_depth;
    uint32_t       volsize;      /* SCB$L_VOLSIZE of BITMAP.SYS (0 if it could not be read): no write goes at or past it */
    bool           indexf_chain_ok; /* INDEXF.SYS's extension header chain was followed to its end at mount; false: read only */
    bool           write_failed; /* sticky: a change failed part way through its write; no more writes on this mount */
} ods2_volume_t;

typedef struct {
    bool        ok;
    const char *problem; /* NULL if ok */
} ods2_result_t;

/* Opens `path` and reads/validates the home block, then reads and
   decodes INDEXF.SYS's own header so later header lookups can walk
   its extents. `vol` is caller-allocated; on failure vol->fp is
   closed automatically if it was opened. Opens read-only. A volume
   whose home block has structure version 0 mounts (vol->version0 is
   set), and so does an ODS-5 volume (structure level 5, vol->ods5 is
   set); every write to either is refused. */
ods2_result_t ods2_mount(const char *path, ods2_volume_t *vol);

/* Same as ods2_mount(), but opens the underlying file for read-write
   ("r+b") so ods2_write_block()/ods2_write_header() etc. can be used.
   The file must already exist (this never creates a new disk image -
   that's INITIALIZE's job, deliberately out of scope here). */
ods2_result_t ods2_mount_write(const char *path, ods2_volume_t *vol);

/* Disk quotas (CORPUS 71/72): VMS marks quotas ACTIVE only by the existence of [000000]QUOTA.SYS (no home block flag; DISKQUOTA DISABLE changes nothing on disk, and MOUNT enables quotas whenever the
   file exists). ods2v2 does not keep that file, so any write would leave its usage figures wrong (VMS charges the owner of every file made, deleted or changed). ods2_mount_write() therefore FAILS on such a
   volume, with a message (ODS2_QUOTA_ACTIVE_TEXT is in it) that says why, names the override and advises DISKQUOTA REBUILD on VMS afterwards. ods2_mount_write_opts() with ODS2_MOUNT_IGNORE_QUOTA mounts it
   for write anyway. Reads (ods2_mount) are unaffected. Whatever the flag, QUOTA.SYS itself is never deleted, renamed, linked to or given a new version, and no entry of that name is made in the master
   directory (VMS refuses a DELETE of it while quotas are active). */
#define ODS2_MOUNT_IGNORE_QUOTA 1u
#define ODS2_QUOTA_ACTIVE_TEXT "disk quotas are active"
/* ... and when the master directory cannot be read to the end, so that nothing can be said about QUOTA.SYS (review 74 F1: the guard fails closed; ODS2_MOUNT_IGNORE_QUOTA overrides this too): */
#define ODS2_QUOTA_UNKNOWN_TEXT "could not be read to look for [000000]QUOTA.SYS"
ods2_result_t ods2_mount_write_opts(const char *path, ods2_volume_t *vol, unsigned flags);
/* True when [000000]QUOTA.SYS exists on the volume (any version): quotas are active. */
bool ods2_quota_active(ods2_volume_t *vol);

/* Writes a single 512-byte block at absolute LBN `lbn`. Fails if the
   volume was mounted read-only via ods2_mount() rather than
   ods2_mount_write(). */
ods2_result_t ods2_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block);

/* Closes the volume. Safe to call even if mount failed partway. */
/* Writes everything written so far to the disk: fflush and fdatasync (ODS2_NOSYNC=1 in the environment skips
   the fdatasync). One call per mount, not one per block. A failure is reported: the command is not done.
   Does nothing on a read mount. ods2_dismount() calls it too, ignoring the result. */
ods2_result_t ods2_sync(ods2_volume_t *vol);

void ods2_dismount(ods2_volume_t *vol);

/* Reads the 512-byte header for `file_number` (1-based) into
   `header_out`. Works for any file number whose header falls within
   the VBN range covered by INDEXF.SYS's decoded extents - which for a
   volume with a small number of files (well under what a full
   INDEXF.SYS extent list would cover) is normally everything that
   exists. Reads any header, primary or extension (seg_num > 0). */
/* The FID of a directory entry or header link, as something this code can follow: RVN 0 (a volume set is
   not supported), NMX 0 until 24-bit file numbers are. */
/* The problem ods2_read_header_fid() gives for an entry whose file is gone (a stale alias: a VMS state, compare
   r.problem with it, not an I/O or format error). */
extern const char ods2_stale_entry[];

ods2_result_t ods2_check_fid(ods2_fid_t fid);

/* Reads the header the FID names; refuses a FID ods2_check_fid() refuses. Use this, not ods2_read_header()
   with fid.fid_num, for every FID read from a directory entry. */
ods2_result_t ods2_read_header_fid(ods2_volume_t *vol, ods2_fid_t fid, uint8_t *header_out);

ods2_result_t ods2_read_header(ods2_volume_t *vol, unsigned file_number,
                                uint8_t *header_out);

/* Write counterpart to ods2_read_header() - writes a caller-built
   512-byte header to file_number's known location. The volume must
   be mounted for write. */
ods2_result_t ods2_write_header(ods2_volume_t *vol, unsigned file_number,
                                 const uint8_t *header_in);

/* Reads virtual block `vbn` (1-based) of the file described by
   `header` into `block_out` (512 bytes), by decoding the header's own
   Map Area and walking its extents. */
ods2_result_t ods2_read_file_block(ods2_volume_t *vol, const uint8_t *header,
                                    unsigned vbn, uint8_t *block_out);

/* Looks up `name` (case-insensitive, e.g. "NETLIB020.DIR", no
   ";version") within the directory whose header is `dir_header`,
   returning the FID of its HIGHEST version. Equivalent to
   ods2_lookup_name_version(..., ODS2_VERSION_HIGHEST, ...). */
ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out);

/* Version selectors for ods2_lookup_name_version() and
   ods2_delete_version(), following VMS file-specification rules:
     > 0   that exact version (1..32767)
     0     the highest existing version (VMS "NAME;" or "NAME;0")
     < 0   relative to the highest existing version: -1 is the next
           lower EXISTING version, -2 the one below that, and so on
           (VMS "NAME;-1"). Counts existing versions, not numbers, so
           gaps (e.g. ;7 and ;3 only) are skipped over. */
#define ODS2_VERSION_HIGHEST 0

/* Looks up one version of `name` (case-insensitive, no ";version")
   within the directory whose header is `dir_header`, selected by
   `version` (see ODS2_VERSION_HIGHEST above). On success returns the
   FID and, if `version_out` is non-NULL, the actual version number
   selected. Fails with "name not found in directory" if the name
   does not exist at all, or "version not found" if it does but the
   selected version does not. Handles names whose versions span
   several directory records and blocks. */
ods2_result_t ods2_lookup_name_version(ods2_volume_t *vol, const uint8_t *dir_header,
                                        const char *name, int version,
                                        ods2_fid_t *fid_out, uint16_t *version_out);

/* RENAME in one directory (PLAN_WRITE_OPS item 5, step 1; CORPUS 65 and 66; the design: RENAME-PROPOSAL.md, review 65). The file the source entry names (src_version: >0 that version, 0 the
   highest, <0 relative; the stored order is followed and a name out of order is refused unless a version is named: H1) gets the name dst_name. dst_version: 0 = none given: the new version is
   1 for a name that is new and one above the highest for a name that exists (CORPUS 66.2: the source's version is never kept); >0 = that version, refused with "...DUPFILENAME" when
   it exists. The target is the same entry (the same name and version): nothing is written (info->same; VMS bumps the revision and the date, ods2v2 does not). A target that differs only in
   case (ODS-5): the record's name bytes are replaced in place (info->case_only). A target entry that already names the source's file is an interrupted rename: it is finished, not
   repeated (info->resumed). The header: name, revision +1, the level's date (ODS-2 revision date; ODS-5 attribute change date) when the entry is the file's primary one or the header names
   no entry (back link 0: adopted, name and back link set); an alias entry leaves the header alone; the extension headers are never touched (CORPUS 66.2). The version limit of the target name
   removes the lowest version as COPY does (CORPUS 66.1). Order of the writes (HEADER FIRST): the header (name, back link, revision, date; link count + 1 on HARDLINKS), the new entry, the old entry removed, link count - 1: one name or two at every
   stop, never none; the header's back link names the target directory from the first write on (CORPUS 68.3, 69). Refused, the volume unchanged: a directory (step 3), a reserved file, an entry that is an alias of a directory... dst_dir_number may be another directory of the volume (step 2; info->moved): the
   entry goes there, the header's back link with it; an alias entry (the header names another directory or name) moves with the header untouched. */
typedef struct {
    uint16_t version;          /* the version the file has now */
    bool same;                 /* the target is the source: nothing changed */
    bool case_only;            /* the record's name bytes were replaced in place */
    bool resumed;              /* an interrupted rename was finished */
    bool header_changed;       /* the file's header was rewritten (false: an alias entry, or a header a stopped RENAME already wrote: header_current) */
    bool header_current;       /* the file's header already named the new name (the first write of a RENAME that was stopped): not written again */
    bool adopted;              /* the header named no entry: its back link was set */
    unsigned pruned_version;   /* the version the version limit removed, or 0 */
    bool moved;                /* the new entry is in another directory (step 2) */
    bool is_dir;               /* the file renamed is a directory (step 3) */
    unsigned deepest_names;    /* a directory moved: the number of names in the path of the deepest directory below it now (an ODS-2 path of more than 8 names cannot be opened by OpenVMS VAX) */
} ods2_rename_info_t;
/* The name `name` of directory `dir_file_number` as the directory STORES it (on an ODS-5 volume the first stored spelling: lower.txt stays lower.txt however it was typed), for a command that fills in what a
   new name leaves out (RENAME [Y]N takes the type of the source: review 69 recheck R2). Fails when the directory has no such name. `out` holds at least ODS5_MAX_LITERAL_NAME + 8 bytes. */
ods2_result_t ods2_stored_name(ods2_volume_t *vol, unsigned dir_file_number, const char *name, char *out, size_t out_n);

/* ods2_rename() for a command that has resolved the new directory by names: `via` lists the whole file IDs of the directories that path passed through (the master directory not counted; the target itself
   last). A DIRECTORY is not moved under itself: the typed path (`via`), the back-link chain of the target and the directories below the source are all compared with the source's whole file ID
   (review 65 C4 and 71 C1): -RMS-E-IDR, nothing written. NULL/0: the path is not known (the other two checks still run). */
ods2_result_t ods2_rename_ex(ods2_volume_t *vol, unsigned src_dir_number, const char *src_name, int src_version,
                             unsigned dst_dir_number, const char *dst_name, int dst_version, const ods2_fid_t *via, unsigned nvia, ods2_rename_info_t *info);
ods2_result_t ods2_rename(ods2_volume_t *vol, unsigned src_dir_number, const char *src_name, int src_version,
                          unsigned dst_dir_number, const char *dst_name, int dst_version, ods2_rename_info_t *info);
/* ods2_rename_ex() for a command that renames SEVERAL files (RENAME with wildcards, review 77): `protect` lists the whole file IDs of every file the command renames (the whole match list). A version-limit
   prune whose victim is a file in that set is REFUSED for this file, before any write (the per-file transaction leaves the volume unchanged), whatever the command's own simulation expected: a file the
   command renames is never deleted by the command. (No HARDLINKS exemption: a link count can be one too high after a stopped rename.) NULL/0: no file is protected. */
ods2_result_t ods2_rename_protect(ods2_volume_t *vol, unsigned src_dir_number, const char *src_name, int src_version,
                                  unsigned dst_dir_number, const char *dst_name, int dst_version, const ods2_fid_t *via, unsigned nvia,
                                  const ods2_fid_t *protect, unsigned nprotect, ods2_rename_info_t *info);
/* Files made by the command now running (IMPORT/NEW_VERSION): a create that would delete one of these files by the version limit of its name is REFUSED for that file (nothing is written; the message says
   so), whatever the command expected (review 77 O1: three host files that fold to one name, default limit 1, used to leave only the last, in silence). The array is not copied: keep it valid while it is set; NULL/0 clears. */
void ods2_set_create_protect(ods2_volume_t *vol, const ods2_fid_t *fids, unsigned n);
/* Are the versions of `name` stored newest first (descending), across all the records of the name? *in_order_out is false when two neighbouring entries of the name
   are not descending. A directory in that state is damaged or written by another tool (ods2v2 and VMS never write it). VMS follows the STORED order for every version form
   (CORPUS 66.4: ;0, TYPE and PURGE take the first stored entry, DELETE with no version is refused, ANALYZE says BAD_VERSORDER): the lookup does too. A command that CHANGES
   something on a name that is not in order should refuse unless it names an explicit version (the commands of ods2 do: DELETE, SET FILE, PURGE; RENAME). A name that does
   not exist is "in order". */
ods2_result_t ods2_name_versions_in_order(ods2_volume_t *vol, const uint8_t *dir_header, const char *name, bool *in_order_out);

/* Splits a VMS-style directory path (e.g. "DECUS.NETLIB020", no
   brackets) on '.' and walks from the root directory (FID 4,4),
   returning the final directory's FID. An empty path returns root
   itself. */
ods2_result_t ods2_lookup_path(ods2_volume_t *vol, const char *path,
                                ods2_fid_t *fid_out);

/* Lists all entries in the directory whose header is `dir_header`
   into a caller-supplied buffer of `max_entries`. Never truncates
   silently: if the directory has more entries than fit, this returns
   a failure, stores the first `max_entries`, and sets *count_out to
   the directory's TOTAL entry count so the caller can size a buffer
   and retry. For directories of unknown size prefer
   ods2_list_directory_alloc(). */
ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out);

/* Lists all entries in the directory whose header is `dir_header`
   into a malloc'd array sized to fit, however large the directory.
   On success *entries_out must be released with free() (it may be
   NULL when *count_out is 0). On failure nothing needs freeing. */
ods2_result_t ods2_list_directory_alloc(ods2_volume_t *vol, const uint8_t *dir_header,
                                         ods2_dir_entry_t **entries_out, int *count_out);

/* Counts the entries in the directory whose header is `dir_header`,
   without storing them. */
ods2_result_t ods2_count_directory_entries(ods2_volume_t *vol, const uint8_t *dir_header,
                                            int *count_out);

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one. See
   ods2_volume.c for the reasoning - without this, files needing more
   than one header would silently appear truncated. */
ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out);

/* The same, into a list this function allocates (free() it; NULL for a file with no extents): no limit on the number of
   extents or of extension headers but memory and FH2$W_SEG_NUM's 65,535. ods2_decode_all_extents() is the fixed-array form for
   callers that hold only a few (it refuses a file with more than `max_extents`, never shortens the list). */
ods2_result_t ods2_decode_all_extents_alloc(ods2_volume_t *vol, const uint8_t *header,
                                             ods2_extent_t **extents_out, int *count_out);

/* Reads a file's entire logical content (respecting FAT$L_EFBLK/
   FAT$W_FFBYTE - the end-of-file mark - not just its physical
   allocation, which is normally larger) into a caller-allocated
   buffer. Returns the number of bytes actually written into
   `buf_out`, or a failure result if `buf_out` is too small or a
   block couldn't be read. Intended for text/binary file content
   (a TYPE/COPY equivalent), not directories - use
   ods2_list_directory() for those. */
ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out);

/* Returns the number of bytes of logical content in the file whose
   header is `header`, from its end-of-file mark: (EFBLK - 1) * 512 +
   FFBYTE (spec 6.1.5/6.1.6). An EOF on a block boundary may be
   recorded either as EFBLK=n+1/FFBYTE=0 (the preferred form, and
   what VMS itself writes) or as EFBLK=n/FFBYTE=512; both give n*512.
   EFBLK 0 is treated as an empty file. This is exactly how many bytes
   ods2_read_file() returns, so callers can size its buffer to fit
   rather than guess. It is not checked against the allocation here;
   a corrupt header can claim more than the file's HIBLK blocks
   (ods2_read_file() rejects that), so callers allocating from it
   should check against HIBLK * 512 first - see
   ods2_file_allocated_bytes(). */
uint64_t ods2_file_content_length(const uint8_t *header);

/* Returns the number of bytes the file's allocation can hold:
   FAT$L_HIBLK * 512. */
uint64_t ods2_file_allocated_bytes(const uint8_t *header);

/* Finds a free file number for a new file, and the file sequence
   number to give it, following spec 5.1.6/5.1.7: the number comes from
   the index file bitmap (searching start_from up to start_from +
   max_search - 1, within MAXFILES); its header slot, if inside
   INDEXF.SYS's end-of-file, is then checked - a valid header there is
   never reused even when the bitmap says free, a deleted one gives
   (old seq)+1, anything else gives 1. A slot beyond the end-of-file
   always gives 1. Changes nothing on disk: ods2_create_file() and
   ods2_create_directory() also move INDEXF.SYS's end-of-file past the
   new header (extending INDEXF.SYS if needed) and mark the bitmap. */
ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out);

/* Writes virtual block `vbn` (1-based) of the file described by
   `header` from `block_in` (512 bytes), by decoding the header's own
   Map Area (walking ext_fid segments) and finding the matching LBN.
   The volume must be mounted for write. */
ods2_result_t ods2_write_file_block(ods2_volume_t *vol, const uint8_t *header,
                                     unsigned vbn, const uint8_t *block_in);

/* Allocates `blocks_needed` blocks from BITMAP.SYS's own storage
   bitmap (spec 5.2), rounding up to the volume's cluster factor (the
   bitmap tracks clusters, not individual blocks - confirmed by the
   old project's own working allocation code). On success, *lbn_out
   is the starting LBN and *blocks_allocated_out is the actual number
   of blocks reserved (may be more than requested, due to cluster
   rounding - callers should use this actual count, not their
   original request, when building extents/hiblk). The volume must be
   mounted for write. */
ods2_result_t ods2_allocate_blocks(ods2_volume_t *vol, unsigned blocks_needed,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out);

/* Like ods2_allocate_blocks(), but when no free run of `max_blocks`
   (rounded up to whole clusters) exists, allocates the longest free
   run there is instead, so a large file can be laid down in as few
   pieces as possible. *blocks_allocated_out says how much was
   reserved - possibly less than asked for; the caller allocates again
   for the rest. Fails only when the volume has no free cluster. */
ods2_result_t ods2_allocate_extent(ods2_volume_t *vol, unsigned max_blocks,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out);

/* Frees `block_count` blocks starting at `lbn` back to BITMAP.SYS's
 * storage bitmap - the exact inverse of ods2_allocate_blocks(). Marks
 * the corresponding clusters free (set, per spec 5.2.2); `lbn` and
 * `block_count` must be cluster-aligned (i.e. exactly what an earlier
 * ods2_allocate_blocks() call returned - this does not check
 * alignment itself, freeing a non-cluster-aligned range would corrupt
 * the bitmap by marking part of an adjacent cluster free too early).
 * The volume must be mounted for write.
 */
ods2_result_t ods2_free_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned block_count);

/* Appends one extent (`block_count` blocks at `lbn`) to INDEXF.SYS's
 * own header (file 1) in memory: encodes it as the smallest retrieval
 * pointer that fits - Format 1, or Format 2/3 above LBN 4,194,303 -
 * after the pointers already in use, and raises HIBLK (and HIGHWATER,
 * when set) by the same count. Writes nothing to disk and takes no
 * blocks; the caller has allocated them and writes the header. Fails,
 * changing nothing, when the header's map area has no room left for
 * the pointer. Used when INDEXF.SYS grows to hold more headers.
 */
ods2_result_t ods2_indexf_map_extent(uint8_t *indexf_header, uint32_t lbn, unsigned block_count);

/* Directories between [000000] and the directory with this header (0 for [000000] itself). */
ods2_result_t ods2_directory_depth(ods2_volume_t *vol, const uint8_t *dir_header, unsigned *depth_out);

/* Creates a new, empty subdirectory named `name` (e.g. "NETLIB020",
 * without ".DIR" - that's appended internally, matching VMS
 * convention) within the directory described by `parent_header`.
 * Allocates a file number, allocates content space, builds and writes
 * the new directory's own header, initializes its content block with
 * just the sentinel record, and inserts an entry for it into the
 * parent's content (via ods2_insert_into_directory() - the parent
 * grows automatically if none of its existing content blocks have
 * room). Fails (without leaving a half-created directory on disk,
 * per-step) if the name already exists in the parent, or if no free
 * file number or disk space is available. The volume must be mounted
 * for write. On success, *new_fid_out is the newly created
 * directory's FID.
 */
ods2_result_t ods2_create_directory(ods2_volume_t *vol, const uint8_t *parent_header,
                                     const char *name, ods2_fid_t *new_fid_out);

/* Creates a new file named `name` (e.g. "README.TXT") within the
 * directory described by `parent_header`, writing `content_len` bytes
 * from `content` as its data. `rtype` should typically be 5
 * (FAB$C_STMLF, stream-LF - the common convention for text files) or
 * 1 (FAB$C_FIX, fixed-length records) for binary data laid out in
 * uniform-size chunks; ods2_header_build.h's ods2_header_spec_t has
 * the full field if finer control is needed via a lower-level call.
 *
 * Any size of file the volume has room for: the content is allocated
 * in as few, as large pieces as the free space allows (one piece on a
 * volume with a long enough free run), each described by the smallest
 * retrieval pointer that fits it (Format 1, 2 or 3). Pointers that
 * don't fit in the primary header's map area continue in extension
 * headers, chained through FH2$W_EXT_FID, so even a file spread over
 * fragmented free space is stored whole. A file that would need more
 * than ODS2_MAX_EXTENTS pieces, or more space than is free, fails
 * cleanly and gives back everything it had allocated. Same
 * parent-has-no-room-so-it-grows and duplicate-name behavior as
 * ods2_create_directory().
 * The volume must be mounted for write.
 */
ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out);

/* Creates a symbolic link `name` in the directory `parent_header`, as VMS
 * V8.4's CREATE/SYMLINK does: a special file (FAT file organization 4,
 * special type 5) whose data is `target`, ISO Latin-1 text (UTF-8 here, no
 * character above U+00FF, at most 1024 bytes), from VBN 1, EOF = its length;
 * the directory entry carries the DIR$V_SPECIAL flag and the access date is
 * left unset. ODS-5 only, and not on a volume with HM2$M_NO_SPECIAL_FILES.
 * The name rules and the duplicate check are those of ods2_create_file(). */
ods2_result_t ods2_create_symlink(ods2_volume_t *vol, const uint8_t *parent_header,
                                  const char *name, const char *target, ods2_fid_t *new_fid_out);

/* Makes another directory entry `name` (version 1) in the directory
 * `dir_file_number` for the existing file `target`, as VMS's SET FILE/ENTER
 * does on a volume with the HARDLINKS characteristic, and raises the file's
 * link count (FH2$W_LINKCOUNT) and attribute change date. Refused on any
 * other volume (there the extra entries are mere aliases with no link count),
 * for a directory, and for a file that has the maximum number of links. */
ods2_result_t ods2_create_link(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                               ods2_fid_t target);

/* True if `header` is the header of the file `fid` names: the same file number and sequence number, and
 * not marked deleted. A directory entry whose file was deleted through another name (an alias entry
 * on a volume without HARDLINKS; VMS keeps no count there) is dangling, and its slot may since have
 * been reused by another file: VMS then says "no such file" (measured, ODS5_FORMAT_NOTES 15.7). */
bool ods2_header_is_file(const uint8_t *header, ods2_fid_t fid);

/* SIMH disk container footer: simh/simh appends 512 bytes ("simh" signature, then simulator, drive type, sizes, a CRC)
   to a raw disk container it opens for write. 0: none. 1: the image is the volume plus the footer (the footer is not
   volume data and is never read or written). 2: the footer lies inside the volume (on an image of 4 GiB or more a
   32-bit overflow puts it into the last data block): the last block may be lost, and the volume is not mounted for
   write. msg describes it. */
int ods2_image_footer(ods2_volume_t *vol, char *msg, size_t cap);

/* True, with a message, when the image file has fewer blocks than the volume's storage control block says it has (SCB$L_VOLSIZE):
 * a truncated copy, or a sparse image that was not copied whole. Reads of the missing part come back as errors; a write mount
 * of such an image is refused (a write past the end would extend the file silently, and the image would then be a different
 * size than the volume it holds). The volume can be read. False when the size is not known (VOLSIZE 0), when the block that
 * should be the storage control block is not one (the placeholder of the starter volume: it stays writable) or when it fits. */
bool ods2_image_short(ods2_volume_t *vol, char *msg, size_t cap);

/* True when INDEXF.SYS's last growth was interrupted between its writes: its map holds more blocks than FH2$L_HIBLK says, or a
   file number is marked in use beyond what the map covers. Nothing is lost; the first change made through a write mount
   repairs it (one header write, and the stray bit), and `ods5check` reports it until then (review 28 a). */
bool ods2_indexf_growth_unfinished(ods2_volume_t *vol);

/* True when INDEXF.SYS has an extension header that looks like one ods2v2 wrote (e0bad43 and earlier test builds): the map at word
   100 on an ODS-2 volume (VMS-made ones have it at word 50), or a level 5 header behind a level 2.1 primary. OpenVMS V8.4
   crashes (BUGCHECK UNXSIGNAL, CORPUS 37/38) on the first file creation on such a volume; reading is safe. A heuristic: no
   VMS-made extension header of INDEXF.SYS has been seen, and if VMS made one like that this is wrong. */
bool ods2_indexf_ext_looks_ods2v2(ods2_volume_t *vol);

/* True, with a message in msg, when the storage control block (BITMAP.SYS VBN 1) is one OpenVMS ANALYZE would
   call invalid (%ANALDISK-W-CHKSCB); read only. */
bool ods2_scb_problem(ods2_volume_t *vol, char *msg, size_t cap);

/* RMS record attributes for a new file (FH2$W_RECATTR, spec 3.5.2.9).
   Record formats (FAT$B_RTYPE, sequential organization): */
#define ODS2_RFM_UDF   0 /* FAB$C_UDF: undefined - a stream of bytes */
#define ODS2_RFM_FIX   1 /* FAB$C_FIX: fixed-length records */
#define ODS2_RFM_VAR   2 /* FAB$C_VAR: variable-length (only as converted
                            records - see ods2_text_var.h) */
#define ODS2_RFM_VFC   3 /* FAB$C_VFC: variable with control (not importable) */
#define ODS2_RFM_STM   4 /* FAB$C_STM: stream, CR-LF terminated */
#define ODS2_RFM_STMLF 5 /* FAB$C_STMLF: stream, LF terminated */
#define ODS2_RFM_STMCR 6 /* FAB$C_STMCR: stream, CR terminated */
/* Record attributes (FAT$B_RATTRIB), combinable as bits: */
#define ODS2_RAT_FTN   0x01 /* FAB$M_FTN: FORTRAN carriage control */
#define ODS2_RAT_CR    0x02 /* FAB$M_CR: implied carriage return */
#define ODS2_RAT_PRN   0x04 /* FAB$M_PRN: print-file carriage control */
#define ODS2_RAT_BLK   0x08 /* FAB$M_BLK: records don't span blocks */

typedef struct {
    uint8_t  rtype;   /* ODS2_RFM_* */
    uint8_t  rattrib; /* ODS2_RAT_* bits; at most one of FTN/CR/PRN */
    uint16_t rsize;   /* FAT$W_RSIZE: the record size of a fixed file,
                         otherwise the longest record (LRL) */
    uint16_t maxrec;  /* FAT$W_MAXREC (MRS): maximum record size; for a
                         fixed file always equal to rsize, as VMS sets
                         it (e.g. INDEXF.SYS: FIX, RSIZE = MRS = 512) */
} ods2_record_attrs_t;

/* Checks `attrs` as the record attributes of a new file holding
 * `content_len` bytes taken from the host as they are. Returns NULL if
 * they're acceptable, else why not:
 *  - only UDF, FIX and the stream formats: VAR and VFC files carry a
 *    count word before each record on disk, which host bytes don't;
 *  - a fixed record size is 2..32766 bytes and even (RMS pads an odd
 *    fixed record to a word boundary on disk - not supported), MRS
 *    equals it, and the content is a whole number of records;
 *  - other sizes are at most 32767; unknown RAT bits, or more than one
 *    carriage control, are refused. */
const char *ods2_check_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len);

/* Checks `attrs` for a new file whose `content_len` bytes are already
 * variable-length records (issue #15). Returns NULL if acceptable,
 * else why not: the format must be VAR; the longest record (RSIZE) at
 * most 32767; MRS 0 (no limit, as VMS FTP leaves it) or at least the
 * longest record; the content a whole number of words; not RAT:BLK
 * (converted records run on across blocks, which BLK forbids); at most
 * one carriage control; no unknown RAT bits. */
const char *ods2_check_var_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len);

/* Is `rtype` a stream format (STM, STMLF, STMCR)? And the longest record of `len` bytes of stream content in that format:
 * the LRL such a file is made with (MRS is 0). */
bool ods2_stream_format(uint8_t rtype);
size_t ods2_longest_record(uint8_t rtype, const uint8_t *data, size_t len);

/* The record length rules of stream files as RMS applies them (see ods2_reclen_feed in ods2_volume.c): the longest record of
 * content fed in order, in any pieces. */
typedef struct {
    uint8_t rtype;
    size_t cur, longest;
    bool prev_cr;
} ods2_reclen_t;
void ods2_reclen_init(ods2_reclen_t *s, uint8_t rtype);
void ods2_reclen_feed(ods2_reclen_t *s, const uint8_t *data, size_t len);
size_t ods2_reclen_longest(const ods2_reclen_t *s);

/* ods2_create_file() with chosen record attributes, checked first by
 * ods2_check_record_attrs() - a file whose attributes don't fit its
 * content is refused, not created: so is a stream file with a record longer than its MRS (when not 0) or its LRL; an LRL of 0
 * for a stream file becomes its longest record (for a UDF file with MRS 0, its length). ods2_create_file(..., rtype, ...) is
 * this with rattrib 0 and: FIX - RSIZE = MRS = 512, unchecked, as before; stream (STM, STMLF, STMCR) - MRS 0 and RSIZE the
 * longest record; UDF - MRS 0 and RSIZE the length.
 */
ods2_result_t ods2_create_file_attrs(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, const uint8_t *content,
                                      size_t content_len, const ods2_record_attrs_t *attrs,
                                      ods2_fid_t *new_fid_out);

/* Content for a new file, read in order rather than held in memory
 * (issue #16): `length` bytes in all, known before anything is
 * allocated, delivered by `read`, which fills exactly `n` bytes of
 * `buf` with the next part of the content and returns true, or returns
 * false if it can't (a read error, or a source shorter than `length`).
 * `read` is called with chunks of at most ODS2_STREAM_CHUNK bytes, in
 * order, `length` bytes in total. `problem`, if `read` sets it, says
 * why it failed; otherwise the failure is reported generically. */
#define ODS2_STREAM_CHUNK (1u << 20)

typedef struct {
    uint64_t length;   /* bytes in all (a file may exceed what size_t holds on a 32-bit host) */
    bool (*read)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
    const char *problem;
    /* true when the content is already RMS variable-length records,
       count words included (ods2_text_var.h makes them from host
       text): RFM:VAR is then accepted, and checked by
       ods2_check_var_record_attrs() instead. */
    bool var_records;
    /* a fixed-length format whose content is not a whole number of records is accepted: the end-of-file mark
       stays exact and the last record is short (as VMS binary FTP makes FIX 512 files) */
    bool partial_fix_ok;
} ods2_content_source_t;

/* ods2_create_file_attrs() with the content taken from `src` a chunk
 * at a time, so memory use stays the same whatever the file's size.
 * The record attributes are used AS GIVEN: the library cannot scan a source it reads once, so for a stream file the caller
 * supplies the LRL (the longest record) and MRS (0 unless wanted), and no check is made that no record is longer than
 * either (ods2_create_file_attrs(), which has the content in memory, computes the LRL and refuses such a file; the CLI
 * scans the host file first). A stream file made here with MRS 512 and a longer line cannot be read by TYPE, COPY or CHECKSUM
 * on VMS (%RMS-W-RTB).
 * Everything is allocated (from src->length) before the first read;
 * if `src` fails part way, everything is given back and no file is
 * created. */
ods2_result_t ods2_create_file_source(ods2_volume_t *vol, const uint8_t *parent_header,
                                       const char *name, ods2_content_source_t *src,
                                       const ods2_record_attrs_t *attrs,
                                       ods2_fid_t *new_fid_out);

/* ods2_create_file_source() with a version. version_req: ODS2_NEWVER_REFUSE (-1) is ods2_create_file_source(): the name must not exist, the
 * file is version 1. 0 makes the next version of a name that exists (highest + 1; version 1 if the name is new); n (1 to 32767) makes that
 * version if it is free and fails with %RMS-E-FEX if it exists. The name is matched as VMS does (on ODS-5 ignoring case) and an existing
 * name keeps the case first stored. A version above 32767 (a plain create after ;32767 too) fails with %RMS-E-CRE and %SYSTEM-W-BADFILEVER.
 * The entry's version limit is enforced as part of the create, in VMS's order: the new file is made from space and a header slot that were free
 * before the change began, then the oldest version is removed, in the same all-or-nothing change; a name with more versions than its limit
 * is NOT refused: exactly one version, the lowest, goes (CORPUS 49.9). The new version takes the owner and protection of the highest one.
 * *version_out (if not NULL) gets the version made, name_out (if not NULL; at least ODS5_MAX_LITERAL_NAME + 8 bytes) the name as stored
 * (UTF-8; on an existing name the case first stored). Everything else is as ods2_create_file_source(). */
#define ODS2_NEWVER_REFUSE (-1)
ods2_result_t ods2_create_file_source_version(ods2_volume_t *vol, const uint8_t *parent_header,
                                               const char *name, int version_req, ods2_content_source_t *src,
                                               const ods2_record_attrs_t *attrs,
                                               ods2_fid_t *new_fid_out, uint16_t *version_out, char *name_out);

/* Reads a file's logical content (as ods2_read_file()) and hands it to
 * `sink` in order, a chunk of at most ODS2_STREAM_CHUNK bytes at a
 * time, so memory use stays the same whatever the file's size. If
 * `sink` returns false the read stops and fails with *sink_failed_out
 * (if non-NULL) set true, telling that apart from a volume read error.
 * *bytes_out (if non-NULL) is how much reached the sink. */
ods2_result_t ods2_read_file_stream(ods2_volume_t *vol, const uint8_t *header,
                                     bool (*sink)(void *ctx, const uint8_t *data, size_t n),
                                     void *ctx, uint64_t *bytes_out, bool *sink_failed_out);

/* The geometry (sectors x tracks) device type `target` (ld, ra, ra82, ra92, rq, scsi, axpbox, rz) reports for a
 * volume of `blocks` blocks; *measured is false for a size that was not measured (a prediction). False: unknown target. */
bool ods2_target_geometry(const char *target, uint64_t blocks, uint32_t *sectors, uint32_t *tracks, bool *measured);

/* The warning to print when *measured came back false (false, nothing written, when it was true). */
bool ods2_target_geometry_warning(const char *target, uint64_t blocks, uint32_t sectors, uint32_t tracks, bool measured,
                                  char *msg, size_t cap);

/* SET VOLUME/PARTIAL_CLUSTER: gives the blocks past the last whole cluster to BADBLK.SYS as VMS INITIALIZE does (for
 * volumes made by an older ods2_init); *changed says whether anything was written. */
ods2_result_t ods2_assign_partial_cluster(ods2_volume_t *vol, bool *changed);

/* SET VOLUME/GEOMETRY: records the disk geometry `sectors` x `tracks` (cylinders = ceil(VOLSIZE / (sectors * tracks)),
 * BLKSIZE 1) in the storage control block, the geometry of the device the volume is to be used on. */
ods2_result_t ods2_set_volume_geometry(ods2_volume_t *vol, uint32_t sectors, uint32_t tracks);

/* SET FILE/VERSION_LIMIT: sets the version limit of the entries named `name` in the directory (all versions); *changed
 * is the number of directory records changed. For volumes written before ods2v2 gave entries VMS's limit (32767). */
ods2_result_t ods2_set_entry_version_limit(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                           uint16_t limit, unsigned *changed);

/* SET FILE/ATTRIBUTES=(MRS:0): a sequential Stream, Stream_LF or Stream_CR file with a maximum record size (MRS) gets MRS 0
 * and its own longest record as LRL, as VMS makes such files (RMS refuses to read one with a longer line: %RMS-W-RTB).
 * Also a file whose LRL is below its longest record (COPY and CHECKSUM fail on it even with MRS 0). Only the file's header
 * changes; *changed is 1 if it did, 0 if it needed nothing (another format, MRS 0 and LRL enough). */
ods2_result_t ods2_set_stream_record_sizes(ods2_volume_t *vol, ods2_fid_t fid, unsigned *changed);

/* SET FILE/PROTECTION and /OWNER_UIC for the file `fid` (CORPUS 49.4): `what` is ODS2_SETSEC_PROT and/or ODS2_SETSEC_OWNER. `prot` is the whole FH2$W_FILEPROT
 * word (a set bit denies: R 1, W 2, E 4, D 8 for system in bits 0-3, owner 4-7, group 8-11, world 12-15); the owner is [group,member] (numbers, not names).
 * The primary header is rewritten in one block; the revision number goes up by 1 on every success, the revision date (level 2 header) or the attribute
 * change date (level 5 header) is the time of the change; the checksum is made again. Refused: the owner of a reserved file. */
#define ODS2_SETSEC_PROT  1u
#define ODS2_SETSEC_OWNER 2u
ods2_result_t ods2_set_file_security(ods2_volume_t *vol, ods2_fid_t fid, unsigned what, uint16_t prot, uint16_t group, uint16_t member);

/* Inserts (name, version, fid) into the directory whose header is at
 * `dir_file_number`, trying each of its existing content blocks
 * first; if none have room, allocates a new block, extends the
 * directory's own header with an additional extent (modified in
 * place - preserves the original creation date, only the revision
 * date/count are updated, unlike rebuilding the header from scratch),
 * and inserts there. Shared by ods2_create_directory() and
 * ods2_create_file() - both need identical "insert into a directory,
 * growing it if necessary" behavior. The volume must be mounted for
 * write. Fails if the directory would need a second extension header
 * (more than roughly 77 extents) - not yet implemented.
 */
ods2_result_t ods2_insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                          const char *name, uint16_t version, ods2_fid_t fid);

/* Deletes `name` from the directory at `dir_file_number`: looks it
 * up, refuses if it's a non-empty directory (matching VMS's own
 * DELETE/DIRECTORY behavior - avoids silently orphaning its
 * contents), frees all of its content blocks back to BITMAP.SYS
 * (walking every extent across any extension headers), frees its
 * file number in the index file's own bitmap, marks its header
 * deleted per spec 3.5.1's exact rules (FH2$V_MARKDEL set,
 * FID_NUM/NMX/RVN zeroed - but NOT fid_seq, which must survive for
 * spec 5.1.7's "next use gets seq+1" rule - and FH2$W_CHECKSUM set to
 * zero), and removes its directory entry from the parent. The volume
 * must be mounted for write. Each step fails cleanly with a specific
 * error rather than leaving partial, inconsistent state where
 * possible, though - like every operation here - a crash mid-
 * operation could still leave the volume in a state needing repair;
 * this is not a transactional filesystem.
 */
ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name);

/* Same as ods2_delete(), for one version of `name` selected by
 * `version` (see ODS2_VERSION_HIGHEST). Only that version's
 * (version, FID) entry is removed from the directory; the name's
 * other versions stay listed and readable. ods2_delete() is this
 * with ODS2_VERSION_HIGHEST. If `version_out` is non-NULL it receives
 * the actual version number deleted.
 */
ods2_result_t ods2_delete_version(ods2_volume_t *vol, unsigned dir_file_number,
                                  const char *name, int version, uint16_t *version_out);

/* What ods2_delete_version_info() did besides the usual: on a volume without HARDLINKS an entry that is not the
 * file's primary one (an alias) is removed and the file stays (kept = 1); an entry whose file no longer exists
 * (a dangling alias) is removed alone (kept = 2). Otherwise kept = 0. For kept = 1: `name_differs` says whether
 * the header names another file name (primary_name, primary_version) or only another back link directory
 * (backlink_dir, a file number). */
typedef struct {
    int      kept;
    bool     link_removed;    /* a HARDLINKS volume: one link of a file with others was removed, the file stays */
    bool     dir_link;        /* ... and the file is a directory (an empty one: its count was above 1: if nothing else enters it, it is now in no directory) */
    bool     no_entry_left;   /* the entry removed was not the one the header names, and the directory the header's back link names has no entry for the file now (a RENAME stopped after its header write, CORPUS 69): the file stays allocated in NO directory (VMS: ANALYZE says LOSTHEADER, /REPAIR puts it in SYSLOST) */
    bool     name_differs;
    char     primary_name[ODS5_MAX_LITERAL_NAME + 8];
    uint16_t primary_version;
    unsigned backlink_dir;
} ods2_delete_info_t;
ods2_result_t ods2_delete_version_info(ods2_volume_t *vol, unsigned dir_file_number,
                                       const char *name, int version, uint16_t *version_out,
                                       ods2_delete_info_t *info);


/* --- the volume label (issue #13) --- */

/* Checks `label` as a volume label and writes it to `out` in its
 * on-disk form: uppercased and space-padded to 12 bytes (HM2$T_VOLNAME,
 * spec 5.1.9.36). Returns NULL if acceptable, else why not: it may not
 * be empty (spec 2.1) or over 12 characters, and - keeping to what the
 * spec recommends and what a DISK$label logical name can hold - only
 * letters, digits, '$', '_' and '-' are allowed. */
const char *ods2_make_volume_label(const char *label, char out[12]);

/* Relabels the volume, as VMS's SET VOLUME/LABEL does, for a volume
 * VMS does not have mounted. Every copy of the home block is changed:
 * the home block and its cluster fillers (INDEXF.SYS VBNs 2 to 2v) and
 * the backup home block's cluster (VBNs 2v+1 to 3v), v the cluster
 * factor (spec 5.1.3-5.1.4). Each keeps its own HOMELBN and HOMEVBN,
 * and both its checksums are recomputed.
 *
 * Nothing is written unless every one of those blocks is first found
 * to be a valid home block naming its own position and otherwise
 * identical to the primary, with the primary and backup among them.
 * Refused for a volume in a volume set (a structure name, a relative
 * volume number or a set count), since one member can't be relabelled
 * alone. The primary is written last and every copy read back.
 * *copies_out (if non-NULL) is how many blocks were rewritten. */
ods2_result_t ods2_set_volume_label(ods2_volume_t *vol, const char *label,
                                     unsigned *copies_out);

/* The volume's size in blocks (SCB$L_VOLSIZE, from BITMAP.SYS's
 * storage control block) and how many of them are free (free clusters
 * in the storage bitmap times the cluster factor). Read only. */
ods2_result_t ods2_volume_space(ods2_volume_t *vol, uint64_t *total_blocks_out,
                                 uint64_t *free_blocks_out);

#endif /* ODS2_VOLUME_H */
