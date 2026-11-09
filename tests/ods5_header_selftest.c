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

/* ods5_header_selftest.c - hand-built file headers: the FI2 and FI5
 * ident areas, the link count, special files, VMS dates. */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods5_header.h"
#include "ods5_ondisk.h"

static void put64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i));
}

/* A level 5 header: IDOFFSET 40 words, the FI5 area as long as the name needs
   (at least 120 bytes), MPOFFSET after it. */
static void build_fi5(uint8_t *h, unsigned control, const uint8_t *name, size_t n)
{
    size_t id = 80, len = ODS5_FI5_FILENAME + n, words;
    memset(h, 0, 512);
    h[0] = 40;
    len = len < 120 ? 120 : len;
    words = (len + 1) / 2;
    h[1] = (uint8_t) (40 + words);
    h[2] = 255; h[3] = 255;
    h[6] = 0x01; h[7] = 0x05;                      /* structure level 5, version 1 */
    h[id + ODS5_FI5_CONTROL] = (uint8_t) control;
    h[id + ODS5_FI5_NAMELEN] = (uint8_t) n;
    h[id + ODS5_FI5_REVISION] = 3;
    put64(h + id + ODS5_FI5_CREDATE, 0x00BC5280FA407E11ull);
    put64(h + id + ODS5_FI5_REVDATE, 0x00BC5280FA407E12ull);
    put64(h + id + ODS5_FI5_ACCDATE, 0x00BC5280FA407E13ull);
    put64(h + id + ODS5_FI5_ATTDATE, 0x00BC5280FA407E14ull);
    memcpy(h + id + ODS5_FI5_FILENAME, name, n);
}

int main(void)
{
    uint8_t h[512];
    ods5_ident_t id;
    const char *why = "";
    char t[64];

    /* FI5, ISO Latin-1 name with ;version */
    {
        const uint8_t name[] = { 'c', 'a', 'f', 0xE9, '.', 'T', 'X', 'T', ';', '1', '2' };
        build_fi5(h, ODS5_NAME_ISL1, name, sizeof(name));
        assert(ods5_read_ident(h, &id, &why));
        assert(id.level == 5 && id.name_type == ODS5_NAME_ISL1);
        assert(strcmp(id.name, "caf\xC3\xA9.TXT") == 0 && id.version == 12);
        assert(id.revision == 3 && id.has_ods5_dates);
        assert(id.credate == 0x00BC5280FA407E11ull && id.attdate == 0x00BC5280FA407E14ull);
        assert(id.expdate == 0 && id.bakdate == 0);
        printf("PASS: FI5 ident area: ISO Latin-1 name, version, revision and all six dates\n");
    }
    /* FI5, UCS-2 name */
    {
        const uint8_t name[] = { 'e', 0, 0xAC, 0x20, '.', 0, 't', 0, ';', 0, '1', 0 };
        build_fi5(h, ODS5_NAME_UCS2, name, sizeof(name));
        assert(ods5_read_ident(h, &id, &why));
        assert(id.name_type == ODS5_NAME_UCS2 && strcmp(id.name, "e\xE2\x82\xAC.t") == 0 && id.version == 1);
        printf("PASS: FI5 ident area: UCS-2 name\n");
    }
    /* FI5: a name of the longest size, header grows with the name */
    {
        uint8_t name[242];
        memset(name, 'A', 236);
        memcpy(name + 236, ";32767", 6);
        build_fi5(h, 0, name, sizeof(name));
        assert(ods5_read_ident(h, &id, &why));
        assert(strlen(id.name) == 236 && id.version == 32767);
        printf("PASS: FI5 ident area: a 236-character name with ;32767\n");
    }
    /* FI5: name running past the ident area / the header, odd UCS-2, bad offsets */
    {
        const uint8_t name[] = "X.Y;1";
        build_fi5(h, 0, name, sizeof(name) - 1);
        h[80 + ODS5_FI5_NAMELEN] = 250;                  /* beyond MPOFFSET */
        assert(!ods5_read_ident(h, &id, &why));
        build_fi5(h, ODS5_NAME_UCS2, name, 5);           /* odd byte count */
        assert(!ods5_read_ident(h, &id, &why));
        build_fi5(h, 0, name, sizeof(name) - 1);
        h[0] = 250; h[1] = 255;                          /* ident area beyond the header */
        assert(!ods5_read_ident(h, &id, &why));
        build_fi5(h, 0, name, sizeof(name) - 1);
        h[0] = 100; h[1] = 50;                           /* IDOFFSET > MPOFFSET */
        assert(!ods5_read_ident(h, &id, &why));
        build_fi5(h, 0, name, sizeof(name) - 1);
        h[7] = 7;                                        /* not level 2 or 5 */
        assert(!ods5_read_ident(h, &id, &why));
        printf("PASS: damaged FI5 headers are refused: %s\n", why);
    }
    /* FI2 (the reserved files of an ODS-5 volume have these) */
    {
        memset(h, 0, 512);
        h[0] = 40; h[1] = 100; h[2] = 255; h[3] = 255;
        h[6] = 0x01; h[7] = 0x02;
        memset(h + 80, ' ', 20);
        memset(h + 80 + 54, ' ', 66);
        memcpy(h + 80, "INDEXF.SYS;1", 12);
        h[80 + 20] = 1;
        put64(h + 80 + 22, 0x00BC5280FA407E11ull);
        assert(ods5_read_ident(h, &id, &why));
        assert(id.level == 2 && strcmp(id.name, "INDEXF.SYS") == 0 && id.version == 1);
        assert(id.revision == 1 && !id.has_ods5_dates && id.credate == 0x00BC5280FA407E11ull);
        /* a long ODS-2 name continues in FILENAMEXT */
        memset(h + 80, ' ', 20);
        memset(h + 80 + 54, ' ', 66);
        memcpy(h + 80, "ABCDEFGHIJKLMNOPQRST", 20);
        memcpy(h + 80 + 54, "UVWXYZ.TXT;7", 12);
        assert(ods5_read_ident(h, &id, &why));
        assert(strcmp(id.name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.TXT") == 0 && id.version == 7);
        printf("PASS: FI2 ident area, including a name that continues in the extension field\n");
    }
    /* link count and special files */
    {
        memset(h, 0, 512);
        h[ODS5_FH2_LINKCOUNT] = 0x34; h[ODS5_FH2_LINKCOUNT + 1] = 0x12;
        assert(ods5_link_count(h) == 0x1234);
        assert(ods5_special_type(h) == 0);
        h[20] = 0x40; h[21] = ODS5_FAT_SYMBOLIC_LINK;     /* special, symbolic link */
        assert(ods5_special_type(h) == ODS5_FAT_SYMBOLIC_LINK);
        h[20] = 0x01; h[21] = 5;                          /* fixed records, attribute bits: not special */
        assert(ods5_special_type(h) == 0);
        printf("PASS: link count and special file type\n");
    }
    /* dates */
    ods5_format_vms_time(0x00BC5280FA407E11ull, t, sizeof(t));
    assert(strcmp(t, "07-NOV-2026 20:42:32.94") == 0);
    ods5_format_vms_time(0, t, sizeof(t));
    assert(strcmp(t, "<none specified>") == 0);
    ods5_format_vms_time(1, t, sizeof(t));
    assert(strcmp(t, "17-NOV-1858 00:00:00.00") == 0);
    ods5_format_vms_time(0x007C95674BEB4000ull, t, sizeof(t));
    assert(strcmp(t, "01-JAN-1970 00:00:00.00") == 0);
    ods5_format_vms_time(0x007C95674BEB4000ull + 86400ull * 10000000ull * 59, t, sizeof(t));
    assert(strcmp(t, "01-MAR-1970 00:00:00.00") == 0);
    ods5_format_vms_time(0x007C95674BEB4000ull + 86400ull * 10000000ull * (365 * 2 + 59) + 86400ull * 10000000ull - 1, t, sizeof(t));
    assert(strcmp(t, "29-FEB-1972 23:59:59.99") == 0);   /* a leap day */
    printf("PASS: VMS dates are written as DD-MMM-YYYY HH:MM:SS.cc\n");

    printf("\nods5_header_selftest: all checks passed\n");
    return 0;
}
