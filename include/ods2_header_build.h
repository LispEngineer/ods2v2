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

/* ods2_header_build.h - constructs a valid 512-byte file header from
 * scratch, for writing a new file/directory to disk. Every field
 * written here corresponds to something verified on the read side
 * against real data earlier in this project.
 */
#ifndef ODS2_HEADER_BUILD_H
#define ODS2_HEADER_BUILD_H

#include "ods2_ondisk.h"
#include "ods2_retrieval.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    ods2_fid_t     fid;        /* this file's own FID */
    ods2_fid_t     backlink;   /* parent directory's FID - or, in an
                                   extension header, the primary
                                   header's FID (spec 3.5.2.16) */
    uint16_t       seg_num;    /* FH2$W_SEG_NUM: 0 for the primary
                                   header, n for the n+1th header of a
                                   multi-header file (spec 3.5.2.5) */
    ods2_fid_t     ext_fid;    /* FH2$W_EXT_FID: the next extension
                                   header's FID, or all zero for none
                                   (spec 3.5.2.8) */
    uint32_t       filechar;   /* FH2$L_FILECHAR bits */
    uint8_t        rtype;      /* FAT$B_RTYPE */
    uint8_t        rattrib;    /* FAT$B_RATTRIB */
    uint16_t       rsize;      /* FAT$W_RSIZE */
    uint16_t       maxrec;     /* FAT$W_MAXREC */
    const ods2_extent_t *extents;
    int            extent_count;
    uint32_t       hiblk;      /* total allocated blocks (plain value -
                                   word-swap is applied internally) */
    uint32_t       efblk;      /* logical EOF VBN (plain value - also
                                   word-swapped internally) */
    uint16_t       ffbyte;
    const char    *ident_name; /* e.g. "ODS2V2TEST.DIR;1" - the file's
                                   own identification string, written
                                   into the Ident Area. Confirmed
                                   against two real VMS-written headers
                                   (root, INDEXF.SYS) that this area is
                                   ALWAYS a fixed 120 bytes
                                   (idoffset=40 words, mpoffset=100
                                   words), space-padded - not sized to
                                   the name's actual length. Must fit
                                   in 120 bytes; longer names are
                                   truncated (matching VMS's own
                                   ODS-2 39-char name / 39-char type
                                   limits, this should never actually
                                   happen in practice).
                                   At level 5 it is the UTF-8 literal
                                   name with its dot and ";version"
                                   ("Read Me.txt;1"), stored in the
                                   FI5 ident area as ISO Latin-1 or
                                   UCS-2 (see ods5_names.h). */
    uint32_t       highwater;  /* FH2$L_HIGHWATER: 0 = not set (the ODS-2 convention here);
                                   at level 5 VMS writes HIBLK + 1 when every allocated
                                   block has been written */
    uint16_t       link_count; /* level 5: FH2$W_LINKCOUNT. VMS V8.4 writes 1 on a volume with
                                   HM2$M_HARDLINKS and 0 on one without (measured, ODS5_FORMAT_NOTES.md 12.1) */
    bool           no_access_date; /* level 5: leave FI5$Q_ACCDATE zero, as VMS
                                   does for a symbolic link */
    unsigned       level;      /* 0 or 2: a level 2 header with an FI2
                                   ident area (ODS-2); 5: a level 5
                                   header with an FI5 ident area,
                                   built as VMS V8.4 builds them
                                   (ODS5_FORMAT_NOTES.md 3, 11) */
} ods2_header_spec_t;

/* Builds a complete, checksummed 512-byte header into `header_out`
 * from `spec`. Each extent is written as the smallest retrieval
 * pointer that describes it (Format 1, 2 or 3 - see
 * ods2_encode_retrieval_pointer()). Returns false if an extent can't
 * be represented at all or there isn't room for all of them in the
 * Map Area (see ods2_header_extents_that_fit()). */
bool ods2_build_file_header(uint8_t *header_out, const ods2_header_spec_t *spec);

/* How many of the `count` extents, taken in order from the first,
 * fit in one header's Map Area as built by ods2_build_file_header()
 * (310 bytes: from the fixed map offset at byte 200 up to the
 * checksum word). Returns -1 if one of them can't be represented by
 * any retrieval pointer format. A file whose extents don't all fit
 * continues in extension headers. */
int ods2_header_extents_that_fit(const ods2_extent_t *extents, int count);

/* The same for a header whose Map Area starts at byte `map_offset_bytes`
 * (see ods2_header_map_offset()). */
int ods2_header_extents_that_fit_at(const ods2_extent_t *extents, int count,
                                    size_t map_offset_bytes);

/* Where the Map Area of the header ods2_build_file_header() makes from
 * `spec` starts, in bytes: 200 at level 2; at level 5 80 plus the FI5
 * ident area (at least 120 bytes, more for a long name) in the primary
 * header (spec->seg_num 0), and 100 in an extension header, whose ident
 * area is only the first 20 bytes of FI5, as VMS makes it. */
size_t ods2_header_map_offset(const ods2_header_spec_t *spec);

#endif /* ODS2_HEADER_BUILD_H */
