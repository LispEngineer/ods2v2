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

/* ods2_init.h - INITIALIZE: makes a new, empty Files-11 volume (ODS-2 or
 * ODS-5) in a new disk image file, laid out as VMS V8.4 lays out the volume
 * INITIALIZE makes (measured on the ODS2T and ODS5T corpus images,
 * ODS5_FORMAT_NOTES.md 12): boot block and home block copies, backup home
 * cluster, backup index file header, index file bitmap and the first headers
 * contiguous in the middle of the disk with the root directory, BITMAP.SYS and
 * SECURITY.SYS, the ten reserved files, the storage control block.
 */
#ifndef ODS2_INIT_H
#define ODS2_INIT_H

#include <stdint.h>
#include "ods2_volume.h"

typedef struct {
    uint64_t    blocks;      /* volume size in 512-byte blocks (at least 8192) */
    const char *label;       /* 1-12 characters, upper-cased; see ods2_make_volume_label() */
    unsigned    level;       /* 2 (ODS-2) or 5 (ODS-5) */
    uint16_t    volchar;     /* HM2$W_VOLCHAR: ODS5_VC_* bits (0x20 ACCESSTIMES, 0x40 HARDLINKS,
                                0x80 NO_SPECIAL_FILES are valid at level 5 only), NOHIGHWATER ... */
    unsigned    cluster;     /* cluster factor; 0 = the default for the size */
    uint32_t    maxfiles;    /* 0 = the default, blocks / (2 * cluster + 2) */
} ods2_init_options_t;

/* Creates the file `path` (which must not exist) as an empty volume. The
 * file is made sparse: only the blocks that hold structures are written.
 * On failure no file is left behind. */
ods2_result_t ods2_initialize(const char *path, const ods2_init_options_t *opt);

#endif /* ODS2_INIT_H */
