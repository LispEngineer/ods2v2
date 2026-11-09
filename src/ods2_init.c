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

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "ods2_init.h"
#include "ods2_checksum.h"
#include "ods2_header_build.h"
#include "ods2_time.h"
#include "ods5_ondisk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static ods2_result_t ok(void) { ods2_result_t r; r.ok = true; r.problem = NULL; return r; }
static ods2_result_t fail(const char *why) { ods2_result_t r; r.ok = false; r.problem = why; return r; }

/* The file being made: blocks are written at their LBN. */
typedef struct { FILE *f; uint64_t blocks; } image_t;

static bool put_block(image_t *img, uint64_t lbn, const uint8_t *data)
{
    return lbn < img->blocks && fseeko(img->f, (off_t) lbn * 512, SEEK_SET) == 0 &&
           fwrite(data, 1, 512, img->f) == 512;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static void set_checksum(uint8_t *b, size_t words, size_t at)
{
    uint16_t c = ods2_checksum(b, words);
    put16(b + at, c);
}

/* number of clusters, bitmap blocks */
static unsigned round_up(unsigned v, unsigned m) { return (v + m - 1) / m * m; }

ods2_result_t ods2_initialize(const char *path, const ods2_init_options_t *opt)
{
    const unsigned level = opt->level;
    uint64_t N = opt->blocks;
    unsigned c, m, bm_blocks, bm_alloc, idx_hdr_alloc, i;
    uint32_t maxfiles;
    uint32_t M, root_lbn, bitmap_lbn, ibmap_lbn, sec_lbn, alt_idx_lbn, backup_cluster_lbn, alhome_lbn;
    uint64_t nclusters;
    char label[12];
    const char *why;
    image_t img;
    uint8_t home[512], blk[512], hdr[512];
    uint8_t *bits = NULL;
    uint64_t now;
    ods2_extent_t ixext[4];
    ods2_header_spec_t spec;
    FILE *probe;

    if (level != ODS5_LEVEL_2 && level != ODS5_LEVEL_5) return fail("structure level must be 2 or 5");
    if (N < 8192) return fail("a volume needs at least 8192 blocks");
    if (N > 0xFFFFFFF0u) return fail("a volume can have at most 2^32 - 16 blocks here");
    why = ods2_make_volume_label(opt->label != NULL ? opt->label : "", label);
    if (why != NULL) return fail(why);
    if (level == ODS5_LEVEL_2 && (opt->volchar & (ODS5_VC_ACCESSTIMES | ODS5_VC_HARDLINKS | ODS5_VC_NO_SPECIAL_FILES))) {
        return fail("ACCESS_DATES, HARDLINKS and NO_SPECIAL_FILES are ODS-5 characteristics");
    }
    c = opt->cluster;
    if (c == 0) c = (N >= (1u << 20)) ? 16 : (N >= (1u << 18)) ? 8 : (N >= (1u << 16)) ? 4 : 1;
    if (c > 256) return fail("the cluster factor is at most 256 here");
    maxfiles = opt->maxfiles != 0 ? opt->maxfiles : (uint32_t) (N / (2 * (uint64_t) c + 2));
    if (maxfiles < 64) maxfiles = 64;
    if (maxfiles > 16711679u) maxfiles = 16711679u;
    m = (maxfiles + 4095) / 4096;                         /* index file bitmap blocks */
    nclusters = N / c;
    bm_blocks = (unsigned) ((nclusters + 4095) / 4096);   /* storage bitmap blocks (without the SCB) */
    bm_alloc = round_up(1 + bm_blocks, c);                /* BITMAP.SYS allocation */
    idx_hdr_alloc = round_up(m + 16, c);                  /* index bitmap + the first 16 headers */

    /* layout: boot + home clusters at the start; the backup home cluster near LBN 1034;
       everything else together in the middle of the disk */
    alhome_lbn = (uint32_t) ((N / 2 > 1100) ? 1034 : 34);
    backup_cluster_lbn = alhome_lbn - alhome_lbn % c;
    if (backup_cluster_lbn < 2 * c) backup_cluster_lbn = 2 * c;
    alhome_lbn = backup_cluster_lbn + (alhome_lbn % c);
    M = (uint32_t) (N / 2);
    M -= M % c;
    root_lbn = M;
    bitmap_lbn = root_lbn + c;
    ibmap_lbn = bitmap_lbn + bm_alloc;
    sec_lbn = ibmap_lbn + idx_hdr_alloc;
    alt_idx_lbn = sec_lbn + c;
    if ((uint64_t) alt_idx_lbn + c > N) return fail("the volume is too small for its structures");
    if (backup_cluster_lbn + c > root_lbn) return fail("the volume is too small for its structures");

    probe = fopen(path, "rb");
    if (probe != NULL) { fclose(probe); return fail("the image file already exists: INITIALIZE never overwrites a volume"); }
    img.f = fopen(path, "w+b");
    if (img.f == NULL) return fail("could not create the image file");
    img.blocks = N;
    now = ods2_vms_time_now();

    /* ---- home block (and its copies) ---- */
    memset(home, 0, sizeof(home));
    put32(home + 0, 1);                                   /* HM2$L_HOMELBN */
    put32(home + 4, alhome_lbn);                          /* HM2$L_ALHOMELBN */
    put32(home + 8, alt_idx_lbn);                         /* HM2$L_ALTIDXLBN */
    put16(home + 12, level == ODS5_LEVEL_5 ? 0x0501 : 0x0201);
    put16(home + 14, c);                                  /* HM2$W_CLUSTER */
    put16(home + 16, 2);                                  /* HM2$W_HOMEVBN */
    put16(home + 18, 2 * c + 1 + (alhome_lbn - backup_cluster_lbn)); /* HM2$W_ALHOMEVBN */
    put16(home + 20, 3 * c + 1);                          /* HM2$W_ALTIDXVBN */
    put16(home + 22, 4 * c + 1);                          /* HM2$W_IBMAPVBN */
    put32(home + 24, ibmap_lbn);                          /* HM2$L_IBMAPLBN */
    put32(home + 28, maxfiles);                           /* HM2$L_MAXFILES */
    put16(home + 32, m);                                  /* HM2$W_IBMAPSIZE */
    put16(home + 34, 10);                                 /* HM2$W_RESFILES */
    put16(home + 42, opt->volchar);                       /* HM2$W_VOLCHAR */
    put16(home + 44, 4); put16(home + 46, 1);             /* HM2$L_VOLOWNER [1,4] */
    put16(home + 54, 0xfa00);                             /* HM2$W_FILEPROT */
    put16(home + 56, 0xfe00);                             /* default record protection, as V8.4 */
    ods2_store_vms_time(home + 60, now);                  /* HM2$Q_CREDATE */
    home[68] = 7; home[69] = 3; put16(home + 70, 5);      /* window, LRU limit, extend: V8.4's */
    memset(home + 460, ' ', 12);                          /* HM2$T_STRUCNAME */
    memcpy(home + 472, label, 12);                        /* HM2$T_VOLNAME */
    memset(home + 484, ' ', 12);                          /* HM2$T_OWNERNAME */
    memcpy(home + 496, "DECFILE11B  ", 12);               /* HM2$T_FORMAT */
    for (i = 0; i < 2 * c - 1; i++) {                     /* LBN 1 .. 2c-1: VBN 2 .. 2c */
        memcpy(blk, home, 512);
        put32(blk + 0, 1 + i);
        put16(blk + 16, 2 + i);
        set_checksum(blk, 29, 58);
        set_checksum(blk, 255, 510);
        if (!put_block(&img, 1 + i, blk)) goto io_error;
    }
    for (i = 0; i < c; i++) {                             /* the backup home cluster: VBN 2c+1 .. 3c */
        memcpy(blk, home, 512);
        put32(blk + 0, backup_cluster_lbn + i);
        put16(blk + 16, 2 * c + 1 + i);
        set_checksum(blk, 29, 58);
        set_checksum(blk, 255, 510);
        if (!put_block(&img, backup_cluster_lbn + i, blk)) goto io_error;
    }

    /* ---- the reserved files' headers ---- */
    {
        static const struct {
            const char *name; uint8_t rtype, rattrib; uint16_t rsize; uint32_t filechar; uint16_t fileprot;
        } files[10] = {
            { "INDEXF.SYS;1",   0x01, 0x00, 512, 0x00000020, 0xfa00 },
            { "BITMAP.SYS;1",   0x01, 0x00, 512, 0x00000080, 0xfa00 },
            { "BADBLK.SYS;1",   0x01, 0x00, 512, 0x00000000, 0xfa00 },
            { "000000.DIR;1",   0x02, 0x08, 512, 0x00002080, 0xba00 },
            { "CORIMG.SYS;1",   0x01, 0x00, 512, 0x00000000, 0xfa00 },
            { "VOLSET.SYS;1",   0x01, 0x00,  64, 0x00000000, 0xfa00 },
            { "CONTIN.SYS;1",   0x01, 0x00, 512, 0x00000000, 0xfa00 },
            { "BACKUP.SYS;1",   0x01, 0x00,  64, 0x00000000, 0xfa00 },
            { "BADLOG.SYS;1",   0x01, 0x00,  16, 0x00000000, 0xfa00 },
            { "SECURITY.SYS;1", 0x01, 0x00, 512, 0x00000380, 0xfa00 },
        };
        unsigned n;
        ods2_extent_t ext[1];
        uint32_t hdr_vbn0 = 4 * c + m;                     /* header n is at VBN hdr_vbn0 + n of INDEXF.SYS */
        uint64_t hdr_lbn0 = (uint64_t) ibmap_lbn + m;      /* ... and at LBN hdr_lbn0 + n - 1 */

        ixext[0].lbn = 0;                 ixext[0].block_count = 2 * c;
        ixext[1].lbn = backup_cluster_lbn; ixext[1].block_count = c;
        ixext[2].lbn = alt_idx_lbn;        ixext[2].block_count = c;
        ixext[3].lbn = ibmap_lbn;          ixext[3].block_count = idx_hdr_alloc;
        for (n = 1; n <= 10; n++) {
            memset(&spec, 0, sizeof(spec));
            spec.fid.fid_num = (uint16_t) n;
            spec.fid.fid_seq = (uint16_t) n;
            spec.backlink.fid_num = 4; spec.backlink.fid_seq = 4;
            spec.rtype = files[n - 1].rtype;
            spec.rattrib = files[n - 1].rattrib;
            spec.rsize = files[n - 1].rsize;
            spec.maxrec = files[n - 1].rsize;
            spec.filechar = files[n - 1].filechar;
            spec.level = ODS5_LEVEL_2;                    /* the reserved files keep level 2 headers, even on an ODS-5 volume */
            spec.ident_name = files[n - 1].name;
            spec.efblk = 1;
            spec.highwater = 1;
            switch (n) {
            case 1:
                spec.extents = ixext; spec.extent_count = 4;
                spec.hiblk = 4 * c + idx_hdr_alloc;
                spec.efblk = hdr_vbn0 + 16 + 1;            /* the first 16 headers are inside the end-of-file */
                spec.highwater = spec.hiblk + 1;
                break;
            case 2:
                ext[0].lbn = bitmap_lbn; ext[0].block_count = bm_alloc;
                spec.extents = ext; spec.extent_count = 1;
                spec.hiblk = bm_alloc;
                spec.efblk = 1 + 1 + bm_blocks;            /* the SCB and the bitmap */
                spec.highwater = bm_alloc + 1;
                break;
            case 4:
                ext[0].lbn = root_lbn; ext[0].block_count = c;
                spec.extents = ext; spec.extent_count = 1;
                spec.hiblk = c;
                spec.efblk = 2;                            /* one block of records in use */
                spec.highwater = 2;
                break;
            case 10:
                ext[0].lbn = sec_lbn; ext[0].block_count = c;
                spec.extents = ext; spec.extent_count = 1;
                spec.hiblk = c;
                spec.efblk = (opt->security == ODS2_INIT_SECURITY_EMPTY) ? 1 : 2;
                spec.highwater = c + 1;
                break;
            default:
                break;
            }
            if (!ods2_build_file_header(hdr, &spec)) { fclose(img.f); remove(path); return fail("could not build a reserved file header"); }
            ((ods2_head_core_t *) hdr)->fileprot = files[n - 1].fileprot;
            /* V8.4: link count 1 in the reserved headers of an ODS-5 volume with HARDLINKS, 0 without */
            if (level == ODS5_LEVEL_5 && (opt->volchar & ODS5_VC_HARDLINKS)) hdr[ODS5_FH2_LINKCOUNT] = 1;
            if (n == 10) {
                /* VMS writes the volume's security profile into SECURITY.SYS right after creating
                   it: revision 2, revised 0.06 s after it was created (corpus header dump) */
                uint8_t *ident = hdr + ((ods2_head_core_t *) hdr)->idoffset * 2;
                put16(ident + 20, 2);
                ods2_store_vms_time(ident + 30, now + 600000);
            }
            set_checksum(hdr, 255, 510);
            if (!put_block(&img, hdr_lbn0 + n - 1, hdr)) goto io_error;
            if (n == 1) {                                  /* the backup index file header: first block of its cluster */
                if (!put_block(&img, alt_idx_lbn, hdr)) goto io_error;
            }
        }
    }

    /* ---- SECURITY.SYS block 1: the volume security profile ---- */
    if (opt->security == ODS2_INIT_SECURITY_PROFILE) {
        /* The record VMS INITIALIZE writes (read from VMS-made volumes: byte 8 on is the same
           on every volume of V7.x and V8.4 measured, ODS5_FORMAT_NOTES.md 12): an 8-byte
           identifier, the record length (90), then fixed items, and at the end the volume
           label as a 12-byte field. The first eight bytes differ on every volume. */
        static const uint8_t fixed[72] = {
            0x5a, 0x00, 0x00, 0x00, 0x06, 0x00, 0x02, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
            0x0c, 0x00, 0x08, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06, 0x00, 0x02, 0x08, 0x08, 0x00,
            0x0c, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x09, 0x00, 0x14, 0x00,
            0xe0, 0xff, 0xff, 0xff, 0xe0, 0xff, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xff,
            0x17, 0x00, 0x08, 0x00, 0, 0, 0, 0
        };
        uint8_t sec[512];
        memset(sec, 0, sizeof(sec));
        put32(sec + 0, (uint32_t) (now & 0xffffffffu));            /* identifier */
        memcpy(sec + 8, fixed, sizeof(fixed));
        put16(sec + 80 - 2, 0x14);                                 /* item type: the volume label */
        put16(sec + 80, 12);
        memcpy(sec + 82, label, 12);
        {
            int k;
            for (k = 11; k >= 0 && sec[82 + k] == ' '; k--) sec[82 + k] = 0;   /* zero padded, as VMS writes it */
        }
        if (!put_block(&img, sec_lbn, sec)) goto io_error;
    } else if (opt->security == ODS2_INIT_SECURITY_ZERO) {
        uint8_t sec[512];
        memset(sec, 0, sizeof(sec));
        if (!put_block(&img, sec_lbn, sec)) goto io_error;
    }

    /* ---- index file bitmap: files 1-10 in use ---- */
    {
        uint8_t *ib = calloc(m, 512);
        if (ib == NULL) goto io_error;
        ib[0] = 0xff; ib[1] = 0x03;
        for (i = 0; i < m; i++) {
            if (!put_block(&img, (uint64_t) ibmap_lbn + i, ib + (size_t) i * 512)) { free(ib); goto io_error; }
        }
        free(ib);
    }

    /* ---- root directory: the ten reserved files, in order ---- */
    {
        static const struct { const char *name; unsigned fid; } ents[] = {
            { "000000.DIR", 4 }, { "BACKUP.SYS", 8 }, { "BADBLK.SYS", 3 }, { "BADLOG.SYS", 9 },
            { "BITMAP.SYS", 2 }, { "CONTIN.SYS", 7 }, { "CORIMG.SYS", 5 }, { "INDEXF.SYS", 1 },
            { "SECURITY.SYS", 10 }, { "VOLSET.SYS", 6 },
        };
        size_t pos = 0, k;
        memset(blk, 0, sizeof(blk));
        for (k = 0; k < sizeof(ents) / sizeof(ents[0]); k++) {
            size_t len = strlen(ents[k].name), padded = len + (len % 2), total = 6 + padded + 8;
            put16(blk + pos, (unsigned) (total - 2));
            put16(blk + pos + 2, 1);                       /* version limit 1, as on the reserved files */
            blk[pos + 4] = 0;
            blk[pos + 5] = (uint8_t) len;
            memcpy(blk + pos + 6, ents[k].name, len);
            put16(blk + pos + 6 + padded, 1);              /* version 1 */
            put16(blk + pos + 6 + padded + 2, ents[k].fid);
            put16(blk + pos + 6 + padded + 4, ents[k].fid);/* sequence number = file number */
            pos += total;
        }
        put16(blk + pos, 0xffff);
        if (!put_block(&img, root_lbn, blk)) goto io_error;
    }

    /* ---- BITMAP.SYS: the storage control block and the bitmap ---- */
    {
        uint8_t scb[512];
        uint64_t k;
        unsigned used_extents[8][2];
        int ue = 0;
        used_extents[ue][0] = 0;                 used_extents[ue++][1] = 2 * c;
        used_extents[ue][0] = backup_cluster_lbn; used_extents[ue++][1] = c;
        used_extents[ue][0] = alt_idx_lbn;        used_extents[ue++][1] = c;
        used_extents[ue][0] = ibmap_lbn;          used_extents[ue++][1] = idx_hdr_alloc;
        used_extents[ue][0] = bitmap_lbn;         used_extents[ue++][1] = bm_alloc;
        used_extents[ue][0] = root_lbn;           used_extents[ue++][1] = c;
        used_extents[ue][0] = sec_lbn;            used_extents[ue++][1] = c;

        memset(scb, 0, sizeof(scb));
        put16(scb + 0, level == ODS5_LEVEL_5 ? 0x0501 : 0x0201);   /* SCB$W_STRUCLEV: the level, as V8.4 writes it */
        put16(scb + 2, c);
        put32(scb + 4, (uint32_t) N);                              /* SCB$L_VOLSIZE */
        put32(scb + 8, 1);                                         /* SCB$L_BLKSIZE */
        put32(scb + 12, 32);                                       /* sectors, tracks, cylinders: the geometry V8.4 gave its virtual disks */
        put32(scb + 16, 16);
        put32(scb + 20, (uint32_t) ((N + 511) / 512));
        memcpy(scb + 34, label, 12);                               /* SCB$T_VOLOCKNAME */
        ods2_store_vms_time(scb + 46, now);                        /* SCB$Q_MOUNTTIME */
        set_checksum(scb, 255, 510);
        if (!put_block(&img, bitmap_lbn, scb)) goto io_error;

        bits = calloc(bm_blocks, 512);
        if (bits == NULL) goto io_error;
        for (k = 0; k < nclusters; k++) bits[k / 8] |= (uint8_t) (1u << (k % 8));   /* 1 = free */
        for (i = 0; i < (unsigned) ue; i++) {
            uint64_t first = used_extents[i][0] / c, last = (used_extents[i][0] + used_extents[i][1] - 1) / c;
            for (k = first; k <= last; k++) bits[k / 8] &= (uint8_t) ~(1u << (k % 8));
        }
        for (i = 0; i < bm_blocks; i++) {
            if (!put_block(&img, (uint64_t) bitmap_lbn + 1 + i, bits + (size_t) i * 512)) goto io_error;
        }
        free(bits);
        bits = NULL;
    }

    /* the file is as long as the volume (the last block is written so that it is) */
    if (ftruncate(fileno(img.f), (off_t) N * 512) != 0) goto io_error;
    if (fclose(img.f) != 0) { remove(path); return fail("could not close the image file"); }
    return ok();

io_error:
    free(bits);
    fclose(img.f);
    remove(path);
    return fail("could not write the image file (is the disk full?)");
}
