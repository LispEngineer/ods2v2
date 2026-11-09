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

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* flock(), fileno(), fdatasync() */
#endif
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods5_ondisk.h"
#include "ods5_header.h"
#include "ods2_bitmap.h"
#include "ods2_header_build.h"
#include "ods2_directory_write.h"
#include "ods2_checksum.h"
#include "ods2_time.h"
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/file.h>
#include <strings.h>
#include <stdlib.h>

static ods2_result_t ok(void)
{
    ods2_result_t r; r.ok = true; r.problem = NULL; return r;
}
static ods2_result_t fail(const char *why)
{
    ods2_result_t r; r.ok = false; r.problem = why; return r;
}

/* Why a write is refused: a version 0 volume is never writable, any
   other volume (ODS-2 or ODS-5) only if mounted with ods2_mount_write(). */
static ods2_result_t not_writable(const ods2_volume_t *vol)
{
    if (vol->version0) {
        return fail("volume has HM2$W_STRUCLEV version 0 and is read-only");
    }
    return fail("volume was not mounted for write (use ods2_mount_write)");
}

static ods2_result_t insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                           const char *name, uint16_t version, ods2_fid_t fid,
                                           uint8_t extra_flags);

/* Uppercases a string in place. Real VMS always stores filenames and
   directory names uppercase - "create/dir [decus]" on real VMS
   creates DECUS.DIR. Applied to every user-supplied name at the
   point of creation. */
static void uppercase_str(char *s)
{
    for (; *s; s++) {
        *s = (char) toupper((unsigned char) *s);
    }
}

/* Checks for ODS-2's two wildcard characters ('*' matches any
   sequence, '%' matches exactly one character - see
   ods2_wildcard.h). A wildcard in a DESTINATION name for a create
   operation is always a mistake, never something meaningful to
   actually store: it either means the caller meant to type a real
   name and fat-fingered a wildcard character into it, or meant to
   express something wildcard-related that doesn't make sense for a
   single create. */
static bool contains_wildcard(const char *name)
{
    return strchr(name, '*') != NULL || strchr(name, '%') != NULL;
}

/* Is `h` the extension header it should be: a valid header (checksum), at level 2 or 5,
   with the file number the chain said, the next segment number, and the primary header's
   file number as back link? A damaged or crafted EXT_FID otherwise makes the file continue
   in another file's blocks. */
static bool extension_header_ok(const uint8_t *h, ods2_fid_t expect, unsigned primary_num,
                                unsigned segment)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    unsigned level = core->struclev >> 8;
    return (level == ODS5_LEVEL_2 || level == ODS5_LEVEL_5) &&
           ods2_validate_head(h).ok &&
           ods2_fid_number(core->fid) == ods2_fid_number(expect) && core->fid.fid_seq == expect.fid_seq &&
           core->seg_num == segment &&
           ods2_fid_number(core->backlink) == primary_num;
}

static int decode_header_extents(const uint8_t *header, ods2_extent_t *extents_out, size_t max)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_offset = (size_t) core->mpoffset * 2;
    if (map_bytes == 0 || map_offset + map_bytes > 512) return 0;
    return ods2_decode_retrieval_pointers(header + map_offset, core->map_inuse,
                                           extents_out, max);
}

/* Maps virtual block `vbn` (1-based) to an absolute LBN, given a list
   of extents describing the file's content in order. */
static bool vbn_to_lbn(const ods2_extent_t *extents, int extent_count,
                        unsigned vbn, uint32_t *lbn_out)
{
    unsigned vbn_cursor = 1;
    int i;
    for (i = 0; i < extent_count; i++) {
        unsigned extent_len = extents[i].block_count;
        if (vbn >= vbn_cursor && vbn < vbn_cursor + extent_len) {
            *lbn_out = extents[i].lbn + (vbn - vbn_cursor);
            return true;
        }
        vbn_cursor += extent_len;
    }
    return false;
}

/* Most extension headers the index file may chain (a loop stops here). */
/* How deep directories may be nested (VMS: 255; DIR [...] and EXPORT stop there). */
#define ODS2_MAX_DIR_DEPTH_DEFAULT 255u
/* The creation limit. The test binary (ods2_fault) can raise it with ODS2_TEST_MAX_DIR_DEPTH, to make the trees
   deeper than 255 levels a VMS volume can hold (V7.3-2 made 400) that the readers must handle. */
static unsigned max_dir_depth(void)
{
#ifdef ODS2_FAULT_INJECTION
    const char *e = getenv("ODS2_TEST_MAX_DIR_DEPTH");
    if (e != NULL && atoi(e) > 0) return (unsigned) atoi(e);
#endif
    return ODS2_MAX_DIR_DEPTH_DEFAULT;
}
#define ODS2_MAX_DIR_DEPTH max_dir_depth()
#define ODS2_MAX_HEADER_SEGMENTS_MOUNT 64

static void load_scb_volsize(ods2_volume_t *vol);

static ods2_result_t mount_internal(const char *path, ods2_volume_t *vol,
                                     const char *fopen_mode, bool writable)
{
    ods2_validate_result_t vr;
    uint8_t indexf_header[512];
    ods2_result_t r;

    memset(vol, 0, sizeof(*vol));
    vol->fp = fopen(path, fopen_mode);
    if (vol->fp == NULL) {
        return fail("could not open disk image file");
    }
    vol->writable = writable;
    /* One writer or any number of readers (review 6, finding 9): two processes writing one image, or one
       writing an image another reads, would corrupt it or read a half-done change. flock() is advisory:
       it stops another ods2v2, not a simulator that has the image open; a file system without flock()
       gives no lock and no error. ODS2_NOLOCK=1 turns it off (a test that holds a volume open while it
       mounts the same image again). */
    if (getenv("ODS2_NOLOCK") == NULL &&
        flock(fileno(vol->fp), (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        fclose(vol->fp);
        vol->fp = NULL;
        return fail(writable ? "the disk image is in use by another process (it is locked): not written"
                             : "the disk image is being written by another process (it is locked)");
    }

    if (fseek(vol->fp, 512, SEEK_SET) != 0 ||
        fread(&vol->home, 1, sizeof(vol->home), vol->fp) != sizeof(vol->home)) {
        ods2_dismount(vol);
        return fail("could not read home block");
    }

    vr = ods2_validate_home(&vol->home);
    if (!vr.ok) {
        ods2_dismount(vol);
        return fail(vr.problem);
    }

    vol->ods5 = ods2_home_is_ods5(&vol->home);
    vol->version0 = ods2_home_is_version0(&vol->home);
    if (vol->version0 && writable) {
        ods2_dismount(vol);
        return fail("HM2$W_STRUCLEV version is 0: the volume can be read, "
                    "not written");
    }

    /* Bootstrap: INDEXF.SYS's own header (file 1) is always locatable
       directly from home block fields alone. Spec 5.1.7 guarantees
       for the first 16 files. */
    {
        uint32_t lbn = vol->home.ibmaplbn + (vol->home.ibmapsize + 1 - 1);
        if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
            fread(indexf_header, 1, sizeof(indexf_header), vol->fp) != sizeof(indexf_header)) {
            ods2_dismount(vol);
            return fail("could not read INDEXF.SYS's own header");
        }
    }

    vol->indexf_extent_count = decode_header_extents(indexf_header,
        vol->indexf_extents, ODS2_MAX_EXTENTS);
    if (vol->indexf_extent_count == ODS2_DECODE_TOO_MANY) {
        ods2_dismount(vol);
        return fail("INDEXF.SYS has more extents than ods2v2 can hold (1024): the volume is not mounted");
    }
    if (vol->indexf_extent_count <= 0) {
        ods2_dismount(vol);
        return fail("could not decode INDEXF.SYS's own retrieval pointers");
    }

    /* A fragmented index file continues in extension headers (FH2$W_EXT_FID),
       which are themselves headers inside the index file: found through the
       extents known so far, each one adding its own. If one cannot be read the
       extents known so far are kept. */
    {
        uint8_t current[512];
        int segment;
        memcpy(current, indexf_header, sizeof(current));
        for (segment = 0; segment < ODS2_MAX_HEADER_SEGMENTS_MOUNT; segment++) {
            const ods2_head_core_t *core = (const ods2_head_core_t *) current;
            uint8_t next[512];
            int n;
            if (ods2_fid_number(core->ext_fid) == 0) break;
            if (!ods2_read_header(vol, ods2_fid_number(core->ext_fid), next).ok) break;
            if (!extension_header_ok(next, core->ext_fid, 1, (unsigned) segment + 1)) break;
            n = decode_header_extents(next, vol->indexf_extents + vol->indexf_extent_count,
                                      (size_t) (ODS2_MAX_EXTENTS - vol->indexf_extent_count));
            if (n == ODS2_DECODE_TOO_MANY) {
                ods2_dismount(vol);
                return fail("INDEXF.SYS has more extents than ods2v2 can hold (1024): the volume is not mounted");
            }
            if (n <= 0) break;
            vol->indexf_extent_count += n;
            memcpy(current, next, sizeof(current));
        }
    }

    load_scb_volsize(vol);
    if (writable) {
        char fmsg[256];
        if (ods2_image_footer(vol, fmsg, sizeof(fmsg)) == 2) {
            ods2_dismount(vol);
            return fail("a SIMH container footer lies inside the volume (the last block may be lost): the "
                        "volume can be read, not written");
        }
    }
    r = ok();
    return r;
}

ods2_result_t ods2_mount(const char *path, ods2_volume_t *vol)
{
    return mount_internal(path, vol, "rb", false);
}

ods2_result_t ods2_mount_write(const char *path, ods2_volume_t *vol)
{
    return mount_internal(path, vol, "r+b", true);
}

#ifdef ODS2_FAULT_INJECTION
/* Fault injection for the test build (not in `make release`): with ODS2_FAULT_AFTER_WRITES=k in the
   environment the process ends at once (_Exit(99), as a kill or a power cut would, nothing more written
   or flushed) after the k-th write to the volume - one write is one ods2_write_block() or one chunk of
   file content in write_extents(), flushed first. ODS2_FAULT_COUNT=1 makes a mount for write print
   "ODS2_FAULT: n writes" to stderr when it is dismounted, so a harness can learn how many k there are. */
static long fault_writes;

static void fault_point(void)
{
    static long limit = -2;
    if (limit == -2) {
        const char *e = getenv("ODS2_FAULT_AFTER_WRITES");
        limit = (e != NULL && *e != '\0') ? atol(e) : -1;
    }
    fault_writes++;
    if (limit >= 0 && fault_writes >= limit) {
        fprintf(stderr, "ODS2_FAULT: ended after %ld writes\n", fault_writes);
        fflush(stderr);
        _Exit(99);
    }
}
#else
#define fault_point() ((void) 0)
#endif

/* --- changes are made in memory first (review: F1, a refused DELETE had already written) ---

   A write to the volume made while a change is open (txn_begin() ... txn_end()) is kept in memory, in order,
   and a read of that block sees it. txn_end() writes them to the image, in the order they were made, only if
   the operation succeeded; if it failed, they are dropped and the image is as it was (INDEXF.SYS's cached
   extents are put back too). So nothing that can fail after the first write can leave a half-done change:
   what is left to fail at commit is the write itself (an I/O error). The order of the writes at commit is the
   order the operation made them, which is the order the crash-safety rules need (a marked header before the
   entry goes, a final header before blocks are freed ...). File content written by write_extents() goes
   straight to free clusters and is done after everything else that can fail (create_file). */
typedef struct {
    uint32_t lbn;
    uint8_t  data[512];
} txn_write_t;

struct ods2_txn {
    txn_write_t  *w;
    size_t        n, cap;
    size_t       *table;       /* open addressing: index + 1 of the latest write of an lbn, 0 = empty */
    size_t        table_cap;   /* a power of two */
    ods2_extent_t saved_extents[ODS2_MAX_EXTENTS];
    int           saved_extent_count;
};

static size_t txn_slot(const struct ods2_txn *t, uint32_t lbn)
{
    size_t h = ((size_t) lbn * 2654435761u) & (t->table_cap - 1);
    while (t->table[h] != 0 && t->w[t->table[h] - 1].lbn != lbn) h = (h + 1) & (t->table_cap - 1);
    return h;
}

static bool txn_grow_table(struct ods2_txn *t)
{
    size_t cap = t->table_cap ? t->table_cap * 2 : 256, i;
    size_t *table = calloc(cap, sizeof(*table));
    if (table == NULL) return false;
    free(t->table);
    t->table = table;
    t->table_cap = cap;
    for (i = 0; i < t->n; i++) t->table[txn_slot(t, t->w[i].lbn)] = i + 1;
    return true;
}

static const uint8_t *txn_find(const ods2_volume_t *vol, uint32_t lbn)
{
    const struct ods2_txn *t = vol->txn;
    if (t == NULL || t->n == 0) return NULL;
    {
        size_t at = t->table[txn_slot(t, lbn)];
        return at != 0 ? t->w[at - 1].data : NULL;
    }
}

/* Nothing at or past the end of the volume (SCB$L_VOLSIZE) is ever written. The check is made when a write is
   buffered, so that an operation which would need such a block is refused with nothing written, and again at the
   last moment (raw_write_block). */
static const char write_past_volume[] = "a block at or past the end of the volume (the storage control block's "
                                        "volume size) is never written: nothing there belongs to the volume";
static bool lbn_in_volume(const ods2_volume_t *vol, uint32_t lbn)
{
    return vol->volsize == 0 || lbn < vol->volsize;
}

static ods2_result_t txn_add(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block)
{
    struct ods2_txn *t = vol->txn;
    if (!lbn_in_volume(vol, lbn)) return fail(write_past_volume);
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        txn_write_t *w = realloc(t->w, cap * sizeof(*w));
        if (w == NULL) return fail("out of memory keeping a change of the volume");
        t->w = w;
        t->cap = cap;
    }
    if ((t->n + 1) * 2 > t->table_cap && !txn_grow_table(t)) return fail("out of memory keeping a change of the volume");
    t->w[t->n].lbn = lbn;
    memcpy(t->w[t->n].data, block, 512);
    t->n++;
    t->table[txn_slot(t, lbn)] = t->n;
    return ok();
}

/* Reads a block as the change in progress sees it. */
static ods2_result_t read_block(ods2_volume_t *vol, uint32_t lbn, uint8_t *out, const char *why)
{
    const uint8_t *pending = txn_find(vol, lbn);
    if (pending != NULL) {
        memcpy(out, pending, 512);
        return ok();
    }
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 || fread(out, 1, 512, vol->fp) != 512) {
        return fail(why);
    }
    return ok();
}

/* The SIMH container footer (simh/simh, sim_disk.c: MIT-licensed): the last 512 bytes of a raw container, "simh",
   the simulator's name (64 bytes), the drive type (16), SectorSize, SectorCount, TransferElementSize (32-bit,
   big-endian), the creation time (28), FooterVersion (0), AccessFormat (1 SIMH, 2 RAW), ... and a CRC-32 of the
   first 508 bytes in the last four. The signature, version 0, the access format and the CRC identify it here. */
static bool simh_footer_at_end(ods2_volume_t *vol, uint64_t *image_blocks, uint8_t foot[512])
{
    long here = ftell(vol->fp), size;

    if (fseek(vol->fp, 0, SEEK_END) != 0) return false;
    size = ftell(vol->fp);
    (void) fseek(vol->fp, here, SEEK_SET);
    if (size < 1024) return false;
    *image_blocks = (uint64_t) size / 512;
    if (fseek(vol->fp, size - 512, SEEK_SET) != 0 || fread(foot, 1, 512, vol->fp) != 512) {
        (void) fseek(vol->fp, here, SEEK_SET);
        return false;
    }
    (void) fseek(vol->fp, here, SEEK_SET);
    if (memcmp(foot, "simh", 4) != 0 || foot[124] != 0 || (foot[125] != 1 && foot[125] != 2)) return false;
    /* the CRC-32 (IEEE, as zlib: the simulator's eth_crc32) of the first 508 bytes, big-endian in the last four: a
       block that merely starts with "simh" is not a footer */
    {
        uint32_t crc = 0xFFFFFFFFu, stored;
        size_t i;
        int b;
        for (i = 0; i < 508; i++) {
            crc ^= foot[i];
            for (b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        crc ^= 0xFFFFFFFFu;
        stored = ((uint32_t) foot[508] << 24) | ((uint32_t) foot[509] << 16) | ((uint32_t) foot[510] << 8) | foot[511];
        return crc == stored;
    }
}

int ods2_image_footer(ods2_volume_t *vol, char *msg, size_t cap)
{
    uint8_t foot[512];
    uint64_t image_blocks = 0;
    char sim[65], dtype[17];

    if (msg != NULL && cap > 0) msg[0] = '\0';
    if (vol->volsize == 0 || !simh_footer_at_end(vol, &image_blocks, foot)) return 0;
    memcpy(sim, foot + 4, 64); sim[64] = '\0';
    memcpy(dtype, foot + 68, 16); dtype[16] = '\0';
    if (image_blocks > vol->volsize) {
        if (msg != NULL) {
            snprintf(msg, cap, "the image ends with a 512-byte SIMH container footer (made by \"%s\", drive type \"%s\"): "
                     "it is not volume data (the volume is %u blocks) and is never read or written", sim, dtype,
                     (unsigned) vol->volsize);
        }
        return 1;
    }
    if (msg != NULL) {
        snprintf(msg, cap, "the last block of the image (block %llu) is a SIMH container footer (made by \"%s\", drive "
                 "type \"%s\"), inside the volume (%u blocks): a 32-bit overflow in the simulator put it there, and the "
                 "last block of the volume may be lost; the volume is read-only", (unsigned long long) (image_blocks - 1),
                 sim, dtype, (unsigned) vol->volsize);
    }
    return 2;
}

/* SCB$L_VOLSIZE, read once at mount: the end of the volume for the write bound (a write at or past it is refused). */
static void load_scb_volsize(ods2_volume_t *vol)
{
    uint8_t h[512], b[512];
    ods2_extent_t ex[ODS2_MAX_EXTENTS];
    int n = 0;

    vol->volsize = 0;
    if (!ods2_read_header(vol, 2, h).ok || !ods2_decode_all_extents(vol, h, ex, ODS2_MAX_EXTENTS, &n).ok || n <= 0 ||
        !read_block(vol, ex[0].lbn, b, "could not read BITMAP.SYS").ok) {
        return;
    }
    vol->volsize = (uint32_t) b[4] | ((uint32_t) b[5] << 8) | ((uint32_t) b[6] << 16) | ((uint32_t) b[7] << 24);
}


/* Opens a change. It fails, and the operation with it, if the buffer cannot be had (R9-1: running unbuffered
   would be the old, unsafe way) or if an earlier write to this image failed (R9-2). */
static ods2_result_t txn_begin(ods2_volume_t *vol)
{
    if (!vol->writable) return ok();
    if (vol->txn_depth == 0) {
        if (vol->write_failed) {
            return fail("an earlier write to the disk image failed: nothing more is written through this "
                        "mount (run ANALYZE/DISK_STRUCTURE/REPAIR on VMS if the image is in doubt)");
        }
#ifdef ODS2_FAULT_INJECTION
        vol->txn = getenv("ODS2_FAULT_NOMEM_TXN") != NULL ? NULL : calloc(1, sizeof(*vol->txn));
#else
        vol->txn = calloc(1, sizeof(*vol->txn));
#endif
        if (vol->txn == NULL) return fail("out of memory: the change cannot be prepared, nothing is written");
        memcpy(vol->txn->saved_extents, vol->indexf_extents, sizeof(vol->txn->saved_extents));
        vol->txn->saved_extent_count = vol->indexf_extent_count;
    }
    vol->txn_depth++;
    return ok();
}

#define TXN_BEGIN_OR_RETURN(vol) do { ods2_result_t b_ = txn_begin(vol); if (!b_.ok) return b_; } while (0)

static ods2_result_t raw_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block);

/* The end of a change: written if `r` says it succeeded, dropped if not. Returns `r`, or the failure of a write
   at commit. A nested change commits or drops with the outermost one. */
static ods2_result_t txn_end(ods2_volume_t *vol, ods2_result_t r)
{
    struct ods2_txn *t;
    size_t i;

    if (!vol->writable) return r;
    if (--vol->txn_depth > 0) return r;
    t = vol->txn;
    vol->txn = NULL;
    if (t == NULL) return r;
    if (r.ok) {
        for (i = 0; i < t->n && r.ok; i++) r = raw_write_block(vol, t->w[i].lbn, t->w[i].data);
        if (!r.ok) {
            /* part of the change is on the disk and the rest is not: nothing more is written through this
               mount (R9-2), and the INDEXF.SYS cache goes back to what it was before the change, as the
               disk may not have its new header */
            vol->write_failed = true;
            memcpy(vol->indexf_extents, t->saved_extents, sizeof(t->saved_extents));
            vol->indexf_extent_count = t->saved_extent_count;
        }
    } else {
        memcpy(vol->indexf_extents, t->saved_extents, sizeof(t->saved_extents));
        vol->indexf_extent_count = t->saved_extent_count;
    }
    free(t->w);
    free(t->table);
    free(t);
    return r;
}

static ods2_result_t raw_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block)
{
#ifdef ODS2_FAULT_INJECTION
    {
        /* ODS2_FAULT_ERROR_AT_WRITE=k: the k-th write of the process fails (an I/O error), the process goes on */
        static long attempts, error_at = -2;
        if (error_at == -2) {
            const char *e = getenv("ODS2_FAULT_ERROR_AT_WRITE");
            error_at = (e != NULL && *e != '\0') ? atol(e) : -1;
        }
        if (error_at > 0 && ++attempts == error_at) return fail("could not write block (injected I/O error)");
    }
#endif
    if (!lbn_in_volume(vol, lbn)) return fail(write_past_volume);
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fwrite(block, 1, 512, vol->fp) != 512) {
        return fail("could not write block");
    }
    if (fflush(vol->fp) != 0) {
        return fail("could not flush written block");
    }
#ifdef ODS2_FAULT_INJECTION
    if (getenv("ODS2_FAULT_TRACE") != NULL) fprintf(stderr, "ODS2_FAULT: write %ld = LBN %u\n", fault_writes + 1, (unsigned) lbn);
#endif
    fault_point();
    return ok();
}

ods2_result_t ods2_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block)
{
    if (!vol->writable) {
        return not_writable(vol);
    }
    if (vol->txn != NULL) return txn_add(vol, lbn, block);
    if (vol->write_failed) return fail("an earlier write to the disk image failed: nothing more is written");
    return raw_write_block(vol, lbn, block);
}

ods2_result_t ods2_sync(ods2_volume_t *vol)
{
    if (vol->fp == NULL || !vol->writable) return ok();
    if (fflush(vol->fp) != 0) return fail("could not flush the disk image");
    if (getenv("ODS2_NOSYNC") == NULL && fdatasync(fileno(vol->fp)) != 0) {
        return fail("could not write the disk image to the disk (fdatasync failed)");
    }
    return ok();
}

void ods2_dismount(ods2_volume_t *vol)
{
#ifdef ODS2_FAULT_INJECTION
    if (vol->writable && getenv("ODS2_FAULT_COUNT") != NULL) {
        fprintf(stderr, "ODS2_FAULT: %ld writes\n", fault_writes);
    }
#endif
    if (vol->fp != NULL) {
        /* a last, best-effort sync for a caller that did not call ods2_sync() and look at its result (ods2
           does, before it reports a command done); see ods2_sync() */
        (void) ods2_sync(vol);
        fclose(vol->fp);
        vol->fp = NULL;
    }
}

static bool header_lbn(ods2_volume_t *vol, unsigned file_number, uint32_t *lbn_out)
{
    unsigned v = vol->home.cluster;
    unsigned m = vol->home.ibmapsize;
    unsigned target_vbn = v * 4 + m + file_number;
    return vbn_to_lbn(vol->indexf_extents, vol->indexf_extent_count, target_vbn, lbn_out);
}

/* The header a directory entry's FID names. An entry names a file by number AND relative volume number AND
   extended file number; finding the header by the low 16 bits alone read another file (review 8, R8-1).
   The file number is 24 bits (ods2_fid_number()). A relative volume number other than 0 names a file on
   another volume of a set: not supported. */
const char ods2_stale_entry[] = "no such file: a dangling directory entry, a stale alias (the file it names was deleted; "
                                "its file number may have been reused)";

ods2_result_t ods2_check_fid(ods2_fid_t fid)
{
    if (fid.fid_rvn != 0) {
        return fail("volume sets are not supported (a directory entry names a file on another volume "
                    "of a set)");
    }
    return ok();
}

ods2_result_t ods2_read_header_fid(ods2_volume_t *vol, ods2_fid_t fid, uint8_t *header_out)
{
    ods2_result_t r = ods2_check_fid(fid);
    if (!r.ok) return r;
    r = ods2_read_header(vol, ods2_fid_number(fid), header_out);
    if (!r.ok) return r;
    /* A directory entry names a file by number AND sequence number. VMS gives a file number a new sequence
       number each time it reuses it, so an entry whose sequence differs names a file that is gone: a stale
       alias (on a volume without HARDLINKS deleting the primary name leaves the other names behind, as on
       VMS). It must never resolve to the file that now has the number (F8). A header marked for delete is
       not a file either. */
    if (!ods2_header_is_file(header_out, fid)) {
        return fail(ods2_stale_entry);
    }
    return ok();
}

ods2_result_t ods2_read_header(ods2_volume_t *vol, unsigned file_number, uint8_t *header_out)
{
    uint32_t lbn;

    if (!header_lbn(vol, file_number, &lbn)) {
        return fail("file number's header VBN is not covered by INDEXF.SYS's known extents");
    }
    return read_block(vol, lbn, header_out, "could not read file header block");
}

ods2_result_t ods2_write_header(ods2_volume_t *vol, unsigned file_number, const uint8_t *header_in)
{
    uint32_t lbn;

    if (!header_lbn(vol, file_number, &lbn)) {
        return fail("file number's header VBN is not covered by INDEXF.SYS's known extents");
    }
    return ods2_write_block(vol, lbn, header_in);
}

ods2_result_t ods2_read_file_block(ods2_volume_t *vol, const uint8_t *header,
                                    unsigned vbn, uint8_t *block_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int n;
    uint32_t lbn;
    ods2_result_t r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);

    if (!r.ok) return r;
    if (n <= 0) {
        return fail("file has no decodable extents");
    }
    if (!vbn_to_lbn(extents, n, vbn, &lbn)) {
        return fail("requested VBN is not covered by this file's extents");
    }
    return read_block(vol, lbn, block_out, "could not read file content block");
}

ods2_result_t ods2_write_file_block(ods2_volume_t *vol, const uint8_t *header,
                                     unsigned vbn, const uint8_t *block_in)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int n;
    uint32_t lbn;
    ods2_result_t r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);

    if (!r.ok) return r;
    if (n <= 0) {
        return fail("file has no decodable extents");
    }
    if (!vbn_to_lbn(extents, n, vbn, &lbn)) {
        return fail("requested VBN is not covered by this file's extents");
    }
    return ods2_write_block(vol, lbn, block_in);
}

/* Shared body of ods2_allocate_blocks() and ods2_allocate_extent():
   with `partial_ok`, settles for the longest free run when none is
   long enough, allocating however many clusters that run holds. */
static ods2_result_t allocate_clusters(ods2_volume_t *vol, unsigned blocks_needed,
                                       bool partial_ok, uint32_t *lbn_out,
                                       unsigned *blocks_allocated_out)
{
    uint8_t bitmap_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_bitmap_blocks = 0;
    unsigned bits_blocks; /* blocks actually holding bitmap bits, excluding the SCB */
    uint8_t *bitmap_data;
    size_t total_bits, total_bytes;
    size_t start_cluster, found_clusters;
    unsigned clusters_needed = (blocks_needed + vol->home.cluster - 1) / vol->home.cluster;
    ods2_result_t r;
    unsigned i;

    if (!vol->writable) {
        return not_writable(vol);
    }

    /* BITMAP.SYS is always file number 2 - confirmed by real root
       directory data.  BITMAP.SYS FID is consistently (2,2,...) on
       every volume examined. */
    r = ods2_read_header(vol, 2, bitmap_header);
    if (!r.ok) return r;

    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode BITMAP.SYS's own extents");
    }
    for (i = 0; i < (unsigned) extent_count; i++) total_bitmap_blocks += extents[i].block_count;

    /* Spec 5.2.1 SCB: Virtual block 1 of the storage bitmap is the
       storage control block, this is a completely separate structure, NOT
       bitmap data. The actual bitmap bits start at VBN 2 confirmed by ANALYZE/DISK
       CHKSCB finding on a real volume. */
    if (total_bitmap_blocks < 2) {
        return fail("BITMAP.SYS is too small to contain both an SCB and bitmap data");
    }
    bits_blocks = total_bitmap_blocks - 1;
    {
        /* a damaged BITMAP.SYS header can claim any size: the bitmap of this image
           needs one bit per cluster, so more than a few blocks over that is damage */
        long here = ftell(vol->fp);
        long image_blocks = 0;
        if (fseek(vol->fp, 0, SEEK_END) == 0) image_blocks = ftell(vol->fp) / 512;
        (void) fseek(vol->fp, here, SEEK_SET);
        if (image_blocks > 0 && vol->home.cluster != 0 &&
            (uint64_t) bits_blocks > (uint64_t) image_blocks / vol->home.cluster / 4096u + vol->home.cluster + 8u) {
            return fail("BITMAP.SYS claims more blocks than the volume needs for its bitmap: damaged");
        }
    }

    total_bytes = (size_t) bits_blocks * 512;
    total_bits = total_bytes * 8;
    /* a cluster is available only if it lies wholly inside the volume (the bitmap's own tail bits, and a volume
       whose SCB says less than its bitmap offers, must not give out blocks past VOLSIZE) */
    if (vol->volsize != 0 && vol->home.cluster != 0 && total_bits > (size_t) (vol->volsize / vol->home.cluster)) {
        total_bits = (size_t) (vol->volsize / vol->home.cluster);
    }
    bitmap_data = malloc(total_bytes);
    if (bitmap_data == NULL) {
        return fail("could not allocate memory to read the storage bitmap");
    }

    /* i is a bitmap-data block index (0-based); the corresponding
       real VBN within BITMAP.SYS is i+2 (i=0 -> VBN 2, the first
       real bitmap block, skipping VBN 1's SCB). */
    for (i = 0; i < bits_blocks; i++) {
        r = ods2_read_file_block(vol, bitmap_header, i + 2, bitmap_data + (size_t) i * 512);
        if (!r.ok) {
            free(bitmap_data);
            return r;
        }
    }

    if (partial_ok) {
        if (!ods2_bitmap_find_run(bitmap_data, total_bits, clusters_needed,
                                  &start_cluster, &found_clusters)) {
            free(bitmap_data);
            return fail("no free space left on the volume");
        }
        clusters_needed = (unsigned) found_clusters;
    } else if (!ods2_bitmap_find_free(bitmap_data, total_bits, clusters_needed,
                                      &start_cluster, &found_clusters)) {
        free(bitmap_data);
        return fail("no free space found on volume for this allocation");
    }
    if (!ods2_bitmap_mark(bitmap_data, total_bits, start_cluster, clusters_needed, true)) {
        free(bitmap_data);
        return fail("internal error marking bitmap bits (should be unreachable)");
    }

    /* Write back only the blocks that could have changed covering the bits
       we just marked - rather than the entire potentially large bitmap. Block indices
       here are again bitmap-data-relative; +2 converts back to the real VBN. */
    {
        size_t first_byte = start_cluster / 8;
        size_t last_byte = (start_cluster + clusters_needed - 1) / 8;
        unsigned first_block = (unsigned) (first_byte / 512);
        unsigned last_block = (unsigned) (last_byte / 512);
        for (i = first_block; i <= last_block; i++) {
            r = ods2_write_file_block(vol, bitmap_header, i + 2,
                                       bitmap_data + (size_t) i * 512);
            if (!r.ok) {
                free(bitmap_data);
                return r;
            }
        }
    }

    free(bitmap_data);

    *lbn_out = (uint32_t) (start_cluster * vol->home.cluster);
    *blocks_allocated_out = clusters_needed * vol->home.cluster;
    return ok();
}

ods2_result_t ods2_allocate_blocks(ods2_volume_t *vol, unsigned blocks_needed,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out)
{
    return allocate_clusters(vol, blocks_needed, false, lbn_out, blocks_allocated_out);
}

ods2_result_t ods2_allocate_extent(ods2_volume_t *vol, unsigned max_blocks,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out)
{
    return allocate_clusters(vol, max_blocks, true, lbn_out, blocks_allocated_out);
}

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one (spec: a header
   whose Map Area can't hold all its retrieval pointers points to a
   continuation header via FH2$W_EXT_FID; that header's own ext_fid
   may point further still). Without this, any file needing more than
   one header would silently appear truncated. A generous but finite
   segment limit guards against a corrupted or circular ext_fid chain
   looping forever. */
#define ODS2_MAX_HEADER_SEGMENTS 64

ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out)
{
    uint8_t current[512];
    int total = 0;
    int segment;

    memcpy(current, header, 512);

    for (segment = 0; segment < ODS2_MAX_HEADER_SEGMENTS; segment++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) current;
        int n = decode_header_extents(current,
                                       extents_out + total,
                                       (max_extents > (size_t) total)
                                           ? max_extents - (size_t) total : 0);
        if (n == ODS2_DECODE_TOO_MANY) {
            /* never a shortened list: a reader would stop short of the file's end, a delete would free only
               the extents it was given and leak the rest */
            return fail("this file has more extents than ods2v2 can hold (1024): refused");
        }
        if (n < 0) {
            return fail("could not decode retrieval pointers for a header segment");
        }
        total += n;

        if (ods2_fid_number(core->ext_fid) == 0) {
            /* No further extension header - done. */
            *count_out = total;
            return ok();
        }

        {
            ods2_fid_t expect = core->ext_fid;
            ods2_result_t r = ods2_read_header(vol, ods2_fid_number(expect), current);
            if (!r.ok) return r;
            if (!extension_header_ok(current, expect,
                                     ods2_fid_number(((const ods2_head_core_t *) header)->fid),
                                     (unsigned) segment + 1)) {
                return fail("an extension header is not the next segment of this file "
                            "(wrong file number, segment number, back link or checksum): damaged");
            }
        }
    }

    return fail("extension header chain exceeded maximum segment limit "
                "(possibly corrupted or circular ext_fid chain)");
}

/* Walks every entry of a directory, one block at a time, handing each
   entry to `visit`. `visit` returns false to stop early (e.g. a name
   lookup that has found its match). Each block is parsed into a
   buffer sized for the most records a block can physically hold, so
   nothing is ever dropped here regardless of directory size - the
   caller decides what to keep. This is the one place that reads
   directory blocks; ods2_list_directory(), ods2_list_directory_alloc(),
   ods2_count_directory_entries() and ods2_lookup_name() are all thin
   visitors on top of it. */
typedef bool (*dir_visit_fn)(const ods2_dir_entry_t *entry, void *ctx);

/* Blocks of a directory that hold entries: those inside its
   end-of-file, at most its allocation. Covers both EOF forms in use:
   VMS's EFBLK n+1 / FFBYTE 0, and EFBLK n / FFBYTE 2, which ods2v2
   writes for a new one-block directory. */
static unsigned dir_blocks_in_use(const uint8_t *dir_header, unsigned allocated)
{
    size_t len = ods2_file_content_length(dir_header);
    size_t used = (len + 511) / 512;
    return (used > allocated) ? allocated : (unsigned) used;
}

static ods2_result_t walk_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    dir_visit_fn visit, void *ctx)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_blocks = 0, block_index;
    int i;
    ods2_result_t r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS, &extent_count);

    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode directory's retrieval pointers");
    }
    for (i = 0; i < extent_count; i++) total_blocks += extents[i].block_count;
    /* Only blocks inside the end-of-file hold entries - VMS reads no
       further, so neither does this: DIR shows what VMS will find. */
    total_blocks = dir_blocks_in_use(dir_header, total_blocks);

    for (block_index = 1; block_index <= total_blocks; block_index++) {
        uint8_t block[512];
        ods2_dir_entry_t block_entries[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        int n;
        r = ods2_read_file_block(vol, dir_header, block_index, block);
        if (!r.ok) return r;

        {
            unsigned bad = 0;
            n = ods2_parse_directory_records(block, sizeof(block),
                                             vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2,
                                             block_entries, ODS2_DIR_MAX_ENTRIES_PER_BLOCK, &bad);
            /* a record that cannot be read is skipped, the others in the block are still
               listed; the count is what the CLI warns about (an entirely empty block, size
               word 0, is no error: not every block of a directory need be full) */
            vol->dir_block_errors += bad;
        }
        if (n > ODS2_DIR_MAX_ENTRIES_PER_BLOCK) {
            /* Can't happen for anything ods2_parse_directory() accepts
               (see ODS2_DIR_MAX_ENTRIES_PER_BLOCK), but refuse loudly
               rather than silently drop entries if it ever does. */
            return fail("directory block holds more records than is physically possible");
        }
        for (i = 0; i < n; i++) {
            if (!visit(&block_entries[i], ctx)) return ok();
        }
    }
    return ok();
}

/* --- ods2_list_directory(): caller-supplied fixed buffer --- */
typedef struct {
    ods2_dir_entry_t *entries;
    size_t max;
    size_t total;
} fixed_list_ctx_t;

static bool visit_fixed_list(const ods2_dir_entry_t *entry, void *ctx_)
{
    fixed_list_ctx_t *ctx = (fixed_list_ctx_t *) ctx_;
    if (ctx->total < ctx->max) ctx->entries[ctx->total] = *entry;
    ctx->total++; /* keep counting past max so overflow is detectable */
    return true;
}

ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out)
{
    fixed_list_ctx_t ctx;
    ods2_result_t r;

    ctx.entries = entries_out;
    ctx.max = max_entries;
    ctx.total = 0;
    r = walk_directory(vol, dir_header, visit_fixed_list, &ctx);
    if (!r.ok) return r;

    *count_out = (int) ctx.total;
    if (ctx.total > max_entries) {
        /* Never silently truncate: report it, with *count_out set to
           the directory's real size so the caller can resize and
           retry (or use ods2_list_directory_alloc()). */
        return fail("directory has more entries than the supplied buffer holds");
    }
    return ok();
}

/* --- ods2_list_directory_alloc(): grows to fit --- */
typedef struct {
    ods2_dir_entry_t *entries;
    size_t count;
    size_t capacity;
    bool out_of_memory;
} alloc_list_ctx_t;

static bool visit_alloc_list(const ods2_dir_entry_t *entry, void *ctx_)
{
    alloc_list_ctx_t *ctx = (alloc_list_ctx_t *) ctx_;
    if (ctx->count == ctx->capacity) {
        size_t new_cap = (ctx->capacity == 0) ? 64 : ctx->capacity * 2;
        ods2_dir_entry_t *grown = realloc(ctx->entries, new_cap * sizeof(*grown));
        if (grown == NULL) {
            ctx->out_of_memory = true;
            return false;
        }
        ctx->entries = grown;
        ctx->capacity = new_cap;
    }
    ctx->entries[ctx->count++] = *entry;
    return true;
}

ods2_result_t ods2_list_directory_alloc(ods2_volume_t *vol, const uint8_t *dir_header,
                                         ods2_dir_entry_t **entries_out, int *count_out)
{
    alloc_list_ctx_t ctx;
    ods2_result_t r;

    ctx.entries = NULL;
    ctx.count = 0;
    ctx.capacity = 0;
    ctx.out_of_memory = false;

    *entries_out = NULL;
    *count_out = 0;

    r = walk_directory(vol, dir_header, visit_alloc_list, &ctx);
    if (r.ok && ctx.out_of_memory) r = fail("out of memory listing directory");
    if (!r.ok) {
        free(ctx.entries);
        return r;
    }
    *entries_out = ctx.entries;
    *count_out = (int) ctx.count;
    return ok();
}

/* --- ods2_count_directory_entries() --- */
static bool visit_count(const ods2_dir_entry_t *entry, void *ctx_)
{
    (void) entry;
    (*(size_t *) ctx_)++;
    return true;
}

ods2_result_t ods2_count_directory_entries(ods2_volume_t *vol, const uint8_t *dir_header,
                                            int *count_out)
{
    size_t total = 0;
    ods2_result_t r = walk_directory(vol, dir_header, visit_count, &total);
    if (!r.ok) return r;
    *count_out = (int) total;
    return ok();
}

size_t ods2_file_content_length(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    uint32_t efblk = ods2_word_swap32(core->recattr.efblk);
    uint16_t ffbyte = core->recattr.ffbyte;

    /* Spec 6.1.5/6.1.6: EFBLK is the VBN holding the end-of-file
       position and FFBYTE is the count of bytes in use in that block,
       so the content is (EFBLK-1) full blocks plus FFBYTE bytes. An
       EOF on a block boundary is EFBLK=n+1/FFBYTE=0 (preferred, and
       what every real VMS header in samples/ uses - e.g. BADBLK.SYS,
       empty: EFBLK=1 FFBYTE=0; a one-block directory: EFBLK=2
       FFBYTE=0) or EFBLK=n/FFBYTE=512. An earlier version read
       FFBYTE=0 as "the whole EFBLK block is in use", which returned
       512 bytes of junk for empty files, one block too many for real
       VMS files ending on a block boundary, and failed outright on a
       file with no allocation at all such as BADBLK.SYS. */
    if (efblk == 0) {
        /* An indexed file (FAT$V_FILEORG 2) keeps no end-of-file mark in its
           header: RMS knows where its buckets end, the file system only
           the allocation (measured on files made by VMS V8.4: EFBLK 0,
           DIRECTORY/FULL shows used = allocated). Its content as a byte
           stream is all of its blocks. Any other file with no mark was never
           written. */
        if ((core->recattr.rtype >> 4) == 2) {
            return (size_t) ods2_word_swap32(core->recattr.hiblk) * 512u;
        }
        return 0;
    }
    if (ffbyte > 512) {
        ffbyte = 512; /* out of range - never claim more than a block */
    }
    return (size_t) (efblk - 1) * 512u + ffbyte;
}

size_t ods2_file_allocated_bytes(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return (size_t) ods2_word_swap32(core->recattr.hiblk) * 512u;
}

ods2_result_t ods2_read_file_stream(ods2_volume_t *vol, const uint8_t *header,
                                     bool (*sink)(void *ctx, const uint8_t *data, size_t n),
                                     void *ctx, size_t *bytes_out, bool *sink_failed_out)
{
    size_t content_len = ods2_file_content_length(header);
    size_t allocated = ods2_file_allocated_bytes(header);
    size_t written = 0;
    ods2_result_t r = ok();

    if (bytes_out) *bytes_out = 0;
    if (sink_failed_out) *sink_failed_out = false;
    if (content_len > allocated) {
        /* Corrupt header: the EOF mark claims more data than the file
           has blocks. Say so, rather than fail later on an unmapped
           VBN (or have a caller size a buffer from a bogus length). */
        return fail("file's end-of-file mark lies beyond its allocated blocks");
    }

    /* Decoded once, then read an extent at a time in chunks of at
       most ODS2_STREAM_CHUNK - a large file is hundreds of thousands
       of blocks, too many to look up one by one, and too much to hold
       in memory at once (issue #16). */
    if (content_len > 0) {
        ods2_extent_t extents[ODS2_MAX_EXTENTS];
        size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? content_len : ODS2_STREAM_CHUNK;
        uint8_t *chunk;
        int n, i;

        r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);
        if (!r.ok) return r;
        chunk = malloc(chunk_cap);
        if (chunk == NULL) return fail("could not allocate a read buffer");
        for (i = 0; i < n && written < content_len && r.ok; i++) {
            size_t want = content_len - written;
            size_t run = (size_t) extents[i].block_count * 512u;
            if (want > run) want = run;
            if (fseek(vol->fp, (long) extents[i].lbn * 512, SEEK_SET) != 0) {
                r = fail("could not read file content block");
                break;
            }
            while (want > 0) {
                size_t piece = want < chunk_cap ? want : chunk_cap;
                if (fread(chunk, 1, piece, vol->fp) != piece) {
                    r = fail("could not read file content block");
                    break;
                }
                if (!sink(ctx, chunk, piece)) {
                    if (sink_failed_out) *sink_failed_out = true;
                    r = fail("could not deliver file content to its destination");
                    break;
                }
                written += piece;
                want -= piece;
            }
        }
        free(chunk);
        if (r.ok && written < content_len) {
            r = fail("requested VBN is not covered by this file's extents");
        }
    }
    if (bytes_out) *bytes_out = written;
    return r;
}

typedef struct {
    uint8_t *buf;
    size_t size, used;
} memory_sink_t;

static bool memory_sink(void *ctx_, const uint8_t *data, size_t n)
{
    memory_sink_t *ctx = (memory_sink_t *) ctx_;
    if (n > ctx->size - ctx->used) return false;
    memcpy(ctx->buf + ctx->used, data, n);
    ctx->used += n;
    return true;
}

ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out)
{
    memory_sink_t ctx;
    ods2_result_t r;

    if (ods2_file_content_length(header) > ods2_file_allocated_bytes(header)) {
        return fail("file's end-of-file mark lies beyond its allocated blocks");
    }
    if (ods2_file_content_length(header) > buf_size) {
        return fail("output buffer too small for file content");
    }
    ctx.buf = buf_out;
    ctx.size = buf_size;
    ctx.used = 0;
    r = ods2_read_file_stream(vol, header, memory_sink, &ctx, NULL, NULL);
    if (!r.ok) return r;
    *bytes_read_out = ctx.used;
    return ok();
}

/* Finds a free file number for a new file, starting the search at
   `start_from` (typically 17, just past the first-16 region that's
   reserved for direct home-block-formula lookup, though any value is
   valid). Spec 5.1.7: "A block containing a valid file header
   must never be used to create a new file, even if it is marked free
   in the index file bitmap. This prevents files from being lost if
   bits are dropped in the bitmap." Follow this literally: rather
   than trusting the index file bitmap as the sole source of truth
   (confirmed empirically to NOT reliably reflect real allocation on
   a freshly VMS-INITIALIZE'd volume - files 1-13 all show as "free"
   in the bitmap despite genuinely existing), we read each candidate
   header and treat fid_num==0 as the actual "unused" signal. This is
   slower than trusting the bitmap but is what the spec describes as
   the only safe approach. */
/* Marks `file_number`'s bit in the index file's own bitmap (the one
   at HM2$L_IBMAPLBN, tracking which file numbers/headers are
   allocated - completely distinct from BITMAP.SYS's data-block
   bitmap, which ods2_allocate_blocks() already maintains).
   Bit 0 corresponds to file number 1. Same set=free/clear=used
   convention as every other ODS-2 bitmap (Spec 5.2.2).

   This bitmap is directly, statically located from home block fields -
   not accessed as a regular file through INDEXF.SYS's own extents. */
static ods2_result_t mark_index_bitmap(ods2_volume_t *vol, unsigned file_number, bool used)
{
    unsigned bit_index = file_number - 1;
    unsigned bits_per_block = 512u * 8u;
    unsigned block_index = bit_index / bits_per_block;
    unsigned bit_in_block = bit_index % bits_per_block;
    unsigned byte_in_block = bit_in_block / 8;
    unsigned bit_in_byte = bit_in_block % 8;
    uint32_t lbn;
    uint8_t block[512];
    ods2_result_t r;

    if (block_index >= vol->home.ibmapsize) {
        return fail("file number is beyond the index bitmap's own extent");
    }
    lbn = vol->home.ibmaplbn + block_index;

    r = read_block(vol, lbn, block, "could not read index bitmap block");
    if (!r.ok) return r;

    /* Deliberately NOT using ods2_bitmap_mark() here - that helper
       implements the STORAGE bitmap's convention (set=free), and the
       index file bitmap uses the opposite (Spec 5.1.6, explicit:
       "if the bit is 1, then that file number is in use"). An
       earlier version of this code called ods2_bitmap_mark() anyway,
       which cleared the bit when marking a file number used -
       exactly backwards, silently telling ANALYZE/DISK this file
       number was NOT in use while a directory entry claimed it
       existed. */
    if (used) {
        block[byte_in_block] |= (uint8_t) (1u << bit_in_byte);
    } else {
        block[byte_in_block] &= (uint8_t) ~(1u << bit_in_byte);
    }

    return ods2_write_block(vol, lbn, block);
}

/* --- File number allocation (spec 5.1.6, 5.1.7) ---

   File numbers come from the index file bitmap. The header block a
   free bit points at is then checked:

   - Inside INDEXF.SYS's end-of-file, it is validated: a valid header
     is never reused even if its bit says free (dropped bitmap bits
     must not lose files); a deleted header gives the new file its
     sequence number + 1; anything else is garbage and gives 1.
   - Beyond the end-of-file, the block is garbage by definition: 1.

   And INDEXF.SYS's end-of-file must stay at or beyond the last header
   ever used. VMS treats a file whose header lies beyond it as not
   existing at all (SYSTEM-W-NOSUCHFILE, ANALDISK-W-LOSTHEADER), so a
   new header past it has to move it - which a freshly INITIALIZEd
   volume always needs, since INITIALIZE puts the end-of-file just
   after the reserved files. */

/* A file number is 24 bits: FID$W_NUM and FID$B_NMX (ods2_fid_number()). The volume's own limit is lower:
   the home block's MAXFILES and the index bitmap's size. */
#define MAX_FILE_NUMBER 0xffffffu

static uint32_t header_vbn(const ods2_volume_t *vol, unsigned file_number)
{
    return (uint32_t) vol->home.cluster * 4 + vol->home.ibmapsize + file_number;
}

typedef enum { SLOT_GARBAGE, SLOT_DELETED, SLOT_VALID } header_slot_t;

/* Classifies a header block found inside the end-of-file (spec 3.5.1,
   5.1.7). A checksum alone can't tell a header from an all-zero block
   (whose checksum is also zero), so a valid header must also have the
   header structure and a nonzero file number. */
static header_slot_t classify_header_slot(const uint8_t *h)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    uint16_t stored = (uint16_t) (h[510] | (h[511] << 8));
    bool structured = ((core->struclev >> 8) == ODS5_LEVEL_2 ||
                       (core->struclev >> 8) == ODS5_LEVEL_5) &&
                      core->idoffset >= 40 && core->idoffset <= core->mpoffset &&
                      core->mpoffset <= core->acoffset && core->acoffset <= core->rsoffset;

    if (!structured) return SLOT_GARBAGE;
    if ((core->fid.fid_num != 0 || core->fid.fid_nmx != 0) &&
        stored == ods2_checksum(h, 255)) {
        return SLOT_VALID;
    }
    if ((core->filechar & 0x8000u) && core->fid.fid_num == 0 && core->fid.fid_nmx == 0) {
        return SLOT_DELETED; /* FH2$M_MARKDEL, FID number zeroed */
    }
    return SLOT_GARBAGE;
}

/* Reads/writes one index file bitmap block (located directly from the
   home block, like mark_index_bitmap()). */
static ods2_result_t read_index_bitmap_block(ods2_volume_t *vol, unsigned block_index,
                                             uint8_t *block)
{
    uint32_t lbn = vol->home.ibmaplbn + block_index;
    return read_block(vol, lbn, block, "could not read index bitmap block");
}

/* Highest file number marked in use in the index file bitmap, or 0. */
static ods2_result_t highest_used_file_number(ods2_volume_t *vol, unsigned *highest_out)
{
    unsigned b, i;
    uint8_t block[512];

    *highest_out = 0;
    for (b = 0; b < vol->home.ibmapsize; b++) {
        ods2_result_t r = read_index_bitmap_block(vol, b, block);
        if (!r.ok) return r;
        for (i = 0; i < 512; i++) {
            if (block[i] != 0) {
                int bit;
                for (bit = 7; bit >= 0; bit--) {
                    if (block[i] & (1u << bit)) {
                        *highest_out = b * 4096u + i * 8u + (unsigned) bit + 1u;
                        break;
                    }
                }
            }
        }
    }
    return ok();
}

ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    uint8_t indexf_header[512];
    uint8_t block[512];
    unsigned limit, n, loaded_block = (unsigned) -1;
    uint32_t efblk;
    ods2_result_t r;

    if (start_from == 0) start_from = 1;
    limit = vol->home.maxfiles;
    if (limit > MAX_FILE_NUMBER) limit = MAX_FILE_NUMBER;
    if ((unsigned long) vol->home.ibmapsize * 4096u < limit) {
        limit = (unsigned) vol->home.ibmapsize * 4096u;
    }
    if (max_search < limit && start_from - 1 + max_search < limit) {
        limit = start_from - 1 + max_search;
    }

    r = ods2_read_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    efblk = ods2_word_swap32(((ods2_head_core_t *) indexf_header)->recattr.efblk);

    for (n = start_from; n <= limit; n++) {
        unsigned bit = n - 1;
        unsigned block_index = bit / 4096u;
        uint8_t header[512];

        if (block_index != loaded_block) {
            r = read_index_bitmap_block(vol, block_index, block);
            if (!r.ok) return r;
            loaded_block = block_index;
        }
        if (block[(bit % 4096u) / 8u] & (1u << (bit % 8u))) {
            continue; /* in use */
        }

        if (header_vbn(vol, n) >= efblk) {
            *file_number_out = n; /* beyond the end-of-file: garbage */
            *seq_out = 1;
            return ok();
        }
        r = ods2_read_header(vol, n, header);
        if (!r.ok) return r; /* inside the end-of-file yet unmapped: corrupt */
        switch (classify_header_slot(header)) {
        case SLOT_VALID:
            continue; /* free in the bitmap, but a live header: never reuse */
        case SLOT_DELETED: {
            uint16_t seq = (uint16_t) (((const ods2_head_core_t *) header)->fid.fid_seq + 1);
            *file_number_out = n;
            *seq_out = (seq == 0) ? 1 : seq; /* 0 is not a valid sequence number */
            return ok();
        }
        case SLOT_GARBAGE:
            *file_number_out = n;
            *seq_out = 1;
            return ok();
        }
    }
    return fail("no free file number: the index file bitmap is full (or every "
                "free number's header slot holds a valid header)");
}

/* Writes INDEXF.SYS's own header (file 1) with a fresh checksum, and
   the same bytes to the backup index file header (spec 5.1.5), which
   exists so the volume can be recovered if the primary goes bad - a
   stale copy would describe the wrong extents. Then refreshes the
   cached INDEXF.SYS extents used to locate headers. */
static ods2_result_t write_indexf_header(ods2_volume_t *vol, uint8_t *indexf_header)
{
    uint16_t checksum = ods2_checksum(indexf_header, 255);
    ods2_result_t r;
    int count;

    indexf_header[510] = (uint8_t) (checksum & 0xff);
    indexf_header[511] = (uint8_t) (checksum >> 8);
    r = ods2_write_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    if (vol->home.altidxlbn != 0) {
        r = ods2_write_block(vol, vol->home.altidxlbn, indexf_header);
        if (!r.ok) return r;
    }
    count = decode_header_extents(indexf_header, vol->indexf_extents, ODS2_MAX_EXTENTS);
    if (count == ODS2_DECODE_TOO_MANY) {
        return fail("INDEXF.SYS would need more extents than ods2v2 can hold (1024)");
    }
    if (count <= 0) {
        return fail("INDEXF.SYS's header no longer decodes after updating it");
    }
    vol->indexf_extent_count = count;
    return ok();
}

ods2_result_t ods2_indexf_map_extent(uint8_t *indexf_header, uint32_t lbn, unsigned block_count)
{
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    size_t size = ods2_retrieval_pointer_size(lbn, block_count);
    uint32_t hiblk;

    if (size == 0) {
        return fail("internal error: INDEXF.SYS extent cannot be described by "
                    "any retrieval pointer format");
    }
    if (map_end > 510 || map_offset + map_bytes + size > map_end) {
        return fail("INDEXF.SYS's header has no room for another extent "
                    "(would need an extension header - not yet implemented)");
    }
    if (ods2_encode_retrieval_pointer(indexf_header + map_offset + map_bytes,
                                      lbn, block_count) != size) {
        return fail("internal error: could not encode an INDEXF.SYS retrieval pointer");
    }
    core->map_inuse = (uint8_t) ((map_bytes + size) / 2);
    hiblk = ods2_word_swap32(core->recattr.hiblk) + block_count;
    core->recattr.hiblk = ods2_word_swap32(hiblk);
    /* INITIALIZE leaves FH2$L_HIGHWATER at HIBLK + 1 on INDEXF.SYS;
       keep that relationship. */
    if (core->highwater != 0) core->highwater = hiblk + 1;
    return ok();
}

/* Allocates more blocks to INDEXF.SYS and maps them, so more headers
   fit. Up to 256 blocks at a time; falls back to a single cluster if
   no run that long is free. The new piece is mapped with the smallest
   retrieval pointer that describes it - Format 1 below LBN 4,194,303,
   Format 2 beyond (issue #14) - so INDEXF.SYS can grow anywhere on a
   volume of any size. */
static ods2_result_t extend_indexf(ods2_volume_t *vol, uint8_t *indexf_header, uint32_t *lbn_out,
                                  unsigned *got_out)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) indexf_header;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    unsigned want = (256u / vol->home.cluster) * vol->home.cluster;
    unsigned hiblk = ods2_word_swap32(core->recattr.hiblk);
    uint32_t lbn;
    unsigned got;
    ods2_result_t r;

    if (hiblk >= 4096) {
        /* A big index file grows by half its size, at most 16384 blocks (one Format 2 pointer): a volume
           with a million files would otherwise need thousands of 256-block pointers, and the primary
           header's map area holds about 77 pointers (there are no INDEXF.SYS extension headers here). */
        want = hiblk / 2;
        if (want > 16384) want = 16384;
        want = (want / vol->home.cluster) * vol->home.cluster;
    }
    if (want == 0) want = vol->home.cluster;
    /* Check first, before taking any blocks, that the smallest pointer
       (4 bytes) could still fit; the exact size is checked after
       allocation, once the LBN is known. */
    if (map_end > 510 || map_offset + map_bytes + 4 > map_end) {
        return fail("INDEXF.SYS's header has no room for another extent "
                    "(would need an extension header - not yet implemented)");
    }
    r = ods2_allocate_blocks(vol, want, &lbn, &got);
    if (!r.ok && want > (256u / vol->home.cluster) * vol->home.cluster && want > vol->home.cluster) {
        want = (256u / vol->home.cluster) * vol->home.cluster; /* no run that long: the smaller step */
        if (want == 0) want = vol->home.cluster;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
    }
    if (!r.ok && want > vol->home.cluster) {
        want = vol->home.cluster;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
    }
    if (!r.ok) return r;
    {
        /* The new blocks are zeroed (review 6, finding 7): the clusters hold whatever was in them, and
           header images left in free space (an earlier index file, a BACKUP save set kept as a file) would
           be seen by ANALYZE as valid headers, to be resurrected by REPAIR. Done before they are mapped. */
        uint8_t zero[512];
        unsigned v;
        memset(zero, 0, sizeof(zero));
        for (v = 0; v < got; v++) {
            r = ods2_write_block(vol, lbn + v, zero);
            if (!r.ok) {
                ods2_free_blocks(vol, lbn, got);
                return r;
            }
        }
    }
    r = ods2_indexf_map_extent(indexf_header, lbn, got);
    if (!r.ok) {
        /* ods2_allocate_blocks() has already marked them used; give
           them back rather than leak them. */
        ods2_free_blocks(vol, lbn, got);
        return r;
    }
    *lbn_out = lbn;
    *got_out = got;
    return ok();
}

/* Allocates a file number for a new file: chooses it (above), makes
   sure INDEXF.SYS is big enough to hold its header and that its
   end-of-file covers it, then marks it in use in the bitmap. The
   end-of-file moves to just past whichever is higher: the new header,
   or the highest header the bitmap says is in use - the second puts
   right volumes where earlier ods2v2 versions wrote headers beyond
   the end-of-file, leaving those files invisible to VMS. INDEXF.SYS's
   header is written before the bitmap, so an interruption leaves at
   worst a garbage block inside the end-of-file, which the spec allows. */
static ods2_result_t header_ok_for_write(const uint8_t *h, unsigned file_number);

static ods2_result_t allocate_file_number(ods2_volume_t *vol, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    uint8_t indexf_header[512];
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    unsigned n, highest_used, extensions = 0;
    uint16_t seq;
    uint32_t vbn, efblk, new_efblk;
    bool changed = false;
    uint32_t new_lbn[64];        /* the extents added to INDEXF.SYS by this call, given back if it fails */
    unsigned new_got[64];
    unsigned added = 0, a;
    ods2_result_t r;

    r = ods2_find_free_file_number(vol, 1, MAX_FILE_NUMBER, &n, &seq);
    if (!r.ok) return r;
    {
        /* a header slot whose block lies past the end of the volume is no slot (a damaged SCB or index) */
        uint32_t slot_lbn;
        if (header_lbn(vol, n, &slot_lbn) && !lbn_in_volume(vol, slot_lbn)) {
            return fail("no free file header lies inside the volume (the storage control block's volume size)");
        }
    }
    r = ods2_read_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    /* INDEXF.SYS's own header is rewritten with a fresh checksum (and its backup with it): it must be sound
       first (review 7, R7-4) */
    r = header_ok_for_write(indexf_header, 1);
    if (!r.ok) return r;

    vbn = header_vbn(vol, n);
    while (vbn > ods2_word_swap32(core->recattr.hiblk)) {
        if (++extensions > 64) {
            r = fail("INDEXF.SYS could not be extended far enough to hold the new header");
            goto give_back;
        }
        r = extend_indexf(vol, indexf_header, &new_lbn[added], &new_got[added]);
        if (!r.ok) goto give_back;
        added++;
        changed = true;
    }

    r = highest_used_file_number(vol, &highest_used);
    if (!r.ok) goto give_back;
    new_efblk = vbn + 1;
    if (highest_used > n) {
        uint32_t used_vbn = header_vbn(vol, highest_used);
        if (used_vbn <= ods2_word_swap32(core->recattr.hiblk) && used_vbn + 1 > new_efblk) {
            new_efblk = used_vbn + 1;
        }
    }
    efblk = ods2_word_swap32(core->recattr.efblk);
    if (new_efblk > efblk) {
        core->recattr.efblk = ods2_word_swap32(new_efblk);
        core->recattr.ffbyte = 0; /* EFBLK n+1 / FFBYTE 0: as VMS writes it */
        changed = true;
    }
    if (changed) {
        r = write_indexf_header(vol, indexf_header);
        if (!r.ok) return r;   /* the header may be on disk by now: the new extents are left as they are */
    }

    r = mark_index_bitmap(vol, n, true);
    if (!r.ok) return r;
    *file_number_out = n;
    *seq_out = seq;
    return ok();

give_back:
    /* nothing of INDEXF.SYS's header was written: the extents taken for it are free again */
    for (a = 0; a < added; a++) ods2_free_blocks(vol, new_lbn[a], new_got[a]);
    return r;
}

/* --- ods2_lookup_name(): streams the directory, no size limit --- */
static const char ods2_name_not_found[] = "name not found in directory"; /* compared by address */
#define ODS2_NAME_NOT_FOUND ods2_name_not_found
typedef struct {
    const char *name;
    ods2_fid_t fid;
    bool found;
} lookup_ctx_t;

static bool visit_lookup(const ods2_dir_entry_t *entry, void *ctx_)
{
    lookup_ctx_t *ctx = (lookup_ctx_t *) ctx_;
    /* Case-insensitive compare - VMS names are conventionally
       uppercase on disk, but callers may pass either case. */
    if (ods2_dir_name_compare(entry->name, strlen(entry->name),
                              ctx->name, strlen(ctx->name)) == 0) {
        ctx->fid = entry->fid;
        ctx->found = true;
        return false; /* stop walking */
    }
    return true;
}

ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out)
{
    /* Walks the directory block by block rather than listing it into
       a fixed buffer first - an earlier version used a 256-entry
       buffer here, so any name sorting after the 256th entry of its
       directory was silently reported as "not found". */
    lookup_ctx_t ctx;
    ods2_result_t r;

    ctx.name = name;
    ctx.found = false;
    r = walk_directory(vol, dir_header, visit_lookup, &ctx);
    if (!r.ok) return r;
    if (!ctx.found) return fail(ODS2_NAME_NOT_FOUND);
    *fid_out = ctx.fid;
    return ok();
}

/* --- ods2_lookup_name_version(): one specific version of a name --- */

/* Case-insensitive equality of two nul-terminated names. */
/* Same name as a directory compares them: case-insensitive, and
   "MAKEFILE" equals "MAKEFILE." (see ods2_dir_name_compare()). */
static bool names_equal(const char *a, const char *b)
{
    return ods2_dir_name_compare(a, strlen(a), b, strlen(b)) == 0;
}

typedef struct {
    const char *name;
    int version;         /* selector, see ODS2_VERSION_HIGHEST */
    int seen;            /* versions of `name` seen so far */
    bool name_found;
    bool found;
    ods2_fid_t fid;
    uint16_t found_version;
} version_lookup_ctx_t;

static bool visit_lookup_version(const ods2_dir_entry_t *entry, void *ctx_)
{
    version_lookup_ctx_t *ctx = (version_lookup_ctx_t *) ctx_;

    if (!names_equal(entry->name, ctx->name)) {
        /* Directories are sorted by name, so every version of a name
           is contiguous (even when split across several records and
           blocks). Once past them there is nothing more to find. */
        return !ctx->name_found;
    }
    ctx->name_found = true;

    if (ctx->version > 0) {
        if (entry->version == (unsigned) ctx->version) {
            ctx->found = true;
        }
    } else {
        /* Highest (0) or relative (-n): versions are stored newest
           first, so the n-th one encountered (0-based) is the one
           wanted - counting existing versions, as VMS does. */
        if (ctx->seen == -ctx->version) {
            ctx->found = true;
        }
    }
    ctx->seen++;

    if (ctx->found) {
        ctx->fid = entry->fid;
        ctx->found_version = entry->version;
        return false; /* stop walking */
    }
    return true;
}

ods2_result_t ods2_lookup_name_version(ods2_volume_t *vol, const uint8_t *dir_header,
                                        const char *name, int version,
                                        ods2_fid_t *fid_out, uint16_t *version_out)
{
    version_lookup_ctx_t ctx;
    ods2_result_t r;

    if (version > ODS2_MAX_VERSION || version < -ODS2_MAX_VERSION) {
        return fail("version number out of range (1..32767)");
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;
    ctx.version = version;
    r = walk_directory(vol, dir_header, visit_lookup_version, &ctx);
    if (!r.ok) return r;
    if (!ctx.name_found) return fail(ODS2_NAME_NOT_FOUND);
    if (!ctx.found) return fail("version not found");
    *fid_out = ctx.fid;
    if (version_out != NULL) *version_out = ctx.found_version;
    return ok();
}

ods2_result_t ods2_lookup_path(ods2_volume_t *vol, const char *path, ods2_fid_t *fid_out)
{
    ods2_fid_t current;
    char component[ODS2_DIR_MAX_NAME + 8];
    const char *p = path;

    /* Root directory's FID is always (4,4) - not a magic constant,
       this is INDEXF.SYS's own well-known convention (file 4 is
       000000.DIR;1 on every ODS-2 volume), confirmed against real
       data throughout this project. */
    current.fid_num = 4;
    current.fid_seq = 4;
    current.fid_rvn = 0;
    current.fid_nmx = 0;

    if (*path == '\0') {
        *fid_out = current;
        return ok();
    }

    while (*p) {
        uint8_t header[512];
        ods2_result_t r;
        size_t len = 0, ulen;
        char spelling[ODS2_DIR_MAX_NAME * 3 + 8];

        /* One directory name, written as a spelling: a '.' only ends it
           if it is not escaped (^.), and ^-escapes name the characters. */
        while (*p && *p != '.' && len < sizeof(spelling) - 1) {
            if (*p == '^' && p[1] != '\0' && len < sizeof(spelling) - 3) {
                spelling[len++] = *p++;
            }
            spelling[len++] = *p++;
        }
        spelling[len] = '\0';
        if (*p == '.') p++;
        ulen = ods5_unescape(spelling, len, component, sizeof(component) - 5);
        if (ulen == (size_t) -1) return fail("bad ^ escape in a directory name");

        r = ods2_read_header_fid(vol, current, header);
        if (!r.ok) return r;

        strcat(component, ".DIR");
        r = ods2_lookup_name(vol, header, component, &current);
        if (!r.ok) return r;
        {
            /* a file named X.DIR is not a directory: never walk into it (a writer would
               rewrite its data as directory blocks) */
            uint8_t next_header[512];
            r = ods2_read_header_fid(vol, current, next_header);
            if (!r.ok) return r;
            if (!(((const ods2_head_core_t *) next_header)->filechar & 0x2000u)) {
                return fail("a directory name in the path is a file that is not a directory");
            }
        }
    }

    *fid_out = current;
    return ok();
}

/* A header read for a change (review 6, finding 4): nothing on the write path may rewrite a header with a
   fresh checksum without looking at it, or the damage would be made to look right. The checksum, the level,
   the offsets in order and inside the block, room for the identification area the writers store into (a
   level 2 ident up to its revision date, a level 5 one up to its attribute date), and the FID's own file
   number. A header that fails is not changed. */
static ods2_result_t header_ok_for_write(const uint8_t *h, unsigned file_number)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    unsigned level = core->struclev >> 8;
    size_t ident_need = (level == ODS5_LEVEL_5) ? ODS5_FI5_ATTDATE + 8 : 38;

    if (ods2_checksum(h, 255) != (uint16_t) (h[510] | (h[511] << 8)) ||
        (level != ODS5_LEVEL_2 && level != ODS5_LEVEL_5) ||
        core->idoffset < 40 || core->idoffset > core->mpoffset || core->mpoffset > core->acoffset ||
        core->acoffset > core->rsoffset ||
        (size_t) core->idoffset * 2 + ident_need > (size_t) core->mpoffset * 2 ||
        ods2_fid_number(core->fid) != file_number || core->fid.fid_rvn != 0) {
        return fail("a file header is damaged (checksum, offsets or file number): it is not changed - "
                    "run ANALYZE/DISK_STRUCTURE/REPAIR on VMS");
    }
    return ok();
}

static ods2_result_t load_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    unsigned spare, uint8_t **blocks_out, unsigned *used_out);

/* header_ok_for_write() for a directory that is about to be changed. A directory with an extension header
   is refused (review 6, finding 5): store_directory moves a directory that outgrows its allocation into one
   new extent and replaces only the primary header's map, so the old chain's extents would stay mapped
   after the move, and later writes would land in clusters now owned by other files. ods2v2 never makes such
   a directory (a contiguous directory has one extent), and VMS keeps them contiguous; to release the chain
   instead would be new, untested code on the one path that holds every name on the volume. */
static ods2_result_t directory_header_ok(const uint8_t *h, unsigned file_number)
{
    ods2_result_t r = header_ok_for_write(h, file_number);
    if (!r.ok) return r;
    if (ods2_fid_number(((const ods2_head_core_t *) h)->ext_fid) != 0) {
        return fail("this directory has an extension header (it is fragmented over many extents): it is "
                    "not changed - ods2v2 does not write to such a directory");
    }
    return ok();
}

/* Before a name is created: ok if the directory has no such name, `exists` if it has, and the lookup's own
   failure if the directory could not be read - a read error or a bad extent is not "no such name"
   (review 6, finding 8: the create went on and could insert a second entry with the same name). */
static ods2_result_t name_available(ods2_volume_t *vol, const uint8_t *dir_header, const char *name,
                                    const char *exists)
{
    ods2_fid_t existing;
    ods2_result_t r = ods2_lookup_name(vol, dir_header, name, &existing);
    if (r.ok) return fail(exists);
    if (r.problem == ODS2_NAME_NOT_FOUND) return ok(); /* the lookup's own status, not its wording */
    return r;
}

/* Before anything is allocated or written for a change to a directory: its header and every block of it
   are in order (header_ok_for_write, load_directory's block check), so a refusal leaves the volume as it
   was instead of undoing half a create. `file_number` is the number the header was read from. */
static ods2_result_t directory_ready(ods2_volume_t *vol, const uint8_t *dir_header, unsigned file_number)
{
    uint8_t *blocks = NULL;
    unsigned used;
    ods2_result_t r = directory_header_ok(dir_header, file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    if (!r.ok) return r;
    free(blocks);
    return ok();
}

/* The number of directories between [000000] and the directory with this header: 0 for the master file
   directory itself, 1 for [A], 2 for [A.B]. Counted up the back links; a loop or a chain longer than the
   limit is refused. */
ods2_result_t ods2_directory_depth(ods2_volume_t *vol, const uint8_t *dir_header, unsigned *depth_out)
{
    unsigned depth = 0;
    uint8_t up[512];
    ods2_result_t r;

    memcpy(up, dir_header, sizeof(up));
    while (ods2_fid_number(((const ods2_head_core_t *) up)->fid) != 4) {
        const ods2_head_core_t *uc = (const ods2_head_core_t *) up;
        ods2_fid_t parent = uc->backlink;
        if (++depth > ODS2_MAX_DIR_DEPTH + 1u) break;
        r = ods2_read_header_fid(vol, parent, up);
        if (!r.ok) return r;
    }
    *depth_out = depth;
    return ok();
}

static ods2_result_t create_directory_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                           const char *name, ods2_fid_t *new_fid_out);

ods2_result_t ods2_create_directory(ods2_volume_t *vol, const uint8_t *parent_header,
                                     const char *name, ods2_fid_t *new_fid_out)
{
    TXN_BEGIN_OR_RETURN(vol);
    return txn_end(vol, create_directory_impl(vol, parent_header, name, new_fid_out));
}

static ods2_result_t create_directory_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                           const char *name, ods2_fid_t *new_fid_out)
{
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    char dirname[ODS5_MAX_LITERAL_NAME + 8];
    unsigned file_number;
    uint16_t seq_num;
    uint32_t content_lbn;
    unsigned content_blocks;
    ods2_extent_t extent;
    ods2_header_spec_t spec;
    uint8_t new_header[512];
    uint8_t content_block[512];
    bool header_written = false;
    ods2_result_t r;

    if (!vol->writable) {
        return not_writable(vol);
    }
    if (vol->ods5) {
        /* Level 5: the name as given, case kept; the characters VMS
           accepts (ods5_check_new_name), at most 236 bytes with ".DIR" */
        const char *why;
        if (strlen(name) + 5 > sizeof(dirname)) return fail("directory name too long");
        snprintf(dirname, sizeof(dirname), "%s.DIR", name);
        why = ods5_check_new_name(dirname);
        if (why != NULL) return fail(why);
    } else {
        if (contains_wildcard(name)) {
            return fail("directory name cannot contain wildcard characters ('*' or '%')");
        }
        if (strchr(name, ';') != NULL) {
            return fail("directory name cannot contain ';' (a version belongs in the "
                        "directory entry, never in the name itself)");
        }
        {
            const char *why;
            if (!ods2_valid_dir_name(name, &why)) return fail(why);
        }
        strcpy(dirname, name); /* at most 39 characters, checked above */
        uppercase_str(dirname); /* real VMS always stores names uppercase */
        strcat(dirname, ".DIR");
    }

    /* Refuse to create a name that already exists matching VMS
       behavior (CREATE/DIRECTORY on an existing name fails) and
       avoids silently shadowing an existing directory. */
    r = directory_ready(vol, parent_header, ods2_fid_number(parent_core->fid));
    if (!r.ok) return r;
    {
        /* VMS nests directories at most 255 deep, and DIR [...] and EXPORT stop there: a tree made deeper
           could not be walked again (F5). The depth of the parent is counted up the back links. */
        unsigned depth = 0;
        r = ods2_directory_depth(vol, parent_header, &depth);
        if (!r.ok) return r;
        if (depth + 1 > ODS2_MAX_DIR_DEPTH) {
            return fail("directories can be nested at most 255 levels deep");
        }
    }
    if (!r.ok) return r;
    r = name_available(vol, parent_header, dirname,
                       "a file or directory with this name already exists in the parent");
    if (!r.ok) return r;

    /* A file number, its header slot inside INDEXF.SYS's end-of-file,
       and its bit marked in use. */
    r = allocate_file_number(vol, &file_number, &seq_num);
    if (!r.ok) return r;

    /* WARNING Allocate one cluster's worth of content space - small and
       simple for this first implementation; a real, populated directory
       would need extension support (allocating and linking a second
       block/extent) once it outgrows this, which is not yet
       implemented. */
    r = ods2_allocate_blocks(vol, vol->home.cluster, &content_lbn, &content_blocks);
    if (!r.ok) {
        /* the file number was taken first: give it back (review 6, finding 6; a full volume is the usual
           way to get here) */
        mark_index_bitmap(vol, file_number, false);
        return r;
    }

    extent.lbn = content_lbn;
    extent.block_count = content_blocks;

    memset(&spec, 0, sizeof(spec));
    ods2_fid_set_number(&spec.fid, file_number);
    spec.fid.fid_seq = seq_num;
    spec.backlink = parent_core->fid;
    /* FH2$M_DIRECTORY | FH2$M_CONTIG - confirmed real values from the
       project fixes, independently re-confirmed by reading real DECUS.DIR/root
       headers earlier in this project. */
    spec.filechar = 0x2080;
    spec.rtype = 2;   /* FAB$C_VAR - confirmed real value for directories */
    spec.rattrib = 8; /* NOSPAN - confirmed real value for directories */
    spec.rsize = 512;
    spec.maxrec = 512;
    spec.extents = &extent;
    spec.extent_count = 1;
    spec.hiblk = content_blocks;
    if (vol->ods5) spec.highwater = content_blocks + 1; /* the whole allocation is written, below */
    /* every directory VMS made has FFBYTE 0 and EFBLK one past its last block in use: ODS-5 (V8.4,
       ODS5_FORMAT_NOTES.md 11) and ODS-2 (the ODS2T corpus volumes on V8.4, V7.3-2 and VAX V7.3: EFBLK 2,
       FFBYTE 0 for a one-block directory). ods2v2 used to write EFBLK 1 / FFBYTE 2 ("the sentinel only")
       on ODS-2. */
    spec.efblk = 2;
    spec.ffbyte = 0;
    /* a directory made in one with a default version limit passes it on (AXP1SYS: 114 of 114 directories under a
       default of 3 carry 3), a directory in one without gets none */
    spec.versions = parent_core->recattr.versions;

    {
        char ident[ODS5_MAX_LITERAL_NAME + 16]; /* dirname is already bounded
                             (checked above) plus ";1" and a nul - this
                             is always large enough, sized generously
                             to also silence a spurious compiler
                             truncation warning */
        snprintf(ident, sizeof(ident), "%s;1", dirname);
        spec.ident_name = ident;
        spec.level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
        spec.link_count = (vol->home.volchar & ODS5_VC_HARDLINKS) ? 1 : 0;

        if (!ods2_build_file_header(new_header, &spec)) {
            ods2_free_blocks(vol, content_lbn, content_blocks);
            mark_index_bitmap(vol, file_number, false);
            return fail("could not construct new directory's header");
        }
    }

    /* The content first, then the header, then the entry (review 6, finding 6): whatever an interruption or
       a failure leaves is a number and some clusters nothing refers to, never a valid directory header
       over a block that is not a directory block. The blocks are written through the header in memory. */
    memset(content_block, 0, sizeof(content_block));
    content_block[0] = 0xff;
    content_block[1] = 0xff;
    r = ods2_write_file_block(vol, new_header, 1, content_block);
    if (!r.ok) goto undo;
    if (vol->ods5) {
        /* the rest of the allocation as zeros, so that HIGHWATER = HIBLK + 1
           (as VMS V8.4 makes a directory) tells the truth */
        unsigned v;
        memset(content_block, 0, sizeof(content_block));
        for (v = 2; v <= content_blocks; v++) {
            r = ods2_write_file_block(vol, new_header, v, content_block);
            if (!r.ok) goto undo;
        }
    }

    r = ods2_write_header(vol, file_number, new_header);
    if (!r.ok) goto undo;
    header_written = true;

    /* Insert an entry for the new directory into the parent -
       growing the parent's own allocation automatically if none of
       its existing content blocks have room. */
    r = ods2_insert_into_directory(vol, ods2_fid_number(parent_core->fid), dirname, 1, spec.fid);
    if (!r.ok) goto undo;

    *new_fid_out = spec.fid;
    return ok();

undo:
    /* no entry in the parent: the new directory must not stay behind unreachable (header marked deleted as
       ods2_delete_version does, blocks and file number given back) */
    if (header_written) {
        ods2_head_core_t *dc = (ods2_head_core_t *) new_header;
        dc->filechar |= 0x8000u; /* FH2$M_MARKDEL */
        dc->fid.fid_num = 0;
        dc->fid.fid_nmx = 0;
        dc->fid.fid_rvn = 0;
        new_header[510] = 0;
        new_header[511] = 0;
        ods2_write_header(vol, file_number, new_header);
    }
    ods2_free_blocks(vol, content_lbn, content_blocks);
    mark_index_bitmap(vol, file_number, false);
    return r;
}

/* Writes src->length bytes, read from `src` a chunk at a time, into
   the blocks described by `extents`, in order, zero-filling the rest
   of the last block and any further allocated blocks. Seeks once per
   extent and writes it a chunk at a time, so memory use stays at one
   chunk however large the file (issue #16). */
static ods2_result_t write_extents(ods2_volume_t *vol, const ods2_extent_t *extents,
                                   int extent_count, ods2_content_source_t *src)
{
    static const uint8_t zeros[65536];
    size_t offset = 0;
    size_t content_len = src->length;
    size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? content_len : ODS2_STREAM_CHUNK;
    uint8_t *chunk = NULL;
    ods2_result_t r = ok();
    int i;

    if (!vol->writable) {
        return not_writable(vol);
    }
    if (chunk_cap > 0) {
        chunk = malloc(chunk_cap);
        if (chunk == NULL) return fail("could not allocate a write buffer");
    }
    /* nothing at or past the end of the volume is written (the footer of a simulator's container, or whatever
       follows, is not the volume's): checked for every extent before the first byte goes out */
    for (i = 0; i < extent_count; i++) {
        if (vol->volsize != 0 && (uint64_t) extents[i].lbn + extents[i].block_count > vol->volsize) {
            free(chunk);
            return fail("a file's blocks lie at or past the end of the volume (the storage control block's volume "
                        "size): nothing is written there");
        }
    }
    for (i = 0; i < extent_count && r.ok; i++) {
        size_t run_bytes = (size_t) extents[i].block_count * 512u;
        size_t data = (offset < content_len) ? content_len - offset : 0;

        if (data > run_bytes) data = run_bytes;
        if (fseek(vol->fp, (long) extents[i].lbn * 512, SEEK_SET) != 0) {
            r = fail("could not seek to file content blocks");
            break;
        }
        run_bytes -= data;
        while (data > 0) {
            size_t piece = data < chunk_cap ? data : chunk_cap;
            if (!src->read(src->ctx, chunk, piece)) {
                r = fail(src->problem != NULL ? src->problem
                                              : "could not read the file's content");
                break;
            }
            if (fwrite(chunk, 1, piece, vol->fp) != piece) {
                r = fail("could not write file content blocks");
                break;
            }
            if (fflush(vol->fp) != 0) {
                r = fail("could not flush written file content");
                break;
            }
            fault_point();
            data -= piece;
            offset += piece;
        }
        while (r.ok && run_bytes > 0) {
            size_t n = (run_bytes < sizeof(zeros)) ? run_bytes : sizeof(zeros);
            if (fwrite(zeros, 1, n, vol->fp) != n) {
                r = fail("could not write file content blocks");
                break;
            }
            run_bytes -= n;
        }
    }
    free(chunk);
    if (r.ok && fflush(vol->fp) != 0) {
        r = fail("could not flush written file content");
    }
    return r;
}

/* An ods2_content_source_t over a buffer already in memory. */
typedef struct {
    const uint8_t *data;
    size_t pos;
} memory_source_t;

static bool memory_source_read(void *ctx_, uint8_t *buf, size_t n)
{
    memory_source_t *ctx = (memory_source_t *) ctx_;
    memcpy(buf, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return true;
}

static ods2_result_t create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                 const char *name, ods2_content_source_t *src,
                                 const ods2_record_attrs_t *attrs, unsigned special,
                                 ods2_fid_t *new_fid_out);

const char *ods2_check_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len)
{
    unsigned cc = attrs->rattrib & (ODS2_RAT_FTN | ODS2_RAT_CR | ODS2_RAT_PRN);

    switch (attrs->rtype) {
    case ODS2_RFM_FIX:
        if (attrs->rsize == 0) {
            return "a fixed-length record format needs a record size";
        }
        if (attrs->rsize > 32766) {
            return "fixed record size is too large (at most 32766 bytes)";
        }
        if (attrs->rsize % 2 != 0) {
            return "odd fixed record sizes are not supported (RMS pads each such "
                   "record to an even length on disk)";
        }
        if (attrs->maxrec != attrs->rsize) {
            return "for fixed-length records the maximum record size (MRS) must "
                   "equal the record size";
        }
        if (content_len % attrs->rsize != 0) {
            return "content length is not a whole number of fixed-length records";
        }
        break;
    case ODS2_RFM_UDF:
    case ODS2_RFM_STM:
    case ODS2_RFM_STMLF:
    case ODS2_RFM_STMCR:
        if (attrs->rsize > 32767 || attrs->maxrec > 32767) {
            return "record size is too large (at most 32767 bytes)";
        }
        break;
    case ODS2_RFM_VAR:
    case ODS2_RFM_VFC:
        return "variable-length record formats (VAR, VFC) can't be imported: on "
               "disk each record starts with a byte count the host file doesn't have";
    default:
        return "unknown record format";
    }
    if (attrs->rattrib & ~0x0fu) {
        return "unknown record attribute bits";
    }
    if (cc != 0 && (cc & (cc - 1)) != 0) {
        return "only one carriage control (FTN, CR or PRN) can be given";
    }
    if (attrs->rattrib & ODS2_RAT_PRN) {
        /* print-file carriage control needs the fixed control area of a VFC file; with any other format VMS
           TYPE refuses the file (RMS-F-RAT, batches m2b and C on V7.3): never made here */
        return "RAT:PRN needs the VFC record format, which can't be imported (VMS refuses any other: RMS-F-RAT)";
    }
    return NULL;
}

const char *ods2_check_var_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len)
{
    unsigned cc = attrs->rattrib & (ODS2_RAT_FTN | ODS2_RAT_CR | ODS2_RAT_PRN);

    if (attrs->rtype != ODS2_RFM_VAR) {
        return "converted records are variable-length: the format must be VAR";
    }
    if (attrs->rsize > 32767) {
        return "a record is too long (at most 32767 bytes)";
    }
    if (attrs->maxrec != 0 && attrs->maxrec < attrs->rsize) {
        return "a record is longer than the maximum record size (MRS) given";
    }
    if (content_len % 2 != 0) {
        return "internal error: variable-length records must fill whole words";
    }
    if (attrs->rattrib & ODS2_RAT_BLK) {
        return "RAT:BLK isn't supported with converted records: they run on "
               "across block boundaries, which BLK forbids";
    }
    if (attrs->rattrib & ~0x0fu) {
        return "unknown record attribute bits";
    }
    if (cc != 0 && (cc & (cc - 1)) != 0) {
        return "only one carriage control (FTN, CR or PRN) can be given";
    }
    if (attrs->rattrib & ODS2_RAT_PRN) {
        /* print-file carriage control needs the fixed control area of a VFC file; with any other format VMS
           TYPE refuses the file (RMS-F-RAT, batches m2b and C on V7.3): never made here */
        return "RAT:PRN needs the VFC record format, which can't be imported (VMS refuses any other: RMS-F-RAT)";
    }
    return NULL;
}

ods2_result_t ods2_create_file_attrs(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, const uint8_t *content,
                                      size_t content_len, const ods2_record_attrs_t *attrs,
                                      ods2_fid_t *new_fid_out)
{
    memory_source_t ctx;
    ods2_content_source_t src;
    ctx.data = content;
    ctx.pos = 0;
    src.length = content_len;
    src.read = memory_source_read;
    src.ctx = &ctx;
    src.problem = NULL;
    src.var_records = false;
    return ods2_create_file_source(vol, parent_header, name, &src, attrs, new_fid_out);
}

ods2_result_t ods2_create_file_source(ods2_volume_t *vol, const uint8_t *parent_header,
                                       const char *name, ods2_content_source_t *src,
                                       const ods2_record_attrs_t *attrs,
                                       ods2_fid_t *new_fid_out)
{
    size_t check_len = src->length;
    const char *why;
    if (src->partial_fix_ok && attrs->rtype == ODS2_RFM_FIX && attrs->rsize != 0) {
        check_len = (check_len / attrs->rsize) * attrs->rsize;
    }
    why = src->var_records ? ods2_check_var_record_attrs(attrs, check_len)
                           : ods2_check_record_attrs(attrs, check_len);
    if (why != NULL) {
        return fail(why);
    }
    return create_file(vol, parent_header, name, src, attrs, 0, new_fid_out);
}

/* Is the header of file number 10 the volume's SECURITY.SYS? Always on ODS-5; on an ODS-2 volume only if it names
   SECURITY.SYS and its back link is the master file directory (an older volume's user file may hold the number). */
static bool file10_is_security(const ods2_volume_t *vol, const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    ods5_ident_t id;
    const char *why;
    static const char name[] = "SECURITY.SYS";
    size_t i;

    if (vol->ods5) return true;
    if (ods2_fid_number(core->backlink) != 4 || !ods5_read_ident(header, &id, &why)) return false;
    if (strlen(id.name) != sizeof(name) - 1) return false;
    for (i = 0; i < sizeof(name) - 1; i++) {
        if (toupper((unsigned char) id.name[i]) != name[i]) return false;
    }
    return true;
}

static ods2_result_t create_link_impl(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                      ods2_fid_t target);

ods2_result_t ods2_create_link(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                               ods2_fid_t target)
{
    TXN_BEGIN_OR_RETURN(vol);
    return txn_end(vol, create_link_impl(vol, dir_file_number, name, target));
}

static ods2_result_t create_link_impl(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                      ods2_fid_t target)
{
    uint8_t dir_header[512], target_header[512];
    ods2_head_core_t *core = (ods2_head_core_t *) target_header;
    char link_name[ODS5_MAX_LITERAL_NAME + 8];
    const char *why;
    unsigned count;
    ods2_result_t r;
    bool hardlinks = vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS) != 0;

    if (!vol->writable) return not_writable(vol);
    if (vol->ods5) {
        why = ods5_check_new_name(name);
        if (why != NULL) return fail(why);
        if (!ods5_complete_name(name, link_name, sizeof(link_name))) return fail("file name too long");
    } else {
        /* ODS-2: upper case, name characters only, the dot completed, as for a new file */
        if (contains_wildcard(name)) return fail("file name cannot contain wildcard characters ('*' or '%')");
        if (strchr(name, ';') != NULL) return fail("file name cannot contain ';' - pass the name without its version");
        if (!ods2_make_file_name(name, link_name, sizeof(link_name), &why)) return fail(why);
    }

    r = ods2_check_fid(target);
    if (!r.ok) return r;
    /* the volume's own files cannot be given another name or link either (F3): a hard link would count into
       INDEXF.SYS's header, an alias would make the master directory list a second name for it */
    if (ods2_fid_number(target) >= 1 && ods2_fid_number(target) <= 9) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be linked");
    }
    r = ods2_read_header(vol, ods2_fid_number(target), target_header);
    if (!r.ok) return r;
    if (ods2_fid_number(target) == 10 && file10_is_security(vol, target_header)) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be linked");
    }
    if (ods2_fid_number(core->fid) != ods2_fid_number(target) || core->fid.fid_seq != target.fid_seq ||
        (core->filechar & 0x8000u)) {
        return fail("the file to link to does not exist");
    }
    if (hardlinks) {
        r = header_ok_for_write(target_header, ods2_fid_number(target)); /* its link count is rewritten */
        if (!r.ok) return r;
    }
    if (core->filechar & 0x2000u) {
        return fail("a link to a directory is not made here");
    }
    if (core->seg_num != 0) return fail("the file to link to is an extension header");
    if (hardlinks) {
        if ((core->struclev >> 8) != ODS5_LEVEL_5) {
            return fail("the file to link to has a level 2 header (a reserved file): not linkable");
        }
        count = (unsigned) (target_header[ODS5_FH2_LINKCOUNT] | (target_header[ODS5_FH2_LINKCOUNT + 1] << 8));
        if (count >= 32760u) return fail("the file already has the most links a file can (32760)");
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_ready(vol, dir_header, dir_file_number);
    if (!r.ok) return r;
    r = name_available(vol, dir_header, link_name,
                       "a file or directory with this name already exists in the directory");
    if (!r.ok) return r;

    if (!hardlinks) {
        /* Without the HARDLINKS characteristic (any ODS-2 volume, an ODS-5 one without it) VMS SET
           FILE/ENTER makes a plain alias: one more directory entry with the file's ID, nothing in
           the file header changes (link count stays as it is; measured, ODS5_FORMAT_NOTES 12.12 and 15) */
        return insert_into_directory(vol, dir_file_number, link_name, 1, target, 0);
    }
    /* the count first, then the entry: interrupted in between, the count is one too high
       (a leaked header at worst), never one too low (a file freed while an entry names it) */
    count++;
    target_header[ODS5_FH2_LINKCOUNT] = (uint8_t) (count & 0xff);
    target_header[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) (count >> 8);
    /* SET FILE/ENTER on V8.4 changes the link count and nothing else in the target's header:
       revision, revision date and attribute change date stay (measured) */
    {
        uint8_t counted[512];
        uint16_t checksum = ods2_checksum(target_header, 255);
        target_header[510] = (uint8_t) (checksum & 0xff);
        target_header[511] = (uint8_t) (checksum >> 8);
        memcpy(counted, target_header, sizeof(counted));
        r = ods2_write_header(vol, ods2_fid_number(target), counted);
        if (!r.ok) return r;
    }
    r = insert_into_directory(vol, dir_file_number, link_name, 1, target, 0);
    if (!r.ok) {
        /* put the count back (the header as it was read, with the count it had) */
        uint8_t back[512];
        if (ods2_read_header(vol, ods2_fid_number(target), back).ok) {
            back[ODS5_FH2_LINKCOUNT] = (uint8_t) ((count - 1) & 0xff);
            back[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) ((count - 1) >> 8);
            {
                uint16_t checksum = ods2_checksum(back, 255);
                back[510] = (uint8_t) (checksum & 0xff);
                back[511] = (uint8_t) (checksum >> 8);
            }
            ods2_write_header(vol, ods2_fid_number(target), back);
        }
        return r;
    }
    return ok();
}

ods2_result_t ods2_create_symlink(ods2_volume_t *vol, const uint8_t *parent_header,
                                  const char *name, const char *target, ods2_fid_t *new_fid_out)
{
    uint8_t text[ODS5_SYMLINK_MAX + 1];
    size_t len = 0;
    bool wide;
    memory_source_t ctx;
    ods2_content_source_t src;
    ods2_record_attrs_t attrs;

    if (!vol->writable) return not_writable(vol);
    if (!vol->ods5) return fail("symbolic links exist only on an ODS-5 volume");
    if (vol->home.volchar & ODS5_VC_NO_SPECIAL_FILES) {
        return fail("the volume forbids special files (symbolic links): HM2$M_NO_SPECIAL_FILES");
    }
    if (target[0] == '\0') return fail("a symbolic link needs a target");
    if (!ods5_utf8_to_stored(target, text, sizeof(text), &len, &wide) || wide) {
        return fail("a symbolic link target is ISO Latin-1 text of at most 1024 characters: "
                    "no character above U+00FF");
    }
    /* What VMS V8.4 makes (CREATE/SYMLINK, ODS5T corpus): file organization
       special (FAT byte 0 = 0x40), special type 5 in the byte after it,
       RSIZE 32767, EFBLK 1 / FFBYTE the target's length; the data is the
       target text from VBN 1; the directory entry carries DIR$V_SPECIAL. */
    attrs.rtype = (uint8_t) (ODS5_FAT_ORG_SPECIAL << 4);
    attrs.rattrib = ODS5_FAT_SYMBOLIC_LINK;
    attrs.rsize = 32767;
    attrs.maxrec = 0;
    ctx.data = text;
    ctx.pos = 0;
    src.length = len;
    src.read = memory_source_read;
    src.ctx = &ctx;
    src.problem = NULL;
    src.var_records = false;
    return create_file(vol, parent_header, name, &src, &attrs, ODS5_FAT_SYMBOLIC_LINK, new_fid_out);
}

ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out)
{
    ods2_record_attrs_t attrs;
    attrs.rtype = rtype;
    attrs.rattrib = 0;
    attrs.rsize = 512;
    attrs.maxrec = 512;
    {
        memory_source_t ctx;
        ods2_content_source_t src;
        ctx.data = content;
        ctx.pos = 0;
        src.length = content_len;
        src.read = memory_source_read;
        src.ctx = &ctx;
        src.problem = NULL;
        src.var_records = false;
        return create_file(vol, parent_header, name, &src, &attrs, 0, new_fid_out);
    }
}

static ods2_result_t create_file_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, ods2_content_source_t *src,
                                      const ods2_record_attrs_t *attrs, unsigned special,
                                      ods2_fid_t *new_fid_out);

static ods2_result_t create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                 const char *name, ods2_content_source_t *src,
                                 const ods2_record_attrs_t *attrs, unsigned special,
                                 ods2_fid_t *new_fid_out)
{
    TXN_BEGIN_OR_RETURN(vol);
    return txn_end(vol, create_file_impl(vol, parent_header, name, src, attrs, special, new_fid_out));
}

static ods2_result_t create_file_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, ods2_content_source_t *src,
                                      const ods2_record_attrs_t *attrs, unsigned special,
                                      ods2_fid_t *new_fid_out)
{
    size_t content_len = src->length;
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    size_t blocks_needed;
    size_t blocks_remaining;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count = 0;
    size_t total_allocated = 0;
    unsigned max_request;
    int seg_first[ODS2_MAX_HEADER_SEGMENTS];
    int seg_count[ODS2_MAX_HEADER_SEGMENTS];
    unsigned seg_file_number[ODS2_MAX_HEADER_SEGMENTS];
    uint16_t seg_seq[ODS2_MAX_HEADER_SEGMENTS];
    int segments = 0, numbered = 0, pos, seg;
    int lowest_written = ODS2_MAX_HEADER_SEGMENTS; /* headers seg.. on disk */
    ods2_header_spec_t spec;
    uint8_t new_header[512];
    ods2_result_t r;
    char upper_name[ODS5_MAX_LITERAL_NAME + 8];
    char ident[ODS5_MAX_LITERAL_NAME + 16]; /* name is already bounded (checked
                        above) plus ";1" and a nul - always large
                        enough, sized generously to also silence a
                        spurious compiler truncation warning */

    if (!vol->writable) {
        return not_writable(vol);
    }
    if (vol->ods5) {
        /* Level 5: the name as given, case kept, its dot completed;
           the characters VMS accepts (ods5_check_new_name). A ';' in
           it is part of the name here, as VMS allows with ^; */
        const char *why = ods5_check_new_name(name);
        if (why != NULL) return fail(why);
        if (!ods5_complete_name(name, upper_name, sizeof(upper_name))) {
            return fail("file name too long");
        }
    } else {
        if (contains_wildcard(name)) {
            return fail("file name cannot contain wildcard characters ('*' or '%')");
        }
        if (strchr(name, ';') != NULL) {
            /* A "NAME;VER" string stored as the name itself would create
               an entry no VMS system can open. Callers split the version
               off first (see ods2_path.h). */
            return fail("file name cannot contain ';' - pass the name without its version");
        }
        /* Validated and in VMS's on-disk form: uppercase, only name
           characters, and always a dot ("MAKEFILE" -> "MAKEFILE."). */
        {
            const char *why;
            if (!ods2_make_file_name(name, upper_name, sizeof(upper_name), &why)) {
                return fail(why);
            }
        }
    }
    name = upper_name; /* every use below now sees the on-disk name */

    r = directory_ready(vol, parent_header, ods2_fid_number(((const ods2_head_core_t *) parent_header)->fid));
    if (!r.ok) return r;
    r = name_available(vol, parent_header, name,
                       "a file or directory with this name already exists in the parent");
    if (!r.ok) return r;

    blocks_needed = (content_len + 511) / 512;
    if (blocks_needed == 0) blocks_needed = 1; /* a zero-length file still needs one block for EFBLK=1... */
    if (blocks_needed > 0xffffffffu - vol->home.cluster) {
        return fail("content is larger than an ODS-2 file can be (2^32 blocks)");
    }

    /* Allocate the content in as few, as large pieces as the free
       space allows: each request asks for everything still needed and
       takes the longest free run when no run is that long. Each piece
       becomes one retrieval pointer - Format 1, 2 or 3, whichever is
       smallest (see ods2_encode_retrieval_pointer()) - so on a volume
       with contiguous free space even a very large file is one or a
       few pointers. A request is capped at Format 3's 2^30-block
       limit, rounded down to whole clusters. */
    /* The primary header's file number first, before the content can
       take the space INDEXF.SYS may need to grow for its header (see
       allocate_file_number()). */
    r = allocate_file_number(vol, &seg_file_number[0], &seg_seq[0]);
    if (!r.ok) return r;
    numbered = 1;

    max_request = (1u << 30) - ((1u << 30) % vol->home.cluster);
    blocks_remaining = blocks_needed;
    while (blocks_remaining > 0) {
        uint32_t chunk_lbn;
        unsigned chunk_allocated;
        unsigned chunk_request = (blocks_remaining < max_request)
                                     ? (unsigned) blocks_remaining : max_request;

        if (extent_count >= ODS2_MAX_EXTENTS) {
            r = fail("the volume's free space is too fragmented to hold this file "
                     "(it would need more than 1024 separate pieces)");
            goto undo;
        }
        r = ods2_allocate_extent(vol, chunk_request, &chunk_lbn, &chunk_allocated);
        if (!r.ok) {
            r = fail("not enough free space on the volume for this file");
            goto undo;
        }
        extents[extent_count].lbn = chunk_lbn;
        extents[extent_count].block_count = chunk_allocated;
        extent_count++;
        total_allocated += chunk_allocated;
        blocks_remaining = (chunk_allocated >= blocks_remaining) ? 0
                                                                 : blocks_remaining - chunk_allocated;
    }

    /* Spread the retrieval pointers over as many headers as they need:
       the primary header holds what fits in its map area, the rest go
       in extension headers chained through FH2$W_EXT_FID (spec 3.3,
       3.5.2.5, 3.5.2.8). */
    snprintf(ident, sizeof(ident), "%s;1", name);
    memset(&spec, 0, sizeof(spec));
    spec.level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
    spec.ident_name = ident;
    for (pos = 0; pos < extent_count; ) {
        int fit;
        spec.seg_num = (uint16_t) segments; /* where the Map Area starts depends on it at level 5 */
        fit = ods2_header_extents_that_fit_at(extents + pos, extent_count - pos,
                                              ods2_header_map_offset(&spec));
        if (fit <= 0) {
            r = fail("internal error: an allocated extent cannot be described by "
                     "any retrieval pointer format (should be unreachable)");
            goto undo;
        }
        if (segments >= ODS2_MAX_HEADER_SEGMENTS) {
            r = fail("the volume's free space is too fragmented to hold this file "
                     "(it would need more than 64 file headers)");
            goto undo;
        }
        seg_first[segments] = pos;
        seg_count[segments] = fit;
        segments++;
        pos += fit;
    }

    /* A file number - and its header slot inside INDEXF.SYS's
       end-of-file, its bit marked in use - for every extension header
       too. */
    while (numbered < segments) {
        r = allocate_file_number(vol, &seg_file_number[numbered], &seg_seq[numbered]);
        if (!r.ok) goto undo;
        numbered++;
    }
    memset(&spec, 0, sizeof(spec));
    spec.level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
    spec.link_count = (vol->home.volchar & ODS5_VC_HARDLINKS) ? 1 : 0;
    spec.rtype = attrs->rtype;
    spec.rattrib = attrs->rattrib;
    spec.rsize = attrs->rsize;
    spec.maxrec = attrs->maxrec;
    spec.no_access_date = (special != 0); /* VMS leaves a symbolic link's access date unset */
    /* Records the whole file's size in every header, as the record
       attributes are copied into extension headers too. */
    spec.hiblk = (uint32_t) total_allocated;
    /* every allocated block is written (zero-filled past the data), which is what
       VMS V8.4 records on a level 5 volume: HIGHWATER = HIBLK + 1 */
    if (vol->ods5) spec.highwater = (uint32_t) total_allocated + 1;
    /* End-of-file mark per spec 6.1.5/6.1.6, in the preferred form
       VMS itself writes: EFBLK is the VBN holding the EOF position,
       FFBYTE the bytes in use there. Content ending exactly on a
       block boundary (including an empty file) gets EFBLK = blocks+1
       and FFBYTE = 0. (An earlier version wrote EFBLK = blocks and
       FFBYTE = 0 for that case, which VMS reads as one block SHORTER
       than the real content, and an empty file as 512 bytes.) */
    spec.efblk = (uint32_t) (content_len / 512) + 1;
    spec.ffbyte = (uint16_t) (content_len % 512);
    /* FH2$M_CONTIG only when truly one piece - and on an ODS-5 volume not at all: the
       files VMS V8.4 makes there (ODS5T corpus) have no characteristics */
    spec.filechar = (extent_count == 1 && !vol->ods5) ? 0x0080 : 0;
    snprintf(ident, sizeof(ident), "%s;1", name);
    spec.ident_name = ident;

    /* Extension headers first, last to first, then the primary, so
       every header written only ever points at headers already on
       disk. Each extension header has its own FID, its segment
       number, and a back link to the primary header (spec 3.5.2.16). */
    for (seg = segments - 1; seg >= 0; seg--) {
        ods2_fid_set_number(&spec.fid, seg_file_number[seg]);
        spec.fid.fid_seq = seg_seq[seg];
        spec.seg_num = (uint16_t) seg;
        memset(&spec.ext_fid, 0, sizeof(spec.ext_fid));
        if (seg + 1 < segments) {
            ods2_fid_set_number(&spec.ext_fid, seg_file_number[seg + 1]);
            spec.ext_fid.fid_seq = seg_seq[seg + 1];
        }
        memset(&spec.backlink, 0, sizeof(spec.backlink));
        if (seg == 0) {
            spec.backlink = parent_core->fid;
        } else {
            ods2_fid_set_number(&spec.backlink, seg_file_number[0]);
            spec.backlink.fid_seq = seg_seq[0];
        }
        spec.extents = extents + seg_first[seg];
        spec.extent_count = seg_count[seg];
        if (!ods2_build_file_header(new_header, &spec)) {
            r = fail("could not construct new file's header");
            goto undo;
        }
        r = ods2_write_header(vol, seg_file_number[seg], new_header);
        if (!r.ok) goto undo;
        lowest_written = seg;
    }
    /* spec.fid is now the primary header's, for the directory entry. */

    /* Insert an entry for the new file into the parent - growing the
       parent's own allocation automatically if none of its existing
       content blocks have room. */
    r = insert_into_directory(vol, ods2_fid_number(parent_core->fid), name, 1, spec.fid,
                              special != 0 ? ODS5_DIR_SPECIAL : 0);
    if (!r.ok) goto undo;   /* no directory entry: give the headers, blocks and file numbers back */

    /* The content goes to the free clusters last, when nothing else can fail: the structure (bitmaps, headers,
       entry) is still only in memory, and reaches the image after this, in the order the crash-safety rules
       need - so the content is on the disk before any header refers to it, and a refused copy has not
       written a byte. */
    r = write_extents(vol, extents, extent_count, src);
    if (!r.ok) goto undo;

    *new_fid_out = spec.fid;
    return ok();

undo:
    /* Give back what was taken before the failure, so a refused or
       failed copy leaves no blocks or file numbers allocated to
       nothing. Any header already written is marked deleted (as
       ods2_delete_version() does), since a slot holding a valid header
       is never reused (spec 5.1.7). */
    for (seg = lowest_written; seg < segments; seg++) {
        memset(new_header, 0, sizeof(new_header));
        if (ods2_read_header(vol, seg_file_number[seg], new_header).ok) {
            ods2_head_core_t *core = (ods2_head_core_t *) new_header;
            core->filechar |= 0x8000u; /* FH2$M_MARKDEL */
            core->fid.fid_num = 0;
            core->fid.fid_nmx = 0;
            core->fid.fid_rvn = 0;
            new_header[510] = 0;
            new_header[511] = 0;
            ods2_write_header(vol, seg_file_number[seg], new_header);
        }
    }
    for (pos = 0; pos < extent_count; pos++) {
        ods2_free_blocks(vol, extents[pos].lbn, extents[pos].block_count);
    }
    for (seg = 0; seg < numbered; seg++) {
        mark_index_bitmap(vol, seg_file_number[seg], false);
    }
    return r;
}

/* Writes back a directory changed in memory: `blocks` holds its
 * `used` blocks, in order; `old_used` is how many it had before, and
 * blocks from index `first_changed` on are written.
 *
 * If `used` exceeds the allocation, the directory moves: spec 4.2 says
 * a directory is a contiguous file (ods2v2 sets FH2$M_CONTIG on every
 * directory it makes), so it can't simply gain a second extent
 * somewhere else on the disk. It gets a new contiguous area of up to
 * twice its old size, is written there in full, its header is switched
 * to the single new extent, and only then is the old area freed.
 *
 * The end-of-file becomes EFBLK used+1 / FFBYTE 0, as VMS writes it;
 * when the directory gains blocks its revision count goes up by 2 per block and
 * its revision date moves (as on VMS), otherwise they stay. */
/* True when VMS will read back what is written at `vbn`: the file is
   not high-water marked, or `vbn` is below its high-water mark. */
static bool below_highwater(const uint8_t *header, unsigned vbn)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return core->highwater == 0 || vbn < core->highwater;
}

/* A directory's end-of-file as VMS keeps it: one block past the last block in use, byte 0. ods2v2 used to
 * write EFBLK 1 / FFBYTE 2 on ODS-2 ("the sentinel only"); OpenVMS VAX V7.3 copies the parent's FFBYTE into
 * the directory CREATE/DIRECTORY makes in it, so that child got EOF (2,2): a second block of two bytes
 * that is not a directory block (ANALYZE: BAD_DIRSIZE, BAD_DIRTYPE; SYSTEM-W-BADIRECTORY; batch phvs2-w1).
 * Writing to such a directory puts its end-of-file right. */
static bool dir_eof_standard(const uint8_t *dir_header, unsigned used)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) dir_header;
    return core->recattr.ffbyte == 0 && ods2_word_swap32(core->recattr.efblk) == used + 1u;
}

static ods2_result_t store_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                     uint8_t *dir_header, const uint8_t *blocks,
                                     unsigned used, unsigned old_used, unsigned first_changed)
{
    ods2_head_core_t *core = (ods2_head_core_t *) dir_header;
    ods2_extent_t old_extents[ODS2_MAX_EXTENTS];
    int old_count, i;
    unsigned allocated = 0, v;
    bool moved = false, in_place_growth = false;
    uint8_t zero[512];
    ods2_result_t r;

    memset(zero, 0, sizeof(zero));
    r = ods2_decode_all_extents(vol, dir_header, old_extents, ODS2_MAX_EXTENTS, &old_count);
    if (!r.ok) return r;
    for (i = 0; i < old_count; i++) allocated += old_extents[i].block_count;

    if (used > allocated) {
        /* One contiguous extent: a Format 1 pointer maps at most 256 blocks, a
           Format 2 one 16384; a directory is held in memory while it is
           changed, so 65536 blocks (32 MiB) is the limit here. */
        unsigned max_blocks = (65536u / vol->home.cluster) * vol->home.cluster;
        unsigned want = allocated * 2;
        uint32_t lbn;
        unsigned got;
        size_t ptr_size;
        size_t map_start = (size_t) core->mpoffset * 2;
        size_t map_end = (size_t) core->acoffset * 2;

        if (used > max_blocks) {
            return fail("directory would need more than 65536 blocks (32 MiB), "
                        "the most this program keeps in one contiguous extent");
        }
        if (want < used) want = used;
        if (want > max_blocks) want = max_blocks;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
        if (!r.ok && want > used) {
            want = used; /* no run that long free: settle for what's needed */
            r = ods2_allocate_blocks(vol, want, &lbn, &got);
        }
        if (!r.ok) return r;
        /* Format 1 below LBN 4,194,303, Format 2 beyond it (issue #14):
           the new area may lie anywhere on a large volume. */
        ptr_size = ods2_retrieval_pointer_size(lbn, got);
        if (ptr_size == 0 || map_end > 510 || map_end < map_start + ptr_size) {
            ods2_free_blocks(vol, lbn, got);
            return fail("cannot map the directory's new area: no room in its "
                        "header's map area for the retrieval pointer");
        }
        for (v = 0; v < got; v++) {
            r = ods2_write_block(vol, lbn + v, (v < used) ? blocks + (size_t) v * 512 : zero);
            if (!r.ok) return r;
        }
        memset(dir_header + map_start, 0, map_end - map_start);
        ods2_encode_retrieval_pointer(dir_header + map_start, lbn, got);
        core->map_inuse = (uint8_t) (ptr_size / 2);
        core->recattr.hiblk = ods2_word_swap32(got);
        moved = true;
    } else if (used == old_used + 1 && first_changed < old_used) {
        /* A block that splits, with the next blocks moving down one (F2, found by the stress harness): the
           blocks are rewritten in an order in which every name stays listed at every write, a name listed
           twice for a moment being harmless and a name missing not. First the old last block's copy goes into
           the block just past the end-of-file (invisible there); the header then moves the end-of-file over
           it (below); then each block takes the place of the one before it, from the end back to the block
           that split, which is rewritten last: until then its names are all still there, and the new second
           half is a second copy of the names the old block still holds. */
        in_place_growth = true;
        r = ods2_write_file_block(vol, dir_header, used, blocks + (size_t) (used - 1) * 512);
        if (!r.ok) return r;
    } else {
        for (v = first_changed; v < used; v++) {
            r = ods2_write_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
            if (!r.ok) return r;
        }
        for (v = used; v < old_used; v++) { /* blocks no longer in use */
            r = ods2_write_file_block(vol, dir_header, v + 1, zero);
            if (!r.ok) return r;
        }
    }

    core->recattr.efblk = ods2_word_swap32(used + 1);
    core->recattr.ffbyte = 0;
    /* FH2$L_HIGHWATER: the first VBN never written. On a volume with
       high-water marking (the INITIALIZE default), VMS returns zeros
       for any read at or past it, whatever is on the disk. A directory
       INITIALIZE made carries HIGHWATER 2 (only VBN 1 written), so a
       block added past it reads back on VMS as a zero record:
       BAD_NAMEORDER "[empty]", BAD_DIRSIZE, BADIRECTORY. Zero means
       the file isn't high-water marked, and stays zero. */
    if (core->highwater != 0) {
        uint32_t written = moved ? ods2_word_swap32(core->recattr.hiblk) : used;
        if (core->highwater < written + 1) core->highwater = written + 1;
    }
    {
        /* FI2$W_REVISION / FI2$Q_REVDATE; FI2$Q_CREDATE stays. Measured on VMS V8.4 and V7.3-2 (ODS-5
           and ODS-2 corpus directories, ODS5_FORMAT_NOTES 15.4): an entry put into a directory block
           that has room changes neither the revision count nor the revision date; each block the
           directory gains adds 2 to the count (BIGDIR 60 blocks: 118; 29 blocks: 56) and moves the
           revision date (and, at level 5, the attribute change date). The header's own level says
           which ident area it has: the reserved files, root directory included, keep level 2 on an
           ODS-5 volume. */
        unsigned gained = used > old_used ? used - old_used : 0;
        bool changed_size = used != old_used;
        uint8_t *ident = dir_header + (size_t) core->idoffset * 2;
        if (changed_size) {
            if ((core->struclev >> 8) == ODS5_LEVEL_5) {
                uint16_t revision = (uint16_t) (ident[ODS5_FI5_REVISION] |
                                                (ident[ODS5_FI5_REVISION + 1] << 8));
                uint64_t now = ods2_vms_time_now();
                revision = (uint16_t) (revision + 2u * gained);
                ident[ODS5_FI5_REVISION] = (uint8_t) (revision & 0xff);
                ident[ODS5_FI5_REVISION + 1] = (uint8_t) (revision >> 8);
                ods2_store_vms_time(ident + ODS5_FI5_REVDATE, now);
                ods2_store_vms_time(ident + ODS5_FI5_ATTDATE, now);
            } else {
                uint16_t revision = (uint16_t) (ident[20] | (ident[21] << 8));
                revision = (uint16_t) (revision + 2u * gained);
                ident[20] = (uint8_t) (revision & 0xff);
                ident[21] = (uint8_t) (revision >> 8);
                ods2_store_vms_time(ident + 30, ods2_vms_time_now()); /* local time */
            }
        }
    }
    {
        uint16_t checksum = ods2_checksum(dir_header, 255);
        dir_header[510] = (uint8_t) (checksum & 0xff);
        dir_header[511] = (uint8_t) (checksum >> 8);
    }
    r = ods2_write_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    if (in_place_growth) {
        for (v = used - 1; v > first_changed; v--) {     /* blocks[v - 1] .. blocks[first_changed], last first */
            r = ods2_write_file_block(vol, dir_header, v, blocks + (size_t) (v - 1) * 512);
            if (!r.ok) return r;
        }
    }

    if (moved) {
        for (i = 0; i < old_count; i++) {
            r = ods2_free_blocks(vol, old_extents[i].lbn, old_extents[i].block_count);
            if (!r.ok) return r;
        }
    }
    return ok();
}

/* Reads a directory's blocks in use into a new buffer, with room for
   `spare` more blocks after them. *used_out is at least 1 (a directory
   always has its first block). */
static ods2_result_t load_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    unsigned spare, uint8_t **blocks_out,
                                    unsigned *used_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int count, i;
    unsigned allocated = 0, used, v;
    uint8_t *blocks;
    ods2_result_t r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS,
                                              &count);
    if (!r.ok) return r;
    for (i = 0; i < count; i++) allocated += extents[i].block_count;
    if (allocated == 0) return fail("directory has no blocks allocated");
    used = dir_blocks_in_use(dir_header, allocated);
    if (used == 0) used = 1;
    if (used > 65536u) {
        return fail("directory claims more than 65536 blocks: damaged, or too big to change here");
    }

    blocks = calloc((size_t) used + spare, 512);
    if (blocks == NULL) return fail("could not allocate memory for the directory");
    for (v = 0; v < used; v++) {
        r = ods2_read_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
        if (!r.ok) {
            free(blocks);
            return r;
        }
    }
    /* Trailing blocks that are all zeros (not even an end-of-records marker) are not directory blocks: OpenVMS
       VAX V7.3 makes a directory with end-of-file (2,2) inside one with the old FFBYTE 2, i.e. a zero second
       block inside its EOF. They are dropped from `used`, so that a write puts the end-of-file right (the
       directory's own block 1 stays, and carries the marker) instead of keeping or moving a zero block. */
    while (used > 1 && blocks[(size_t) (used - 1) * 512] == 0 && blocks[(size_t) (used - 1) * 512 + 1] == 0) used--;
    /* A directory is changed only if every block of it is well formed (review 6, finding 3): a record whose
       name count or size runs past its block would be read, shifted or split on trust. */
    for (v = 0; v < used; v++) {
        if (!ods2_dir_block_ok(blocks + (size_t) v * 512, 512)) {
            free(blocks);
            return fail("a directory block is damaged (a record runs past the block): the directory is not "
                        "changed - run ANALYZE/DISK_STRUCTURE/REPAIR on VMS");
        }
    }
    *blocks_out = blocks;
    *used_out = used;
    return ok();
}

/* Inserts (name, version, fid) where it belongs in the directory's
 * overall sorted order (spec 4.2): into the block whose names it falls
 * among. If that block is full, it is split in two - its records,
 * with the new one, divided between it and a new block placed right
 * after it, the following blocks moving down one - so the order holds
 * across blocks and every block inside the end-of-file holds entries.
 * VMS's own directory search depends on that order. */
static ods2_result_t insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                           const char *name, uint16_t version, ods2_fid_t fid,
                                           uint8_t extra_flags);

ods2_result_t ods2_insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                          const char *name, uint16_t version, ods2_fid_t fid)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = insert_into_directory(vol, dir_file_number, name, version, fid, 0);
    return txn_end(vol, r);
}

static ods2_result_t insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                           const char *name, uint16_t version, ods2_fid_t fid,
                                           uint8_t extra_flags)
{
    uint8_t dir_header[512];
    uint8_t *blocks = NULL;
    unsigned used, t;
    uint16_t verlimit;
    ods2_result_t r;

    if (!vol->writable) {
        return not_writable(vol);
    }
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_header_ok(dir_header, dir_file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 1, &blocks, &used);
    if (!r.ok) return r;

    t = ods2_dir_choose_block(blocks, used, name);
    verlimit = ((const ods2_head_core_t *) dir_header)->recattr.versions;   /* the directory's default version limit */
    if (ods2_insert_dir_entry_vl(blocks + (size_t) t * 512, 512, name, version, fid,
                                 vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2, extra_flags, verlimit)) {
        if (ods2_file_content_length(dir_header) == 0 || !dir_eof_standard(dir_header, used) ||
            !below_highwater(dir_header, t + 1)) {
            /* An end-of-file that didn't cover even block 1, or a block
               VMS would read as zeros until the high-water mark moves */
            r = store_directory(vol, dir_file_number, dir_header, blocks, used, used, t);
        } else {
            r = ods2_write_file_block(vol, dir_header, t + 1, blocks + (size_t) t * 512);
        }
    } else {
        uint8_t combined[1024];
        memcpy(combined, blocks + (size_t) t * 512, 512);
        memset(combined + 512, 0, 512);
        if (!ods2_insert_dir_entry_vl(combined, sizeof(combined), name, version, fid,
                                      vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2, extra_flags, verlimit)) {
            free(blocks);
            return fail("internal error: a block plus one record overflowed 1024 "
                        "bytes (should be unreachable)");
        }
        memmove(blocks + (size_t) (t + 2) * 512, blocks + (size_t) (t + 1) * 512,
                (size_t) (used - t - 1) * 512);
        if (!ods2_dir_split_block(combined, sizeof(combined), blocks + (size_t) t * 512,
                                  blocks + (size_t) (t + 1) * 512)) {
            free(blocks);
            return fail("internal error: could not split a full directory block "
                        "(should be unreachable)");
        }
        r = store_directory(vol, dir_file_number, dir_header, blocks, used + 1, used, t);
    }
    free(blocks);
    return r;
}

/* SET FILE/VERSION_LIMIT: the version limit of the directory entries named `name` in a directory, written block by
   block in one transaction. *changed gets the number of records changed (0 when the name is not there or already
   has the limit). */
static ods2_result_t set_entry_version_limit_impl(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                                  uint16_t limit, unsigned *changed)
{
    uint8_t dir_header[512];
    uint8_t *blocks = NULL;
    unsigned used, v;
    ods2_result_t r;

    *changed = 0;
    if (!vol->writable) return not_writable(vol);
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_header_ok(dir_header, dir_file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    if (!r.ok) return r;
    for (v = 0; v < used && r.ok; v++) {
        int n = ods2_set_dir_entry_verlimit(blocks + (size_t) v * 512, 512, name, limit);
        if (n < 0) { r = fail("a directory block is damaged: not changed"); break; }
        if (n > 0) {
            *changed += (unsigned) n;
            r = ods2_write_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
        }
    }
    free(blocks);
    return r;
}

ods2_result_t ods2_set_entry_version_limit(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                           uint16_t limit, unsigned *changed)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = set_entry_version_limit_impl(vol, dir_file_number, name, limit, changed);
    return txn_end(vol, r);
}

/* SET VOLUME/GEOMETRY: the disk geometry the storage control block records (BITMAP.SYS VBN 1: sectors, tracks,
   cylinders = ceil(VOLSIZE / (sectors * tracks)), BLKSIZE 1, checksum). OpenVMS VAX V7.3 takes an SCB as valid only
   if it records the geometry of the device the volume is on (measured: scb1, PHVS2), so an image made for one device
   is fitted to another with this. Nothing else changes. */
static ods2_result_t set_volume_geometry_impl(ods2_volume_t *vol, uint32_t sectors, uint32_t tracks)
{
    uint8_t h[512], scb[512];
    uint32_t volsize, cyl;
    uint64_t per_cyl = (uint64_t) sectors * tracks, c;
    uint16_t ck = 0;
    unsigned i;
    ods2_result_t r;

    if (!vol->writable) return not_writable(vol);
    if (sectors == 0 || tracks == 0 || per_cyl > 0xFFFFFFFFu) return fail("the geometry needs sectors and tracks of at least 1");
    r = ods2_read_header(vol, 2, h);
    if (!r.ok) return r;
    r = header_ok_for_write(h, 2);
    if (!r.ok) return r;
    r = ods2_read_file_block(vol, h, 1, scb);
    if (!r.ok) return r;
    if ((scb[0] | (scb[1] << 8)) != (vol->ods5 ? 0x0501 : 0x0201)) {
        return fail("BITMAP.SYS's storage control block is not one (its structure level is not the volume's): not changed");
    }
    volsize = (uint32_t) scb[4] | ((uint32_t) scb[5] << 8) | ((uint32_t) scb[6] << 16) | ((uint32_t) scb[7] << 24);
    c = ((uint64_t) volsize + per_cyl - 1) / per_cyl;
    if (c == 0 || c > 0xFFFFFFFFu) return fail("the geometry gives no usable cylinder count");
    cyl = (uint32_t) c;
    scb[8] = 1; scb[9] = scb[10] = scb[11] = 0;                      /* BLKSIZE 1, as VMS writes it */
    for (i = 0; i < 4; i++) {
        scb[12 + i] = (uint8_t) (sectors >> (8 * i));
        scb[16 + i] = (uint8_t) (tracks >> (8 * i));
        scb[20 + i] = (uint8_t) (cyl >> (8 * i));
    }
    ck = ods2_checksum(scb, 255);
    scb[510] = (uint8_t) (ck & 0xFF);
    scb[511] = (uint8_t) (ck >> 8);
    return ods2_write_file_block(vol, h, 1, scb);
}

ods2_result_t ods2_set_volume_geometry(ods2_volume_t *vol, uint32_t sectors, uint32_t tracks)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = set_volume_geometry_impl(vol, sectors, tracks);
    return txn_end(vol, r);
}

ods2_result_t ods2_free_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned block_count)
{
    uint8_t bitmap_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_bitmap_blocks = 0;
    unsigned bits_blocks;
    ods2_result_t r;
    unsigned i;
    size_t start_cluster, clusters_to_free;
    size_t first_byte, last_byte;
    unsigned first_block, last_block;

    if (!vol->writable) {
        return not_writable(vol);
    }
    if (block_count == 0) {
        return ok(); /* nothing to do */
    }
    if (lbn % vol->home.cluster != 0 || block_count % vol->home.cluster != 0) {
        return fail("lbn/block_count must be cluster-aligned (exactly what "
                    "ods2_allocate_blocks() returned)");
    }

    r = ods2_read_header(vol, 2, bitmap_header); /* BITMAP.SYS is always file 2 */
    if (!r.ok) return r;

    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode BITMAP.SYS's own extents");
    }
    for (i = 0; i < (unsigned) extent_count; i++) total_bitmap_blocks += extents[i].block_count;
    if (total_bitmap_blocks < 2) {
        return fail("BITMAP.SYS is too small to contain both an SCB and bitmap data");
    }
    bits_blocks = total_bitmap_blocks - 1; /* excluding VBN 1's SCB, spec 5.2.1 */

    start_cluster = lbn / vol->home.cluster;
    clusters_to_free = block_count / vol->home.cluster;

    first_byte = start_cluster / 8;
    last_byte = (start_cluster + clusters_to_free - 1) / 8;
    first_block = (unsigned) (first_byte / 512);
    last_block = (unsigned) (last_byte / 512);
    if (last_block >= bits_blocks) {
        return fail("block range to free is beyond BITMAP.SYS's own extent");
    }

    for (i = first_block; i <= last_block; i++) {
        uint8_t bitmap_block[512];
        size_t block_start_cluster = (size_t) i * 512 * 8;
        size_t local_start, local_count;

        r = ods2_read_file_block(vol, bitmap_header, i + 2, bitmap_block); /* +2: skip SCB, spec 5.2.1 */
        if (!r.ok) return r;

        /* Clip the free range to just the bits within this block. */
        local_start = (start_cluster > block_start_cluster)
                          ? start_cluster - block_start_cluster : 0;
        {
            size_t range_end = start_cluster + clusters_to_free; /* exclusive */
            size_t block_end = block_start_cluster + 512 * 8;
            size_t clipped_end = (range_end < block_end) ? range_end : block_end;
            local_count = (clipped_end > block_start_cluster + local_start)
                              ? clipped_end - (block_start_cluster + local_start) : 0;
        }

        if (local_count > 0) {
            if (!ods2_bitmap_mark(bitmap_block, 512u * 8u, local_start, local_count, false)) {
                return fail("internal error freeing bitmap bits (should be unreachable)");
            }
            r = ods2_write_file_block(vol, bitmap_header, i + 2, bitmap_block);
            if (!r.ok) return r;
        }
    }

    return ok();
}

/* Removes just the (name, version) entry from the directory - trying each
   block in use until the record holding it is found. Removing the whole
   record instead (as an earlier version did) would also drop every other
   version of the name stored there, leaving their headers allocated but
   unreachable. A block left with no entries is removed and the blocks after
   it move up, as the VMS file system does, so every block inside the
   end-of-file holds entries. */
static ods2_result_t remove_directory_entry_once(ods2_volume_t *vol, unsigned dir_file_number,
                                                 uint8_t *dir_header, const char *name,
                                                 uint16_t target_version, ods2_fid_t fid, bool *removed_out)
{
    uint8_t *blocks = NULL;
    unsigned used, v;
    bool removed = false;
    ods2_result_t r = directory_header_ok(dir_header, dir_file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    if (!r.ok) return r;
    for (v = 0; v < used && !removed; v++) {
        uint8_t *block = blocks + (size_t) v * 512;
        if (!ods2_remove_dir_version_fid(block, 512, name, target_version, &fid)) continue;
        removed = true;
        if (used > 1 && ods2_dir_block_is_empty(block)) {
            memmove(block, block + 512, (size_t) (used - v - 1) * 512);
            r = store_directory(vol, dir_file_number, dir_header, blocks, used - 1,
                                used, v);
        } else if (!below_highwater(dir_header, v + 1) || !dir_eof_standard(dir_header, used)) {
            r = store_directory(vol, dir_file_number, dir_header, blocks, used,
                                used, v);
        } else {
            r = ods2_write_file_block(vol, dir_header, v + 1, block);
        }
    }
    free(blocks);
    *removed_out = removed;
    return r;
}

/* Removes the entry (name, version, fid) - every one of them: an interrupted block split can leave an entry
   listed twice, exactly alike (the second is the file's own entry again, not another file's), and removing one
   would leave the other pointing at a file that is gone. A pair with another FID is never touched. */
static ods2_result_t remove_directory_entry(ods2_volume_t *vol, unsigned dir_file_number,
                                            uint8_t *dir_header, const char *name,
                                            uint16_t target_version, ods2_fid_t fid)
{
    unsigned pass;
    for (pass = 0; pass < 64; pass++) {
        bool removed = false;
        ods2_result_t r = remove_directory_entry_once(vol, dir_file_number, dir_header, name, target_version,
                                                      fid, &removed);
        if (!r.ok) return r;
        if (!removed) {
            if (pass == 0) {
                return fail("internal error: entry existed at lookup time but was not "
                            "found for removal (should be unreachable)");
            }
            return ok();
        }
    }
    return ok();
}

ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name)
{
    return ods2_delete_version(vol, dir_file_number, name, ODS2_VERSION_HIGHEST, NULL);
}

/* The disk geometry (sectors x tracks) a device type reports for a volume of `blocks` blocks, which the storage
   control block must record for OpenVMS VAX V7.3's ANALYZE (cylinders = ceil(blocks / (sectors * tracks)) always).
   Measured by ods5-vms2 (CORPUS section 16); `*measured` is false when this size was not one of the measured
   points and the answer is a prediction (confirm with SHOW DEVICE/FULL on the target).
     ld      the LD driver's containers: sectors and tracks grow with the size (LD is no fixed geometry): measured at
             2048, 20480, 204800, 1000001, 1024000, 2097152, 4000000 and 4190000 blocks; otherwise the largest
             sectors x tracks not above the square root of the size, grown from the fourth root (fits seven of the eight)
     ra      SIMH MSCP units: RA82 57 x 15 (measured to 4,194,304 blocks), RA92 73 x 13 (measured at 8,388,608 and
             33,554,432); the type changes somewhere between 4 and 8 Mi blocks (taken at 4,194,304 here)
     scsi    a physical SCSI disk: 63 x 255 (AXP1SYS)
     axpbox  AXPbox SCSI units: 96 x 96 (the blank-volume library)
     rz      Alpha virtual disks on ASIM5 (RZ58): 32 x 16 */
bool ods2_target_geometry(const char *target, uint64_t blocks, uint32_t *sectors, uint32_t *tracks, bool *measured)
{
    static const struct { uint64_t n; uint32_t s, t; } ld[] = {
        { 2048, 7, 6 }, { 20480, 12, 11 }, { 204800, 21, 21 }, { 1000001, 32, 31 }, { 1024000, 32, 31 },
        { 2097152, 38, 38 }, { 4000000, 44, 45 }, { 4190000, 45, 45 },
    };
    static const uint64_t ra_measured[] = { 131072, 1049298, 2097152, 4194304, 8388608, 33554432 };
    size_t i;
    bool m = false;

    if (strcasecmp(target, "ld") == 0) {
        for (i = 0; i < sizeof(ld) / sizeof(ld[0]); i++) {
            if (ld[i].n == blocks) { *sectors = ld[i].s; *tracks = ld[i].t; if (measured) *measured = true; return true; }
        }
        {
            uint64_t root = 0, r4 = 0;
            uint32_t sec, trk;
            while ((root + 1) * (root + 1) <= blocks) root++;           /* integer square root */
            while ((r4 + 1) * (r4 + 1) <= root) r4++;                    /* and fourth root */
            sec = trk = (uint32_t) (r4 < 1 ? 1 : r4);
            if ((uint64_t) (sec + 1) * trk <= root) sec++;
            if ((uint64_t) sec * (trk + 1) <= root) trk++;
            *sectors = sec; *tracks = trk;
        }
        if (measured) *measured = false;
        return true;
    }
    if (strcasecmp(target, "ra") == 0 || strcasecmp(target, "ra82") == 0 || strcasecmp(target, "ra92") == 0 ||
        strcasecmp(target, "rq") == 0) {
        if (strcasecmp(target, "ra92") == 0 || (strcasecmp(target, "ra82") != 0 && blocks > 4194304)) {
            *sectors = 73; *tracks = 13;
        } else {
            *sectors = 57; *tracks = 15;
        }
        for (i = 0; i < sizeof(ra_measured) / sizeof(ra_measured[0]); i++) if (ra_measured[i] == blocks) m = true;
        if (measured) *measured = m;
        return true;
    }
    if (strcasecmp(target, "scsi") == 0) { *sectors = 63; *tracks = 255; if (measured) *measured = true; return true; }
    if (strcasecmp(target, "axpbox") == 0) { *sectors = 96; *tracks = 96; if (measured) *measured = true; return true; }
    if (strcasecmp(target, "rz") == 0) { *sectors = 32; *tracks = 16; if (measured) *measured = true; return true; }
    return false;
}

/* The storage control block (BITMAP.SYS, VBN 1) as OpenVMS reads it. VAX V7.3's ANALYZE/DISK_STRUCTURE (VERIFY X-17)
   takes the SCB as invalid when its structure level or cluster factor is not the volume's, or when
   |VOLSIZE - sectors*tracks*cylinders/BLKSIZE| is more than one cylinder (sectors*tracks); it divides by BLKSIZE
   without looking at it, so BLKSIZE 0 (a SIMH RQ disk of more than about 26.7 GiB: the 16-bit cylinder count wraps)
   makes it trap with INTDIV, and V8.4 reports %ANALDISK-W-CHKSCB. Returns true, with a message, for such an SCB.
   Read only: nothing is changed, the volume stays usable. */
bool ods2_scb_problem(ods2_volume_t *vol, char *msg, size_t cap)
{
    uint8_t bitmap_header[512], scb[512];
    uint16_t level;
    uint32_t volsize, blksize, sec, trk, cyl;
    uint64_t product, diff;
    char scratch[2];

    if (msg != NULL && cap > 0) msg[0] = '\0';
    if (!ods2_read_header(vol, 2, bitmap_header).ok ||
        !ods2_read_file_block(vol, bitmap_header, 1, scb).ok) {
        return false;     /* a damaged BITMAP.SYS is reported where it is used */
    }
    level = (uint16_t) (scb[0] | (scb[1] << 8));
    volsize = (uint32_t) scb[4] | ((uint32_t) scb[5] << 8) | ((uint32_t) scb[6] << 16) | ((uint32_t) scb[7] << 24);
    blksize = (uint32_t) scb[8] | ((uint32_t) scb[9] << 8) | ((uint32_t) scb[10] << 16) | ((uint32_t) scb[11] << 24);
    sec = (uint32_t) scb[12] | ((uint32_t) scb[13] << 8) | ((uint32_t) scb[14] << 16) | ((uint32_t) scb[15] << 24);
    trk = (uint32_t) scb[16] | ((uint32_t) scb[17] << 8) | ((uint32_t) scb[18] << 16) | ((uint32_t) scb[19] << 24);
    cyl = (uint32_t) scb[20] | ((uint32_t) scb[21] << 8) | ((uint32_t) scb[22] << 16) | ((uint32_t) scb[23] << 24);
    if (msg == NULL || cap == 0) { msg = scratch; cap = sizeof(scratch); }

    /* a block that is no SCB at all (the starter volume's all-ones or 0xAAAA placeholder) is not judged */
    if (level != 0x0201u && level != 0x0501u) return false;
    if (level != (vol->ods5 ? 0x0501u : 0x0201u)) {
        snprintf(msg, cap, "the storage control block says structure level 0x%04X; OpenVMS ANALYZE/DISK_STRUCTURE "
                 "would report %%ANALDISK-W-CHKSCB", level);
        return true;
    }
    if (scb[2] != (vol->home.cluster & 0xFFu) || scb[3] != (vol->home.cluster >> 8)) {
        snprintf(msg, cap, "the storage control block's cluster factor (%u) is not the volume's (%u); OpenVMS "
                 "ANALYZE/DISK_STRUCTURE would report %%ANALDISK-W-CHKSCB", (unsigned) (scb[2] | (scb[3] << 8)),
                 (unsigned) vol->home.cluster);
        return true;
    }
    if (blksize == 0) {
        snprintf(msg, cap, "the storage control block's blocking factor is 0 (the disk geometry %u x %u x %u "
                 "wrapped its 16-bit cylinder count when the volume was initialized, a SIMH RQ disk above about "
                 "26.7 GiB): OpenVMS VAX V7.3 ANALYZE/DISK_STRUCTURE fails with INTDIV, V8.4 reports "
                 "%%ANALDISK-W-CHKSCB. The data is not affected; SET VOLUME/GEOMETRY=sectors,tracks rewrites the geometry", sec, trk, cyl);
        return true;
    }
    product = (uint64_t) sec * trk * cyl / blksize;
    diff = product > volsize ? product - volsize : (uint64_t) volsize - product;
    if (diff > (uint64_t) sec * trk) {
        snprintf(msg, cap, "the storage control block's geometry (%u sectors x %u tracks x %u cylinders = %llu "
                 "blocks) is more than one cylinder from the volume size (%u): OpenVMS ANALYZE/DISK_STRUCTURE "
                 "would report %%ANALDISK-W-CHKSCB. The data is not affected; SET VOLUME/GEOMETRY=sectors,tracks rewrites the geometry", sec, trk, cyl,
                 (unsigned long long) product, volsize);
        return true;
    }
    return false;
}

bool ods2_header_is_file(const uint8_t *header, ods2_fid_t fid)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return core->fid.fid_num == fid.fid_num && core->fid.fid_seq == fid.fid_seq &&
           core->fid.fid_nmx == fid.fid_nmx && core->fid.fid_rvn == fid.fid_rvn &&
           !(core->filechar & 0x8000u);
}

ods2_result_t ods2_delete_version(ods2_volume_t *vol, unsigned dir_file_number,
                                  const char *name, int version, uint16_t *version_out)
{
    return ods2_delete_version_info(vol, dir_file_number, name, version, version_out, NULL);
}

/* True for a file number the volume reserves for its own files: 1-9 always, 10 (SECURITY.SYS) on an ODS-5
   volume, or on an ODS-2 volume when the entry is SECURITY.SYS in the master file directory. */
static bool is_reserved_file(const ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                             unsigned file_number)
{
    if (file_number >= 1 && file_number <= 9) return true;
    if (file_number == 10) {
        size_t i;
        static const char security[] = "SECURITY.SYS";
        if (vol->ods5) return true;
        if (dir_file_number != 4 || strlen(name) != sizeof(security) - 1) return false;
        for (i = 0; i < sizeof(security) - 1; i++) {
            if (toupper((unsigned char) name[i]) != security[i]) return false;
        }
        return true;
    }
    return false;
}

static ods2_result_t delete_version_impl(ods2_volume_t *vol, unsigned dir_file_number,
                                         const char *name, int version, uint16_t *version_out,
                                         ods2_delete_info_t *info);

ods2_result_t ods2_delete_version_info(ods2_volume_t *vol, unsigned dir_file_number,
                                       const char *name, int version, uint16_t *version_out,
                                       ods2_delete_info_t *info)
{
    TXN_BEGIN_OR_RETURN(vol);
    return txn_end(vol, delete_version_impl(vol, dir_file_number, name, version, version_out, info));
}

static ods2_result_t delete_version_impl(ods2_volume_t *vol, unsigned dir_file_number,
                                         const char *name, int version, uint16_t *version_out,
                                         ods2_delete_info_t *info)
{
    uint16_t target_version;
    uint8_t dir_header[512];
    ods2_fid_t target_fid;
    uint8_t target_header[512];
    ods2_head_core_t *target_core;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    int i;
    uint8_t ext_headers[ODS2_MAX_HEADER_SEGMENTS][512];
    unsigned ext_numbers[ODS2_MAX_HEADER_SEGMENTS];
    int ext_count = 0;
    ods2_result_t r;

    if (info != NULL) memset(info, 0, sizeof(*info));
    if (!vol->writable) {
        return not_writable(vol);
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_ready(vol, dir_header, dir_file_number);
    if (!r.ok) return r;

    r = ods2_lookup_name_version(vol, dir_header, name, version, &target_fid, &target_version);
    if (!r.ok) return r;

    /* An entry's FID names a file by number, sequence, relative volume number and extended number; this
       code finds a file by the number alone, in this volume. An entry with an RVN or an NMX (a volume set
       member, a file number above 65535) would resolve to some other file's header (review 7, R7-2): refused.
       No VMS-made directory in the corpus has one. */
    r = ods2_check_fid(target_fid);
    if (!r.ok) return r;

    /* The volume's own files cannot be deleted, whichever directory lists them (review 6, finding 1):
       INDEXF.SYS, BITMAP.SYS, BADBLK.SYS, the master file directory, CORIMG.SYS, VOLSET.SYS, CONTIN.SYS,
       BACKUP.SYS and BADLOG.SYS are file numbers 1-9; SECURITY.SYS is 10 on every volume INITIALIZE makes
       (ODS-5 always; an older ODS-2 volume has no file 10, and a user file may hold the number). Deleting
       one frees clusters the volume still depends on. */
    if (is_reserved_file(vol, dir_file_number, name, ods2_fid_number(target_fid))) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be deleted");
    }

    r = ods2_read_header(vol, ods2_fid_number(target_fid), target_header);
    if (!r.ok) return r;
    target_core = (ods2_head_core_t *) target_header;
    /* file 10 of an ODS-2 volume: by what its header says, wherever the entry is (an alias in another directory
       is refused too; VMS's SET FILE/REMOVE took such entries and left LOSTHEADER) */
    if (ods2_fid_number(target_fid) == 10 && ods2_header_is_file(target_header, target_fid) &&
        file10_is_security(vol, target_header)) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be deleted");
    }
    if (!ods2_header_is_file(target_header, target_fid)) {
        /* a dangling entry (its file went with another name, the slot may be free or reused): take
           the entry out and touch nothing else, as VMS does (batch m1) */
        r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version, target_fid);
        if (!r.ok) return r;
        if (version_out != NULL) *version_out = target_version;
        if (info != NULL) info->kept = 2;
        return ok();
    }

    if (vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS) &&
        (target_core->struclev >> 8) == ODS5_LEVEL_5 && target_core->seg_num == 0) {
        /* A file with several links: deleting one link takes its directory
           entry and one off the count; the file goes with the last. The
           header holds the primary link's directory and name, so the primary
           link of a file that has others is not deleted here. */
        unsigned links = (unsigned) (target_header[ODS5_FH2_LINKCOUNT] |
                                     (target_header[ODS5_FH2_LINKCOUNT + 1] << 8));
        if (links > 1) {
            r = header_ok_for_write(target_header, ods2_fid_number(target_fid)); /* its link count is rewritten */
            if (!r.ok) return r;
            /* VMS V8.4 (measured): deleting a link of a file with others takes its entry and one off
               the count; the header keeps its name even if it was the primary link's, and when it was,
               the back link becomes (0xFFFFFF,0,0); dates and revision do not change; the data stays */
            ods5_ident_t id;
            const char *why;
            char a[ODS5_MAX_LITERAL_NAME + 8], b[ODS5_MAX_LITERAL_NAME + 8];
            bool primary = false;
            if (ods5_read_ident(target_header, &id, &why) &&
                ods2_fid_number(target_core->backlink) == dir_file_number &&
                ods5_complete_name(id.name, a, sizeof(a)) && ods5_complete_name(name, b, sizeof(b)) &&
                ods2_dir_name_compare(a, strlen(a), b, strlen(b)) == 0 &&
                id.version == target_version) {
                primary = true;
            }
            r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version, target_fid);
            if (!r.ok) return r;
            links--;
            target_header[ODS5_FH2_LINKCOUNT] = (uint8_t) (links & 0xff);
            target_header[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) (links >> 8);
            if (primary) {
                target_core->backlink.fid_num = 0xFFFF;
                target_core->backlink.fid_seq = 0;
                target_core->backlink.fid_rvn = 0;
                target_core->backlink.fid_nmx = 0xFF;
            }
            {
                uint16_t checksum = ods2_checksum(target_header, 255);
                target_header[510] = (uint8_t) (checksum & 0xff);
                target_header[511] = (uint8_t) (checksum >> 8);
            }
            r = ods2_write_header(vol, ods2_fid_number(target_fid), target_header);
            if (!r.ok) return r;
            if (version_out != NULL) *version_out = target_version;
            return ok();
        }
    }

    if (!(vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS)) && !(target_core->filechar & 0x2000u) &&
        target_core->seg_num == 0) {
        /* No HARDLINKS characteristic: an extra directory entry for a file is an alias (SET FILE/ENTER,
           LINK), with no link count. An entry that is not the file's primary one (the header names
           another directory or another name) is just taken out; the file stays. Measured on V8.4 (batch m1,
           ODS5_FORMAT_NOTES 15.7): the file stays; the entries left when the primary one is
           deleted dangle. */
        ods5_ident_t id;
        const char *why;
        char a[ODS5_MAX_LITERAL_NAME + 8], b[ODS5_MAX_LITERAL_NAME + 8];
        if (ods5_read_ident(target_header, &id, &why) &&
            ods5_complete_name(id.name, a, sizeof(a)) && ods5_complete_name(name, b, sizeof(b))) {
            /* the version is not compared: a header may carry another one than the entry's */
            bool primary = ods2_fid_number(target_core->backlink) == dir_file_number &&
                           ods2_dir_name_compare(a, strlen(a), b, strlen(b)) == 0;
            if (!primary) {
                r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version, target_fid);
                if (!r.ok) return r;
                if (version_out != NULL) *version_out = target_version;
                if (info != NULL) {
                    info->kept = 1;
                    info->name_differs = ods2_dir_name_compare(a, strlen(a), b, strlen(b)) != 0;
                    snprintf(info->primary_name, sizeof(info->primary_name), "%s", id.name);
                    info->primary_version = id.version;
                    info->backlink_dir = ods2_fid_number(target_core->backlink);
                }
                return ok();
            }
        }
    }

    /* Refuse to delete a non-empty directory - matches VMS's own
       DELETE/DIRECTORY behavior and avoids silently orphaning its
       contents (their headers would become unreachable but never
       freed - a real, if recoverable via ANALYZE/DISK, leak). */
    if (target_core->filechar & 0x2000u) { /* FH2$M_DIRECTORY, confirmed value */
        int sub_count = 0;
        r = ods2_count_directory_entries(vol, target_header, &sub_count);
        if (!r.ok) return r;
        if (sub_count > 0) {
            return fail("directory is not empty - refusing to delete "
                        "(matches VMS's own DELETE/DIRECTORY behavior)");
        }
    }

    /* The header is rewritten (marked for delete) with a fresh checksum: it must be a sound one */
    r = header_ok_for_write(target_header, ods2_fid_number(target_fid));
    if (!r.ok) return r;

    /* Read everything the delete needs before anything is changed, so that a failure here (a damaged
       map, an extension header that cannot be read) leaves the file as it was. */
    r = ods2_decode_all_extents(vol, target_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    for (i = 0; i < extent_count; i++) {
        /* ods2_free_blocks() refuses an extent that is not whole clusters: refuse here, before anything is
           buffered (R9-6) */
        if (extents[i].lbn % vol->home.cluster != 0 || extents[i].block_count % vol->home.cluster != 0) {
            return fail("an extent of this file is not a whole number of clusters at a cluster boundary "
                        "(damaged map): not changed");
        }
    }
    {
        ods2_fid_t next = target_core->ext_fid;
        while (ods2_fid_number(next) != 0) {
            if (ext_count >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("extension header chain exceeded maximum segment limit "
                            "(possibly corrupted or circular ext_fid chain)");
            }
            ext_numbers[ext_count] = ods2_fid_number(next);
            r = ods2_read_header(vol, ods2_fid_number(next), ext_headers[ext_count]);
            if (!r.ok) return r;
            next = ((ods2_head_core_t *) ext_headers[ext_count])->ext_fid;
            ext_count++;
        }
    }

    /* VMS's own order (review 6, finding 2; review 7, R7-1), so that an interruption never leaves a listed
       file whose clusters are free, nor a header that still maps clusters another file may be given:
       (a) mark the header for delete, FID and checksum intact; (b) remove the directory entry; (c) write the
       extension headers and then the primary header in their final deleted form (FID zeroed, checksum 0);
       (d) free the blocks; (e) clear the file numbers' bits. After (a) the file reads as gone
       (ods2_header_is_file); an interruption after (a) leaves a marked header whose clusters are still
       allocated, and after (c) only clusters that nothing owns (lost clusters, which ANALYZE/DISK/REPAIR
       frees): in no state does a header that could be finished or recovered map free clusters. */
    target_core->filechar |= 0x8000u; /* FH2$M_MARKDEL, confirmed value */
    {
        uint16_t checksum = ods2_checksum(target_header, 255);
        target_header[510] = (uint8_t) (checksum & 0xff);
        target_header[511] = (uint8_t) (checksum >> 8);
    }
    r = ods2_write_header(vol, ods2_fid_number(target_fid), target_header);
    if (!r.ok) return r;

    /* If the entry cannot be removed the header stays marked: the entry is then dangling and the file reads
       as gone, a leak; writing the old header back could make a half-removed entry's file live and unlisted
       (review 7, R7-5). */
    r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version, target_fid);
    if (!r.ok) return r;

    /* Extension headers (spec 3.3): each has its own file number and header slot, released like the
       primary. Left alone they would stay allocated, owned by nothing. */
    for (i = 0; i < ext_count; i++) {
        ods2_head_core_t *ext_core = (ods2_head_core_t *) ext_headers[i];

        ext_core->filechar |= 0x8000u;
        ext_core->fid.fid_num = 0;
        ext_core->fid.fid_nmx = 0;
        ext_core->fid.fid_rvn = 0;
        ext_headers[i][510] = 0;
        ext_headers[i][511] = 0;
        r = ods2_write_header(vol, ext_numbers[i], ext_headers[i]);
        if (!r.ok) return r;
    }

    /* The header itself, per spec 3.5.1's exact rules: FH2$V_MARKDEL set, FID_NUM/NMX/RVN zeroed - but
       FH2$W_FID_SEQ is explicitly NOT touched, since spec 5.1.7 requires it to survive so the next use
       of this slot gets seq+1, not a fresh seq of 1 (that rule is only for a slot that was never used at
       all - "garbage" - which this no longer is). Checksum is set to zero, not recomputed - also an
       explicit spec rule, not an oversight. */
    target_core->fid.fid_num = 0;
    target_core->fid.fid_nmx = 0;
    target_core->fid.fid_rvn = 0;
    target_header[510] = 0;
    target_header[511] = 0;
    r = ods2_write_header(vol, ods2_fid_number(target_fid), target_header);
    if (!r.ok) return r;

    for (i = 0; i < extent_count; i++) {
        r = ods2_free_blocks(vol, extents[i].lbn, extents[i].block_count);
        if (!r.ok) return r;
    }

    /* the file numbers are free in the index file's bitmap: the extension headers, the primary last */
    for (i = 0; i < ext_count; i++) {
        r = mark_index_bitmap(vol, ext_numbers[i], false);
        if (!r.ok) return r;
    }
    r = mark_index_bitmap(vol, ods2_fid_number(target_fid), false);
    if (!r.ok) return r;

    if (version_out != NULL) *version_out = target_version;
    return ok();
}

/* --- the volume label (issue #13) --- */

const char *ods2_make_volume_label(const char *label, char out[12])
{
    size_t len = strlen(label), i;

    if (len == 0) return "a volume label may not be empty";
    if (len > 12) return "a volume label is at most 12 characters";
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char) label[i];
        if (!(isalnum(c) || c == '$' || c == '_' || c == '-')) {
            return "a volume label may contain only letters, digits, '$', '_' and '-'";
        }
    }
    for (i = 0; i < 12; i++) {
        out[i] = (i < len) ? (char) toupper((unsigned char) label[i]) : ' ';
    }
    return NULL;
}

/* A 12-byte name field is blank: spaces (as the spec says) or NULs. */
static bool name_is_blank(const char *field)
{
    int i;
    for (i = 0; i < 12; i++) {
        if (field[i] != ' ' && field[i] != '\0') return false;
    }
    return true;
}

static void set_home_checksums(uint8_t *raw)
{
    /* checksum1 first: checksum2's coverage includes it (see
       ods2_validate.h). */
    uint16_t c1 = ods2_checksum(raw, 29), c2;
    raw[58] = (uint8_t) (c1 & 0xff);
    raw[59] = (uint8_t) (c1 >> 8);
    c2 = ods2_checksum(raw, 255);
    raw[510] = (uint8_t) (c2 & 0xff);
    raw[511] = (uint8_t) (c2 >> 8);
}

/* Is `block` the copy of `primary` that belongs at `lbn` / INDEXF.SYS
   VBN `vbn`? A valid home block whose HOMELBN and HOMEVBN name its own
   position, and otherwise identical to the primary: only those two
   fields and the checksums differ between copies (as on every volume
   VMS INITIALIZE made that has been examined). */
static bool is_home_copy(const uint8_t *block, const uint8_t *primary, uint32_t lbn,
                         uint32_t vbn)
{
    const ods2_home_t *h = (const ods2_home_t *) block;
    size_t i;

    if (!ods2_validate_home(h).ok) return false;
    if (h->homelbn != lbn || h->homevbn != vbn) return false;
    for (i = 0; i < 512; i++) {
        bool own_field = i < 4 || i == 16 || i == 17 || i == 58 || i == 59 || i >= 510;
        if (!own_field && block[i] != primary[i]) return false;
    }
    return true;
}

ods2_result_t ods2_set_volume_label(ods2_volume_t *vol, const char *label,
                                     unsigned *copies_out)
{
    /* The home block and its cluster fillers are INDEXF.SYS VBNs 2 to
       2v, the backup home block's cluster VBNs 2v+1 to 3v, where v is
       the cluster factor (spec 5.1.3, 5.1.4); VBN 1 is the boot
       block. Every one of them holds a copy of the home block. */
    char name[12];
    const char *why;
    uint8_t primary[512];
    uint8_t (*blocks)[512] = NULL;
    uint32_t *lbns = NULL;
    unsigned v = vol->home.cluster, count, i, primary_index = 0;
    bool have_primary = false, have_backup = false;
    ods2_result_t r = ok();

    if (copies_out) *copies_out = 0;
    if (!vol->writable) {
        return not_writable(vol);
    }
    why = ods2_make_volume_label(label, name);
    if (why != NULL) return fail(why);
    if (!name_is_blank(vol->home.strucname) || vol->home.rvn != 0 || vol->home.setcount != 0) {
        return fail("this volume is part of a volume set: relabelling one member "
                    "alone isn't supported - relabel the set on VMS");
    }
    if (v == 0) return fail("home block has a cluster factor of 0");

    memcpy(primary, &vol->home, 512);
    count = 3u * v - 1u; /* VBNs 2 .. 3v */
    blocks = malloc((size_t) count * 512u);
    lbns = malloc((size_t) count * sizeof(*lbns));
    if (blocks == NULL || lbns == NULL) {
        free(blocks);
        free(lbns);
        return fail("could not allocate memory for the home block copies");
    }

    /* Read and check every copy first; write nothing unless all of
       them are what they should be. */
    for (i = 0; i < count && r.ok; i++) {
        uint32_t vbn = i + 2, lbn;
        if (!vbn_to_lbn(vol->indexf_extents, vol->indexf_extent_count, vbn, &lbn)) {
            r = fail("INDEXF.SYS doesn't map all of the home block's clusters");
            break;
        }
        lbns[i] = lbn;
        if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
            fread(blocks[i], 1, 512, vol->fp) != 512) {
            r = fail("could not read a home block copy");
            break;
        }
        if (!is_home_copy(blocks[i], primary, lbn, vbn)) {
            static char msg[200];
            snprintf(msg, sizeof msg, "the block at LBN %u (INDEXF.SYS VBN %u) isn't a "
                     "valid copy of the home block - nothing changed; check the volume "
                     "with ANALYZE/DISK on VMS", (unsigned) lbn, (unsigned) vbn);
            r = fail(msg);
            break;
        }
        if (lbn == vol->home.homelbn) {
            have_primary = true;
            primary_index = i;
        }
        if (lbn == vol->home.alhomelbn) have_backup = true;
    }
    if (r.ok && !have_primary) {
        r = fail("the primary home block isn't among INDEXF.SYS's home block clusters");
    }
    if (r.ok && !have_backup) {
        r = fail("the backup home block isn't among INDEXF.SYS's home block clusters");
    }

    /* Relabel each copy, keeping its own HOMELBN/HOMEVBN, and write the
       primary last: until it is written VMS still sees one consistent
       (old) label, and running this again finishes the job. */
    if (r.ok) {
        for (i = 0; i < count; i++) {
            memcpy(((ods2_home_t *) blocks[i])->volname, name, 12);
            set_home_checksums(blocks[i]);
            if (!ods2_validate_home((const ods2_home_t *) blocks[i]).ok) {
                r = fail("internal error: a relabelled home block doesn't validate");
                break;
            }
        }
    }
    for (i = 0; r.ok && i <= count; i++) {
        unsigned k = (i < count) ? i : primary_index;
        if (i < count && i == primary_index) continue;
        r = ods2_write_block(vol, lbns[k], blocks[k]);
    }
    if (r.ok && fflush(vol->fp) != 0) r = fail("could not flush the home blocks");

    /* Read every copy back. */
    for (i = 0; r.ok && i < count; i++) {
        uint8_t back[512];
        if (fseek(vol->fp, (long) lbns[i] * 512, SEEK_SET) != 0 ||
            fread(back, 1, 512, vol->fp) != 512 || memcmp(back, blocks[i], 512) != 0) {
            r = fail("a home block copy didn't read back as written");
        }
    }
    if (r.ok) {
        memcpy(&vol->home, blocks[primary_index], 512);
        if (copies_out) *copies_out = count;
    }
    free(blocks);
    free(lbns);
    return r;
}

ods2_result_t ods2_volume_space(ods2_volume_t *vol, uint64_t *total_blocks_out,
                                 uint64_t *free_blocks_out)
{
    uint8_t bitmap_header[512], block[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    uint32_t volsize = 0, vbn = 1, cluster = vol->home.cluster, bitmap_blocks = 0;
    uint64_t clusters = 0, free_clusters = 0, bit = 0;
    int i;
    ods2_result_t r;

    r = ods2_read_header(vol, 2, bitmap_header); /* BITMAP.SYS */
    if (!r.ok) return r;
    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0 || cluster == 0) return fail("could not decode BITMAP.SYS");
    for (i = 0; i < extent_count; i++) bitmap_blocks += extents[i].block_count;

    /* VBN 1 is the storage control block (spec 5.2.1): SCB$L_VOLSIZE
       at byte 4. The bits follow from VBN 2, one per cluster, set =
       free (spec 5.2.2). */
    for (i = 0; i < extent_count; i++) {
        uint32_t b;
        for (b = 0; b < extents[i].block_count; b++, vbn++) {
            uint32_t k;
            if (!read_block(vol, extents[i].lbn + b, block, "could not read BITMAP.SYS").ok) {
                return fail("could not read BITMAP.SYS");
            }
            if (vbn == 1) {
                volsize = (uint32_t) block[4] | ((uint32_t) block[5] << 8) |
                          ((uint32_t) block[6] << 16) | ((uint32_t) block[7] << 24);
                clusters = ((uint64_t) volsize + cluster - 1) / cluster;
                /* The bitmap (VBN 2 on) must have a bit for every
                   cluster; an SCB that says otherwise isn't believed. */
                if (volsize == 0 || bitmap_blocks < 2 ||
                    clusters > (uint64_t) (bitmap_blocks - 1) * 4096u) {
                    return fail("BITMAP.SYS's storage control block gives a volume "
                                "size its bitmap doesn't cover");
                }
                *total_blocks_out = volsize;
                continue;
            }
            for (k = 0; k < 4096 && bit < clusters; k++, bit++) {
                if ((block[k / 8] >> (k % 8)) & 1u) free_clusters++;
            }
        }
    }
    if (vbn == 1) return fail("BITMAP.SYS is empty");
    *free_blocks_out = free_clusters * cluster;
    return ok();
}
