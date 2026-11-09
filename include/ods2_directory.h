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

/* ods2_directory.h - parses directory file content (spec 4.2/4.3)
 * into a simple list of (name, version, fid) entries.
 */
#ifndef ODS2_DIRECTORY_H
#define ODS2_DIRECTORY_H

#include "ods2_ondisk.h"
#include "ods5_names.h"
#include <stddef.h>

/* Longest name an entry holds, in bytes. ODS-2 names are at most 80;
   on an ODS-5 volume names are held as UTF-8 (see ods5_names.h), which
   needs up to ODS5_MAX_LITERAL_NAME. */
#define ODS2_DIR_MAX_NAME ODS5_MAX_LITERAL_NAME

typedef struct {
    char       name[ODS2_DIR_MAX_NAME + 1]; /* nul-terminated; UTF-8 on an ODS-5 volume */
    uint16_t   version;
    ods2_fid_t fid;
    uint8_t    flags;      /* DIR$B_FLAGS of the record (0 on ODS-2) */
    uint8_t    name_type;  /* ODS5_NAME_ODS2/ISL1/UCS2 as stored (ODS-5 only; else 0) */
} ods2_dir_entry_t;

/* Upper bound on how many ENTRIES one 512-byte directory block can
 * physically hold. An entry is one (name, version, FID) triple, and a
 * directory record is one name followed by one or more 8-byte version
 * entries (spec 4.2/4.3, newest version first). The densest possible
 * block is a single record with an empty name: 6-byte record header
 * plus 8 bytes per version, so at most (512-6)/8 = 63 entries. Every
 * additional record only adds header bytes, so no block can hold
 * more. Callers that parse block by block use this to size a
 * per-block buffer that never truncates. */
#define ODS2_DIR_MAX_ENTRIES_PER_BLOCK 63

/* Highest file version ODS-2 allows (versions are 1..32767). */
#define ODS2_MAX_VERSION 32767

/* Parses directory records starting at `data` (raw block bytes) for
 * up to `len` bytes, writing entries into `entries_out` (up to
 * `max_entries`). Stops at the first sentinel record (dir$size ==
 * 0xffff) or when `len` is exhausted.
 *
 * Produces ONE ENTRY PER VERSION, not one per record: a record holds
 * a name followed by every (version, FID) pair that fits, newest
 * first, and each pair becomes its own entry carrying the record's
 * name. (An earlier version read only the first pair of each record,
 * so a file with many versions showed up once per record - e.g. a
 * 64-version file split across two records listed as 2 entries.)
 * Entries come out in on-disk order: records sorted by name, and
 * versions within a name newest first.
 *
 * Returns the TOTAL number of entries found in the block - which can
 * be larger than `max_entries`. Only the first `max_entries` are
 * stored; a return value greater than `max_entries` is how a caller
 * knows entries were dropped.
 *
 * Returns -1 if a record's declared size would run past `len`, or a
 * record holds no complete version entry (a genuinely malformed or
 * truncated block - callers should treat this as a real error, not a
 * soft empty-directory case). */
int ods2_parse_directory(const uint8_t *data, size_t len,
                          ods2_dir_entry_t *entries_out, size_t max_entries);

/* The same for a volume of structure `level` (2 or 5). Level 2 is
   exactly ods2_parse_directory(). At level 5 the record flags are
   read: the name type (DIR$V_NAMETYPE) says whether the name bytes are
   ODS-2/ISO Latin-1 (1 byte per character) or UCS-2 (2 bytes, little
   endian), and the name is stored in the entry as UTF-8 (see
   ods5_names.h); a record of type DIR$C_LINKNAME is not a file entry
   and is skipped; a name type this code does not know is read as ISO
   Latin-1. Also -1 for a UCS-2 name of odd
   length or a name too long to hold. */
int ods2_parse_directory_level(const uint8_t *data, size_t len, unsigned level,
                               ods2_dir_entry_t *entries_out, size_t max_entries);

/* The same, but a record that cannot be read (a name too long to hold or of odd
   length in UCS-2, a record too small for its name and a version) is skipped and
   counted in *bad_records, and the records after it are still read; a record whose size
   runs past the buffer ends the block (counted too). Never returns -1. */
int ods2_parse_directory_records(const uint8_t *data, size_t len, unsigned level,
                                 ods2_dir_entry_t *entries_out, size_t max_entries,
                                 unsigned *bad_records);

#endif /* ODS2_DIRECTORY_H */
