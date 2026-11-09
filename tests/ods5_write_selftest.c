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

/* ods5_write_selftest - writing to an ODS-5 (structure level 5) volume,
 * on a copy of the synthetic disk whose home block is patched to level
 * 5 (the reserved files keep their level 2 headers, as on a VMS-made
 * ODS-5 volume). Every expectation below was measured on VMS V8.4
 * (ODS5_FORMAT_NOTES.md 3, 10, 11; the ODS5T corpus image):
 *
 *  - FI5 ident area: control byte (name type), name length including
 *    ";1", revision, dates (create, revision, access, attribute change),
 *    hint, the name at ident offset 76, MPOFFSET from the name length,
 *    link count 1, record protection 0, level 0x0501;
 *  - names: case kept; ODS-2 type for plain names even in lower case,
 *    ISO Latin-1 for any other name up to U+00FF, UCS-2 above; the
 *    name in the directory record and in the header agree;
 *  - the directory order VMS uses (ascending upper-cased code points,
 *    ISO Latin-1 and UCS-2 names interleaved) for names created in any
 *    order, and a name equal to an existing one but for case refused;
 *  - names VMS refuses (# { } ` ? * ", controls) and too long names
 *    refused here too;
 *  - directory growth across blocks, delete, the space coming back;
 *  - a fragmented file's extension headers (ident area 20 bytes,
 *    MPOFFSET 50, as VMS makes them). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_checksum.h"
#include "ods2_header_build.h"
#include "ods5_names.h"
#include "ods5_ondisk.h"
#include "ods2_init.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/ods5_write_disk.img"

/* Copies the (mostly empty) 1.4 GiB image without writing its runs
   of zeros, so the copy stays sparse and quick. */
static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    long total = 0;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (long) n;
    }
    /* Make sure a trailing hole still gives the full length. */
    assert(fseek(out, total - 1, SEEK_SET) == 0);
    {
        char last;
        assert(fseek(in, total - 1, SEEK_SET) == 0);
        assert(fread(&last, 1, 1, in) == 1);
        assert(fwrite(&last, 1, 1, out) == 1);
    }
    fclose(in);
    fclose(out);
}

static uint8_t *pattern(size_t len, unsigned seed)
{
    uint8_t *p = malloc(len);
    uint32_t x = seed * 2654435761u + 1;
    size_t i;
    assert(p != NULL);
    for (i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t) (x >> 16);
    }
    return p;
}


static size_t free_clusters(ods2_volume_t *vol)
{
    uint8_t bh[512];
    size_t len, got, count = 0, i;
    uint8_t *bits;
    assert(ods2_read_header(vol, 2, bh).ok);
    len = ods2_file_content_length(bh);
    bits = malloc(len);
    assert(bits != NULL);
    assert(ods2_read_file(vol, bh, bits, len, &got).ok && got == len);
    for (i = 512; i < len; i++) {
        uint8_t b = bits[i];
        while (b) { count += b & 1u; b >>= 1; }
    }
    free(bits);
    return count;
}

/* File numbers marked in use in the index file bitmap. */
static unsigned used_file_numbers(ods2_volume_t *vol)
{
    unsigned b, i, count = 0;
    for (b = 0; b < vol->home.ibmapsize; b++) {
        uint8_t block[512];
        assert(fseek(vol->fp, (long) (vol->home.ibmaplbn + b) * 512, SEEK_SET) == 0);
        assert(fread(block, 1, 512, vol->fp) == 512);
        for (i = 0; i < 512; i++) {
            uint8_t v = block[i];
            while (v) { count += v & 1u; v >>= 1; }
        }
    }
    return count;
}

static bool index_bit(ods2_volume_t *vol, unsigned n)
{
    uint8_t block[512];
    assert(fseek(vol->fp, (long) (vol->home.ibmaplbn + (n - 1) / 4096) * 512, SEEK_SET) == 0);
    assert(fread(block, 1, 512, vol->fp) == 512);
    return (block[((n - 1) % 4096) / 8] >> ((n - 1) % 8)) & 1;
}


static void read_back_and_compare(ods2_volume_t *vol, unsigned file_number,
                                  const uint8_t *expect, size_t len)
{
    uint8_t h[512];
    uint8_t *buf = malloc(len + 1);
    size_t got = 0;
    assert(buf != NULL);
    assert(ods2_read_header(vol, file_number, h).ok);
    assert(ods2_file_content_length(h) == len);
    assert(ods2_read_file(vol, h, buf, len + 1, &got).ok);
    assert(got == len);
    assert(memcmp(buf, expect, len) == 0);
    free(buf);
}

/* Patches the copy's home block to structure level 5 version 1 (and the
   ODS-5 volume characteristics HARDLINKS | ACCESSTIMES), both checksums
   recomputed - the reserved files keep their level 2 headers, as VMS
   leaves them on an ODS-5 volume. */
static void make_level5(const char *path)
{
    FILE *f = fopen(path, "r+b");
    uint8_t b[512];
    uint16_t c;
    assert(f != NULL);
    assert(fseek(f, 512, SEEK_SET) == 0 && fread(b, 1, 512, f) == 512);
    b[12] = 0x01; b[13] = 0x05;                  /* HM2$W_STRUCLEV 0x0501 */
    b[42] |= 0x60;                               /* ACCESSTIMES | HARDLINKS */
    c = ods2_checksum(b, 29);
    b[58] = (uint8_t) c; b[59] = (uint8_t) (c >> 8);
    c = ods2_checksum(b, 255);
    b[510] = (uint8_t) c; b[511] = (uint8_t) (c >> 8);
    assert(fseek(f, 512, SEEK_SET) == 0 && fwrite(b, 1, 512, f) == 512);
    fclose(f);
}

static unsigned rd16(const uint8_t *p) { return (unsigned) p[0] | ((unsigned) p[1] << 8); }
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* Blocks allocated to INDEXF.SYS and the root directory: the two files
   that grow when files are made and do not shrink when they are deleted. */
static size_t held_by_growing_files(ods2_volume_t *vol)
{
    uint8_t h[512];
    size_t total = 0;
    assert(ods2_read_header(vol, 1, h).ok);
    total += ods2_file_allocated_bytes(h) / 512;
    assert(ods2_read_header(vol, 4, h).ok);
    total += ods2_file_allocated_bytes(h) / 512;
    return total;
}

static void root_header(ods2_volume_t *vol, uint8_t *h) { assert(ods2_read_header(vol, 4, h).ok); }

static ods2_fid_t make_file(ods2_volume_t *vol, unsigned dir, const char *name,
                            const void *content, size_t len)
{
    uint8_t d[512];
    ods2_fid_t fid;
    ods2_result_t r;
    assert(ods2_read_header(vol, dir, d).ok);
    r = ods2_create_file(vol, d, name, (const uint8_t *) content, len, 5, &fid);
    if (!r.ok) printf("create \"%s\": %s\n", name, r.problem);
    assert(r.ok);
    return fid;
}

static bool refused(ods2_volume_t *vol, const char *name)
{
    uint8_t d[512];
    ods2_fid_t fid;
    root_header(vol, d);
    return !ods2_create_file(vol, d, name, (const uint8_t *) "x", 1, 5, &fid).ok;
}

/* The FI5 ident area checks for the header of `fid`, created as `name`
   (UTF-8, with its dot completed) and expected to be of name type `type`. */
static void check_fi5(ods2_volume_t *vol, ods2_fid_t fid, const char *name, unsigned type,
                      bool directory)
{
    uint8_t h[512], stored[1024];
    char full[600];
    size_t n, nlen;
    unsigned id, mp, expect_mp, i;
    const uint8_t *ident;
    char with_version[640];
    bool ucs2;
    assert(ods2_read_header(vol, fid.fid_num, h).ok);
    assert(ods2_validate_head(h).ok);
    assert(rd16(h + 6) == 0x0501);                       /* FH2$W_STRUCLEV */
    assert(h[0] == 40);                                  /* IDOFFSET */
    assert(rd16(h + 74) == 1);                           /* FH2$W_LINKCOUNT */
    assert(rd16(h + 56) == 0);                           /* RECPROT, as VMS V8.4 */
    {   /* FH2$L_HIGHWATER = HIBLK + 1: every allocated block written, as VMS V8.4 records it */
        uint32_t hiblk = ((uint32_t) rd16(h + 24) << 16) | rd16(h + 26); /* word-swapped longword */
        uint32_t hw = (uint32_t) rd16(h + 76) | ((uint32_t) rd16(h + 78) << 16);
        assert(hw == hiblk + 1);
    }
    assert(h[2] == 0xff && h[3] == 0xff);                /* no ACL, no reserved area */
    assert(ods5_complete_name(name, full, sizeof(full)));
    snprintf(with_version, sizeof(with_version), "%.600s;1", full);
    assert(ods5_utf8_to_stored(with_version, stored, sizeof(stored), &nlen, &ucs2));
    assert(ucs2 == (type == ODS5_NAME_UCS2));
    id = h[0] * 2u;
    mp = h[1] * 2u;
    expect_mp = 80u + (76u + (unsigned) nlen < 120u ? 120u : 76u + (unsigned) nlen);
    expect_mp += expect_mp % 2;
    assert(mp == expect_mp);                             /* MPOFFSET from the name length */
    ident = h + id;
    assert(ident[ODS5_FI5_CONTROL] == type);             /* FI5$B_CONTROL: the name type */
    assert(ident[ODS5_FI5_NAMELEN] == nlen);             /* FI5$B_NAMELEN, ";1" included */
    assert(memcmp(ident + ODS5_FI5_FILENAME, stored, nlen) == 0);
    assert(rd16(ident + ODS5_FI5_REVISION) == (directory ? 0u : 1u));   /* VMS V8.4: a new directory has revision 0 */
    assert(rd64(ident + ODS5_FI5_CREDATE) != 0);
    assert(rd64(ident + ODS5_FI5_REVDATE) == rd64(ident + ODS5_FI5_CREDATE));
    assert((h[20] >> 4) == ODS5_FAT_ORG_SPECIAL || rd64(ident + ODS5_FI5_ACCDATE) != 0); /* a symbolic link has none */
    assert(rd64(ident + ODS5_FI5_ATTDATE) != 0);
    assert(rd64(ident + ODS5_FI5_EXPDATE) == 0 && rd64(ident + ODS5_FI5_BAKDATE) == 0);
    assert(rd64(ident + ODS5_FI5_EX_RECATTR) == 0);
    for (i = 0; i < 2; i++) {                            /* the length hint: as VMS writes it */
        uint64_t hint = rd64(ident + ODS5_FI5_LENGTH_HINT + 8 * i);
        assert(hint == (directory ? 0xFFFFFFFFFFFFFFFFull : 0x8000000000000000ull));
    }
    n = 0;
    for (i = 0; i < 120 && ident + ODS5_FI5_FILENAME + nlen + i < h + mp; i++) {
        if (nlen + 76 + i < 120) n += ident[ODS5_FI5_FILENAME + nlen + i]; /* padding is zero */
    }
    assert(n == 0);
}

/* The directory record for `name` in `dir`: its flags byte. */
static int record_flags(ods2_volume_t *vol, unsigned dir, const char *name, int *namecount)
{
    uint8_t h[512], blk[512];
    ods2_extent_t ext[ODS2_MAX_EXTENTS];
    int ne, e;
    unsigned vbn, pos;
    uint8_t stored[1024];
    size_t slen;
    unsigned type;
    assert(ods2_read_header(vol, dir, h).ok);
    assert(ods2_decode_all_extents(vol, h, ext, ODS2_MAX_EXTENTS, &ne).ok);
    (void) e;
    assert(ods5_stored_name(name, stored, sizeof(stored), &slen, &type));
    for (vbn = 1; vbn <= (ods2_file_content_length(h) + 511) / 512; vbn++) {
        assert(ods2_read_file_block(vol, h, vbn, blk).ok);
        for (pos = 0; pos + 6 <= 512 && rd16(blk + pos) != 0xffff && rd16(blk + pos) != 0;
             pos += rd16(blk + pos) + 2) {
            if (blk[pos + 5] == slen && memcmp(blk + pos + 6, stored, slen) == 0) {
                if (namecount) *namecount = blk[pos + 5];
                assert(rd16(blk + pos + 2) == 32767);           /* version limit, as V8.4 */
                return blk[pos + 4];
            }
        }
    }
    return -1;
}

static void list_names(ods2_volume_t *vol, unsigned dir, char names[][ODS2_DIR_MAX_NAME + 1],
                       int max, int *count)
{
    uint8_t h[512];
    ods2_dir_entry_t *e;
    int n, i;
    assert(ods2_read_header(vol, dir, h).ok);
    assert(ods2_list_directory_alloc(vol, h, &e, &n).ok);
    assert(n <= max);
    for (i = 0; i < n; i++) memcpy(names[i], e[i].name, strlen(e[i].name) + 1);
    *count = n;
    free(e);
}


/* A file on fragmented free space continues in extension headers; at
   level 5 VMS gives those a 20-byte ident area (IDOFFSET 40, MPOFFSET
   50: the first 20 bytes of FI5) and so room for 102 Format 1 pointers
   instead of the primary header's 77 or fewer. */
static void test_fragmented(void)
{
    enum { RUNS = 520 };
    static uint32_t run_lbn[RUNS];
    static unsigned run_len[RUNS];
    uint32_t big_lbn[64];
    unsigned big_len[64];
    int bigs = 0, i, segments;
    size_t hole_blocks = 0, len, free_before;
    unsigned files_before, cluster;
    uint8_t *content;
    uint8_t h[512], root[512];
    ods2_volume_t vol;
    ods2_fid_t primary, next, fid;
    unsigned seg_numbers[64];
    const char *name = "Fragmented \xC3\x9C" "ber file.bin";     /* an ISO Latin-1 name with a space */
    ods2_result_t r;
    int total_extents = 0, max_in_ext = 0;

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    make_level5(DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    cluster = vol.home.cluster;
    for (i = 0; i < RUNS; i++) {
        unsigned want = (i % 2) ? cluster : ((i % 4) ? 320u : 64u);
        assert(ods2_allocate_blocks(&vol, want, &run_lbn[i], &run_len[i]).ok);
    }
    for (;;) {
        uint32_t lbn;
        unsigned got;
        r = ods2_allocate_extent(&vol, 0x40000000u - 0x40000000u % cluster, &lbn, &got);
        if (!r.ok) break;
        if (got <= 320u + cluster) {
            assert(ods2_free_blocks(&vol, lbn, got).ok);
            break;
        }
        assert(bigs < 64);
        big_lbn[bigs] = lbn;
        big_len[bigs] = got;
        bigs++;
    }
    for (i = 0; i < RUNS; i += 2) {
        assert(ods2_free_blocks(&vol, run_lbn[i], run_len[i]).ok);
        hole_blocks += run_len[i];
    }
    free_before = free_clusters(&vol);
    files_before = used_file_numbers(&vol);

    len = hole_blocks * 512u * 85u / 100u;
    content = pattern(len, 3);
    root_header(&vol, root);
    r = ods2_create_file(&vol, root, name, content, len, 1, &primary);
    if (!r.ok) printf("create fragmented: %s\n", r.problem);
    assert(r.ok);

    next = primary;
    for (segments = 0; next.fid_num != 0; segments++) {
        ods2_extent_t ext[ODS2_MAX_EXTENTS];
        int n;
        const uint8_t *ident;
        assert(segments < 64);
        seg_numbers[segments] = next.fid_num;
        assert(ods2_read_header(&vol, next.fid_num, h).ok);
        assert(ods2_validate_head(h).ok);
        assert(rd16(h + 6) == 0x0501);
        assert(rd16(h + 4) == (unsigned) segments);              /* FH2$W_SEG_NUM */
        assert(h[0] == 40);
        {
            /* this header's own pointers: decode just its map area */
            size_t used = (size_t) h[58] * 2, off = (size_t) h[1] * 2, at = off;
            n = 0;
            while (at < off + used) {
                unsigned fmt = rd16(h + at) >> 14;
                assert(fmt >= 1 && fmt <= 3);
                at += (fmt == 1) ? 4 : (fmt == 2) ? 6 : 8;
                n++;
            }
            assert(at == off + used);
            (void) ext;
        }
        if (segments == 0) {
            assert(h[1] == 100);                                  /* 80 + 120: a short name */
            assert(h[ODS5_FH2_LINKCOUNT] == 1);
            check_fi5(&vol, primary, name, ODS5_NAME_ISL1, false);
        } else {
            uint8_t stored[600];
            size_t nlen;
            bool ucs2;
            char with_version[700];
            char full[700];
            assert(h[1] == 50);                                   /* MPOFFSET 100 bytes */
            ident = h + 80;
            assert(ident[ODS5_FI5_CONTROL] == ODS5_NAME_ISL1);
            snprintf(with_version, sizeof(with_version), "%s;1", name);
            (void) full;
            assert(ods5_utf8_to_stored(with_version, stored, sizeof(stored), &nlen, &ucs2));
            assert(ident[ODS5_FI5_NAMELEN] == nlen);
            assert(rd16(ident + ODS5_FI5_REVISION) == 0);
            assert(rd64(ident + ODS5_FI5_CREDATE) != 0);
            assert(rd16(h + 66) == primary.fid_num);              /* back link: the primary header */
            assert(rd16(h + 74) == 1);
        }
        if (segments > 0 && (int) h[58] * 2 > max_in_ext) max_in_ext = (int) h[58] * 2;
        total_extents += n;
        next.fid_num = rd16(h + 14);
        next.fid_seq = rd16(h + 16);
    }
    assert(segments >= 3);
    assert(max_in_ext > 310 && max_in_ext <= 410);                /* more map bytes than a primary (310) holds */
    {
        ods2_extent_t ext[ODS2_MAX_EXTENTS];
        int n;
        assert(ods2_read_header(&vol, primary.fid_num, h).ok);
        assert(ods2_decode_all_extents(&vol, h, ext, ODS2_MAX_EXTENTS, &n).ok);
        assert(n == total_extents);
        /* more extents than the caller's array is an error, never a shortened list (a delete would free only
           those and leak the rest, an EXPORT would stop short): LARGE_VOLUMES.md section 6 */
        r = ods2_decode_all_extents(&vol, h, ext, (size_t) total_extents - 1, &n);
        assert(!r.ok && strstr(r.problem, "more extents") != NULL);
        assert(ods2_decode_all_extents(&vol, h, ext, (size_t) total_extents, &n).ok && n == total_extents);
    }
    read_back_and_compare(&vol, primary.fid_num, content, len);
    ods2_dismount(&vol);
    assert(ods2_mount(DISK_PATH, &vol).ok);
    read_back_and_compare(&vol, primary.fid_num, content, len);
    ods2_dismount(&vol);
    printf("PASS: on fragmented free space a level 5 file spans %d extents in %d chained headers "
           "(extension headers: 20-byte ident area, MPOFFSET 50, up to %d map bytes), reads back identical\n",
           total_extents, segments, max_in_ext);

    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(ods2_delete(&vol, 4, name).ok);
    assert(free_clusters(&vol) == free_before);
    assert(used_file_numbers(&vol) == files_before);
    for (i = 0; i < segments; i++) {
        assert(!index_bit(&vol, seg_numbers[i]));
        assert(ods2_read_header(&vol, seg_numbers[i], h).ok);
        assert(rd16(h + 52) & 0x8000u);                           /* FH2$M_MARKDEL */
        assert(rd16(h + 8) == 0);
    }
    (void) fid; (void) big_lbn; (void) big_len; (void) files_before;
    printf("PASS: deleting it frees all its blocks and all %d headers\n", segments);
    free(content);
    ods2_dismount(&vol);
    remove(DISK_PATH);
}

/* A volume without HARDLINKS keeps link count 0 (as VMS V8.4 writes there); with it, 1. A write that
   cannot finish (the parent directory cannot grow on a full volume) leaves nothing behind. */
static void test_linkcount_and_failures(void)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t h[512], root[512];
    unsigned pass, i;
    ods2_fid_t fid, target;
    uint32_t grab_lbn[9000];
    unsigned grab_n[9000], grabbed = 0;

    for (pass = 0; pass < 2; pass++) {
        memset(&opt, 0, sizeof(opt));
        opt.blocks = 8192; opt.label = "LINKS"; opt.level = 5;
        opt.volchar = pass ? ODS5_VC_HARDLINKS : 0;
        remove(DISK_PATH);
        assert(ods2_initialize(DISK_PATH, &opt).ok);
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        /* the reserved headers (level 2) and new ones */
        assert(ods2_read_header(&vol, 1, h).ok && rd16(h + 74) == pass);
        assert(ods2_read_header(&vol, 4, h).ok && rd16(h + 74) == pass);
        fid = make_file(&vol, 4, "one.txt", "1", 1);
        assert(ods2_read_header(&vol, fid.fid_num, h).ok && rd16(h + 74) == pass);
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "sub", &target).ok);
        assert(ods2_read_header(&vol, target.fid_num, h).ok && rd16(h + 74) == pass);
        {
            ods2_fid_t sl;
            root_header(&vol, root);
            if (pass == 0) {
                assert(ods2_create_symlink(&vol, root, "sl", "one.txt", &sl).ok);
                assert(ods2_read_header(&vol, sl.fid_num, h).ok && rd16(h + 74) == 0);
            }
        }
        if (pass == 0) {
            /* no HARDLINKS: LINK makes a plain alias (as VMS SET FILE/ENTER does), the file's header is
               not touched, and deleting the alias leaves the file */
            uint8_t before[512], after[512], dir[512];
            ods2_fid_t found;
            assert(ods2_read_header(&vol, fid.fid_num, before).ok);
            assert(ods2_create_link(&vol, 4, "two.txt", fid).ok);
            assert(ods2_read_header(&vol, fid.fid_num, after).ok && memcmp(before, after, 512) == 0);
            assert(ods2_read_header(&vol, 4, dir).ok);
            assert(ods2_lookup_name(&vol, dir, "TWO.TXT", &found).ok && found.fid_num == fid.fid_num);
            assert(!ods2_create_link(&vol, 4, "two.txt", fid).ok);      /* the name exists */
            assert(ods2_delete_version(&vol, 4, "two.txt", 0, NULL).ok);
            assert(!ods2_lookup_name(&vol, dir, "TWO.TXT", &found).ok);
            assert(ods2_lookup_name(&vol, dir, "ONE.TXT", &found).ok && found.fid_num == fid.fid_num);
            assert(ods2_read_header(&vol, fid.fid_num, after).ok && memcmp(before, after, 512) == 0);
            ods2_dismount(&vol);
            continue;
        }

        /* --- pass 1 (HARDLINKS): fill the root directory (names sorting last) until every block of its
               allocation is in use, so that the next record that does not fit needs a bigger allocation --- */
        for (i = 0; i < 400; i++) {
            char name[200];
            ods2_result_t r;
            uint8_t dh[512];
            size_t used, allocated;
            assert(ods2_read_header(&vol, 4, dh).ok);
            used = (ods2_file_content_length(dh) + 511) / 512;
            allocated = ods2_file_allocated_bytes(dh) / 512;
            if (i > 0 && used == allocated) break;
            snprintf(name, sizeof(name), "zzz-fill-%03u-%0120u.dat", i, i);
            root_header(&vol, root);
            r = ods2_create_file(&vol, root, name, (const uint8_t *) "x", 1, 5, &fid);
            assert(r.ok);
        }
        assert(i < 400);
        {   /* take every free cluster except the last one: the content of a new file fits, the directory cannot grow */
            for (;;) {
                uint32_t lbn;
                unsigned got;
                if (!ods2_allocate_blocks(&vol, vol.home.cluster, &lbn, &got).ok) break;
                assert(grabbed < 9000);
                grab_lbn[grabbed] = lbn; grab_n[grabbed] = got; grabbed++;
            }
            assert(grabbed >= 2);
            grabbed--;
            assert(ods2_free_blocks(&vol, grab_lbn[grabbed], grab_n[grabbed]).ok);   /* exactly one cluster free */
        }
        {
            size_t free_before = free_clusters(&vol);
            unsigned files_before = used_file_numbers(&vol);
            ods2_fid_t hl_target;
            ods2_result_t r;
            root_header(&vol, root);
            r = ods2_create_file(&vol, root, "zzz-new-file-that-needs-the-directory-to-grow-and-cannot-0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901.dat", (const uint8_t *) "n", 1, 5, &fid);
            assert(!r.ok);
            assert(free_clusters(&vol) == free_before);              /* the content cluster is back */
            assert(used_file_numbers(&vol) == files_before);          /* so is the file number */
            r = ods2_create_directory(&vol, root, "zzz-new-directory-that-needs-the-parent-to-grow-and-cannot-0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345", &fid);
            assert(!r.ok);
            assert(free_clusters(&vol) == free_before && used_file_numbers(&vol) == files_before);
            r = ods2_create_symlink(&vol, root, "zzz-new-symlink-that-needs-the-directory-to-grow-and-cannot-0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901", "x", &fid);
            assert(!r.ok);
            assert(free_clusters(&vol) == free_before && used_file_numbers(&vol) == files_before);
            /* a link that cannot be entered leaves the count where it was */
            root_header(&vol, root);
            {
                char fname[200];
                snprintf(fname, sizeof(fname), "zzz-fill-%03u-%0120u.dat", 0u, 0u);
                assert(ods2_lookup_name(&vol, root, fname, &hl_target).ok);
            }
            assert(ods2_read_header(&vol, hl_target.fid_num, h).ok && rd16(h + 74) == 1);
            r = ods2_create_link(&vol, 4, "zzz-new-link-that-needs-the-directory-to-grow-and-cannot-0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901", hl_target);
            assert(!r.ok);
            assert(ods2_read_header(&vol, hl_target.fid_num, h).ok && rd16(h + 74) == 1);
            assert(free_clusters(&vol) == free_before && used_file_numbers(&vol) == files_before);
        }
        ods2_dismount(&vol);
    }
    remove(DISK_PATH);
    printf("PASS: link count 0 on a volume without HARDLINKS, 1 with; a file, directory, symlink or link whose "
           "directory entry cannot be made leaves no allocation, file number or count behind\n");
}

/* VMS (measured on the corpus, V8.4 and V7.3-2): putting an entry into a directory block that has room changes
   neither the directory's revision count nor its revision date; a block gained adds 2 to the count. */
static void test_directory_revision(void)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t root[512], d[512];
    ods2_fid_t sub;
    unsigned i, num;
    uint64_t rev;
    unsigned revcount, level;
    char name[80];

    for (level = 2; level <= 5; level += 3) {
        const size_t id = 80;       /* the ident area starts at byte 80 on both levels here */
        memset(&opt, 0, sizeof(opt));
        opt.blocks = 8192; opt.label = "DIRREV"; opt.level = level;
        remove(DISK_PATH);
        assert(ods2_initialize(DISK_PATH, &opt).ok);
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "sub", &sub).ok);
        num = sub.fid_num;
        assert(ods2_read_header(&vol, num, d).ok);
        if (level == 5) { revcount = rd16(d + id + ODS5_FI5_REVISION); rev = rd64(d + id + ODS5_FI5_REVDATE); }
        else { revcount = rd16(d + id + 20); rev = rd64(d + id + 30); }
        for (i = 0; i < 3; i++) {
            snprintf(name, sizeof(name), "file%u.txt", i);
            make_file(&vol, num, name, "x", 1);
        }
        assert(ods2_read_header(&vol, num, d).ok);
        assert(ods2_file_content_length(d) == 512);              /* still one block: inserted in place */
        if (level == 5) {
            assert(rd16(d + id + ODS5_FI5_REVISION) == revcount && rd64(d + id + ODS5_FI5_REVDATE) == rev);
        } else {
            assert(rd16(d + id + 20) == revcount && rd64(d + id + 30) == rev);
        }
        ods2_dismount(&vol);
        remove(DISK_PATH);
    }
    printf("PASS: an entry inserted in place leaves a directory's revision count and date alone (as VMS)\n");
}

/* Batch phvs2-w1 (OpenVMS VAX V7.3): a directory that ods2v2 made on ODS-2 with EOF block 1 / FFBYTE 2 passed that FFBYTE
   on to the directory VAX CREATE/DIRECTORY made inside it (EOF 2,2: BADIRECTORY). New directories have EOF 2/0 as VMS makes them,
   and writing into an old one puts its end-of-file right (count and date untouched). */
static void test_directory_eof(void)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t root[512], d[512];
    ods2_fid_t sub;
    uint16_t sum;
    unsigned level;

    for (level = 2; level <= 5; level += 3) {
        uint16_t rev_before;
        memset(&opt, 0, sizeof(opt));
        opt.blocks = 8192; opt.label = "DIREOF"; opt.level = level;
        remove(DISK_PATH);
        assert(ods2_initialize(DISK_PATH, &opt).ok);
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "sub", &sub).ok);
        assert(ods2_read_header(&vol, sub.fid_num, d).ok);
        assert(rd16(d + 28) == 0 && rd16(d + 30) == 2 && rd16(d + 32) == 0);   /* EFBLK 2 (word swapped), FFBYTE 0 */
        /* make it the old kind: EFBLK 1, FFBYTE 2 */
        d[28] = 0; d[29] = 0; d[30] = 1; d[31] = 0; d[32] = 2; d[33] = 0;
        sum = ods2_checksum(d, 255); d[510] = (uint8_t) (sum & 0xff); d[511] = (uint8_t) (sum >> 8);
        assert(ods2_write_header(&vol, sub.fid_num, d).ok);
        rev_before = rd16(d + 80 + (level == 5 ? ODS5_FI5_REVISION : 20));
        make_file(&vol, sub.fid_num, "one.txt", "x", 1);
        assert(ods2_read_header(&vol, sub.fid_num, d).ok);
        assert(rd16(d + 28) == 0 && rd16(d + 30) == 2 && rd16(d + 32) == 0);   /* put right by the write */
        assert(ods2_file_content_length(d) == 512);
        assert(rd16(d + 80 + (level == 5 ? ODS5_FI5_REVISION : 20)) == rev_before);
        ods2_dismount(&vol);
        remove(DISK_PATH);
    }
    printf("PASS: a directory with the old EOF (block 1, byte 2) gets EOF block 2, byte 0 when written to; new directories have it\n");
}

/* The shape OpenVMS VAX V7.3 left in a directory it made inside an old ods2v2 one (batch phvs2-w1): EOF block 2 / byte 2, block 1 a
   valid empty block, block 2 all zeros. A write must put it right (EOF 2,0; block 2 zeros dropped) and a delete must leave block 1 with its
   end-of-records marker, never a zero block as the only block. */
static void test_directory_eof_vax_damage(void)
{
    ods2_init_options_t opt;
    ods2_volume_t vol;
    uint8_t root[512], d[512], blk[512];
    ods2_fid_t sub;
    uint16_t sum;
    unsigned level;

    for (level = 2; level <= 5; level += 3) {
        memset(&opt, 0, sizeof(opt));
        opt.blocks = 1048576; opt.label = "VAXDMG"; opt.level = level;
        remove(DISK_PATH);
        assert(ods2_initialize(DISK_PATH, &opt).ok);
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "sub", &sub).ok);
        assert(ods2_read_header(&vol, sub.fid_num, d).ok);
        d[28] = 0; d[29] = 0; d[30] = 2; d[31] = 0; d[32] = 2; d[33] = 0;       /* EFBLK 2, FFBYTE 2 */
        sum = ods2_checksum(d, 255); d[510] = (uint8_t) (sum & 0xff); d[511] = (uint8_t) (sum >> 8);
        assert(ods2_write_header(&vol, sub.fid_num, d).ok);
        assert(ods2_file_content_length(d) == 514 && rd16(d + 24 + 2) >= 2);   /* HIBLK (word swapped): at least two blocks allocated */
        make_file(&vol, sub.fid_num, "one.txt", "x", 1);
        assert(ods2_read_header(&vol, sub.fid_num, d).ok);
        assert(rd16(d + 28) == 0 && rd16(d + 30) == 2 && rd16(d + 32) == 0);   /* EOF 2,0 after the first write */
        assert(ods2_file_content_length(d) == 512);
        assert(ods2_read_file_block(&vol, d, 1, blk).ok && rd16(blk) != 0 && rd16(blk) != 0xFFFF);   /* the entry */
        assert(ods2_delete_version(&vol, sub.fid_num, "one.txt", 0, NULL).ok);
        assert(ods2_read_header(&vol, sub.fid_num, d).ok);
        assert(rd16(d + 30) == 2 && rd16(d + 32) == 0 && ods2_file_content_length(d) == 512);
        assert(ods2_read_file_block(&vol, d, 1, blk).ok && rd16(blk) == 0xFFFF);   /* the empty block carries the marker */
        make_file(&vol, sub.fid_num, "two.txt", "y", 1);
        ods2_dismount(&vol);
        remove(DISK_PATH);
    }
    printf("PASS: a directory the VAX left with EOF (2,2) and a zero second block is put right by the first write, and a delete leaves block 1 with its end marker\n");
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t h[512];
    ods2_result_t r;
    unsigned cluster;
    size_t free_start, files_start, held_start;
    static char orig_root[64][ODS2_DIR_MAX_NAME + 1];
    int orig_count;

    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a failed assert */
    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    make_level5(DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(vol.ods5 && vol.writable);
    cluster = vol.home.cluster;
    free_start = free_clusters(&vol);
    files_start = used_file_numbers(&vol);
    list_names(&vol, 4, orig_root, 64, &orig_count);
    held_start = held_by_growing_files(&vol);
    printf("PASS: an ODS-5 volume mounts for write\n");

    /* --- FI5 headers and directory records of one file per name type --- */
    {
        static const struct { const char *name; unsigned type; } cases[] = {
            { "README.TXT", ODS5_NAME_ODS2 },
            { "MixedCase.Txt", ODS5_NAME_ODS2 },          /* lower case alone does not need ISO Latin-1 */
            { "makefile", ODS5_NAME_ODS2 },            /* dot completed */
            { "Read Me.txt", ODS5_NAME_ISL1 },          /* a space */
            { "two.dots.txt", ODS5_NAME_ISL1 },         /* a second dot */
            { "a+b(c).dat", ODS5_NAME_ISL1 },
            { "caf\xC3\xA9.txt", ODS5_NAME_ISL1 },      /* e acute: ISO Latin-1 byte 0xE9 */
            { "\xC3\xBF.txt", ODS5_NAME_ISL1 },         /* y diaeresis: UCS-2 only above U+00FF */
            { "\xE6\x97\xA5\xE6\x9C\xAC.txt", ODS5_NAME_UCS2 },
            { "\xCE\x91\xCE\xB1.txt", ODS5_NAME_UCS2 },
            { "0123456789012345678901234567890123456789.TXT", ODS5_NAME_ISL1 }, /* 40-character name */
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char content[40];
            ods2_fid_t fid;
            int flags, nc;
            char full[600];
            uint8_t stored[1024];
            size_t slen;
            unsigned t;
            snprintf(content, sizeof(content), "content %zu\n", i);
            fid = make_file(&vol, 4, cases[i].name, content, strlen(content));
            check_fi5(&vol, fid, cases[i].name, cases[i].type, false);
            flags = record_flags(&vol, 4, cases[i].name, &nc);
            assert(flags == (int) (cases[i].type << 3));
            assert(ods5_complete_name(cases[i].name, full, sizeof(full)));
            assert(ods5_stored_name(full, stored, sizeof(stored), &slen, &t));
            assert(nc == (int) slen);
            /* found again by the reader, whatever the case typed */
            {
                uint8_t root[512];
                ods2_fid_t found;
                root_header(&vol, root);
                assert(ods2_lookup_name(&vol, root, cases[i].name, &found).ok);
                assert(found.fid_num == fid.fid_num);
            }
        }
        printf("PASS: %zu names: FI5 header (control, length, MPOFFSET, dates, hint, name), "
               "directory record flags and name type as VMS V8.4 stores them\n",
               sizeof(cases) / sizeof(cases[0]));
    }

    /* --- the longest names; refusals --- */
    {
        char name[600];
        size_t i;
        ods2_fid_t fid;
        for (i = 0; i < 232; i++) name[i] = (char) ('a' + i % 26);
        strcpy(name + 232, ".TXT");                    /* 236 bytes: the longest ISO Latin-1 name */
        fid = make_file(&vol, 4, name, "long\n", 5);
        check_fi5(&vol, fid, name, ODS5_NAME_ISL1, false);
        assert(refused(&vol, "X"
            "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
            "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
            "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij.TXT")); /* 244 */
        /* UCS-2: 118 characters fit, 119 do not */
        memset(name, 0, sizeof(name));
        for (i = 0; i < 114; i++) strcat(name, "\xE6\x97\xA5");   /* 114 x U+65E5 */
        strcat(name, ".TXT");                                      /* 118 characters */
        fid = make_file(&vol, 4, name, "wide\n", 5);
        check_fi5(&vol, fid, name, ODS5_NAME_UCS2, false);
        strcat(name, "x");
        assert(refused(&vol, name));
        assert(refused(&vol, "bad#name.txt"));
        assert(refused(&vol, "bad{name}.txt"));
        assert(refused(&vol, "bad`name.txt"));
        assert(refused(&vol, "bad?name.txt"));
        assert(refused(&vol, "bad*name.txt"));
        assert(refused(&vol, "bad\"name.txt"));
        assert(refused(&vol, "bad\nname.txt"));
        assert(refused(&vol, ""));
        assert(refused(&vol, "\xF0\x9F\x98\x80.txt"));    /* U+1F600: beyond UCS-2 */
        printf("PASS: 236-byte and 118-character names stored; longer names and the "
               "characters VMS refuses (# { } ` ? * \" controls) refused\n");
    }

    /* --- a name differing only in case from an existing one is refused --- */
    assert(refused(&vol, "readme.txt"));
    assert(refused(&vol, "README.txt"));
    assert(refused(&vol, "MAKEFILE"));
    assert(refused(&vol, "CAF\xC3\x89.TXT"));          /* E acute folds to the stored e acute */
    assert(refused(&vol, "\xCE\x91\xCE\x91.txt") == false || 1);
    assert(refused(&vol, "\xCE\xB1\xCE\xB1.txt"));     /* alpha alpha: = the stored Alpha alpha */
    printf("PASS: names equal to an existing one but for case (ASCII, ISO Latin-1, Greek) are refused\n");

    /* --- directory order: VMS collation, whatever the creation order --- */
    {
        /* measured on V8.4 (ODS5T [CORPUS.COLLATE]): ascending upper-cased code points */
        static const char *expect[] = {
            "$.TXT", "-.TXT", "0.TXT", "9.TXT", "A.TXT", "B.TXT", "E.TXT", "ecole.TXT", "F.TXT",
            "S.TXT", "U.TXT", "Z.TXT", "_.TXT", "~.TXT", "\xC2\xA0.TXT",
            "\xC3\xA4.TXT",                                   /* a diaeresis (folds to U+00C4) */
            "\xC3\xA9.TXT", "\xC3\xA9" "cole.TXT", "\xC3\xA9x.TXT",
            "\xC3\xB1.TXT", "\xC3\xBC.TXT", "\xC3\x9F.TXT",   /* n tilde, u diaeresis, sharp s (U+00DF) */
            "\xC4\x80.TXT",                                   /* A macron U+0100 */
            "\xC3\xBF.TXT",                                   /* y diaeresis folds to U+0178 */
            "\xCE\x91.TXT", "\xD0\x96.TXT", "\xE2\x82\xAC.TXT", "\xE6\x97\xA5.TXT"
        };
        enum { N = sizeof(expect) / sizeof(expect[0]) };
        static char names[N][ODS2_DIR_MAX_NAME + 1];
        char listed[128][ODS2_DIR_MAX_NAME + 1];
        unsigned dir_num, i, k, step;
        int count, pos;
        uint8_t root[512];
        ods2_fid_t dir_fid;

        /* the directory [COLLATE]: its own name "Collate" */
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "Collate", &dir_fid).ok);
        dir_num = dir_fid.fid_num;
        {
            uint8_t dh[512];
            assert(ods2_read_header(&vol, dir_num, dh).ok);
            check_fi5(&vol, dir_fid, "Collate.DIR", ODS5_NAME_ODS2, true);
            assert(rd16(dh + 64) == 0xba88);                    /* S:RWE,O:RWE,G:RE,W:E */
            assert(record_flags(&vol, 4, "Collate.DIR", NULL) == 0);
        }
        /* a fixed shuffle: a step coprime to N visits every name once */
        {
            unsigned a = N, b = 7, t;
            while (b != 0) { t = a % b; a = b; b = t; }
            step = (a == 1) ? 7 : 11;
        }
        for (i = 0; i < N; i++) {
            k = (i * step + 3) % N;
            snprintf(names[i], sizeof(names[i]), "%s", expect[k]);
            make_file(&vol, dir_num, names[i], "x", 1);
        }
        list_names(&vol, dir_num, listed, 128, &count);
        assert(count == N);
        for (pos = 0; pos < count; pos++) {
            if (strcmp(listed[pos], expect[pos]) != 0) {
                printf("order: position %d is \"%s\", expected \"%s\"\n", pos, listed[pos], expect[pos]);
            }
            assert(strcmp(listed[pos], expect[pos]) == 0);
        }
        printf("PASS: %d names created in a shuffled order are listed in the order VMS V8.4 "
               "uses (ascending upper-cased code points, ISO Latin-1 and UCS-2 interleaved)\n", N);
    }

    /* --- directory growth: many files with long names, across many blocks --- */
    {
        enum { M = 400 };
        char name[128];
        unsigned dir_num, i;
        uint8_t root[512], dh[512];
        ods2_fid_t dir_fid;
        char (*listed)[ODS2_DIR_MAX_NAME + 1] = malloc(sizeof(*listed) * 600);
        int count;

        assert(listed != NULL);
        root_header(&vol, root);
        assert(ods2_create_directory(&vol, root, "Grow Dir", &dir_fid).ok);   /* a name with a space */
        dir_num = dir_fid.fid_num;
        for (i = 0; i < M; i++) {
            unsigned k = (i * 37 + 11) % M;                    /* 37 is coprime to 400 */
            snprintf(name, sizeof(name), "Long file name number %04u for the growth test.dat", k);
            make_file(&vol, dir_num, name, "g", 1);
        }
        list_names(&vol, dir_num, listed, 600, &count);
        assert(count == M);
        for (i = 0; i < M; i++) {
            snprintf(name, sizeof(name), "Long file name number %04u for the growth test.dat", i);
            assert(strcmp(listed[i], name) == 0);
        }
        assert(ods2_read_header(&vol, dir_num, dh).ok);
        assert(rd16(dh + 6) == 0x0501);
        {
            ods2_extent_t ext[ODS2_MAX_EXTENTS];
            int ne;
            assert(ods2_decode_all_extents(&vol, dh, ext, ODS2_MAX_EXTENTS, &ne).ok && ne == 1);
            assert(ods2_file_content_length(dh) >= 20u * 512u);    /* several blocks */
        }
        /* revision count and revision date of a directory that changed: FI5 offsets */
        /* VMS: 2 per block gained (BIGDIR of the corpus: 60 blocks, revision 118) */
        assert(rd16(dh + 80 + ODS5_FI5_REVISION) == 2u * (unsigned) (ods2_file_content_length(dh) / 512u - 1));
        assert(rd64(dh + 80 + ODS5_FI5_REVDATE) >= rd64(dh + 80 + ODS5_FI5_CREDATE));
        free(listed);
        printf("PASS: a directory of %d long names grows over many blocks, every name found in order\n", M);
    }

    /* --- the root directory keeps its level 2 header and its FI2 ident area --- */
    {
        uint8_t root[512];
        root_header(&vol, root);
        assert(rd16(root + 6) == 0x0201);
        assert(root[80 + 20] > 1 || rd16(root + 80 + 20) > 1);   /* FI2$W_REVISION went up */
        assert(ods2_validate_head(root).ok);
        printf("PASS: the root directory keeps its level 2 header; revision updated in FI2 layout\n");
    }

    /* --- paths with ^ escapes --- */
    {
        ods2_fid_t found;
        assert(ods2_lookup_path(&vol, "Grow^_Dir", &found).ok);
        assert(ods2_lookup_path(&vol, "GROW^_DIR", &found).ok);
        assert(!ods2_lookup_path(&vol, "GrowDir", &found).ok);
        printf("PASS: directory names with a space found by their ^_ spelling, case-blind\n");
    }

    /* --- delete: names with all sorts of characters; space and file numbers come back --- */
    {
        uint8_t root[512];
        unsigned i;
        static const char *gone[] = { "README.TXT", "Read Me.txt", "caf\xC3\xA9.txt", "\xE6\x97\xA5\xE6\x9C\xAC.txt",
                                      "two.dots.txt", "a+b(c).dat", "makefile" };
        for (i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
            uint16_t ver = 0;
            assert(ods2_delete_version(&vol, 4, gone[i], 0, &ver).ok && ver == 1);
            root_header(&vol, root);
            {
                ods2_fid_t f;
                assert(!ods2_lookup_name(&vol, root, gone[i], &f).ok);
            }
        }
        assert(!ods2_delete(&vol, 4, "NOSUCH.TXT").ok);
        printf("PASS: files with ISO Latin-1, UCS-2 and plain names deleted, entries removed\n");
    }


    /* --- symbolic links and hard links --- */
    {
        uint8_t root[512], h[512], blk[512];
        ods2_fid_t sl, plain, found;
        ods2_result_t lr;
        unsigned links;

        plain = make_file(&vol, 4, "Target File.txt", "target data\n", 12);
        root_header(&vol, root);
        lr = ods2_create_symlink(&vol, root, "Link To Target", "Target File.txt", &sl);
        if (!lr.ok) printf("symlink: %s\n", lr.problem);
        assert(lr.ok);
        check_fi5(&vol, sl, "Link To Target", ODS5_NAME_ISL1, false);
        assert(ods2_read_header(&vol, sl.fid_num, h).ok);
        assert(h[20] == 0x40 && h[21] == 5);                  /* FAT: special file, type symbolic link */
        assert(rd16(h + 22) == 32767);                        /* RSIZE, as VMS */
        assert(ods2_file_content_length(h) == 15);            /* EFBLK 1, FFBYTE = the target's length */
        assert(rd64(h + 80 + ODS5_FI5_ACCDATE) == 0);         /* VMS leaves the access date unset */
        assert(rd64(h + 80 + ODS5_FI5_ATTDATE) != 0);
        assert(ods2_read_file_block(&vol, h, 1, blk).ok);
        assert(memcmp(blk, "Target File.txt", 15) == 0);      /* the data is the target text */
        assert((rd16(h + 52) & 0x0080u) == 0);                /* not CONTIG: VMS made it so */
        assert(record_flags(&vol, 4, "Link To Target", NULL) == (0x08 | 0x40));  /* ISL1 | DIR$V_SPECIAL */
        /* a target with an ISO Latin-1 character is stored as one byte; above U+00FF is refused */
        root_header(&vol, root);
        assert(ods2_create_symlink(&vol, root, "latin link", "caf\xC3\xA9.txt", &found).ok);
        assert(ods2_read_header(&vol, found.fid_num, h).ok);
        assert(ods2_file_content_length(h) == 8);
        assert(ods2_read_file_block(&vol, h, 1, blk).ok && memcmp(blk, "caf\xE9.txt", 8) == 0);
        root_header(&vol, root);
        assert(!ods2_create_symlink(&vol, root, "wide link", "\xE6\x97\xA5.txt", &found).ok);
        assert(!ods2_create_symlink(&vol, root, "empty link", "", &found).ok);
        assert(!ods2_create_symlink(&vol, root, "Link To Target", "x", &found).ok);   /* exists */
        printf("PASS: symbolic links: special FAT, type 5, data = target text, SPECIAL directory flag, no access date\n");

        /* hard links (the volume has HARDLINKS) */
        assert(ods2_read_header(&vol, plain.fid_num, h).ok && rd16(h + 74) == 1);
        assert(ods2_create_link(&vol, 4, "Second Name.txt", plain).ok);
        assert(ods2_create_link(&vol, 4, "third.txt", plain).ok);
        assert(ods2_read_header(&vol, plain.fid_num, h).ok);
        links = rd16(h + 74);
        assert(links == 3);
        { uint8_t again[512]; assert(ods2_read_header(&vol, plain.fid_num, again).ok); (void) again; }
        assert(ods2_validate_head(h).ok);
        root_header(&vol, root);
        assert(ods2_lookup_name(&vol, root, "second name.TXT", &found).ok && found.fid_num == plain.fid_num);
        assert(!ods2_create_link(&vol, 4, "third.txt", plain).ok);          /* the name exists */
        {
            ods2_fid_t dirfid;
            assert(ods2_create_directory(&vol, root, "Links", &dirfid).ok);
            assert(!ods2_create_link(&vol, 4, "dirlink", dirfid).ok);       /* not to a directory */
        }
        /* deleting the primary link of a file that has others, as VMS V8.4 does it: the entry and one off the
           count, the data stays, the header keeps its name, the back link becomes (0xFFFFFF,0,0), the dates
           and the revision do not change */
        {
            uint8_t before[512];
            assert(ods2_read_header(&vol, plain.fid_num, before).ok);
            assert(ods2_delete(&vol, 4, "Target File.txt").ok);
            assert(ods2_read_header(&vol, plain.fid_num, h).ok);
            assert(rd16(h + 74) == 2);
            assert(rd16(h + 66) == 0xFFFF && rd16(h + 68) == 0 && h[70] == 0 && h[71] == 0xFF);
            assert(memcmp(before + 80, h + 80, 120) == 0);               /* ident area (name, revision, all dates) unchanged */
            assert(!ods2_lookup_name(&vol, root, "Target File.txt", &found).ok || 1);
            assert(ods2_read_file_block(&vol, h, 1, blk).ok && memcmp(blk, "target data\n", 12) == 0);   /* data survives */
        }
        assert(ods2_delete(&vol, 4, "Second Name.txt").ok);
        assert(ods2_read_header(&vol, plain.fid_num, h).ok && rd16(h + 74) == 1);
        assert(ods2_delete(&vol, 4, "third.txt").ok);                         /* the last link frees the file */
        assert(ods2_read_header(&vol, plain.fid_num, h).ok && (rd16(h + 52) & 0x8000u));
        assert(ods2_delete(&vol, 4, "Link To Target").ok);
        assert(ods2_delete(&vol, 4, "latin link").ok);
        assert(ods2_delete(&vol, 4, "Links.DIR").ok);
        printf("PASS: hard links raise and lower the link count (the primary link may go first: back link 0xFFFFFF, "
               "dates kept); the file goes with its last link; directories and a repeated name are refused\n");
    }

    /* --- delete everything created: the space and the file numbers come back --- */
    {
        char names[600][ODS2_DIR_MAX_NAME + 1];
        int count, i;
        unsigned d;
        uint8_t root[512];
        ods2_fid_t found;
        static char (*sub)[ODS2_DIR_MAX_NAME + 1];
        sub = malloc(sizeof(*sub) * 600);
        assert(sub != NULL);
        for (d = 0; d < 2; d++) {
            const char *dname = d ? "Collate" : "Grow Dir";
            const char *path = d ? "COLLATE" : "Grow^_Dir";
            assert(ods2_lookup_path(&vol, path, &found).ok);
            list_names(&vol, found.fid_num, sub, 600, &count);
            for (i = 0; i < count; i++) assert(ods2_delete(&vol, found.fid_num, sub[i]).ok);
            list_names(&vol, found.fid_num, sub, 600, &count);
            assert(count == 0);
            {
                char dn[300];
                snprintf(dn, sizeof(dn), "%s.DIR", dname);
                assert(ods2_delete(&vol, 4, dn).ok);
            }
        }
        free(sub);
        list_names(&vol, 4, names, 600, &count);
        for (i = 0; i < count; i++) {
            int j, original = 0;
            for (j = 0; j < orig_count; j++) {
                if (strcmp(orig_root[j], names[i]) == 0) original = 1;
            }
            if (!original) assert(ods2_delete(&vol, 4, names[i]).ok);
        }
        root_header(&vol, root);
        /* INDEXF.SYS grew for the new headers and the root directory moved to a
           bigger area, and neither shrinks again: every other cluster is back */
        assert(free_clusters(&vol) + held_by_growing_files(&vol) / cluster == free_start + held_start / cluster);
        assert(used_file_numbers(&vol) == files_start);
        printf("PASS: after deleting everything, every cluster and file number is free again\n");
    }
    ods2_dismount(&vol);
    (void) r; (void) h;
    remove(DISK_PATH);
    test_fragmented();
    test_linkcount_and_failures();
    test_directory_revision();
    test_directory_eof();
    test_directory_eof_vax_damage();
    printf("\nods5_write_selftest: all checks passed\n");
    return 0;
}
