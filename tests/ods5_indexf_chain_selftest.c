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

/* ods5_indexf_chain_selftest.c - a fragmented index file: INDEXF.SYS's
 * own header maps only part of it and names an extension header (itself
 * a header inside the index file) for the rest. A tiny hand-built level 5
 * volume: the header of file 3 lies in the part that the extension
 * describes, so it can be read only if the chain is followed at mount.
 * A chain that loops, and one whose extension header cannot be read, must
 * not hang or crash. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_checksum.h"
#include "ods5_ondisk.h"

#define IMG "samples/ods5_indexf_chain.img"
#define NBLK 64

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }

/* A format 1 retrieval pointer: count blocks at lbn. */
static void ptr1(uint8_t *p, unsigned lbn, unsigned count)
{
    w16(p, (1u << 14) | (((lbn >> 16) & 0x3F) << 8) | (count - 1));
    w16(p + 2, lbn & 0xFFFF);
}

static void finish_header(uint8_t *h)
{
    w16(h + 510, ods2_checksum(h, 255));
}

/* A header: file number n, one extent, extension header number ext (0 = none). */
static void header(uint8_t *h, unsigned n, unsigned seg, unsigned lbn, unsigned count,
                   unsigned ext, int two_extents, unsigned lbn2, unsigned count2)
{
    memset(h, 0, 512);
    h[0] = 40; h[1] = 100; h[2] = 255; h[3] = 255;
    w16(h + 4, seg);
    w16(h + 6, 0x0501);
    w16(h + 8, n); w16(h + 10, 1);
    w16(h + 14, ext); w16(h + 16, ext ? 1 : 0);
    ptr1(h + 200, lbn, count);
    h[58] = 2;
    if (two_extents) { ptr1(h + 204, lbn2, count2); h[58] = 4; }
    finish_header(h);
}

static void write_image(const char *path, unsigned ext_num)
{
    uint8_t img[NBLK * 512];
    uint8_t *home = img + 512;
    FILE *f;

    memset(img, 0, sizeof(img));
    /* home block: cluster 1, index file bitmap at LBN 10 (1 block), so header n is at
       INDEXF VBN 4 + 1 + n; INDEXF VBN 1 is LBN 6 */
    w32(home + 0, 1); w32(home + 4, 20); w32(home + 8, 30);
    w16(home + 12, 0x0501); w16(home + 14, 1);
    w16(home + 16, 2); w16(home + 18, 3); w16(home + 20, 4); w16(home + 22, 5);
    w32(home + 24, 10); w32(home + 28, 100);
    w16(home + 32, 1); w16(home + 34, 10);
    memcpy(home + 496, "DECFILE11B  ", 12);
    memcpy(home + 472, "CHAINTEST   ", 12);
    w16(home + 58, ods2_checksum(home, 29));
    w16(home + 510, ods2_checksum(home, 255));
    /* header 1 (LBN 11 = VBN 6): maps VBN 1..7 to LBN 6..12 (itself and header 2), then
       names header 2 as its extension */
    header(img + 11 * 512, 1, 0, 6, 7, ext_num, 0, 0, 0);
    /* header 2 (LBN 12 = VBN 7): the extension: VBN 8..27 at LBN 13..32 */
    if (ext_num == 2) header(img + 12 * 512, 2, 1, 13, 20, 0, 0, 0, 0);
    if (ext_num == 1) header(img + 12 * 512, 2, 1, 13, 20, 1, 0, 0, 0);   /* points back at header 1: a loop */
    /* header 3 (VBN 8 = LBN 13): only reachable through the extension */
    header(img + 13 * 512, 3, 0, 40, 1, 0, 0, 0, 0);
    f = fopen(path, "wb");
    assert(f != NULL && fwrite(img, 1, sizeof(img), f) == sizeof(img));
    fclose(f);
}

int main(void)
{
    ods2_volume_t vol;
    ods2_result_t r;
    uint8_t h[512];

    /* the chain is followed: header 3 can be read */
    write_image(IMG, 2);
    r = ods2_mount(IMG, &vol);
    assert(r.ok && vol.ods5);
    assert(vol.indexf_extent_count == 2);
    r = ods2_read_header(&vol, 3, h);
    assert(r.ok);
    assert(h[8] == 3 && h[10] == 1);                /* it is header 3 */
    r = ods2_read_header(&vol, 21, h);               /* VBN 26: still in the extension */
    assert(r.ok);
    r = ods2_read_header(&vol, 23, h);               /* VBN 28: beyond both */
    assert(!r.ok);
    ods2_dismount(&vol);
    printf("PASS: an index file continued in an extension header is read through the chain\n");

    /* without an extension header, header 3 is not mapped */
    write_image(IMG, 0);
    r = ods2_mount(IMG, &vol);
    assert(r.ok && vol.indexf_extent_count == 1);
    r = ods2_read_header(&vol, 3, h);
    assert(!r.ok);
    ods2_dismount(&vol);
    printf("PASS: without the extension header the later headers are reported as not covered\n");

    /* a chain that loops stops: header 2 names header 1 as its extension */
    write_image(IMG, 1);
    r = ods2_mount(IMG, &vol);
    assert(r.ok);
    assert(vol.indexf_extent_count <= ODS2_MAX_EXTENTS);
    ods2_dismount(&vol);
    printf("PASS: a looping extension chain does not hang the mount\n");

    /* an extension header that lies outside the mapped part: mount still works */
    write_image(IMG, 22);
    r = ods2_mount(IMG, &vol);
    assert(r.ok && vol.indexf_extent_count == 1);
    ods2_dismount(&vol);
    printf("PASS: an unreadable extension header leaves the extents known so far\n");

    remove(IMG);
    printf("\nods5_indexf_chain_selftest: all checks passed\n");
    return 0;
}
