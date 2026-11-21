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

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods2_header_build.h"
#include "ods2_validate.h"
#include "ods2_checksum.h"
#include "ods5_ondisk.h"

int main(void)
{
    uint8_t header[512];
    ods2_extent_t extents[2];
    ods2_header_spec_t spec;
    ods2_head_core_t *core;
    ods2_validate_result_t vr;

    extents[0].lbn = 1000;
    extents[0].block_count = 16;
    extents[1].lbn = 2000;
    extents[1].block_count = 8;

    memset(&spec, 0, sizeof(spec));
    spec.fid.fid_num = 11;
    spec.fid.fid_seq = 1;
    spec.backlink.fid_num = 4;
    spec.backlink.fid_seq = 4;
    spec.filechar = 0x2080; /* FH2$M_DIRECTORY | FH2$M_CONTIG, confirmed real values */
    spec.rtype = 2;   /* FAB$C_VAR, confirmed real value for directories */
    spec.rattrib = 8; /* NOSPAN, confirmed real value for directories */
    spec.rsize = 512;
    spec.maxrec = 512;
    spec.extents = extents;
    spec.extent_count = 2;
    spec.hiblk = 24; /* 16 + 8 */
    spec.efblk = 1;  /* just the sentinel block used so far */
    spec.ffbyte = 2; /* sentinel is 2 bytes */
    spec.ident_name = "ODS2V2TEST.DIR;1";

    assert(ods2_build_file_header(header, &spec));
    printf("PASS: header construction succeeds for a 2-extent directory header\n");

    vr = ods2_validate_head(header);
    assert(vr.ok);
    printf("PASS: constructed header passes ods2_validate_head() (checksum + "
           "hiblk-vs-extents consistency)\n");

    core = (ods2_head_core_t *) header;
    assert(core->fid.fid_num == 11);
    assert(core->fid.fid_seq == 1);
    assert(core->backlink.fid_num == 4);
    assert(core->filechar == 0x2080u);
    assert(core->recattr.rtype == 2);
    assert(core->recattr.rattrib == 8);
    assert(ods2_word_swap32(core->recattr.hiblk) == 24u);
    assert(ods2_word_swap32(core->recattr.efblk) == 1u);
    printf("PASS: all scalar fields read back correctly\n");

    /* Ident Area: must be the fixed 120 bytes real VMS always uses
       (idoffset=40 words=byte 80, mpoffset=100 words=byte 200) -
       confirmed against two independently-examined real headers
       (root's own, INDEXF.SYS's own). An earlier version of this
       code used a zero-length Ident Area, which worked fine for a
       regular file but produced ANALYZE/DISK's BAD_DIRHEADER finding
       for a directory on a real volume - this fixed-size, populated
       layout is the actual fix. */
    assert(core->idoffset == 40);
    assert(core->mpoffset == 100);
    assert(memcmp(header + 80, "ODS2V2TEST.DIR;1", 16) == 0); /* 16 chars, no NUL */
    {
        int i;
        /* bytes 96-99: remaining space-padding within the 20-byte
           FI2$T_FILENAME field (16-char name + 4 spaces = 20). */
        for (i = 96; i < 100; i++) {
            assert(header[i] == ' ');
        }
        /* bytes 100-101: FI2$W_REVISION, binary - NOT a space: 0 for a directory made by CREATE/DIRECTORY (CORPUS 53; follow-up c), 1 for a file. */
        assert(header[100] == 0 && header[101] == 0);
        /* bytes 102-117: FI2$Q_CREDATE/REVDATE, binary timestamps -
           just confirm they're plausible non-zero values (an actual
           timestamp), not literally checking the exact value since
           that depends on when the test runs. */
        {
            int any_nonzero = 0;
            for (i = 102; i < 118; i++) if (header[i] != 0) any_nonzero = 1;
            assert(any_nonzero);
        }
        /* bytes 118-125, 126-133: FI2$Q_EXPDATE/BAKDATE, zero. */
        for (i = 118; i < 134; i++) {
            assert(header[i] == 0);
        }
        /* bytes 134-199: FI2$T_FILENAMEXT, space-padded (our 16-char
           name fits entirely within the first 20 bytes). */
        for (i = 134; i < 200; i++) {
            assert(header[i] == ' ');
        }
    }
    printf("PASS: Ident Area is the fixed 120 bytes, correctly populated with the "
           "name and space-padded, matching real VMS's own layout exactly\n");

    {
        ods2_extent_t decoded[4];
        int n = ods2_decode_retrieval_pointers(
            header + (size_t) core->mpoffset * 2, core->map_inuse, decoded, 4);
        assert(n == 2);
        assert(decoded[0].lbn == 1000 && decoded[0].block_count == 16);
        assert(decoded[1].lbn == 2000 && decoded[1].block_count == 8);
        printf("PASS: both extents decode back correctly from the constructed map area\n");
    }

    /* A header requesting more extents than fit in 512 bytes is
       correctly rejected rather than silently truncated or
       overflowing the buffer (sanitizers would catch the latter
       regardless, but the explicit false return is the real API
       contract). */
    {
        ods2_extent_t many[128];
        ods2_header_spec_t big_spec = spec;
        int i;
        for (i = 0; i < 128; i++) { many[i].lbn = (uint32_t) i; many[i].block_count = 1; }
        big_spec.extents = many;
        big_spec.extent_count = 128; /* 128*4 = 512 bytes, won't fit with the fixed
                                         200-byte offset (80-byte core + 120-byte Ident Area) */
        assert(!ods2_build_file_header(header, &big_spec));
        printf("PASS: too many extents to fit in the header is correctly rejected\n");
    }

    /* Explicit check: fileowner_uic/fileprot/recprot must match real
       VMS-written values (confirmed via ods2_dump_header against a
       real INDEXF.SYS header) - not zero, which an earlier version
       of this code left them at, and which real VMS never does.
       A FILE's: a directory's are VMS's own, below (follow-up c). */
    {
        ods2_header_spec_t fspec = spec;
        uint8_t fh[512];
        const uint8_t *ident;
        fspec.filechar = 0;
        assert(ods2_build_file_header(fh, &fspec));
        core = (ods2_head_core_t *) fh;
        ident = fh + (size_t) core->idoffset * 2;
        assert(core->fileowner_uic[0] == 4);
        assert(core->fileowner_uic[1] == 1);
        assert(core->fileprot == 0xfa00);
        /* RECPROT of a user file: VMS writes 0 (all 2,644 user files of the VMS-made ODS-2 images on VAX V7.3, V7.3-2 and V8.4; only the reserved files carry
           0xFE00): follow-up b. ods2v2 wrote 0xFE00 for every file at ODS-2. */
        assert(core->recprot == 0);
        assert(ident[20] == 1 && ident[21] == 0);
        printf("PASS: a file's fileowner_uic/fileprot/recprot/revision match real VMS-written values (RECPROT 0)\n");
    }

    /* A directory header at level 2 as VMS's CREATE/DIRECTORY makes it (CORPUS 49 and 53: the directories of the VAX V7.3 and V8.4 ODS-2 step 0 containers): protection
       0xBA88 (S:RWE,O:RWE,G:RE,W:E), record protection 0, revision 0 (a file's is 1). Follow-up c. */
    {
        uint8_t dh[512];
        const uint8_t *ident;
        assert(spec.filechar == 0x2080u);
        assert(ods2_build_file_header(dh, &spec));
        core = (ods2_head_core_t *) dh;
        ident = dh + (size_t) core->idoffset * 2;
        assert(core->fileowner_uic[0] == 4 && core->fileowner_uic[1] == 1);
        assert(core->fileprot == 0xba88);
        assert(core->recprot == 0);
        assert(ident[20] == 0 && ident[21] == 0);
        printf("PASS: a level 2 directory header has protection 0xBA88, record protection 0 and revision 0\n");
    }

    /* Extension headers at level 2 as VMS makes them (follow-up d; CORPUS 58.6 and the VMS-made extension headers of asim5-frag-sf-2.img and phvs2-ods2v: 12 of 12): IDOFFSET 40,
       MPOFFSET 50 words (byte 100), ACOFFSET and RSOFFSET 255, the "ident area" the 20-byte file name only (no revision, no dates, no FILENAMEXT), the map straight after and up to
       204 words (102 Format 1 pointers). The INDEXF.SYS extension header (the spec flag reserved_recprot, which ods2_volume.c sets for it) and the level 5 one are unchanged. */
    {
        ods2_header_spec_t xs;
        ods2_extent_t many[130];
        uint8_t xh[512];
        int i, fit;
        const uint8_t *map;

        for (i = 0; i < 130; i++) { many[i].lbn = 5000 + 4 * (uint32_t) i; many[i].block_count = 1; }
        memset(&xs, 0, sizeof(xs));
        xs.level = ODS5_LEVEL_2;
        xs.fid.fid_num = 40; xs.fid.fid_seq = 1;
        xs.backlink.fid_num = 12; xs.backlink.fid_seq = 1;
        xs.seg_num = 2;
        xs.ident_name = "A.DAT;1";
        xs.hiblk = 78; xs.efblk = 1; xs.highwater = 79;
        xs.extents = many;
        fit = ods2_header_extents_that_fit_at(many, 130, ods2_header_map_offset(&xs));
        assert(ods2_header_map_offset(&xs) == 100);         /* the map starts at byte 100 (word 50) */
        assert(fit == 102);                                  /* 102 pointers of 4 bytes: 204 words, as the VMS-made ones hold */
        xs.extent_count = fit;
        assert(ods2_build_file_header(xh, &xs));
        core = (ods2_head_core_t *) xh;
        assert(core->idoffset == 40 && core->mpoffset == 50 && core->acoffset == 255 && core->rsoffset == 255);
        assert(core->seg_num == 2 && core->map_inuse == 204);
        assert(memcmp(xh + 80, "A.DAT;1             ", 20) == 0);        /* the name, space padded to 20 bytes */
        map = xh + 100;
        assert((map[1] >> 6) == 1 && map[2] == (5000 & 0xff));     /* the first pointer is right at byte 100 */
        for (i = 100 + 4 * fit; i < 510; i++) assert(xh[i] == 0);   /* nothing (no revision, no dates, no FILENAMEXT) beyond the pointers */
        assert(ods2_checksum(xh, 255) == (uint16_t) (xh[510] | (xh[511] << 8)));
        printf("PASS: a level 2 extension header has MPOFFSET 50, a 20-byte name-only ident area, 102 pointers and nothing else, as VMS makes it\n");

        xs.reserved_recprot = true;         /* the INDEXF.SYS extension header: the old layout, MPOFFSET 100 words */
        xs.extent_count = 1;
        assert(ods2_header_map_offset(&xs) == 200);
        assert(ods2_build_file_header(xh, &xs));
        assert(((ods2_head_core_t *) xh)->mpoffset == 100);
        xs.reserved_recprot = false;
        xs.level = ODS5_LEVEL_5;            /* level 5: already VMS's layout (20 bytes of FI5, MPOFFSET 50) */
        assert(ods2_header_map_offset(&xs) == 100);
        printf("PASS: the INDEXF.SYS extension header (MPOFFSET 100) and the level 5 one (MPOFFSET 50) are unchanged\n");
    }

    printf("\nods2_header_build_selftest: all checks passed\n");
    return 0;
}
