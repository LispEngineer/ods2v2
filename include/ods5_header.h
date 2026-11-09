/*
 * MIT License
 *
 * Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer)
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

/* ods5_header.h - reading the ident area of a file header: the file
 * name stored in the header, the revision number and the dates, for a
 * level 2 header (FI2) and a level 5 header (FI5) alike; also the
 * link count and the special file type of ODS-5. See
 * ODS5_FORMAT_NOTES.md in the ods5-cli project. */
#ifndef ODS5_HEADER_H
#define ODS5_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ods5_names.h"

typedef struct {
    unsigned level;        /* 2 (FI2 ident area) or 5 (FI5), from the header's FH2$W_STRUCLEV */
    unsigned name_type;    /* ODS5_NAME_*; 0 at level 2 */
    char     name[ODS5_MAX_LITERAL_NAME + 1]; /* the primary link's name, UTF-8, no ";version" */
    unsigned version;      /* the ";version" that followed it (0 if none) */
    uint16_t revision;     /* revision number */
    uint64_t credate;      /* VMS times: 100 ns ticks since 17-NOV-1858; 0 = not set */
    uint64_t revdate;
    uint64_t expdate;
    uint64_t bakdate;
    uint64_t accdate;      /* level 5 only; 0 at level 2 */
    uint64_t attdate;      /* level 5 only; 0 at level 2 */
    bool     has_ods5_dates;
} ods5_ident_t;

/* Reads the ident area of the 512-byte header. False, with *why set to
   a static message, if the header is not level 2 or 5 or the ident area
   does not fit inside it. */
bool ods5_read_ident(const uint8_t *header, ods5_ident_t *out, const char **why);

/* FH2$W_LINKCOUNT (0 where the volume does not keep one). */
unsigned ods5_link_count(const uint8_t *header);

/* FAT$B_SPECIAL_TYPE if the file organization is "special" (symbolic
   link, FIFO, ...), else 0. */
unsigned ods5_special_type(const uint8_t *header);

/* "08-NOV-2026 20:42:32.94" for a VMS time (no zone: VMS dates are local
   wall-clock time); "<none specified>" for 0. */
void ods5_format_vms_time(uint64_t vms_time, char *out, size_t cap);

#endif /* ODS5_HEADER_H */
