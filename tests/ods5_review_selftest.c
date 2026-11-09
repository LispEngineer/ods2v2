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

/* ods5_review_selftest - the library side of the ods5-read review fixes:
 *  2  INDEXF.SYS extension headers (and those of any file) are accepted only if they are the
 *     next segment: valid checksum, the file number the chain names, SEG_NUM, back link;
 *  3  ods2_parse_directory_records skips a record it cannot read and goes on;
 *  4  ods5_escape_for_linux / ods5_unescape_from_linux: control characters, '/' and a '%' that
 *     would read back as an escape, reversibly. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_init.h"
#include "ods2_checksum.h"
#include "ods2_directory.h"
#include "ods5_names.h"

#define IMG "samples/ods5_review_disk.img"

static void fix_checksum(uint8_t *h)
{
    uint16_t c = ods2_checksum(h, 255);
    h[510] = (uint8_t) c; h[511] = (uint8_t) (c >> 8);
}

static void roundtrip(const char *name, const char *expect_escaped)
{
    char esc[1200], back[1200];
    assert(ods5_escape_for_linux(name, esc, sizeof(esc)) != (size_t) -1);
    if (strcmp(esc, expect_escaped) != 0) printf("escape of \"%s\": \"%s\", expected \"%s\"\n", name, esc, expect_escaped);
    assert(strcmp(esc, expect_escaped) == 0);
    assert(strchr(esc, '/') == NULL);
    assert(ods5_unescape_from_linux(esc, back, sizeof(back)) != (size_t) -1);
    assert(strcmp(back, name) == 0);
}

int main(void)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t h1[512], root[512], saved[512];
    ods2_extent_t ext[ODS2_MAX_EXTENTS];
    int count_before, count, i;
    ods2_fid_t fid;
    ods2_dir_entry_t e[8];
    unsigned bad;
    int n;

    /* --- 4: escapes --- */
    roundtrip("plain.txt", "plain.txt");
    roundtrip("a\nb", "a%0Ab");
    roundtrip("esc\x1b[31m", "esc%1B[31m");
    roundtrip("a/b", "a%2Fb");
    roundtrip("c1\xC2\x85x", "c1%85x");                 /* U+0085 */
    roundtrip("del\x7f", "del%7F");
    roundtrip("100%", "100%");                           /* a '%' that reads back as itself stays */
    roundtrip("%zz", "%zz");
    roundtrip("pct%0A", "pct%250A");                     /* a literal "%0A": the '%' is written %25 */
    roundtrip("pct%2f", "pct%252f");
    roundtrip("pct%25", "pct%2525");
    roundtrip("caf\xC3\xA9 \xE6\x97\xA5", "caf\xC3\xA9 \xE6\x97\xA5");   /* ordinary characters untouched */
    {
        char tiny[3];
        assert(ods5_escape_for_linux("a\nb", tiny, sizeof(tiny)) == (size_t) -1);
    }
    printf("PASS: control characters, '/' and ambiguous '%%' escape to %%XX and back\n");

    /* --- the folding VMS V8.4 applies to names (ods5-vms, CORPUS.md 8 "Question (b)") --- */
    {
        /* names that fold together are one name (versions of it on VMS) */
        assert(ods5_fold('d') == ods5_fold('D') && ods5_fold(0x1C4) == ods5_fold(0x1C5) && ods5_fold(0x1C5) == ods5_fold(0x1C6));
        assert(ods5_fold(0x1F1) == ods5_fold(0x1F2) && ods5_fold(0x1F2) == ods5_fold(0x1F3));
        assert(ods5_fold(0x3A3) == ods5_fold(0x3C3) && ods5_fold(0x3C3) == ods5_fold(0x3C2));      /* sigma, small, final */
        assert(ods5_fold('k') == ods5_fold('K'));
        assert(ods5_fold(0xC9) == ods5_fold(0xE9));                                                /* Latin-1 and the UCS-2 spelling */
        /* separate names */
        assert(ods5_fold(0x212A) != ods5_fold('K'));                                               /* Kelvin sign */
        assert(ods5_fold(0xDF) != ods5_fold(0x1E9E));                                              /* sharp s, capital sharp s */
        assert(ods5_fold(0x130) != ods5_fold(0x131) && ods5_fold(0xFB01) == 0xFB01);
        assert(ods5_fold(0x1C4) != ods5_fold(0x1F1));
        /* the order VMS lists them in: by the upper-cased code point (dotless i sorts as I, between E and K) */
        assert(ods5_fold(0x131) == 'I' && ods5_fold('E') < ods5_fold(0x131) && ods5_fold(0x131) < ods5_fold('K'));
        assert(ods5_fold(0xE9) < ods5_fold(0xDF) && ods5_fold(0xDF) < ods5_fold(0x130) && ods5_fold(0x130) < ods5_fold(0x1C4));
        assert(ods5_fold(0x1C4) < ods5_fold(0x1F1) && ods5_fold(0x1F1) < ods5_fold(0x3C3) && ods5_fold(0x3C3) < ods5_fold(0x1E9E));
        assert(ods5_fold(0x1E9E) < ods5_fold(0x212A) && ods5_fold(0x212A) < ods5_fold(0xFB01));
        printf("PASS: the case folding agrees with what VMS V8.4 did to digraphs, sigmas, Kelvin, sharp s, dotless i (names and order)\n");
    }

    /* --- 3: a bad record is skipped, the others are read --- */
    {
        uint8_t blk[512];
        size_t pos = 0, k;
        static const struct { const char *name; uint8_t flags; uint8_t count; } recs[] = {
            { "a.txt", 0, 5 }, { "b.txt", 0x18, 5 } /* UCS-2, odd length */, { "c.txt", 0, 5 },
            { "d.txt", 0, 200 } /* namecount past the record */, { "e.txt", 0, 5 },
        };
        memset(blk, 0, sizeof(blk));
        for (k = 0; k < sizeof(recs) / sizeof(recs[0]); k++) {
            size_t len = strlen(recs[k].name), padded = len + (len % 2), total = 6 + padded + 8;
            blk[pos] = (uint8_t) (total - 2); blk[pos + 1] = 0;
            blk[pos + 2] = 1; blk[pos + 3] = 0;
            blk[pos + 4] = recs[k].flags; blk[pos + 5] = recs[k].count;
            memcpy(blk + pos + 6, recs[k].name, len);
            blk[pos + 6 + padded] = 1;                          /* version 1 */
            blk[pos + 6 + padded + 2] = (uint8_t) (20 + k);     /* FID */
            pos += total;
        }
        blk[pos] = 0xff; blk[pos + 1] = 0xff;
        bad = 0;
        n = ods2_parse_directory_records(blk, sizeof(blk), 5, e, 8, &bad);
        assert(n == 3 && bad == 2);
        assert(strcmp(e[0].name, "a.txt") == 0 && strcmp(e[1].name, "c.txt") == 0 && strcmp(e[2].name, "e.txt") == 0);
        assert(ods2_parse_directory_level(blk, sizeof(blk), 5, e, 8) == -1);   /* the old call still says "malformed" */
        /* a record whose size runs past the block ends it, with what came before */
        blk[0] = 0xfe; blk[1] = 0x7f;
        bad = 0;
        n = ods2_parse_directory_records(blk, sizeof(blk), 5, e, 8, &bad);
        assert(n == 0 && bad == 1);
        printf("PASS: a record that cannot be read is skipped and counted, the records after it are still read\n");
    }

    /* --- 2: extension headers must be the next segment --- */
    memset(&opt, 0, sizeof(opt));
    opt.blocks = 16384; opt.label = "REVIEW"; opt.level = 5;
    remove(IMG);
    assert(ods2_initialize(IMG, &opt).ok);
    assert(ods2_mount_write(IMG, &vol).ok);
    count_before = vol.indexf_extent_count;
    assert(ods2_read_header(&vol, 1, h1).ok);
    memcpy(saved, h1, 512);
    assert(ods2_read_header(&vol, 4, root).ok);
    ods2_dismount(&vol);

    /* EXT_FID of INDEXF.SYS made to point at the root directory's header (file 4): not segment 1 of file 1 */
    {
        FILE *f = fopen(IMG, "r+b");
        uint8_t home[512];
        uint32_t lbn;
        assert(f != NULL);
        assert(fseek(f, 512, SEEK_SET) == 0 && fread(home, 1, 512, f) == 512);
        lbn = (uint32_t) home[24] | ((uint32_t) home[25] << 8) | ((uint32_t) home[26] << 16) | ((uint32_t) home[27] << 24);
        lbn += (uint32_t) home[32] | ((uint32_t) home[33] << 8);       /* + IBMAPSIZE: header 1 */
        h1[14] = 4; h1[15] = 0; h1[16] = 4; h1[17] = 0;                 /* EXT_FID (4,4) */
        fix_checksum(h1);
        assert(fseek(f, (long) lbn * 512, SEEK_SET) == 0 && fwrite(h1, 1, 512, f) == 512);
        fclose(f);
        /* the same on a copy whose header 4 is a valid header of segment 0 with the wrong back link */
    }
    assert(ods2_mount(IMG, &vol).ok);
    assert(vol.indexf_extent_count == count_before);                    /* the bogus link was not followed */
    assert(ods2_lookup_path(&vol, "", &fid).ok && fid.fid_num == 4);
    ods2_dismount(&vol);
    printf("PASS: an INDEXF.SYS EXT_FID that is not its next segment is ignored at mount\n");

    /* the same rule for an ordinary file's chain: decode_all_extents refuses a wrong extension */
    {
        uint8_t hdr[512];
        assert(ods2_mount(IMG, &vol).ok);
        assert(ods2_read_header(&vol, 4, hdr).ok);
        hdr[14] = 2; hdr[15] = 0; hdr[16] = 2; hdr[17] = 0;             /* root's EXT_FID -> BITMAP.SYS's header */
        fix_checksum(hdr);
        {
            ods2_result_t r = ods2_decode_all_extents(&vol, hdr, ext, ODS2_MAX_EXTENTS, &count);
            assert(!r.ok);
            printf("PASS: a file whose extension header is another file's is refused (%s)\n", r.problem);
        }
        ods2_dismount(&vol);
    }
    (void) i; (void) saved; (void) root;
    remove(IMG);
    printf("\nods5_review_selftest: all checks passed\n");
    return 0;
}
