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

/* ods5_directory_selftest.c - hand-built ODS-5 directory blocks: name
 * types (ODS-2, ISO Latin-1, UCS-2) become UTF-8 entry names, link name
 * records are skipped, bad records are refused, level 2 is unchanged;
 * and directory and file specifications written with ^-escapes. */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods2_directory.h"
#include "ods2_wildcard.h"
#include "ods2_path.h"
#include "ods5_ondisk.h"

/* Appends one record to a 512-byte block at *pos: flags, name bytes
   (namecount of them), then versions[i] / fids[i] for nv versions. */
static void add_record(uint8_t *blk, size_t *pos, uint8_t flags, const uint8_t *name,
                       size_t namecount, int nv, const uint16_t *versions,
                       const uint16_t *fids)
{
    size_t p = *pos, padded = namecount + (namecount & 1), size;
    int i;
    size = 6 + padded + 8 * (size_t) nv;
    blk[p] = (uint8_t) ((size - 2) & 0xFF);
    blk[p + 1] = (uint8_t) ((size - 2) >> 8);
    blk[p + 2] = 0xFF; blk[p + 3] = 0x7F;           /* version limit 32767 */
    blk[p + 4] = flags;
    blk[p + 5] = (uint8_t) namecount;
    memcpy(blk + p + 6, name, namecount);
    for (i = 0; i < nv; i++) {
        size_t e = p + 6 + padded + 8 * (size_t) i;
        blk[e] = (uint8_t) (versions[i] & 0xFF); blk[e + 1] = (uint8_t) (versions[i] >> 8);
        blk[e + 2] = (uint8_t) (fids[i] & 0xFF); blk[e + 3] = (uint8_t) (fids[i] >> 8);
        blk[e + 4] = 1; blk[e + 5] = 0;              /* seq 1 */
        blk[e + 6] = 0; blk[e + 7] = 0;
    }
    *pos = p + size;
    blk[*pos] = 0xFF; blk[*pos + 1] = 0xFF;          /* end of block */
}

int main(void)
{
    uint8_t blk[512];
    size_t pos = 0;
    ods2_dir_entry_t e[16];
    int n;
    const uint16_t v1[] = { 1 }, f1[] = { 20 };
    const uint16_t v3[] = { 3, 2, 1 }, f3[] = { 31, 32, 33 };
    const uint8_t plain[] = "Plain.txt";
    const uint8_t isl1[] = { 'c', 'a', 'f', 0xE9, '.', 'T', 'X', 'T' };
    const uint8_t spaced[] = "two words.txt";
    const uint8_t ucs2[] = { 'e', 0, 'u', 0, 'r', 0, 'o', 0, 0xAC, 0x20, '.', 0, 't', 0 };
    const uint8_t ucs2_odd[] = { 'e', 0, 'u' };
    const uint8_t linkname[] = "TARGET.DAT";

    memset(blk, 0, sizeof(blk));
    blk[0] = 0xFF; blk[1] = 0xFF;
    add_record(blk, &pos, 0x00, plain, sizeof(plain) - 1, 1, v1, f1);
    add_record(blk, &pos, ODS5_NAME_ISL1 << ODS5_DIR_NAMETYPE_SHIFT, isl1, sizeof(isl1), 3, v3, f3);
    add_record(blk, &pos, ODS5_NAME_ISL1 << ODS5_DIR_NAMETYPE_SHIFT, spaced, sizeof(spaced) - 1, 1, v1, f1);
    add_record(blk, &pos, ODS5_NAME_UCS2 << ODS5_DIR_NAMETYPE_SHIFT, ucs2, sizeof(ucs2), 1, v1, f1);
    add_record(blk, &pos, ODS5_DIR_TYPE_LINKNAME, linkname, sizeof(linkname) - 1, 1, v1, f1);
    add_record(blk, &pos, 0x00, plain, sizeof(plain) - 1, 1, v1, f1);

    n = ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16);
    assert(n == 1 + 3 + 1 + 1 + 1);        /* the link name record is not an entry */
    assert(strcmp(e[0].name, "Plain.txt") == 0 && e[0].name_type == ODS5_NAME_ODS2);
    assert(strcmp(e[1].name, "caf\xC3\xA9.TXT") == 0 && e[1].name_type == ODS5_NAME_ISL1);
    assert(e[1].version == 3 && e[2].version == 2 && e[3].version == 1);
    assert(e[1].fid.fid_num == 31 && e[3].fid.fid_num == 33);
    assert(strcmp(e[4].name, "two words.txt") == 0);
    assert(strcmp(e[5].name, "euro\xE2\x82\xAC.t") == 0 && e[5].name_type == ODS5_NAME_UCS2);
    assert(e[5].flags == (ODS5_NAME_UCS2 << ODS5_DIR_NAMETYPE_SHIFT));
    assert(strcmp(e[6].name, "Plain.txt") == 0);
    printf("PASS: ODS-5 directory records: ODS-2, ISO Latin-1 and UCS-2 names become UTF-8, all versions kept, link name records skipped\n");

    /* level 2 reads the same block as before: flags ignored, bytes copied */
    n = ods2_parse_directory(blk, sizeof(blk), e, 16);
    assert(n == 1 + 3 + 1 + 1 + 1 + 1);    /* everything is a plain entry at level 2 */
    assert(strcmp(e[0].name, "Plain.txt") == 0 && e[0].name_type == 0 && e[0].flags == 0);
    assert(memcmp(e[1].name, "caf\xE9.TXT", 8) == 0);
    printf("PASS: level 2 parsing of the same block is unchanged (raw bytes, flags ignored)\n");

    /* a UCS-2 name with an odd byte count is malformed */
    memset(blk, 0, sizeof(blk)); pos = 0;
    add_record(blk, &pos, ODS5_NAME_UCS2 << ODS5_DIR_NAMETYPE_SHIFT, ucs2_odd, sizeof(ucs2_odd), 1, v1, f1);
    assert(ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16) == -1);
    printf("PASS: a UCS-2 name of odd length is refused\n");

    /* a name type this code does not know is read as ISO Latin-1 */
    memset(blk, 0, sizeof(blk)); pos = 0;
    add_record(blk, &pos, 2 << ODS5_DIR_NAMETYPE_SHIFT, plain, sizeof(plain) - 1, 1, v1, f1);
    n = ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16);
    assert(n == 1 && strcmp(e[0].name, "Plain.txt") == 0);
    printf("PASS: an unknown name type still lists the file\n");

    /* the longest names: 236 ISO Latin-1 bytes of é = 472 UTF-8 bytes; 118 UCS-2 euro signs */
    {
        uint8_t big[240];
        uint16_t i;
        memset(blk, 0, sizeof(blk)); pos = 0;
        for (i = 0; i < 236; i++) big[i] = 0xE9;
        add_record(blk, &pos, ODS5_NAME_ISL1 << ODS5_DIR_NAMETYPE_SHIFT, big, 236, 1, v1, f1);
        n = ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16);
        assert(n == 1 && strlen(e[0].name) == 472);
        memset(blk, 0, sizeof(blk)); pos = 0;
        for (i = 0; i < 118; i++) { big[2 * i] = 0xAC; big[2 * i + 1] = 0x20; }
        add_record(blk, &pos, ODS5_NAME_UCS2 << ODS5_DIR_NAMETYPE_SHIFT, big, 236, 1, v1, f1);
        n = ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16);
        assert(n == 1 && strlen(e[0].name) == 354);
        printf("PASS: the longest ODS-5 names (236 bytes, 118 UCS-2 characters) fit an entry\n");
    }

    /* truncated and overrunning records are errors, never reads past the block */
    memset(blk, 0, sizeof(blk)); pos = 0;
    add_record(blk, &pos, 0x00, plain, sizeof(plain) - 1, 1, v1, f1);
    blk[0] = 0xFF; blk[1] = 0x01;           /* size beyond the buffer */
    assert(ods2_parse_directory_level(blk, sizeof(blk), ODS5_LEVEL_5, e, 16) == -1);
    assert(ods2_parse_directory_level(blk, 20, ODS5_LEVEL_5, e, 16) == -1);
    printf("PASS: a record that overruns the block is refused\n");

    /* --- specifications with escapes --- */
    assert(ods2_dir_spec_valid("A^.B.C"));
    assert(ods2_dir_spec_match("A^.B", "A^.B"));
    assert(ods2_dir_spec_match("A^.B*", "A^.BCD"));
    assert(!ods2_dir_spec_match("A^.B", "A.B"));              /* two names vs one */
    assert(ods2_dir_spec_match("A.B", "A.B"));
    assert(ods2_dir_spec_match("a^.b...", "A^.B.X.Y"));
    assert(ods2_dir_spec_has_wildcard("A*"));
    assert(!ods2_dir_spec_has_wildcard("A^*"));
    assert(!ods2_dir_spec_has_wildcard("A^.B"));
    assert(ods2_dir_spec_has_wildcard("A?"));
    assert(ods2_dir_spec_has_wildcard("A..."));
    assert(!ods2_dir_spec_has_wildcard("A^..^."));
    {
        char prefix[64];
        const char *rest = ods2_dir_spec_split_prefix("A^.B.C.*.D", prefix, sizeof(prefix));
        assert(rest != NULL && strcmp(prefix, "A^.B.C") == 0 && strcmp(rest, "*.D") == 0);
    }
    printf("PASS: directory specifications honour ^. and ^* escapes\n");

    {
        ods2_parsed_path_t p;
        assert(ods2_parse_path("[A^.B.C]x^;y.txt;3", &p));
        assert(strcmp(p.dir_path, "A^.B.C") == 0 && strcmp(p.filename, "x^;y.txt") == 0);
        assert(p.has_version && p.version == 3);
        assert(ods2_parse_path("bracket^[x^].txt", &p));
        assert(!p.had_brackets && strcmp(p.filename, "bracket^[x^].txt") == 0);
        assert(ods2_parse_path("[D]a^]b.txt", &p));
        assert(strcmp(p.dir_path, "D") == 0 && strcmp(p.filename, "a^]b.txt") == 0);
        assert(ods2_parse_path("[D]two^_words.txt;*", &p));
        assert(p.version_wildcard && strcmp(p.filename, "two^_words.txt") == 0);
        printf("PASS: path parsing keeps escaped brackets, semicolons and dots in names\n");
    }

    printf("\nods5_directory_selftest: all checks passed\n");
    return 0;
}
