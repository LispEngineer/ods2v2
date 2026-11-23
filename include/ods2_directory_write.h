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

/* ods2_directory_write.h - inserts a new (name, version, fid) record
 * into a directory content block. The record format here matches
 * ods2_directory.c's reader exactly - both were built against the
 * same real, parsed root directory data (see ods2_directory_selftest.c).
 */
#ifndef ODS2_DIRECTORY_WRITE_H
#define ODS2_DIRECTORY_WRITE_H

#include "ods2_ondisk.h"
#include <stddef.h>
#include <stdbool.h>

/* Inserts a new directory entry at the first available space in
 * `block` (searching for the sentinel record - dir$size==0xffff - or
 * the end of existing valid records, then writing there and placing a
 * new sentinel immediately after). `name` is copied as given (callers
 * should pass an already-uppercased name; VMS convention, not
 * enforced here since the directory format itself is case-agnostic).
 * Returns false if there isn't room for the new record plus a
 * trailing sentinel within `block_size` bytes - callers must check
 * this and extend the directory (allocate another block) rather than
 * assume success.
 */
bool ods2_insert_dir_entry(uint8_t *block, size_t block_size,
                            const char *name, uint16_t version, ods2_fid_t fid);

/* The same for a volume of structure `level` (2 or 5). Level 2 is
 * exactly ods2_insert_dir_entry(). At level 5 `name` is the UTF-8
 * literal name (case kept; a '.' is added if it has none), stored as
 * ISO Latin-1 or UCS-2 as ods5_stored_name() says, with that name type
 * in the record's flags, a version limit of 32767, and the record put
 * where VMS's ordering (ascending upper-cased code points, ISO Latin-1
 * and UCS-2 names interleaved) places it. Names of existing records
 * are compared in the same way, whatever their name type. */
bool ods2_insert_dir_entry_level(uint8_t *block, size_t block_size,
                                 const char *name, uint16_t version, ods2_fid_t fid,
                                 unsigned level);

/* The same, with `extra_flags` ORed into DIR$B_FLAGS at level 5 (the
 * only one used: ODS5_DIR_SPECIAL, 0x40, which VMS sets on the entry of a
 * symbolic link). Ignored at level 2. */
bool ods2_insert_dir_entry_flags(uint8_t *block, size_t block_size,
                                 const char *name, uint16_t version, ods2_fid_t fid,
                                 unsigned level, uint8_t extra_flags);

/* The same, with the entry's version limit (DIR$W_VERLIMIT): the default version limit of the directory it goes
 * into, or 0 for none (stored as 32767, as VMS does). The functions above pass 0. */
bool ods2_insert_dir_entry_vl(uint8_t *block, size_t block_size,
                              const char *name, uint16_t version, ods2_fid_t fid,
                              unsigned level, uint8_t extra_flags, uint16_t verlimit);

/* Sets the version limit of the records named `name` in `block` (SET FILE/VERSION_LIMIT): the number of records
 * changed, or -1 if the block is malformed (then nothing is changed). */
int ods2_set_dir_entry_verlimit(uint8_t *block, size_t block_size, const char *name, uint16_t limit);

/* Removes just the (`version`, FID) pair of `name` (case-insensitive)
 * from `block`, shrinking that record by 8 bytes and shifting all
 * following bytes (through the sentinel) back to close the gap. If
 * that pair was the record's only version, the whole record is
 * removed instead. Other versions of the same name - in this record
 * or in records in other blocks - are left untouched.
 * Returns true if the pair was found and removed, false if this block
 * has no record for `name` holding `version` (callers checking
 * multiple blocks should treat false as "not in this one, try the
 * next").
 */
bool ods2_remove_dir_version(uint8_t *block, size_t block_size, const char *name,
                             uint16_t version);

/* The same, but only a pair whose FID is `fid` (NULL: any): the entries of an interrupted directory block split
 * are listed twice, exactly alike, and DELETE removes all of them (one removal per call). */
bool ods2_remove_dir_version_fid(uint8_t *block, size_t block_size, const char *name,
                                 uint16_t version, const ods2_fid_t *fid);

/* --- Names (spec 4.2.5) ---
 *
 * A directory record's name is "NAME.TYPE": the dot is always there,
 * even when the type (or name) is empty ("MAKEFILE."), and only
 * letters, digits, '$', '_' and '-' may appear. VMS also limits NAME
 * and TYPE to 39 characters each. */
#define ODS2_NAME_PART_MAX 39

/* Validates a file name for a new file and writes its canonical
   on-disk form to `out`: uppercased, with the dot added if missing
   ("makefile" -> "MAKEFILE."). On failure returns false and sets
   *why to a message naming the problem. */
bool ods2_make_file_name(const char *in, char *out, size_t out_size, const char **why);

/* Validates a directory's own name (the part before ".DIR"): 1-39
   letters, digits, '$', '_' or '-'. Sets *why on failure. */
bool ods2_valid_dir_name(const char *name, const char **why);

/* Compares two names the way a directory is ordered: case-insensitive
   byte order, with a name that has no dot compared as if it ended in
   one - so "MAKEFILE" (as ods2v2 once stored it) and "MAKEFILE." are
   the same name. Returns <0, 0, >0 like strcmp. */
int ods2_dir_name_compare(const char *a, size_t a_len, const char *b, size_t b_len);

/* --- Multi-block directories (spec 4.2) ---
 *
 * Entries are sorted across the whole directory, not just within each
 * block: VMS searches a directory by comparing against each block's
 * names and stops looking once it is past where a name would be, so
 * an entry in the wrong block is never found (TYPE gives RMS-E-FNF,
 * ANALYZE/DISK gives BAD_NAMEORDER). */

/* Is `block` empty (no records before its -1 end marker)? */
bool ods2_dir_block_is_empty(const uint8_t *block);

/* Is every record of `block` (up to its end marker, or the end of the block) well formed and inside the block? */
bool ods2_dir_block_ok(const uint8_t *block, size_t block_size);

/* Which of `count` consecutive 512-byte blocks `name` belongs in: the
   last non-empty block whose first record sorts at or before `name`,
   or block 0 if `name` sorts before them all. */
unsigned ods2_dir_choose_block(const uint8_t *blocks, unsigned count, const char *name);

/* Splits the records of `src` (a directory block of `src_size` bytes,
   which may be larger than 512 - e.g. a full block with one more
   record inserted into a 1024-byte copy) into two 512-byte blocks `a`
   and `b`, keeping their order and dividing them as evenly as
   possible. Each gets its -1 end marker; the rest is zeroed. Returns
   false if they can't be divided into two blocks that fit (fewer than
   two records, or too large). */
bool ods2_dir_split_block(const uint8_t *src, size_t src_size, uint8_t *a, uint8_t *b);

/* A new record holding the one version `version` at the record boundary `pos` of the block (or at its end). false: no room in
   this block, or `pos` is not a boundary. */
bool ods2_dir_insert_record_at(uint8_t *block, size_t block_size, size_t pos,
                               const char *name, uint16_t version, ods2_fid_t fid,
                               unsigned level, uint8_t extra_flags, uint16_t verlimit);

/* One record of a name in a block (a name's versions may be spread over several records, newest first). */
typedef struct {
    size_t   pos, total;       /* where it starts, its bytes with the size word */
    size_t   first_pair;       /* where its first (version, file id) pair starts */
    unsigned nentries;         /* pairs in it */
    uint16_t first, last;      /* its highest and lowest version */
    uint16_t verlimit;         /* DIR$W_VERLIMIT of the record */
    uint8_t  flags;            /* DIR$B_FLAGS (the name type at level 5) */
} ods2_dir_namerec_t;

/* The records of `name` (case-insensitive as VMS compares) in one block, in order: how many (more than `max` are counted, not
   stored), or -1 if a record is malformed. */
int ods2_dir_name_records(const uint8_t *block, size_t block_size, const char *name,
                          ods2_dir_namerec_t *out, int max);

/* Replaces the name bytes of the record at `rec_pos` with `name` (RENAME that changes only the case, ODS-5: the record keeps its place, its versions and its flags). Only when the new name
   takes the same number of bytes and the same name type as the old one; false, nothing changed, otherwise. */
bool ods2_dir_record_set_name_same_length(uint8_t *block, size_t block_size, size_t rec_pos, const char *name, unsigned level);

/* Adds the pair (version, fid) to the record at `rec_pos` as its pair number `index` (0 = in front: the new highest version).
   1: done. 0: the block has no room (split it). -1: the record cannot grow (a record is at most 510 bytes): add a record.
   -2: the record or the block is malformed, or the index is beyond the record's pairs: nothing changed. */
int ods2_dir_record_add_version(uint8_t *block, size_t block_size, size_t rec_pos, unsigned index,
                                uint16_t version, ods2_fid_t fid);

#endif /* ODS2_DIRECTORY_WRITE_H */
