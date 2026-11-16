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

/* ods2_dirsearch_selftest.c - the search of a large directory by halving its blocks (LARGE_VOLUMES.md 8), with names that
 * have many versions, inserted so that the versions of a name run across records and blocks (review 27 T2:
 * ods2v2's CLI makes version 1 only, so the CLI test could not show this). Every (name, version) is looked up and must give
 * the FID that was inserted; versions 0, -1 and -5 select the highest, the next and the fifth below it; names and versions
 * that are not there are not found; the count of entries listed is the count inserted. Then two of the directory's blocks are
 * swapped (a directory that is not in order: damage, another tool) and an insert of a name and version that exists is
 * refused, not added twice (review 27 N5). ODS-2 and ODS-5. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include "ods2_volume.h"
#include "ods2_init.h"

#define DISK_PATH "samples/dirsearch_disk.img"
#define NAMES 40
#define VERSIONS 70

static void name_of(unsigned i, char *out) { sprintf(out, "NAME%03u.TXT", i * 7 + 1); }
static ods2_fid_t fid_of(unsigned i, unsigned v)
{
    ods2_fid_t f;
    memset(&f, 0, sizeof(f));
    ods2_fid_set_number(&f, 100 + i * VERSIONS + v);
    f.fid_seq = 1;
    return f;
}

static unsigned dir_blocks(ods2_volume_t *vol, const uint8_t *h)
{
    ods2_extent_t ext[ODS2_MAX_EXTENTS];
    int n, i;
    unsigned total = 0;
    assert(ods2_decode_all_extents(vol, h, ext, ODS2_MAX_EXTENTS, &n).ok);
    for (i = 0; i < n; i++) total += ext[i].block_count;
    return total;
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    uint8_t buf[65536];
    size_t n;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) assert(fwrite(buf, 1, n, out) == n);
    fclose(in);
    fclose(out);
}

/* Swaps two neighbouring records of one name in some block, so that two versions of it lie oldest first (what the insert
   did before review 28 b). Returns true when a block was changed. */
static bool swap_two_versions(ods2_volume_t *vol, const uint8_t *hdr, unsigned blocks)
{
    unsigned b;
    for (b = 1; b <= blocks; b++) {
        uint8_t blk[512], tmp[512];
        size_t pos = 0, prev_pos = (size_t) -1;
        assert(ods2_read_file_block(vol, hdr, b, blk).ok);
        while (pos + 6 <= 512) {
            unsigned size = blk[pos] | (blk[pos + 1] << 8);
            if (size == 0xFFFF || size == 0) break;
            if (prev_pos != (size_t) -1) {
                unsigned psize = blk[prev_pos] | (blk[prev_pos + 1] << 8);
                if (blk[prev_pos + 5] == blk[pos + 5] && memcmp(blk + prev_pos + 6, blk + pos + 6, blk[pos + 5]) == 0) {
                    size_t plen = (size_t) psize + 2, clen = (size_t) size + 2;
                    memcpy(tmp, blk + prev_pos, plen + clen);
                    memcpy(blk + prev_pos, tmp + plen, clen);
                    memcpy(blk + prev_pos + clen, tmp, plen);
                    assert(ods2_write_file_block(vol, hdr, b, blk).ok);
                    return true;
                }
            }
            prev_pos = pos;
            pos += (size_t) size + 2;
        }
    }
    return false;
}

static void run(unsigned level)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t root[512];
    unsigned order[NAMES], i, j, k, blocks;
    ods2_dir_entry_t *list = NULL;
    int count = 0;

    memset(&opt, 0, sizeof(opt));
    opt.blocks = 20000; opt.label = "DSEARCH"; opt.level = level; opt.cluster = 1; opt.maxfiles = 40000;
    remove(DISK_PATH);
    assert(ods2_initialize(DISK_PATH, &opt).ok);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    /* the directory as VMS keeps it: the versions of a name newest first (a record is added after the records of its name,
       so the versions go in from 70 down to 1; the CLI makes version 1 only and never inserts a higher one); the names
       come in random order */
    srand(12345 + level);
    for (j = VERSIONS; j >= 1; j--) {
        for (i = 0; i < NAMES; i++) order[i] = i;
        for (i = NAMES - 1; i > 0; i--) { unsigned m = (unsigned) rand() % (i + 1); k = order[i]; order[i] = order[m]; order[m] = k; }
        for (i = 0; i < NAMES; i++) {
            char nm[32];
            ods2_result_t r;
            name_of(order[i], nm);
            r = ods2_insert_into_directory(&vol, 4, nm, (uint16_t) j, fid_of(order[i], j));
            if (!r.ok) { fprintf(stderr, "insert %s;%u: %s\n", nm, j, r.problem); assert(0); }
        }
    }
    assert(ods2_read_header(&vol, 4, root).ok);
    blocks = dir_blocks(&vol, root);
    assert(blocks >= 32);                                   /* the halving search is in use */
    for (i = 0; i < NAMES; i++) {
        char nm[32];
        ods2_fid_t f;
        uint16_t ver;
        name_of(i, nm);
        for (j = 1; j <= VERSIONS; j++) {
            assert(ods2_lookup_name_version(&vol, root, nm, (int) j, &f, &ver).ok);
            assert(ods2_fid_number(f) == ods2_fid_number(fid_of(i, j)) && ver == j);
        }
        assert(ods2_lookup_name_version(&vol, root, nm, 0, &f, &ver).ok && ver == VERSIONS);
        assert(ods2_lookup_name_version(&vol, root, nm, -1, &f, &ver).ok && ver == VERSIONS - 1);
        assert(ods2_lookup_name_version(&vol, root, nm, -5, &f, &ver).ok && ver == VERSIONS - 5);
        assert(!ods2_lookup_name_version(&vol, root, nm, VERSIONS + 1, &f, &ver).ok);
        sprintf(nm, "NAME%03uX.TXT", i * 7 + 1);              /* between two names */
        assert(!ods2_lookup_name_version(&vol, root, nm, 1, &f, &ver).ok);
    }
    {
        ods2_fid_t f;
        assert(!ods2_lookup_name(&vol, root, "A.TXT", &f).ok);       /* before the first */
        assert(!ods2_lookup_name(&vol, root, "ZZZ.TXT", &f).ok);     /* after the last */
    }
    assert(ods2_list_directory_alloc(&vol, root, &list, &count).ok);
    assert(count >= NAMES * VERSIONS);                      /* plus the reserved files' entries */
    free(list);

    /* a version that would not be stored newest first is refused (review 28 b): the record goes after the records of its name */
    {
        ods2_fid_t f;
        uint16_t ver;
        assert(ods2_insert_into_directory(&vol, 4, "ORDER.TXT", 5, fid_of(0, 5)).ok);
        assert(!ods2_insert_into_directory(&vol, 4, "ORDER.TXT", 7, fid_of(0, 7)).ok);       /* higher than ;5 */
        assert(!ods2_insert_into_directory(&vol, 4, "ORDER.TXT", 5, fid_of(0, 5)).ok);       /* the same */
        assert(ods2_insert_into_directory(&vol, 4, "ORDER.TXT", 3, fid_of(0, 3)).ok);        /* lower: stored after ;5 */
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(ods2_lookup_name_version(&vol, root, "ORDER.TXT", 0, &f, &ver).ok && ver == 5);
        assert(ods2_lookup_name_version(&vol, root, "ORDER.TXT", -1, &f, &ver).ok && ver == 3);
        blocks = dir_blocks(&vol, root);
    }
    if (level == 2) {
        /* kept for tests/cli_verorder_check.sh: the directory as built (versions newest first), and the same with two
           versions of a name swapped */
        ods2_dismount(&vol);
        copy_file(DISK_PATH, "samples/dirsearch_order_ok.img");
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(swap_two_versions(&vol, root, blocks));
        ods2_dismount(&vol);
        copy_file(DISK_PATH, "samples/dirsearch_order_bad.img");
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        assert(ods2_read_header(&vol, 4, root).ok);
    }

    /* a directory out of order: two blocks swapped; an existing name and version is not added a second time */
    {
        uint8_t a[512], b[512];
        char nm[32];
        unsigned n_before;
        ods2_result_t r;
        assert(ods2_read_file_block(&vol, root, 3, a).ok && ods2_read_file_block(&vol, root, blocks - 2, b).ok);
        assert(ods2_write_file_block(&vol, root, 3, b).ok && ods2_write_file_block(&vol, root, blocks - 2, a).ok);
        name_of(5, nm);
        r = ods2_insert_into_directory(&vol, 4, nm, 3, fid_of(5, 3));
        assert(!r.ok);
        assert(strstr(r.problem, "exists already") != NULL);
        assert(ods2_list_directory_alloc(&vol, root, &list, &count).ok);
        n_before = (unsigned) count;
        free(list);
        r = ods2_insert_into_directory(&vol, 4, "BRANDNEW.TXT", 1, fid_of(0, 1));      /* a new name is added as always */
        assert(r.ok);
        assert(ods2_list_directory_alloc(&vol, root, &list, &count).ok);
        assert((unsigned) count == n_before + 1);
        free(list);
    }
    ods2_dismount(&vol);
    remove(DISK_PATH);
}

int main(void)
{
    run(2);
    run(5);
    printf("ods2_dirsearch_selftest: all checks passed\n");
    return 0;
}
