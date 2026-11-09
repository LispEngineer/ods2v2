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

#include "ods2_header_build.h"
#include "ods2_checksum.h"
#include "ods2_time.h"
#include "ods5_names.h"
#include "ods5_ondisk.h"
#include <string.h>

/* Header layout matching real VMS exactly, confirmed against two
   independently-examined real headers (root's own, INDEXF.SYS's own):
   fixed core fields end at byte 80 (idoffset), then a FIXED 120-byte
   Ident Area (space-padded, holding the file's own "NAME.TYPE;VER"
   string) through byte 200 (mpoffset), then the Map Area.

   An earlier version of this code used an empty (zero-length) Ident
   Area, reasoning it wasn't required for structural correctness.
   That held for a regular file (a real file written and read back
   with TYPE worked fine) but not for a directory - ANALYZE/DISK's
   BAD_DIRHEADER finding on a real, created-with-this-code directory
   pointed at exactly this difference from every real header examined.
   Populating it properly, matching real VMS's fixed-size convention
   rather than sizing it to the actual name length. */
#define ODS2_HEADER_IDOFFSET_BYTES 80
#define ODS2_HEADER_IDENT_BYTES 120
#define ODS2_HEADER_MPOFFSET_BYTES (ODS2_HEADER_IDOFFSET_BYTES + ODS2_HEADER_IDENT_BYTES)

int ods2_header_extents_that_fit_at(const ods2_extent_t *extents, int count,
                                    size_t map_offset_bytes)
{
    size_t used = 0;
    int i;
    for (i = 0; i < count; i++) {
        size_t n = ods2_retrieval_pointer_size(extents[i].lbn, extents[i].block_count);
        if (n == 0) return -1;
        if (map_offset_bytes + used + n > 510) break;
        used += n;
    }
    return i;
}

int ods2_header_extents_that_fit(const ods2_extent_t *extents, int count)
{
    return ods2_header_extents_that_fit_at(extents, count, ODS2_HEADER_MPOFFSET_BYTES);
}

/* A level 5 header's name field, as stored: the literal name with its
   ";version" (ISO Latin-1, or UCS-2 units if any character is above
   U+00FF), and the name type of the name without the version. */
static bool level5_name(const char *ident_name, uint8_t *out, size_t cap, size_t *len,
                        unsigned *type)
{
    char bare[ODS5_MAX_LITERAL_NAME + 2];
    uint8_t unused[2 * ODS5_MAX_LITERAL_NAME + 4];
    size_t unused_len;
    const char *semi = strrchr(ident_name, ';');
    size_t n = semi != NULL ? (size_t) (semi - ident_name) : strlen(ident_name);
    bool ucs2;

    if (n >= sizeof(bare)) return false;
    memcpy(bare, ident_name, n);
    bare[n] = '\0';
    if (!ods5_stored_name(bare, unused, sizeof(unused), &unused_len, type)) return false;
    if (!ods5_utf8_to_stored(ident_name, out, cap, len, &ucs2)) return false;
    return !ucs2 || *type == ODS5_NAME_UCS2;
}

size_t ods2_header_map_offset(const ods2_header_spec_t *spec)
{
    uint8_t name[2 * ODS5_MAX_LITERAL_NAME + 4];
    size_t len, ident;
    unsigned type;

    if (spec->level != ODS5_LEVEL_5) return ODS2_HEADER_MPOFFSET_BYTES;
    if (spec->seg_num != 0) return ODS2_HEADER_IDOFFSET_BYTES + 20;
    if (spec->ident_name == NULL ||
        !level5_name(spec->ident_name, name, sizeof(name), &len, &type) ||
        len > 255) {
        len = 0;
    }
    ident = ODS5_FI5_FILENAME + len;
    if (ident < ODS5_FI5_MIN_LENGTH) ident = ODS5_FI5_MIN_LENGTH;
    ident += ident % 2;
    return ODS2_HEADER_IDOFFSET_BYTES + ident;
}

bool ods2_build_file_header(uint8_t *header_out, const ods2_header_spec_t *spec)
{
    ods2_head_core_t *core;
    size_t map_offset_bytes = ods2_header_map_offset(spec);
    size_t map_bytes = 0;
    int i;
    uint16_t checksum;
    bool level5 = spec->level == ODS5_LEVEL_5;

    if (spec->extent_count < 0 ||
        ods2_header_extents_that_fit_at(spec->extents, spec->extent_count,
                                        map_offset_bytes) != spec->extent_count) {
        return false; /* unrepresentable, or no room before the checksum word */
    }
    for (i = 0; i < spec->extent_count; i++) {
        map_bytes += ods2_retrieval_pointer_size(spec->extents[i].lbn,
                                                 spec->extents[i].block_count);
    }

    memset(header_out, 0, 512);
    core = (ods2_head_core_t *) header_out;

    core->idoffset = ODS2_HEADER_IDOFFSET_BYTES / 2;
    core->mpoffset = (uint8_t) (map_offset_bytes / 2);
    core->acoffset = 0xff; /* no ACL area */
    core->rsoffset = 0xff; /* no reserved area */
    core->seg_num = spec->seg_num;
    core->struclev = level5 ? 0x0501 : 0x0201; /* level, version 1 - confirmed convention */
    core->fid = spec->fid;
    core->ext_fid = spec->ext_fid; /* all zero: no extension header */
    core->recattr.rtype = spec->rtype;
    core->recattr.rattrib = spec->rattrib;
    core->recattr.rsize = spec->rsize;
    core->recattr.hiblk = ods2_word_swap32(spec->hiblk);
    core->recattr.efblk = ods2_word_swap32(spec->efblk);
    core->recattr.ffbyte = spec->ffbyte;
    core->recattr.maxrec = spec->maxrec;
    core->recattr.versions = spec->versions;
    core->filechar = spec->filechar;
    core->map_inuse = (uint8_t) (map_bytes / 2);
    /* FH2$L_FILEOWNER, FH2$W_FILEPROT, FH2$W_RECPROT: confirmed via
       ods2_dump_header against real, VMS-written headers that real
       files always carry non-zero values here - fileowner_uic=(4,1),
       fileprot=0xfa00, recprot=0xfe00 (root itself shows a slightly
       different fileprot=0xba00, likely just a different default
       protection class for directories vs. plain files - using the
       more common 0xfa00 value here, seen on both INDEXF.SYS and our
       own file 11). */
    core->fileowner_uic[0] = 4;
    core->fileowner_uic[1] = 1;
    core->fileprot = 0xfa00;
    core->recprot = 0xfe00;
    if (level5) {
        /* What VMS V8.4 writes for a file made on a level 5 volume
           (headers dumped from the ODS5T corpus image): the record
           protection word is zero, a directory is not deletable by
           group or world (S:RWE,O:RWE,G:RE,W:E); the link count (the word at
           byte 74) is 1 on a volume with HARDLINKS and 0 on one without. */
        if (spec->filechar & 0x2000u) core->fileprot = 0xba88;
        core->recprot = 0;
        header_out[ODS5_FH2_LINKCOUNT] = (uint8_t) (spec->link_count & 0xff);
        header_out[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) (spec->link_count >> 8);
    }
    core->highwater = spec->highwater;
    core->backlink = spec->backlink;

    if (level5) {
        /* FI5 ident area (ODS5_FORMAT_NOTES.md 3.2, DEF FI5DEF):
             0 control (name type)  1 name length incl. ";version"
             2 revision             4 create  12 revision  20 expire
             28 backup  36 access  44 attribute change  52 extended
             record attributes  60 length hint (16)  76 the name.
           The primary header's ident area is max(120, 76 + name length)
           bytes; an extension header's is these first 20 bytes only,
           as VMS makes them. */
        uint8_t *ident = header_out + ODS2_HEADER_IDOFFSET_BYTES;
        uint8_t name[2 * ODS5_MAX_LITERAL_NAME + 4];
        size_t name_len = 0;
        unsigned name_type = ODS5_NAME_ODS2;
        uint64_t vms_now = ods2_vms_time_now();

        if (spec->ident_name == NULL ||
            !level5_name(spec->ident_name, name, sizeof(name), &name_len, &name_type) ||
            name_len > 255) {
            return false;
        }
        ident[ODS5_FI5_CONTROL] = (uint8_t) name_type;
        ident[ODS5_FI5_NAMELEN] = (uint8_t) name_len;
        /* revision 1 for a file, 0 for a directory (VMS V8.4: a directory made by CREATE/DIRECTORY has
           revision 0, also after files were put into it), 0 in an extension header */
        ident[ODS5_FI5_REVISION] = (spec->seg_num == 0 && !(spec->filechar & 0x2000u)) ? 1 : 0;
        ods2_store_vms_time(ident + ODS5_FI5_CREDATE, vms_now);
        ods2_store_vms_time(ident + ODS5_FI5_REVDATE, vms_now);
        if (spec->seg_num == 0) {
            if (!spec->no_access_date) ods2_store_vms_time(ident + ODS5_FI5_ACCDATE, vms_now);
            ods2_store_vms_time(ident + ODS5_FI5_ATTDATE, vms_now);
            /* length hint: all ones on a directory, "invalidated"
               (0x8000000000000000 twice) on a file, as VMS writes them */
            if (spec->filechar & 0x2000u) {
                memset(ident + ODS5_FI5_LENGTH_HINT, 0xff, 16);
            } else {
                ident[ODS5_FI5_LENGTH_HINT + 7] = 0x80;
                ident[ODS5_FI5_LENGTH_HINT + 15] = 0x80;
            }
            memcpy(ident + ODS5_FI5_FILENAME, name, name_len);
        }
    } else {
    /* Ident Area (spec 3.5.3) Layout:
         FI2$T_FILENAME    20 bytes  - name;version, space-padded
         FI2$W_REVISION     2 bytes  - binary revision count
         FI2$Q_CREDATE      8 bytes  - binary VMS timestamp
         FI2$Q_REVDATE      8 bytes  - binary VMS timestamp
         FI2$Q_EXPDATE      8 bytes  - binary VMS timestamp
         FI2$Q_BAKDATE      8 bytes  - binary VMS timestamp
         FI2$T_FILENAMEXT  66 bytes  - name continuation
       Total 120 bytes - found while chasing ANALYZE/DISK's BADDIRENT/
       BAD_DIRHEADER findings */
    {
        uint8_t *ident = header_out + ODS2_HEADER_IDOFFSET_BYTES;
        size_t name_len;
        uint64_t vms_now;

        memset(ident, ' ', 20); /* FI2$T_FILENAME: space-padded text */
        if (spec->ident_name != NULL) {
            name_len = strlen(spec->ident_name);
            if (name_len > 20) name_len = 20; /* overflow goes to FILENAMEXT below */
            memcpy(ident, spec->ident_name, name_len);
        }

        /* FI2$W_REVISION: binary revision count. 1, matching every
           real freshly-created file examined - not 0 (this file has
           been "revised" once, by being written). */
        ident[20] = 1;
        ident[21] = 0;

        /* FI2$Q_CREDATE / FI2$Q_REVDATE: binary 64-bit VMS timestamp -
           (spec 3.5.3.3) Both set to "now" for a freshly
           created file that hasn't been revised since. VMS dates
           are local wall-clock time, not UTC: see ods2_time.h. */
        vms_now = ods2_vms_time_now();
        ods2_store_vms_time(ident + 22, vms_now); /* CREDATE */
        ods2_store_vms_time(ident + 30, vms_now); /* REVDATE */

        /* FI2$Q_EXPDATE / FI2$Q_BAKDATE: zero - "no expiration set" /
           "never backed up", a normal, valid state for a new file
           (not a missing/invalid value - VMS itself leaves these
           zero until the corresponding operation actually happens). */
        memset(ident + 38, 0, 8);  /* EXPDATE */
        memset(ident + 46, 0, 8);  /* BAKDATE */

        /* FI2$T_FILENAMEXT: continuation of the name if it didn't
           fit in the first 20 bytes, space-padded otherwise. */
        memset(ident + 54, ' ', 66);
        if (spec->ident_name != NULL && strlen(spec->ident_name) > 20) {
            size_t ext_len = strlen(spec->ident_name) - 20;
            if (ext_len > 66) ext_len = 66;
            memcpy(ident + 54, spec->ident_name + 20, ext_len);
        }
    }

    }
    {
        uint8_t *dest = header_out + map_offset_bytes;
        for (i = 0; i < spec->extent_count; i++) {
            size_t n = ods2_encode_retrieval_pointer(dest, spec->extents[i].lbn,
                                                     spec->extents[i].block_count);
            if (n == 0) return false; /* unreachable: checked above */
            dest += n;
        }
    }

    /* FH2$W_CHECKSUM occupies the last word of the block, covering
       the 255 words before it - same algorithm/coverage as every
       other checksum in this project. */
    checksum = ods2_checksum(header_out, 255);
    header_out[510] = (uint8_t) (checksum & 0xff);
    header_out[511] = (uint8_t) (checksum >> 8);

    return true;
}
