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
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64 /* off_t is 64 bits: images and files above 2 GiB (LARGE_VOLUMES 5, 7) */
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
#include <sys/types.h>

/* every seek in this file is fseeko/ftello with an off_t: a 32-bit off_t would put LBNs above 2^22 in the wrong place */
_Static_assert(sizeof(off_t) >= 8, "off_t must be 64 bits: compile with -D_FILE_OFFSET_BITS=64");
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

/* A failure of a write or flush says why the host refused (ENOSPC, EFBIG, EIO, ...): `what` and strerror(errno), in a buffer that
   lives until the next such failure. errno is read at once, before anything else can change it. */
static ods2_result_t fail_io(const char *what)
{
    static char msg[160];
    int e = errno;
    ods2_result_t r;
    snprintf(msg, sizeof(msg), "%s: %s", what, e != 0 ? strerror(e) : "write failed");
    r.ok = false;
    r.problem = msg;
    return r;
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

/* Why `h` is not the extension header it should be, or NULL if it is: a valid header (checksum), at level 2 or 5, with the file number and sequence the chain said, the next
   segment number, and the primary header's file number AND sequence number as back link. A damaged or crafted EXT_FID otherwise makes the file continue in another file's blocks; a
   back link with the right number and another sequence names a file that had the number before (review 54: every VMS-made extension header of the corpus, 248 of them, carries the
   primary's sequence number). The text names the one cause with the file numbers and sequences involved (review 59 S2); it lives in a static buffer until the next call. */
static const char *extension_header_why(const uint8_t *h, ods2_fid_t expect, unsigned primary_num, uint16_t primary_seq, unsigned segment)
{
    static char why[240];
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    unsigned level = core->struclev >> 8;
    if (level != ODS5_LEVEL_2 && level != ODS5_LEVEL_5) {
        snprintf(why, sizeof(why), "extension header %u of file %u is not a level 2 or level 5 header (structure level 0x%04x)", ods2_fid_number(expect), primary_num, core->struclev);
    } else if (!ods2_validate_head(h).ok) {
        snprintf(why, sizeof(why), "extension header %u of file %u is not a valid header (its checksum or structure is wrong)", ods2_fid_number(expect), primary_num);
    } else if (ods2_fid_number(core->fid) != ods2_fid_number(expect) || core->fid.fid_seq != expect.fid_seq) {
        snprintf(why, sizeof(why), "the extension header of file %u read as file %u sequence %u, but the chain said file %u sequence %u", primary_num,
                 ods2_fid_number(core->fid), core->fid.fid_seq, ods2_fid_number(expect), expect.fid_seq);
    } else if (core->seg_num != segment) {
        snprintf(why, sizeof(why), "extension header %u of file %u has segment number %u, expected %u", ods2_fid_number(expect), primary_num, core->seg_num, segment);
    } else if (ods2_fid_number(core->backlink) != primary_num) {
        snprintf(why, sizeof(why), "extension header %u: its back link names file %u, not file %u", ods2_fid_number(expect), ods2_fid_number(core->backlink), primary_num);
    } else if (core->backlink.fid_seq != primary_seq) {
        snprintf(why, sizeof(why), "extension header %u of file %u: its back link names sequence %u, not the primary header's sequence %u", ods2_fid_number(expect), primary_num,
                 core->backlink.fid_seq, primary_seq);
    } else {
        return NULL;
    }
    return why;
}

static bool extension_header_ok(const uint8_t *h, ods2_fid_t expect, unsigned primary_num, uint16_t primary_seq, unsigned segment)
{
    return extension_header_why(h, expect, primary_num, primary_seq, segment) == NULL;
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
#define ODS2_MAX_DIR_DEPTH_DEFAULT 2048u
/* The creation limit. The test binary (ods2_fault) can raise it with ODS2_TEST_MAX_DIR_DEPTH, to make the trees
   deeper than 2,048 levels a VMS volume can hold (V7.3-2, V8.4 made 10,000) that the readers must handle. */
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

/* INDEXF.SYS's extent cache: replaced as a whole (a change that is dropped puts the old list back) or appended to. */
static bool indexf_extents_room(ods2_volume_t *vol, size_t count)
{
    if (count > vol->indexf_extent_cap) {
        size_t ncap = vol->indexf_extent_cap == 0 ? 64 : vol->indexf_extent_cap * 2;
        ods2_extent_t *bigger;
        while (ncap < count) ncap *= 2;
        bigger = realloc(vol->indexf_extents, ncap * sizeof(*bigger));
        if (bigger == NULL) return false;
        vol->indexf_extents = bigger;
        vol->indexf_extent_cap = ncap;
    }
    return true;
}

/* Reads INDEXF.SYS's whole map into the cache: `primary` (file 1's header) and every extension header after it, which are
   themselves inside the index file and found through the extents known so far. A chain that cannot be followed to its end
   keeps what was read before it unless `strict`; the number of headers is FH2$W_SEG_NUM's, 65,535 (each must be the next
   segment of file 1, so a loop stops at its second visit). Returns false when the primary's own map is unusable or memory
   is out. */
static bool indexf_extents_load(ods2_volume_t *vol, const uint8_t *primary, bool strict, const char **why)
{
    uint8_t current[512];
    ods2_extent_t part[256];
    int n;
    unsigned segment;

    vol->indexf_extent_count = 0;
    n = decode_header_extents(primary, part, sizeof(part) / sizeof(part[0]));
    if (n <= 0) { if (why) *why = "could not decode INDEXF.SYS's own retrieval pointers"; return false; }
    if (!indexf_extents_room(vol, (size_t) n)) { if (why) *why = "out of memory"; return false; }
    memcpy(vol->indexf_extents, part, (size_t) n * sizeof(part[0]));
    vol->indexf_extent_count = n;
    memcpy(current, primary, sizeof(current));
    for (segment = 0; segment < 65535u; segment++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) current;
        uint8_t next[512];
        if (ods2_fid_number(core->ext_fid) == 0) break;
        if (!ods2_read_header(vol, ods2_fid_number(core->ext_fid), next).ok ||
            !extension_header_ok(next, core->ext_fid, 1, ((const ods2_head_core_t *) primary)->fid.fid_seq, segment + 1)) {
            if (strict) { if (why) *why = "an INDEXF.SYS extension header cannot be read"; return false; }
            break;
        }
        n = decode_header_extents(next, part, sizeof(part) / sizeof(part[0]));
        if (n < 0) { if (strict) { if (why) *why = "an INDEXF.SYS extension header does not decode"; return false; } break; }
        if (n == 0) { memcpy(current, next, sizeof(current)); continue; }   /* an empty map: the next link is still followed */
        if (!indexf_extents_room(vol, (size_t) vol->indexf_extent_count + (size_t) n)) { if (why) *why = "out of memory"; return false; }
        memcpy(vol->indexf_extents + vol->indexf_extent_count, part, (size_t) n * sizeof(part[0]));
        vol->indexf_extent_count += n;
        memcpy(current, next, sizeof(current));
    }
    return true;
}


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

    if (fseeko(vol->fp, 512, SEEK_SET) != 0 ||
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
        if (fseeko(vol->fp, (off_t) lbn * 512, SEEK_SET) != 0 ||
            fread(indexf_header, 1, sizeof(indexf_header), vol->fp) != sizeof(indexf_header)) {
            ods2_dismount(vol);
            return fail("could not read INDEXF.SYS's own header");
        }
    }

    {
        const char *why = "could not decode INDEXF.SYS's own retrieval pointers";
        /* A fragmented index file continues in extension headers (FH2$W_EXT_FID); if one cannot be read the extents
           known so far are kept for reading. A volume is written only when INDEXF.SYS's whole chain can be followed
           (review 27 N4): a growth would otherwise build on a chain it has not seen. */
        vol->indexf_chain_ok = indexf_extents_load(vol, indexf_header, true, &why);
        if (!vol->indexf_chain_ok && !indexf_extents_load(vol, indexf_header, false, &why)) {
            ods2_dismount(vol);
            return fail(why);
        }
        if (writable && !vol->indexf_chain_ok) {
            ods2_dismount(vol);
            return fail("INDEXF.SYS's extension header chain cannot be followed to its end: the volume can be read, "
                        "not written");
        }
    }

    load_scb_volsize(vol);
    if (writable) {
        char fmsg[256];
        /* Without SCB$L_VOLSIZE nothing bounds the allocator or the writes to the volume (lbn_in_volume,
           allocate_clusters): such a volume is not written (upstream issue #23, b167ac0, refuses to allocate). */
        if (vol->volsize == 0) {
            ods2_dismount(vol);
            return fail("the storage control block's volume size is 0 or unreadable: the volume can be read, "
                        "not written");
        }
        if (ods2_image_footer(vol, fmsg, sizeof(fmsg)) == 2) {
            ods2_dismount(vol);
            return fail("a SIMH container footer lies inside the volume (the last block may be lost): the "
                        "volume can be read, not written");
        }
        if (ods2_image_short(vol, fmsg, sizeof(fmsg))) {
            ods2_dismount(vol);
            return fail("the disk image is shorter than the volume it holds: the volume can be read, not written (extend "
                        "the image file to the volume's size first)");
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

/* Test builds only (tests/ods2_integrity_selftest.c, the upstream pull request's interruption tests, run against
   this code). ods2_fault_after_writes: the (n+1)th write after it is set to n, and every one after it, fails, as if
   the machine stopped there; -1 is off. While ods2_fault_journal_on is set every block written is kept with what it
   replaced, so that the test can put the image back (ods2_fault_rollback) instead of copying a volume of 1.5 GB for
   each interruption it tries. The writes counted are the ones a change makes at its commit, in the order it made them. */
long ods2_fault_after_writes = -1;
bool ods2_fault_journal_on = false;
static struct { uint32_t lbn; uint8_t old[512]; } *fault_journal;
static size_t fault_journal_n, fault_journal_cap;

void ods2_fault_forget(void)
{
    fault_journal_n = 0;
}

void ods2_fault_rollback(ods2_volume_t *vol)
{
    while (fault_journal_n > 0) {
        fault_journal_n--;
        if (fseeko(vol->fp, (off_t) fault_journal[fault_journal_n].lbn * 512, SEEK_SET) != 0 ||
            fwrite(fault_journal[fault_journal_n].old, 1, 512, vol->fp) != 512) {
            abort();
        }
    }
    fflush(vol->fp);
    vol->write_failed = false;   /* the image is as it was: this mount may write again (a fresh mount would) */
}

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
    size_t   prev;       /* index + 1 of the write before this one to the same lbn, 0 = none */
    bool     dropped;    /* the block was freed after this write: it is not written at commit (txn_forget_blocks) */
    uint8_t  data[512];
} txn_write_t;

struct ods2_txn {
    txn_write_t  *w;
    size_t        n, cap;
    size_t       *table;       /* open addressing: index + 1 of the latest write of an lbn, 0 = empty */
    size_t        table_cap;   /* a power of two */
    ods2_extent_t *saved_extents;   /* INDEXF.SYS's extent cache as it was when the change began */
    int           saved_extent_count;
    ods2_extent_t *allocd;       /* blocks allocated inside this change (they are on disk as free until the commit): giving such blocks
                                    back is no danger to anything that was on the disk before */
    size_t        nallocd, allocd_cap;
    ods2_extent_t *freed;        /* blocks given back inside this change that were allocated BEFORE it: file content must never be written onto them before the
                                    commit (content goes straight to the image; the bitmap that frees them is only written at the
                                    commit, so an interrupted change would leave the new bytes under a file that still owns the blocks) */
    size_t        nfreed, freed_cap;
};

/* Remembers blocks allocated inside the open change. */
static void txn_note_alloc(struct ods2_txn *t, uint32_t lbn, unsigned count)
{
    if (t->nallocd == t->allocd_cap) {
        size_t ncap = t->allocd_cap ? t->allocd_cap * 2 : 16;
        ods2_extent_t *na = realloc(t->allocd, ncap * sizeof(*na));
        if (na == NULL) return;                 /* not remembered: a later free of these blocks is then noted as a free of old blocks: a guard
                                                   that refuses too much, never too little */
        t->allocd = na;
        t->allocd_cap = ncap;
    }
    t->allocd[t->nallocd].lbn = lbn;
    t->allocd[t->nallocd].block_count = count;
    t->nallocd++;
}

/* Blocks given back inside the open change: the part that was allocated inside this change itself is taken off the allocated list (it was never
   owned on the disk); the rest (blocks that a file owned on the disk before the change) is remembered, so that file content is never written onto
   it before the commit (see struct ods2_txn). */
static bool txn_note_free(struct ods2_txn *t, uint32_t lbn, unsigned count)
{
    uint64_t lo = lbn, hi = (uint64_t) lbn + count;       /* the part still to place, [lo, hi) */
    size_t i;
    for (i = 0; i < t->nallocd && lo < hi; i++) {
        uint64_t alo = t->allocd[i].lbn, ahi = alo + t->allocd[i].block_count;
        if (ahi <= lo || alo >= hi) continue;
        {
            uint64_t olo = alo > lo ? alo : lo, ohi = ahi < hi ? ahi : hi;      /* the overlap: allocated in this change */
            /* the allocated record loses [olo, ohi) */
            if (olo > alo && ohi < ahi) {                                         /* in the middle: split */
                txn_note_alloc(t, (uint32_t) ohi, (unsigned) (ahi - ohi));
                t->allocd[i].block_count = (unsigned) (olo - alo);
            } else if (olo > alo) {
                t->allocd[i].block_count = (unsigned) (olo - alo);
            } else if (ohi < ahi) {
                t->allocd[i].lbn = (uint32_t) ohi;
                t->allocd[i].block_count = (unsigned) (ahi - ohi);
            } else {
                t->allocd[i].block_count = 0;
            }
            /* what is left of the freed range: below and above the overlap */
            if (olo > lo) {
                /* the lower part is old (or overlaps other records: handled by a recursive note) */
                if (!txn_note_free(t, (uint32_t) lo, (unsigned) (olo - lo))) return false;
            }
            lo = ohi;
        }
    }
    if (lo < hi) {
#ifdef ODS2_FAULT_INJECTION
        if (getenv("ODS2_FAULT_NOTE_FAIL") != NULL) return false;      /* test: the list cannot grow */
#endif
        if (t->nfreed == t->freed_cap) {
            size_t ncap = t->freed_cap ? t->freed_cap * 2 : 16;
            ods2_extent_t *nf = realloc(t->freed, ncap * sizeof(*nf));
            if (nf == NULL) return false;
            t->freed = nf;
            t->freed_cap = ncap;
        }
        t->freed[t->nfreed].lbn = (uint32_t) lo;
        t->freed[t->nfreed].block_count = (unsigned) (hi - lo);
        t->nfreed++;
    }
    return true;
}

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
        return at != 0 && !t->w[at - 1].dropped ? t->w[at - 1].data : NULL;
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
    t->w[t->n].prev = t->table[txn_slot(t, lbn)];
    t->w[t->n].dropped = false;
    memcpy(t->w[t->n].data, block, 512);
    t->n++;
    t->table[txn_slot(t, lbn)] = t->n;
    return ok();
}

/* Blocks freed inside a change are no longer the change's: a write kept for one of them must not be made at commit, when
   the blocks may belong to something else (review 28 F1: the clusters are given to the next file at once and its content
   is written straight to the image). Every kept write to a block of lbn .. lbn+count-1 is dropped. */
static void txn_forget_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned count)
{
    struct ods2_txn *t = vol->txn;
    size_t i;
    if (t == NULL || t->n == 0 || count == 0) return;
    if (count < t->n) {
        unsigned k;
        for (k = 0; k < count; k++) {
            size_t at = t->table[txn_slot(t, lbn + k)];
            while (at != 0) {
                t->w[at - 1].dropped = true;
                at = t->w[at - 1].prev;
            }
        }
    } else {
        for (i = 0; i < t->n; i++) {
            if (t->w[i].lbn >= lbn && t->w[i].lbn - lbn < count) t->w[i].dropped = true;
        }
    }
}

/* Reads a block as the change in progress sees it. */
static ods2_result_t read_block(ods2_volume_t *vol, uint32_t lbn, uint8_t *out, const char *why)
{
    const uint8_t *pending = txn_find(vol, lbn);
    if (pending != NULL) {
        memcpy(out, pending, 512);
        return ok();
    }
    if (fseeko(vol->fp, (off_t) lbn * 512, SEEK_SET) != 0 || fread(out, 1, 512, vol->fp) != 512) {
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
    off_t here = ftello(vol->fp), size;

    if (fseeko(vol->fp, 0, SEEK_END) != 0) return false;
    size = ftello(vol->fp);
    (void) fseeko(vol->fp, here, SEEK_SET);
    if (size < 1024) return false;
    *image_blocks = (uint64_t) size / 512;
    if (fseeko(vol->fp, size - 512, SEEK_SET) != 0 || fread(foot, 1, 512, vol->fp) != 512) {
        (void) fseeko(vol->fp, here, SEEK_SET);
        return false;
    }
    (void) fseeko(vol->fp, here, SEEK_SET);
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

bool ods2_image_short(ods2_volume_t *vol, char *msg, size_t cap)
{
    off_t here, size;
    uint64_t image_blocks;

    if (msg != NULL && cap > 0) msg[0] = '\0';
    if (vol->fp == NULL || vol->volsize == 0) return false;
    {
        /* The starter volume's storage control block is a placeholder (all ones, or 0xAAAA...: its "volume size" is no size): a block
           that is no SCB claims nothing to compare, and that volume stays writable (README: make starter-volume). */
        uint8_t bh[512], scb[512];
        if (!ods2_read_header(vol, 2, bh).ok || !ods2_read_file_block(vol, bh, 1, scb).ok) return false;
        {
            unsigned level = (unsigned) (scb[0] | (scb[1] << 8));
            if (level != 0x0201u && level != 0x0501u) return false;
        }
    }
    here = ftello(vol->fp);
    if (fseeko(vol->fp, 0, SEEK_END) != 0) return false;
    size = ftello(vol->fp);
    (void) fseeko(vol->fp, here < 0 ? 0 : here, SEEK_SET);
    if (size < 0) return false;
    image_blocks = (uint64_t) size / 512;
    if (image_blocks >= vol->volsize) return false;
    if (msg != NULL) {
        snprintf(msg, cap, "the disk image has %llu blocks but the volume's storage control block says it has %u: the image "
                 "is shorter than the volume (a truncated copy, or a sparse image copied without its holes?); reading the "
                 "missing blocks fails, and the volume is not written", (unsigned long long) image_blocks,
                 (unsigned) vol->volsize);
    }
    return true;
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
static ods2_result_t indexf_repair_growth(ods2_volume_t *vol);

/* The first change of any kind made through a write mount repairs an interrupted growth of the index file (review 28 a), not
   only a create: once per mount, before the change's own buffer exists (it is one header and a bit). Every write path
   calls it: txn_begin() for the changes, and the ones that write without a buffer. */
static ods2_result_t first_change(ods2_volume_t *vol)
{
    ods2_result_t h;
    if (!vol->writable || vol->indexf_checked || vol->txn != NULL) return ok();
    h = indexf_repair_growth(vol);
    if (!h.ok) return h;
    vol->indexf_checked = true;
    return ok();
}

static ods2_result_t txn_begin(ods2_volume_t *vol)
{
    if (!vol->writable) return ok();
    if (vol->txn_depth == 0) {
        if (vol->write_failed) {
            return fail("an earlier write to the disk image failed: nothing more is written through this "
                        "mount (run ANALYZE/DISK_STRUCTURE/REPAIR on VMS if the image is in doubt)");
        }
        {
            ods2_result_t h = first_change(vol);
            if (!h.ok) return h;
        }
#ifdef ODS2_FAULT_INJECTION
        vol->txn = getenv("ODS2_FAULT_NOMEM_TXN") != NULL ? NULL : calloc(1, sizeof(*vol->txn));
#else
        vol->txn = calloc(1, sizeof(*vol->txn));
#endif
        if (vol->txn == NULL) return fail("out of memory: the change cannot be prepared, nothing is written");
        vol->txn->saved_extents = malloc((size_t) (vol->indexf_extent_count > 0 ? vol->indexf_extent_count : 1) *
                                         sizeof(*vol->txn->saved_extents));
        if (vol->txn->saved_extents == NULL) {
            free(vol->txn);
            vol->txn = NULL;
            return fail("out of memory: the change cannot be prepared, nothing is written");
        }
        if (vol->indexf_extent_count > 0) {
            memcpy(vol->txn->saved_extents, vol->indexf_extents, (size_t) vol->indexf_extent_count * sizeof(ods2_extent_t));
        }
        vol->txn->saved_extent_count = vol->indexf_extent_count;
    }
    vol->txn_depth++;
    return ok();
}

#define TXN_BEGIN_OR_RETURN(vol) do { ods2_result_t b_ = txn_begin(vol); if (!b_.ok) return b_; } while (0)

static ods2_result_t raw_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block);

static void bitmap_cache_drop(ods2_volume_t *vol);

/* puts INDEXF.SYS's extent cache back to what it was when the change began (room was made for it then) */
static void indexf_extents_restore(ods2_volume_t *vol, const struct ods2_txn *t)
{
    if (t->saved_extent_count > 0 && indexf_extents_room(vol, (size_t) t->saved_extent_count)) {
        memcpy(vol->indexf_extents, t->saved_extents, (size_t) t->saved_extent_count * sizeof(ods2_extent_t));
        vol->indexf_extent_count = t->saved_extent_count;
    }
}

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
        for (i = 0; i < t->n && r.ok; i++) {
            if (!t->w[i].dropped) r = raw_write_block(vol, t->w[i].lbn, t->w[i].data);
        }
        if (!r.ok) {
            /* part of the change is on the disk and the rest is not: nothing more is written through this
               mount (R9-2), and the INDEXF.SYS cache goes back to what it was before the change, as the
               disk may not have its new header */
            vol->write_failed = true;
            indexf_extents_restore(vol, t);
            vol->idx_highest_valid = false;
            bitmap_cache_drop(vol);
        }
    } else {
        indexf_extents_restore(vol, t);
        vol->idx_highest_valid = false;
        bitmap_cache_drop(vol);
    }
    free(t->saved_extents);
    free(t->allocd);
    free(t->freed);
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
#ifdef ODS2_FAULT_INJECTION
    if (ods2_fault_after_writes >= 0) {
        if (ods2_fault_after_writes == 0) return fail("injected write failure");   /* and every write after it */
        ods2_fault_after_writes--;
    }
    if (ods2_fault_journal_on) {
        if (fault_journal_n == fault_journal_cap) {
            fault_journal_cap = fault_journal_cap ? fault_journal_cap * 2 : 256;
            fault_journal = realloc(fault_journal, fault_journal_cap * sizeof(*fault_journal));
            if (fault_journal == NULL) abort();
        }
        fault_journal[fault_journal_n].lbn = lbn;
        if (fseeko(vol->fp, (off_t) lbn * 512, SEEK_SET) != 0 ||
            fread(fault_journal[fault_journal_n].old, 1, 512, vol->fp) != 512) {
            memset(fault_journal[fault_journal_n].old, 0, 512);
        }
        fault_journal_n++;
    }
#endif
    if (fseeko(vol->fp, (off_t) lbn * 512, SEEK_SET) != 0 ||
        fwrite(block, 1, 512, vol->fp) != 512) {
        return fail_io("could not write block");
    }
    if (fflush(vol->fp) != 0) {
        return fail_io("could not flush written block");
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
    if (fflush(vol->fp) != 0) return fail_io("could not flush the disk image");
    if (getenv("ODS2_NOSYNC") == NULL && fdatasync(fileno(vol->fp)) != 0) {
        return fail_io("could not write the disk image to the disk (fdatasync failed)");
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
    free(vol->bm_data);
    vol->bm_data = NULL;
    vol->bm_valid = false;
    free(vol->indexf_extents);
    vol->indexf_extents = NULL;
    vol->indexf_extent_cap = 0;
    vol->indexf_extent_count = 0;
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
    if (ods2_fid_number(fid) == 0) {
        return fail("a directory entry names file number 0, which is no file");
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
/* Bitmaps above this are not kept in memory (a 2 TiB volume with one-block clusters: 512 MiB): each allocation reads
   them whole, as before. */
#define ODS2_BITMAP_CACHE_MAX ((size_t) 64 << 20)

static void bitmap_cache_drop(ods2_volume_t *vol)
{
    free(vol->bm_data);
    vol->bm_data = NULL;
    vol->bm_bytes = 0;
    vol->bm_hint = 0;
    vol->bm_valid = false;
}

static ods2_result_t allocate_clusters_inner(ods2_volume_t *vol, unsigned blocks_needed,
                                       bool partial_ok, uint32_t *lbn_out,
                                       unsigned *blocks_allocated_out)
{
    uint8_t bitmap_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_bitmap_blocks = 0;
    unsigned bits_blocks; /* blocks actually holding bitmap bits, excluding the SCB */
    uint8_t *bitmap_data;
    bool cached = false;
    size_t total_bits, total_bytes;
    size_t start_cluster, found_clusters, from_bit = 0;
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
    if (!(vol->bm_valid && vol->bm_bytes == (size_t) bits_blocks * 512)) {
        /* a damaged BITMAP.SYS header can claim any size: the bitmap of this image
           needs one bit per cluster, so more than a few blocks over that is damage */
        off_t here = ftello(vol->fp);
        off_t image_blocks = 0;
        if (fseeko(vol->fp, 0, SEEK_END) == 0) image_blocks = ftello(vol->fp) / 512;
        (void) fseeko(vol->fp, here, SEEK_SET);
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
    if (vol->bm_valid && vol->bm_bytes == total_bytes) {
        bitmap_data = vol->bm_data;
        cached = true;
    } else {
        bitmap_cache_drop(vol);
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
        if (total_bytes <= ODS2_BITMAP_CACHE_MAX) {
            vol->bm_data = bitmap_data;
            vol->bm_bytes = total_bytes;
            vol->bm_hint = 0;
            vol->bm_valid = true;
            cached = true;
        }
    }
    /* every cluster below the hint is allocated: first fit starts at its byte (the search is unchanged otherwise) */
    if (cached) from_bit = (vol->bm_hint / 8) * 8;
    if (from_bit >= total_bits) from_bit = 0;

    if (partial_ok) {
        if (!ods2_bitmap_find_run(bitmap_data + from_bit / 8, total_bits - from_bit, clusters_needed,
                                  &start_cluster, &found_clusters)) {
            if (!cached) free(bitmap_data);
            return fail("no free space left on the volume");
        }
        clusters_needed = (unsigned) found_clusters;
    } else if (!ods2_bitmap_find_free(bitmap_data + from_bit / 8, total_bits - from_bit, clusters_needed,
                                      &start_cluster, &found_clusters)) {
        if (!cached) free(bitmap_data);
        return fail("no free space found on volume for this allocation");
    }
    start_cluster += from_bit;
    if (!ods2_bitmap_mark(bitmap_data, total_bits, start_cluster, clusters_needed, true)) {
        if (!cached) free(bitmap_data);
        if (cached) bitmap_cache_drop(vol);
        return fail("internal error marking bitmap bits (should be unreachable)");
    }
    if (cached && start_cluster <= vol->bm_hint) vol->bm_hint = start_cluster + clusters_needed;

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
                if (!cached) free(bitmap_data);
                if (cached) bitmap_cache_drop(vol);
                return r;
            }
        }
    }

    if (!cached) free(bitmap_data);

    *lbn_out = (uint32_t) (start_cluster * vol->home.cluster);
    *blocks_allocated_out = clusters_needed * vol->home.cluster;
    return ok();
}

static ods2_result_t allocate_clusters(ods2_volume_t *vol, unsigned blocks_needed, bool partial_ok, uint32_t *lbn_out,
                                       unsigned *blocks_allocated_out)
{
    ods2_result_t r = allocate_clusters_inner(vol, blocks_needed, partial_ok, lbn_out, blocks_allocated_out);
    if (r.ok && vol->txn != NULL) txn_note_alloc(vol->txn, *lbn_out, *blocks_allocated_out);
    return r;
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

/* The most headers one file can have: FH2$W_SEG_NUM is a word. The chain is followed to its end; each extension header must
   be the next segment number of this file (extension_header_ok), so a circular chain stops at the second visit. */
#define ODS2_SEGMENT_LIMIT 65535

ods2_result_t ods2_decode_all_extents_alloc(ods2_volume_t *vol, const uint8_t *header,
                                             ods2_extent_t **extents_out, int *count_out)
{
    uint8_t current[512];
    ods2_extent_t *list = NULL;
    size_t cap = 0;
    int total = 0;
    unsigned segment;

    *extents_out = NULL;
    *count_out = 0;
    memcpy(current, header, 512);

    for (segment = 0; segment < ODS2_SEGMENT_LIMIT; segment++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) current;
        ods2_extent_t part[256];      /* a header's map area holds at most 255 words: 127 pointers */
        int n = decode_header_extents(current, part, sizeof(part) / sizeof(part[0]));

        if (n == ODS2_DECODE_TOO_MANY || n < 0) {
            free(list);
            return fail("could not decode retrieval pointers for a header segment");
        }
        if (n > 0) {
            if ((size_t) total + (size_t) n > cap) {
                size_t ncap = cap == 0 ? 256 : cap * 2;
                ods2_extent_t *bigger;
                while (ncap < (size_t) total + (size_t) n) ncap *= 2;
                if (ncap > (size_t) 0x7fffffff / sizeof(ods2_extent_t)) { free(list); return fail("too many extents"); }
                bigger = realloc(list, ncap * sizeof(*list));
                if (bigger == NULL) { free(list); return fail("out of memory reading a file's retrieval pointers"); }
                list = bigger;
                cap = ncap;
            }
            memcpy(list + total, part, (size_t) n * sizeof(*list));
            total += n;
        }

        if (ods2_fid_number(core->ext_fid) == 0) {
            /* No further extension header - done. */
            *extents_out = list;
            *count_out = total;
            return ok();
        }

        {
            ods2_fid_t expect = core->ext_fid;
            ods2_result_t r = ods2_read_header(vol, ods2_fid_number(expect), current);
            if (!r.ok) { free(list); return r; }
            {
                const char *why = extension_header_why(current, expect, ods2_fid_number(((const ods2_head_core_t *) header)->fid),
                                                       ((const ods2_head_core_t *) header)->fid.fid_seq, segment + 1);
                if (why != NULL) {
                    free(list);
                    return fail(why);
                }
            }
        }
    }

    free(list);
    return fail("extension header chain exceeded maximum segment limit "
                "(possibly corrupted or circular ext_fid chain)");
}

/* The fixed-array form, for callers that hold a few extents (system files, directories): the same walk, and a file with
   more than `max_extents` pieces is refused, never given as a shortened list (a reader would stop short of the file's end,
   a delete would free only the extents it was given and leak the rest). */
ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out)
{
    ods2_extent_t *list = NULL;
    int total = 0;
    ods2_result_t r = ods2_decode_all_extents_alloc(vol, header, &list, &total);

    if (!r.ok) return r;
    if ((size_t) total > max_extents) {
        free(list);
        return fail("this file has more extents than this caller can hold: refused");
    }
    if (total > 0) memcpy(extents_out, list, (size_t) total * sizeof(*list));
    free(list);
    *count_out = total;
    return ok();
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
    uint64_t len = ods2_file_content_length(dir_header);
    uint64_t used = (len + 511) / 512;
    return (used > allocated) ? allocated : (unsigned) used;
}

static ods2_result_t walk_directory_from(ods2_volume_t *vol, const uint8_t *dir_header, unsigned first_block,
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

    for (block_index = first_block < 1 ? 1 : first_block; block_index <= total_blocks; block_index++) {
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

static ods2_result_t walk_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    dir_visit_fn visit, void *ctx)
{
    return walk_directory_from(vol, dir_header, 1, visit, ctx);
}

/* A directory of this many blocks or more is searched by halving (a name lookup reads about log2(blocks) blocks, not all of
   them): its blocks are sorted by name, as every directory VMS or this program makes is (the version lookup already stops
   at the first name past the one it wants). Smaller ones are walked whole as before. */
#define ODS2_DIR_BSEARCH_MIN 32u

/* Where a walk for `name` may start: the last block whose first record's name sorts before `name` (every record of `name`
   is in it or after it); 1 when the directory is small or a probed block holds no records (an unusual layout: no
   shortcut). *sorted says the search was used, so a visitor may stop at the first name past `name`. */
static ods2_result_t dir_search_start(ods2_volume_t *vol, const uint8_t *dir_header, const char *name,
                                      unsigned *start_out, bool *sorted_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count, i;
    unsigned total = 0, lo, hi;
    ods2_result_t r;

    *start_out = 1;
    *sorted_out = false;
    r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok || extent_count <= 0) return ok();           /* the walk itself reports it */
    for (i = 0; i < extent_count; i++) total += extents[i].block_count;
    total = dir_blocks_in_use(dir_header, total);
    if (total < ODS2_DIR_BSEARCH_MIN) return ok();
    lo = 1;
    hi = total;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo + 1) / 2, bad = 0;
        uint8_t block[512];
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        int n;
        r = ods2_read_file_block(vol, dir_header, mid, block);
        if (!r.ok) return ok();
        n = ods2_parse_directory_records(block, sizeof(block), vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2, e,
                                         ODS2_DIR_MAX_ENTRIES_PER_BLOCK, &bad);
        if (n <= 0 || bad != 0) return ok();                /* an empty or damaged block: walk it all */
        if (ods2_dir_name_compare(e[0].name, strlen(e[0].name), name, strlen(name)) < 0) lo = mid;
        else hi = mid - 1;
    }
    *start_out = lo;
    *sorted_out = true;
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

uint64_t ods2_file_content_length(const uint8_t *header)
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
    return (uint64_t) (efblk - 1) * 512u + ffbyte;
}

uint64_t ods2_file_allocated_bytes(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return (uint64_t) ods2_word_swap32(core->recattr.hiblk) * 512u;
}

ods2_result_t ods2_read_file_stream(ods2_volume_t *vol, const uint8_t *header,
                                     bool (*sink)(void *ctx, const uint8_t *data, size_t n),
                                     void *ctx, uint64_t *bytes_out, bool *sink_failed_out)
{
    uint64_t content_len = ods2_file_content_length(header);
    uint64_t allocated = ods2_file_allocated_bytes(header);
    uint64_t written = 0;
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
        ods2_extent_t *extents = NULL;
        size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? (size_t) content_len : ODS2_STREAM_CHUNK;
        uint8_t *chunk;
        int n, i;

        r = ods2_decode_all_extents_alloc(vol, header, &extents, &n);
        if (!r.ok) return r;
        chunk = malloc(chunk_cap);
        if (chunk == NULL) { free(extents); return fail("could not allocate a read buffer"); }
        for (i = 0; i < n && written < content_len && r.ok; i++) {
            uint64_t want = content_len - written;
            uint64_t run = (uint64_t) extents[i].block_count * 512u;
            if (want > run) want = run;
            if (fseeko(vol->fp, (off_t) extents[i].lbn * 512, SEEK_SET) != 0) {
                r = fail("could not read file content block");
                break;
            }
            while (want > 0) {
                size_t piece = want < chunk_cap ? (size_t) want : chunk_cap;
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
        free(extents);
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

    r = ods2_write_block(vol, lbn, block);
    if (r.ok && vol->idx_highest_valid) {
        if (used && file_number > vol->idx_highest) vol->idx_highest = file_number;
        else if (!used && file_number == vol->idx_highest) vol->idx_highest_valid = false;   /* rescanned when needed */
    }
    return r;
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

    if (vol->idx_highest_valid) {
        *highest_out = vol->idx_highest;
        return ok();
    }
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
    vol->idx_highest = *highest_out;
    vol->idx_highest_valid = true;
    return ok();
}

ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    uint8_t indexf_header[512];
    uint8_t block[512];
    unsigned limit, n, loaded_block = (unsigned) -1;
    uint32_t efblk, hiblk;
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
    hiblk = ods2_word_swap32(((ods2_head_core_t *) indexf_header)->recattr.hiblk);

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
        /* A number whose low 16 bits are 0 (65536, 131072, ...) is never handed out: its FID_NUM would be 0, which means
           "no file". VMS made 70,000 files on a volume and DUMP/HEADER of file 65536 gave %DUMP-E-BADHEADER while 65535
           and 65537 were normal (measured on VMS); the slot stays as it is. */
        if ((n & 0xFFFFu) == 0) continue;

        if (header_vbn(vol, n) >= efblk) {
            /* beyond the end-of-file: garbage, unless the block is mapped and holds a valid header (an extension
               header of INDEXF.SYS made by a growth that was interrupted: its slot lies past EFBLK until the next
               create raises it; review 27 F2) */
            if (header_vbn(vol, n) > hiblk || !ods2_read_header(vol, n, header).ok ||
                classify_header_slot(header) != SLOT_VALID) {
                *file_number_out = n;
                *seq_out = 1;
                return ok();
            }
            continue;
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

    indexf_header[510] = (uint8_t) (checksum & 0xff);
    indexf_header[511] = (uint8_t) (checksum >> 8);
    r = ods2_write_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    if (vol->home.altidxlbn != 0) {
        r = ods2_write_block(vol, vol->home.altidxlbn, indexf_header);
        if (!r.ok) return r;
    }
    {
        const char *why = "INDEXF.SYS's header no longer decodes after updating it";
        if (!indexf_extents_load(vol, indexf_header, true, &why)) return fail(why);
    }
    return ok();
}

static uint32_t indexf_total_blocks(const ods2_volume_t *vol);
static ods2_result_t header_ok_for_write(const uint8_t *h, unsigned file_number);

/* Whether INDEXF.SYS's last growth was interrupted (review 28 a): *hiblk_behind says the map holds more blocks than HIBLK, and
   *stray_from is the one file number marked in use whose header lies beyond the map (0 if none, or if several). */
static ods2_result_t indexf_growth_state(ods2_volume_t *vol, uint8_t *primary, bool *hiblk_behind, unsigned *stray_from)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) primary;
    uint32_t mapped = indexf_total_blocks(vol), hib;
    unsigned highest;
    ods2_result_t r = ods2_read_header(vol, 1, primary);

    *hiblk_behind = false;
    *stray_from = 0;
    if (!r.ok) return r;
    hib = ods2_word_swap32(core->recattr.hiblk);
    *hiblk_behind = mapped > hib;
    r = highest_used_file_number(vol, &highest);
    if (!r.ok) return r;
    if (highest > 0 && header_vbn(vol, highest) > (mapped > hib ? mapped : hib)) {
        /* exactly one number marked past the map is the mark of an interrupted growth (the new extension header's slot, set
           before it was linked); more are something else (a volume made to look full, damage) and are left as they are */
        unsigned n, marked = 0, scanned = 0;
        uint8_t block[512];
        unsigned loaded = (unsigned) -1;
        uint32_t limit = mapped > hib ? mapped : hib;
        for (n = highest; n > 0 && header_vbn(vol, n) > limit && scanned < 200000u && marked < 2; n--, scanned++) {
            unsigned bit = n - 1;
            if (bit / 4096u != loaded) {
                r = read_index_bitmap_block(vol, bit / 4096u, block);
                if (!r.ok) return r;
                loaded = bit / 4096u;
            }
            if (block[(bit % 4096u) / 8u] & (1u << (bit % 8u))) marked++;
        }
        if (marked == 1 && scanned < 200000u) *stray_from = highest;
    }
    return ok();
}

bool ods2_indexf_growth_unfinished(ods2_volume_t *vol)
{
    uint8_t primary[512];
    bool behind;
    unsigned stray;
    return indexf_growth_state(vol, primary, &behind, &stray).ok && (behind || stray != 0);
}

bool ods2_indexf_ext_looks_ods2v2(ods2_volume_t *vol)
{
    uint8_t cur[512];
    unsigned level, count;

    if (ods2_read_header(vol, 1, cur).ok == false) return false;
    level = ((const ods2_head_core_t *) cur)->struclev >> 8;
    for (count = 0; count < 65535u && ods2_fid_number(((const ods2_head_core_t *) cur)->ext_fid) != 0; count++) {
        unsigned next = ods2_fid_number(((const ods2_head_core_t *) cur)->ext_fid);
        const ods2_head_core_t *c;
        if (!ods2_read_header(vol, next, cur).ok) return false;
        c = (const ods2_head_core_t *) cur;
        if ((!vol->ods5 && c->mpoffset == 100) || (c->struclev >> 8) != level) return true;
    }
    return false;
}

/* The repair: HIBLK from the map (and EFBLK over the highest header marked in use inside it), and the one bit marked beyond the
   map cleared (a growth interrupted after its new extension header's bit was set and before the header was linked: the
   number belongs to nothing). */
static ods2_result_t indexf_repair_growth(ods2_volume_t *vol)
{
    uint8_t primary[512];
    ods2_head_core_t *core = (ods2_head_core_t *) primary;
    bool behind, changed = false;
    unsigned stray;
    uint32_t mapped = indexf_total_blocks(vol);
    ods2_result_t r = indexf_growth_state(vol, primary, &behind, &stray);

    if (!r.ok) return ok();                       /* an unreadable index file is reported by the change itself */
    if (!behind && stray == 0) return ok();
    if (!header_ok_for_write(primary, 1).ok) return ok();
    if (behind) {
        unsigned highest;
        core->recattr.hiblk = ods2_word_swap32(mapped);
        if (core->highwater != 0) core->highwater = mapped + 1;
        r = highest_used_file_number(vol, &highest);
        if (!r.ok) return r;
        if (highest > 0 && header_vbn(vol, highest) <= mapped && header_vbn(vol, highest) + 1 > ods2_word_swap32(core->recattr.efblk)) {
            core->recattr.efblk = ods2_word_swap32(header_vbn(vol, highest) + 1);
            core->recattr.ffbyte = 0;
        }
        changed = true;
    }
    if (stray != 0) {
        r = mark_index_bitmap(vol, stray, false);
        if (!r.ok) return r;
    }
    if (changed) {
        r = write_indexf_header(vol, primary);
        if (!r.ok) return r;
    }
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

/* Test hooks (the fault-injection build only): ODS2_TEST_INDEXF=step=N[,nomerge] makes INDEXF.SYS grow N blocks at a time
   (rounded to clusters) and never lengthen its last pointer, so a few hundred files fill the primary header's map area and
   the extension-header code runs on a small volume. */
static unsigned indexf_test_step(void)
{
#ifdef ODS2_FAULT_INJECTION
    const char *e = getenv("ODS2_TEST_INDEXF");
    const char *k = e != NULL ? strstr(e, "step=") : NULL;
    if (k != NULL && atoi(k + 5) > 0) return (unsigned) atoi(k + 5);
#endif
    return 0;
}

static bool indexf_test_nomerge(void)
{
#ifdef ODS2_FAULT_INJECTION
    const char *e = getenv("ODS2_TEST_INDEXF");
    return e != NULL && strstr(e, "nomerge") != NULL;
#else
    return false;
#endif
}

/* The old growth rule (an extension header only when less than 4 bytes of map remain, and none to carry a pointer that does
   not fit), to make the stuck state of review 27 F1 for tests/cli_indexf_stall_check.sh: ODS2_TEST_INDEXF=...,noreserve. */
static bool indexf_test_noreserve(void)
{
#ifdef ODS2_FAULT_INJECTION
    const char *e = getenv("ODS2_TEST_INDEXF");
    return e != NULL && strstr(e, "noreserve") != NULL;
#else
    return false;
#endif
}

/* ods2v2 never creates an INDEXF.SYS extension header on its own (CORPUS 37, 38: OpenVMS V8.4 reads a volume with the headers this
   code wrote but bugchecks (UNXSIGNAL in the ACP) at the first growth of INDEXF.SYS through one; with pointers in it and files
   behind it ANALYZE/DISK_STRUCTURE gives %ANALDISK-F-OPENINDEX and the files are "no such file"; VMS makes none itself). The code that makes and grows them stays for the tests, behind the fault-injection build's
   ODS2_TEST_INDEXF hook (`noext` in the hook turns it off again). Not to be switched on: VMS makes none itself. */
static bool indexf_ext_creation_allowed(void)
{
#ifdef ODS2_FAULT_INJECTION
    const char *e = getenv("ODS2_TEST_INDEXF");
    return e != NULL && strstr(e, "noext") == NULL;
#else
    return false;
#endif
}

/* Bytes of map area still free in header `h`. */
static size_t map_room(const uint8_t *h)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    if (map_end > 510 || map_offset + map_bytes > map_end) return 0;
    return map_end - (map_offset + map_bytes);
}

/* Puts (lbn, count) into the map of `h`: a new pointer at the end, or - when the new blocks follow the last pointer's
   directly and the longer run still fits one pointer - the last pointer made longer (INDEXF.SYS grows in runs that are
   usually adjacent: thousands of steps stay one pointer). False, `h` untouched, when there is no room. Does not touch
   HIBLK. */
static bool indexf_map_append(uint8_t *h, uint32_t lbn, unsigned count, bool merge)
{
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t size = ods2_retrieval_pointer_size(lbn, count);

    if (size == 0) return false;
    if (merge && map_bytes > 0) {
        size_t w = 0, words = map_bytes / 2, last = (size_t) -1, last_size = 0;
        ods2_extent_t le;
        while (w < words) {
            unsigned fmt = (unsigned) (h[map_offset + w * 2 + 1] >> 6);
            size_t sw = fmt == 0 ? 1 : (fmt == 1 ? 2 : (fmt == 2 ? 3 : 4));
            if (fmt != 0) { last = w; last_size = sw; }
            w += sw;
        }
        if (last != (size_t) -1 && w == words &&
            ods2_decode_retrieval_pointers(h + map_offset + last * 2, last_size, &le, 1) == 1 &&
            (uint64_t) le.lbn + le.block_count == lbn && (uint64_t) le.block_count + count <= (1u << 30)) {
            size_t nsize = ods2_retrieval_pointer_size(le.lbn, le.block_count + count);
            if (nsize != 0 && nsize >= last_size * 2 && nsize - last_size * 2 <= map_room(h)) {
                uint8_t enc[8];
                if (ods2_encode_retrieval_pointer(enc, le.lbn, le.block_count + count) == nsize) {
                    memcpy(h + map_offset + last * 2, enc, nsize);
                    core->map_inuse = (uint8_t) ((map_bytes - last_size * 2 + nsize) / 2);
                    return true;
                }
            }
        }
    }
    if (size > map_room(h)) return false;
    if (ods2_encode_retrieval_pointer(h + map_offset + map_bytes, lbn, count) != size) return false;
    core->map_inuse = (uint8_t) ((map_bytes + size) / 2);
    return true;
}

/* The last header of INDEXF.SYS's chain: `primary` itself, or the last extension header (read into `tail`). Returns the
   tail's file number in *tail_num and how many extension headers there are in *ext_count. */
static ods2_result_t indexf_tail(ods2_volume_t *vol, const uint8_t *primary, uint8_t *tail, unsigned *tail_num,
                                 unsigned *ext_count)
{
    unsigned count = 0;
    memcpy(tail, primary, 512);
    *tail_num = 1;
    while (ods2_fid_number(((const ods2_head_core_t *) tail)->ext_fid) != 0) {
        ods2_fid_t link = ((const ods2_head_core_t *) tail)->ext_fid;
        unsigned next = ods2_fid_number(link);
        ods2_result_t r;
        if (count >= 65535u) return fail("INDEXF.SYS's extension header chain does not end");
        r = ods2_read_header(vol, next, tail);
        if (!r.ok) return r;
        /* each header must be the next segment of file 1, as at mount (review 27 N1): a wrong header is never written */
        if (!extension_header_ok(tail, link, 1, ((const ods2_head_core_t *) primary)->fid.fid_seq, count + 1)) return fail("an INDEXF.SYS extension header is not the next segment of the index file");
        *tail_num = next;
        count++;
    }
    *ext_count = count;
    return ok();
}

static void header_seal(uint8_t *h)
{
    uint16_t checksum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (checksum & 0xff);
    h[511] = (uint8_t) (checksum >> 8);
}

/* The total of INDEXF.SYS's extents as the cache holds them: the file's real size. FH2$L_HIBLK of the primary must say the
   same; a growth that was interrupted between its two blocks leaves it smaller (review 27 F2), and the next growth
   repairs it from here. */
static uint32_t indexf_total_blocks(const ods2_volume_t *vol)
{
    uint64_t total = 0;
    int i;
    for (i = 0; i < vol->indexf_extent_count; i++) total += vol->indexf_extents[i].block_count;
    return total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) total;
}

/* A header slot for the new extension header of INDEXF.SYS among the slots of the blocks just added (VBNs first_vbn up to
   first_vbn+got-1): the last one that is free in the index bitmap, is not `n` (the file number the caller is about to use),
   is no multiple of 65536 (FID_NUM 0 is "no file") and lies inside MAXFILES and the index bitmap. 0 if there is none
   (review 27 F3). */
static unsigned indexf_pick_slot(ods2_volume_t *vol, uint32_t first_vbn, unsigned got, unsigned n)
{
    unsigned base = (unsigned) vol->home.cluster * 4 + vol->home.ibmapsize;
    unsigned limit = vol->home.maxfiles;
    uint8_t block[512];
    unsigned loaded = (unsigned) -1, v;

    if (limit > MAX_FILE_NUMBER) limit = MAX_FILE_NUMBER;
    if ((unsigned long) vol->home.ibmapsize * 4096u < limit) limit = (unsigned) vol->home.ibmapsize * 4096u;
    for (v = got; v > 0; v--) {
        uint32_t vbn = first_vbn + v - 1;
        unsigned slot, bit;
        if (vbn <= base) break;
        slot = vbn - base;
        if (slot > limit || slot == n || (slot & 0xFFFFu) == 0) continue;
        bit = slot - 1;
        if (bit / 4096u != loaded) {
            if (!read_index_bitmap_block(vol, bit / 4096u, block).ok) return 0;
            loaded = bit / 4096u;
        }
        if (block[(bit % 4096u) / 8u] & (1u << (bit % 8u))) continue;
        return slot;
    }
    return 0;
}

/* VMS CRASHES AT THE FIRST GROWTH THROUGH SUCH A HEADER (CORPUS 37/38): without the test hook (indexf_ext_creation_allowed) this function never makes an
   extension header and never grows an index file that has one: it takes a piece for the primary header's map, or refuses with
   nothing written. The extension-header code below is for the tests only.

   Allocates more blocks to INDEXF.SYS and maps them, so more headers fit; `indexf_header` (file 1's header, the caller's
   copy) comes back updated, and the change is on the disk when this returns ok. Up to 256 blocks at a time, half the file's
   size once it is big (at most 16384 blocks, one Format 2 pointer); falls back to a single cluster if no run that long is
   free. The new piece is mapped with the smallest retrieval pointer that describes it, or lengthens the last pointer when
   it follows it directly. When the map area of the last header of the chain would be left with less than 8 bytes (room
   for one more pointer of any size), the chain gets an EXTENSION header: made in a free slot of the new blocks (the header
   is written to its block directly; the last header maps the blocks), as the place for the next pointers. A pointer
   that does not fit the last header takes a short piece instead (a Format 1 pointer, 4 bytes) or is refused: the
   extension header that would carry it could not be read before the pointer that maps it. (Not measured on VMS: no
   VMS-made index file with an extension header was available.)

   Crash safety (review 27 F2): the writes go in this order, and every point between them is a state that is sound or
   heals at the next create. (1) the new blocks are zeroed (they are only allocated: a leak); (2) the new extension
   header, not linked from anywhere yet; (3) its bit in the index bitmap (set before it is linked, so the file number is
   never handed out; a number whose slot holds a valid header is not handed out in any case); (4) when the last header is an
   extension header: that header, with the new pointer and the link to the new extension header together (the index file
   then holds more blocks than the primary's HIBLK says, and the new header's bit lies past EFBLK: the next create sets
   both right from the map); (5) the primary with HIBLK, EFBLK (covering the new header) and, when it is the last header,
   the pointer and the link, then its backup copy. When the primary is the last header (4) does not exist and (5) is a
   single block. HIBLK is taken from the map, never only incremented. */
static ods2_result_t extend_indexf(ods2_volume_t *vol, uint8_t *indexf_header, unsigned n)
{
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    unsigned want = (256u / vol->home.cluster) * vol->home.cluster;
    unsigned hiblk = ods2_word_swap32(core->recattr.hiblk);
    uint32_t lbn, total = indexf_total_blocks(vol);
    unsigned got;
    uint8_t tail[512], ext[512], primary[512];
    unsigned tail_num = 1, ext_count = 0, slot = 0;
    size_t reserve = indexf_test_noreserve() ? 4 : 8;
    bool nomerge = indexf_test_nomerge(), fits, need_ext, ext_ok;
    unsigned attempt;
    ods2_result_t r;

    if (total > hiblk) hiblk = total;                      /* the file's size is its map's, not a count kept beside it */
    ext_ok = indexf_ext_creation_allowed();
    if (hiblk >= 4096) {
        /* A big index file grows by half its size, at most 16384 blocks (one Format 2 pointer) */
        want = hiblk / 2;
        if (want > 16384) want = 16384;
        want = (want / vol->home.cluster) * vol->home.cluster;
    }
    if (indexf_test_step() != 0) want = (indexf_test_step() / vol->home.cluster) * vol->home.cluster;
    if (want == 0) want = vol->home.cluster;
    r = indexf_tail(vol, indexf_header, tail, &tail_num, &ext_count);
    if (!r.ok) return r;
    if (tail_num != 1 && !ext_ok) {
        /* as at f70cee8: an index file with extension headers is read, not extended (nothing is written) */
        return fail("INDEXF.SYS has extension headers and its map is full: ods2v2 does not extend an index file that has "
                    "extension headers (VMS V8.4 crashes at the first growth of INDEXF.SYS through such a header: CORPUS 37/38)");
    }
    for (attempt = 0; ; attempt++) {
        uint8_t trial[512];
        if (attempt > 0 && !ext_ok) {
            /* the short piece for the last 4 bytes of map: as long as a Format 1 pointer can say (256 blocks) and a free run
               below LBN 4,194,304 allows, at least a cluster (review 30 W1) */
            r = ods2_allocate_extent(vol, want, &lbn, &got);
        } else {
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
        }
        if (!r.ok) return r;
        /* what the last header becomes, in memory */
        memcpy(trial, tail, 512);
        fits = indexf_map_append(trial, lbn, got, !nomerge);
        if (indexf_test_noreserve() || attempt > 0 ||
            (fits && (!ext_ok || map_room(trial) >= reserve || indexf_pick_slot(vol, (uint32_t) hiblk + 1, got, n) != 0))) {
            memcpy(tail, trial, 512);
            break;
        }
        /* The pointer does not fit the last header (a Format 2 pointer needs 6 bytes; an index file stuck like this by an
           older version, review 27 F1), or it fits and the header must get its extension header but the piece has no
           free slot for it (a piece of one block has only the new file's own): a short piece takes a Format 1 pointer (4 bytes)
           when its blocks lie below LBN 4,194,304 (2 blocks or more, and the extension header is made in it, under the test
           hook; up to 256 blocks without it). */
        ods2_free_blocks(vol, lbn, got);
        if (ext_ok) {
            want = (2u + vol->home.cluster - 1) / vol->home.cluster * vol->home.cluster;   /* the new file's slot and the header's */
        } else {
            want = (256u / vol->home.cluster) * vol->home.cluster;                         /* no extension header: as long as it can be */
            if (want == 0) want = vol->home.cluster;
        }
    }
    if (!fits) {
        /* Still no room: a header slot for an extension header to carry the pointer cannot be read before the pointer that
           maps it, and no slot is free inside the file when it needs to grow. Refused, nothing written. */
        ods2_free_blocks(vol, lbn, got);
        return fail("INDEXF.SYS's last header has no room for another extent: the index file cannot grow (ods2v2 does not "
                    "write INDEXF.SYS extension headers: VMS V8.4 crashes at the first growth of INDEXF.SYS through such a header: CORPUS 37/38)");
    }
    need_ext = ext_ok && map_room(tail) < reserve;
    if (need_ext) {
        slot = indexf_pick_slot(vol, (uint32_t) hiblk + 1, got, n);
        if (slot == 0) need_ext = false;                   /* only the reserve: the next growth tries again */
    }
    {
        /* The piece that is kept is zeroed now, not before it was known to be kept (review 28 F1: a piece zeroed in the
           change's buffer and then given back had its zeros written over the content of the file that was given the
           blocks next). The clusters hold whatever was in them, and header images left in free space (an earlier index
           file, a BACKUP save set kept as a file) would be seen by ANALYZE as valid headers, to be resurrected by REPAIR
           (review 6, finding 7): zeroed before they are mapped. */
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
    hiblk += got;
    memcpy(primary, indexf_header, 512);
    {
        ods2_head_core_t *pc = (ods2_head_core_t *) primary;
        ods2_head_core_t *tc = (ods2_head_core_t *) tail;
        if (need_ext) {
            ods2_header_spec_t spec;
            char ident[24] = "INDEXF.SYS;1";
            memset(&spec, 0, sizeof(spec));
            spec.level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
            ods2_fid_set_number(&spec.fid, slot);
            spec.fid.fid_seq = 1;
            spec.backlink = core->fid;                     /* the primary header's FID (spec 3.5.2.16) */
            spec.seg_num = (uint16_t) (ext_count + 1);
            spec.filechar = core->filechar;
            spec.rtype = core->recattr.rtype;
            spec.rattrib = core->recattr.rattrib;
            spec.rsize = core->recattr.rsize;
            spec.maxrec = core->recattr.maxrec;
            spec.hiblk = hiblk;                            /* the size at its making: the primary's attributes count */
            spec.efblk = ods2_word_swap32(core->recattr.efblk);
            spec.ffbyte = core->recattr.ffbyte;
            spec.highwater = core->highwater;
            spec.reserved_recprot = !vol->ods5;                /* as before follow-up b: 0xFE00 at level 2, 0 at level 5 */
            spec.link_count = (uint16_t) (indexf_header[ODS5_FH2_LINKCOUNT] | (indexf_header[ODS5_FH2_LINKCOUNT + 1] << 8));
            spec.ident_name = ident;
            if (!ods2_build_file_header(ext, &spec)) {
                ods2_free_blocks(vol, lbn, got);
                return fail("could not construct an INDEXF.SYS extension header");
            }
            ods2_fid_set_number(&tc->ext_fid, slot);
            tc->ext_fid.fid_seq = 1;
        }
        if (tail_num == 1) memcpy(primary, tail, 512);
        pc->recattr.hiblk = ods2_word_swap32(hiblk);
        if (pc->highwater != 0) pc->highwater = hiblk + 1;   /* INITIALIZE leaves HIGHWATER at HIBLK + 1 on INDEXF.SYS */
        if (need_ext && ods2_word_swap32(pc->recattr.efblk) < header_vbn(vol, slot) + 1) {
            pc->recattr.efblk = ods2_word_swap32(header_vbn(vol, slot) + 1);
            pc->recattr.ffbyte = 0;
        }
    }
    if (need_ext) {
        /* (2), (3): written to the blocks directly: nothing maps them yet */
        r = ods2_write_block(vol, lbn + (header_vbn(vol, slot) - ((uint32_t) hiblk - got + 1)), ext);
        if (r.ok) r = mark_index_bitmap(vol, slot, true);
        if (!r.ok) {
            ods2_free_blocks(vol, lbn, got);               /* nothing is linked yet */
            return r;
        }
    }
    if (tail_num != 1) {
        header_seal(tail);
        r = ods2_write_header(vol, tail_num, tail);        /* (4): its place is inside the old extents */
        if (!r.ok) return r;                               /* it may be on the disk: the blocks stay as they are */
    }
    memcpy(indexf_header, primary, 512);
    r = write_indexf_header(vol, indexf_header);           /* (5), and the cache of the extents from the whole chain */
    if (!r.ok) return r;
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
    uint32_t vbn, efblk, new_efblk, mapped;
    bool changed = false;
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

    /* HIBLK is the size of the map (an interrupted growth of an index file with extension headers leaves it smaller:
       review 27 F2); repaired here, with EFBLK below, before anything is built on it */
    mapped = indexf_total_blocks(vol);
    if (mapped > ods2_word_swap32(core->recattr.hiblk)) {
        core->recattr.hiblk = ods2_word_swap32(mapped);
        if (core->highwater != 0) core->highwater = mapped + 1;
        changed = true;
    }
    vbn = header_vbn(vol, n);
    while (vbn > ods2_word_swap32(core->recattr.hiblk)) {
        if (++extensions > 64) return fail("INDEXF.SYS could not be extended far enough to hold the new header");
        /* each growth is written complete (see extend_indexf): the blocks it took stay the index file's */
        r = extend_indexf(vol, indexf_header, n);
        if (!r.ok) return r;
        changed = false;
    }

    r = highest_used_file_number(vol, &highest_used);
    if (!r.ok) return r;
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

    /* The numbers the allocator skips (multiples of 65536: FID_NUM 0, "no file") are marked in use in the index bitmap
       once the file numbers have passed them, as VMS does (a volume with 70,000 files made on V8.4: file 65536 has its bit
       set and a header slot that was never written); the slot is left alone. */
    {
        unsigned m;
        for (m = 65536u; m < n; m += 65536u) {
            uint8_t bm[512];
            unsigned bit = m - 1;
            r = read_index_bitmap_block(vol, bit / 4096u, bm);
            if (!r.ok) return r;
            if (!(bm[(bit % 4096u) / 8u] & (1u << (bit % 8u)))) {
                r = mark_index_bitmap(vol, m, true);
                if (!r.ok) return r;
            }
        }
    }
    r = mark_index_bitmap(vol, n, true);
    if (!r.ok) return r;
    *file_number_out = n;
    *seq_out = seq;
    return ok();
}

/* --- ods2_lookup_name(): streams the directory, no size limit --- */
static const char ods2_name_not_found[] = "name not found in directory"; /* compared by address */
#define ODS2_NAME_NOT_FOUND ods2_name_not_found
typedef struct {
    const char *name;
    ods2_fid_t fid;
    bool found;
    bool sorted;        /* the walk started by halving: stop at the first name that sorts past `name` */
} lookup_ctx_t;

static bool visit_lookup(const ods2_dir_entry_t *entry, void *ctx_)
{
    lookup_ctx_t *ctx = (lookup_ctx_t *) ctx_;
    /* Case-insensitive compare - VMS names are conventionally
       uppercase on disk, but callers may pass either case. */
    int c = ods2_dir_name_compare(entry->name, strlen(entry->name), ctx->name, strlen(ctx->name));
    if (c == 0) {
        ctx->fid = entry->fid;
        ctx->found = true;
        return false; /* stop walking */
    }
    return !(ctx->sorted && c > 0);
}

ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out)
{
    /* Walks the directory block by block rather than listing it into
       a fixed buffer first - an earlier version used a 256-entry
       buffer here, so any name sorting after the 256th entry of its
       directory was silently reported as "not found". */
    lookup_ctx_t ctx;
    unsigned start;
    ods2_result_t r;

    ctx.name = name;
    ctx.found = false;
    ctx.sorted = false;
    r = dir_search_start(vol, dir_header, name, &start, &ctx.sorted);
    if (!r.ok) return r;
    r = walk_directory_from(vol, dir_header, start, visit_lookup, &ctx);
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
    bool sorted;         /* the walk started by halving: stop at the first name past `name` */
} version_lookup_ctx_t;

static bool visit_lookup_version(const ods2_dir_entry_t *entry, void *ctx_)
{
    version_lookup_ctx_t *ctx = (version_lookup_ctx_t *) ctx_;

    if (!names_equal(entry->name, ctx->name)) {
        /* Directories are sorted by name, so every version of a name
           is contiguous (even when split across several records and
           blocks). Once past them there is nothing more to find. */
        if (ctx->sorted && ods2_dir_name_compare(entry->name, strlen(entry->name), ctx->name, strlen(ctx->name)) > 0) {
            return false;
        }
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

typedef struct {
    const char *name;
    bool sorted;
    bool name_found;
    bool have_prev;
    unsigned prev;
    bool in_order;
} version_order_ctx_t;

static bool visit_version_order(const ods2_dir_entry_t *entry, void *ctx_)
{
    version_order_ctx_t *ctx = (version_order_ctx_t *) ctx_;

    if (!names_equal(entry->name, ctx->name)) {
        if (ctx->sorted && ods2_dir_name_compare(entry->name, strlen(entry->name), ctx->name, strlen(ctx->name)) > 0) return false;
        return !ctx->name_found;
    }
    ctx->name_found = true;
    if (ctx->have_prev && entry->version >= ctx->prev) ctx->in_order = false;     /* two neighbours that are not descending (or equal) */
    ctx->prev = entry->version;
    ctx->have_prev = true;
    return true;
}

ods2_result_t ods2_name_versions_in_order(ods2_volume_t *vol, const uint8_t *dir_header, const char *name, bool *in_order_out)
{
    version_order_ctx_t ctx;
    unsigned start;
    bool sorted;
    ods2_result_t r;

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;
    ctx.in_order = true;
    r = dir_search_start(vol, dir_header, name, &start, &sorted);
    if (!r.ok) return r;
    ctx.sorted = sorted;
    r = walk_directory_from(vol, dir_header, start, visit_version_order, &ctx);
    if (!r.ok) return r;
    *in_order_out = ctx.in_order;
    return ok();
}

ods2_result_t ods2_lookup_name_version(ods2_volume_t *vol, const uint8_t *dir_header,
                                        const char *name, int version,
                                        ods2_fid_t *fid_out, uint16_t *version_out)
{
    version_lookup_ctx_t ctx;
    unsigned start;
    bool sorted;
    ods2_result_t r;

    if (version > ODS2_MAX_VERSION || version < -ODS2_MAX_VERSION) {
        return fail("version number out of range (1..32767)");
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;
    ctx.version = version;
    r = dir_search_start(vol, dir_header, name, &start, &sorted);
    if (!r.ok) return r;
    ctx.sorted = sorted;
    r = walk_directory_from(vol, dir_header, start, visit_lookup_version, &ctx);
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
        (size_t) core->mpoffset * 2 >= 510 ||
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
#ifdef ODS2_FAULT_INJECTION
            /* test build only: ODS2_TEST_DASH_DIR=1 lets a name start with '-', to make the directories older versions made
               (the checker's warning and the VMS-unreachable case are tested on them) */
            if (name[0] == '-' && getenv("ODS2_TEST_DASH_DIR") != NULL) {
                why = NULL;
            } else
#endif
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
            return fail("directories can be nested at most 2048 levels deep (the depth DIR [...] and EXPORT read back)");
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
    spec.highwater = content_blocks + 1; /* the whole allocation is written, below: HIBLK + 1, as VMS makes a directory at both levels (CORPUS 53) */
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
    {
        /* the rest of the allocation as zeros at BOTH levels, so that HIGHWATER = HIBLK + 1 (as VMS makes a directory) tells the truth: with a cluster over 1 the
           blocks after the first hold whatever a deleted file left there, and a stored HIGHWATER over them says "written" (review 56 F1: this was under
           `if (vol->ods5)` while the HIGHWATER already was HIBLK + 1 at ODS-2) */
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
    uint64_t offset = 0;
    uint64_t content_len = src->length;
    size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? (size_t) content_len : ODS2_STREAM_CHUNK;
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
        if (vol->txn != NULL) {
            /* the allocator must not have handed out blocks freed in this still open change (review 40 F1: the oldest version's
               clusters were overwritten before the commit): refused here, before the first byte goes out */
            size_t k;
            for (k = 0; k < vol->txn->nfreed; k++) {
                const ods2_extent_t *f = &vol->txn->freed[k];
                if ((uint64_t) extents[i].lbn < (uint64_t) f->lbn + f->block_count &&
                    (uint64_t) f->lbn < (uint64_t) extents[i].lbn + extents[i].block_count) {
                    free(chunk);
                    return fail("internal error: file content would be written onto blocks freed in the same, uncommitted change: "
                                "nothing is written");
                }
            }
        }
    }
    for (i = 0; i < extent_count && r.ok; i++) {
        uint64_t run_bytes = (uint64_t) extents[i].block_count * 512u;
        uint64_t data = (offset < content_len) ? content_len - offset : 0;

        if (data > run_bytes) data = run_bytes;
        if (fseeko(vol->fp, (off_t) extents[i].lbn * 512, SEEK_SET) != 0) {
            r = fail("could not seek to file content blocks");
            break;
        }
        run_bytes -= data;
        while (data > 0) {
            size_t piece = data < chunk_cap ? (size_t) data : chunk_cap;
            if (!src->read(src->ctx, chunk, piece)) {
                r = fail(src->problem != NULL ? src->problem
                                              : "could not read the file's content");
                break;
            }
            if (fwrite(chunk, 1, piece, vol->fp) != piece) {
                r = fail_io("could not write file content blocks");
                break;
            }
            if (fflush(vol->fp) != 0) {
                r = fail_io("could not flush written file content");
                break;
            }
            fault_point();
            data -= piece;
            offset += piece;
        }
        while (r.ok && run_bytes > 0) {
            size_t n = (run_bytes < sizeof(zeros)) ? (size_t) run_bytes : sizeof(zeros);
            if (fwrite(zeros, 1, n, vol->fp) != n) {
                r = fail_io("could not write file content blocks");
                break;
            }
            run_bytes -= n;
        }
    }
    free(chunk);
    if (r.ok && fflush(vol->fp) != 0) {
        r = fail_io("could not flush written file content");
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
                                 ods2_fid_t *new_fid_out, int version_req, uint16_t *version_out, char *name_out);

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

bool ods2_stream_format(uint8_t rtype)
{
    return rtype == ODS2_RFM_STM || rtype == ODS2_RFM_STMLF || rtype == ODS2_RFM_STMCR;
}

/* The length of the records of stream content, as RMS reads them (measured on OpenVMS V8.4 and VAX V7.3, ods2v2-made and
   VMS-made files alike, ODS5_FORMAT_NOTES 15.28):
     Stream_LF (STMLF)  a record ends at LF; the LF is not part of it; a CR before the LF stays in the record.
     Stream_CR (STMCR)  a record ends at CR; the CR is not part of it.
     Stream (STM)       a record ends at LF, FF or VT, and that byte STAYS in the record ("abc" LF is a record of 4 bytes);
                        it also ends at CR LF, which is stripped ("abc" CR LF is 3 bytes); a lone CR is data.
   The last record may have no terminator. A file whose longest record is longer than its LRL (or MRS, when not 0) cannot be
   read in record mode (%RMS-F-RSZ, %RMS-W-RTB). One state machine, fed any number of pieces in order, used by the CLI's
   scan of a host file, the library, SET FILE/ATTRIBUTES=(MRS:0) and (in Python, the same rules) ods5check. */
void ods2_reclen_init(ods2_reclen_t *s, uint8_t rtype)
{
    s->rtype = rtype;
    s->cur = s->longest = 0;
    s->prev_cr = false;
}

static void reclen_end(ods2_reclen_t *s, size_t len)
{
    if (len > s->longest) s->longest = len;
    s->cur = 0;
    s->prev_cr = false;
}

void ods2_reclen_feed(ods2_reclen_t *s, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (s->rtype == ODS2_RFM_STMCR) {
            if (c == '\r') reclen_end(s, s->cur); else s->cur++;
        } else if (s->rtype == ODS2_RFM_STM) {
            if (c == '\n') {
                /* CR LF: the CR (counted as data so far) is part of the terminator; a lone LF stays in the record */
                reclen_end(s, s->prev_cr ? s->cur - 1 : s->cur + 1);
            } else if (c == '\f' || c == '\v') {
                reclen_end(s, s->cur + 1);
            } else {
                s->cur++;
                s->prev_cr = (c == '\r');
            }
        } else {
            if (c == '\n') reclen_end(s, s->cur); else s->cur++;
        }
    }
}

size_t ods2_reclen_longest(const ods2_reclen_t *s)
{
    return s->cur > s->longest ? s->cur : s->longest;
}

size_t ods2_longest_record(uint8_t rtype, const uint8_t *data, size_t len)
{
    ods2_reclen_t st;

    ods2_reclen_init(&st, rtype);
    ods2_reclen_feed(&st, data, len);
    return ods2_reclen_longest(&st);
}

ods2_result_t ods2_create_file_attrs(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, const uint8_t *content,
                                      size_t content_len, const ods2_record_attrs_t *attrs,
                                      ods2_fid_t *new_fid_out)
{
    memory_source_t ctx;
    ods2_content_source_t src = { 0 };           /* every field set: partial_fix_ok was read uninitialized (UBSan, make sancheck) */
    if (ods2_stream_format(attrs->rtype)) {
        /* RMS refuses a stream file read back with a record longer than its MRS or its LRL (%RMS-W-RTB) */
        size_t longest = ods2_longest_record(attrs->rtype, content, content_len);
        if (longest > 32767) return fail("a line is longer than a stream record can be (32767 bytes)");
        if (attrs->maxrec != 0 && longest > attrs->maxrec) {
            return fail("a record is longer than the maximum record size (MRS) given: VMS could not read the file back "
                        "(RMS-W-RTB); use MRS 0");
        }
        if (attrs->rsize != 0 && longest > attrs->rsize) {
            return fail("a record is longer than the longest record length (LRL) given: VMS could not read the file back "
                        "(RMS-W-RTB)");
        }
        if (attrs->rsize == 0) {                  /* no LRL given: the file's own longest record, as VMS stores it */
            ods2_record_attrs_t own = *attrs;
            own.rsize = (uint16_t) longest;
            return ods2_create_file_attrs(vol, parent_header, name, content, content_len, &own, new_fid_out);
        }
    } else if (attrs->rtype == ODS2_RFM_UDF && attrs->rsize == 0 && attrs->maxrec == 0 && content_len != 0) {
        /* UDF given no LRL: the length, as ods2_create_file() and COPY give it */
        ods2_record_attrs_t own = *attrs;
        own.rsize = (uint16_t) (content_len > 32767 ? 32767 : content_len);
        return ods2_create_file_attrs(vol, parent_header, name, content, content_len, &own, new_fid_out);
    }
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
    return ods2_create_file_source_version(vol, parent_header, name, ODS2_NEWVER_REFUSE, src, attrs, new_fid_out, NULL, NULL);
}

ods2_result_t ods2_create_file_source_version(ods2_volume_t *vol, const uint8_t *parent_header,
                                               const char *name, int version_req, ods2_content_source_t *src,
                                               const ods2_record_attrs_t *attrs,
                                               ods2_fid_t *new_fid_out, uint16_t *version_out, char *name_out)
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
    return create_file(vol, parent_header, name, src, attrs, 0, new_fid_out, version_req, version_out, name_out);
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
    if (!vol->ods5 && (core->filechar & 0x2000u) && link_name[0] == '-') {
        /* (a link to a directory is refused just below; this keeps the rule of ods2_valid_dir_name if that is ever allowed) */
        return fail("an ODS-2 directory name cannot start with '-': VAX VMS (V7.3) reads a leading '-' as the parent "
                    "directory and cannot reach files inside (%RMS-F-SYN; ODS-2 has no '^' escape); Alpha V8.4 can. "
                    "Refused because an ODS-2 volume is the one that goes to VAXen");
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
    ods2_content_source_t src = { 0 };           /* every field set: partial_fix_ok was read uninitialized (UBSan, make sancheck) */
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
    return create_file(vol, parent_header, name, &src, &attrs, ODS5_FAT_SYMBOLIC_LINK, new_fid_out, ODS2_NEWVER_REFUSE, NULL, NULL);
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
    if (ods2_stream_format(rtype)) {
        /* A stream file has no maximum record size (MRS 0, as VMS makes it) and its longest record is its own: MRS 512 made
           RMS refuse such a file with a longer line (%RMS-W-RTB, record too large for the user's buffer) */
        size_t longest = ods2_longest_record(rtype, content, content_len);
        if (longest > 32767) return fail("a line is longer than a stream record can be (32767 bytes)");
        attrs.rsize = (uint16_t) longest;
        attrs.maxrec = 0;
    } else if (rtype == ODS2_RFM_UDF) {
        attrs.rsize = (uint16_t) (content_len > 32767 ? 32767 : content_len);
        attrs.maxrec = 0;
    }
    {
        memory_source_t ctx;
        ods2_content_source_t src = { 0 };           /* every field set: partial_fix_ok was read uninitialized (UBSan, make sancheck) */
        ctx.data = content;
        ctx.pos = 0;
        src.length = content_len;
        src.read = memory_source_read;
        src.ctx = &ctx;
        src.problem = NULL;
        src.var_records = false;
        return create_file(vol, parent_header, name, &src, &attrs, 0, new_fid_out, ODS2_NEWVER_REFUSE, NULL, NULL);
    }
}

/* What creating a file keeps for its pieces and its headers, grown as the file needs (a file may take thousands of extents
   and, up to FH2$W_SEG_NUM, 65,535 headers); freed by create_file on every path. */
typedef struct {
    ods2_extent_t *extents;
    size_t         extent_cap;
    int           *seg_first, *seg_count;
    unsigned      *seg_file_number;
    uint16_t      *seg_seq;
    size_t         seg_cap;
} create_scratch_t;

/* room for `count` extents / `count` segments; false when out of memory */
static bool scratch_extent_room(create_scratch_t *sc, size_t count)
{
    if (count > sc->extent_cap) {
        size_t ncap = sc->extent_cap == 0 ? 64 : sc->extent_cap * 2;
        ods2_extent_t *bigger;
        while (ncap < count) ncap *= 2;
        bigger = realloc(sc->extents, ncap * sizeof(*bigger));
        if (bigger == NULL) return false;
        sc->extents = bigger;
        sc->extent_cap = ncap;
    }
    return true;
}

static bool scratch_seg_room(create_scratch_t *sc, size_t count)
{
    if (count > sc->seg_cap) {
        size_t ncap = sc->seg_cap == 0 ? 8 : sc->seg_cap * 2;
        int *a, *b;
        unsigned *c;
        uint16_t *d;
        while (ncap < count) ncap *= 2;
        a = realloc(sc->seg_first, ncap * sizeof(*a));
        if (a == NULL) return false;
        sc->seg_first = a;
        b = realloc(sc->seg_count, ncap * sizeof(*b));
        if (b == NULL) return false;
        sc->seg_count = b;
        c = realloc(sc->seg_file_number, ncap * sizeof(*c));
        if (c == NULL) return false;
        sc->seg_file_number = c;
        d = realloc(sc->seg_seq, ncap * sizeof(*d));
        if (d == NULL) return false;
        sc->seg_seq = d;
        sc->seg_cap = ncap;
    }
    return true;
}

typedef struct {
    bool exists;
    unsigned count;
    uint16_t *versions;
    uint16_t highest, verlimit;
    ods2_fid_t first_fid;               /* the file of the first (highest) entry */
    char stored[ODS5_MAX_LITERAL_NAME + 8];
} dir_name_info_t;
static bool is_reserved_file(const ods2_volume_t *vol, unsigned dir_file_number, const char *name, unsigned file_number);
static ods2_result_t dir_name_info(ods2_volume_t *vol, const uint8_t *dir_header, const char *name, dir_name_info_t *info);
static ods2_result_t insert_new_version(ods2_volume_t *vol, unsigned dir_file_number, const char *name, uint16_t version,
                                        ods2_fid_t fid, uint8_t extra_flags);

/* Which version a create makes (PLAN_WRITE_OPS item 1; CORPUS 49 on VAX V7.3, 49.8 on V8.4). `name` holds the normalized name on entry and,
   if the directory has the name already, the name as the directory stores it (the first stored case wins on ODS-5) on return.
   version_req 0: highest + 1 (1 for a new name); n: that version if it is free, %RMS-E-FEX if it is not. A version above 32767 is
   refused (%RMS-E-CRE, %SYSTEM-W-BADFILEVER). *drop_out is the one old version the entry's version limit makes the create remove, or 0:
   it is NOT removed here. The create makes the new file from space and a header slot that were free before the change began, writes
   it, and only then removes that version, in the same change, as VMS does (the new version gets a new header, the freed slot goes to
   the next create: CORPUS 49.1 C4). Removing it first let the allocator give the new file its clusters, which were written before the
   commit: an interrupted change left a listed version holding the new file's bytes (review 40 F1).
   With the limit reached (the count is the limit or more) exactly ONE version goes, the lowest, however far over the limit the name is: VMS
   makes the new version and deletes one, so the count stays and the excess is never trimmed; SET FILE/VERSION_LIMIT deletes nothing
   (CORPUS 49.9, VAX V7.3 and V8.4). Refused, changing nothing: a version the limit would delete at once (an explicit one below all the
   kept ones: not measured). *prev_out is the file of the highest existing version: the new version takes its owner and protection. */
/* The rules a new version of an existing name follows, shared by COPY (choose_version) and RENAME (review 67 F1 and S2: RENAME had its own copy, without the first two refusals). */

/* A directory, or one of the volume's own files, is not given a new version: its entry stays the first one of the name and the rest would be out of reach (RENAME A.TXT SUB.DIR made SUB.DIR;2) */
static ods2_result_t version_target_ok(ods2_volume_t *vol, unsigned dir_number, const char *name, const dir_name_info_t *info)
{
    uint8_t h[512];
    if (!info->exists) return ok();
    if (ods2_check_fid(info->first_fid).ok && ods2_read_header_fid(vol, info->first_fid, h).ok &&
        ods2_header_is_file(h, info->first_fid) && (((const ods2_head_core_t *) h)->filechar & 0x2000u)) {
        return fail("a directory of this name exists: a file cannot be made a version of it");
    }
    if (is_reserved_file(vol, dir_number, name, ods2_fid_number(info->first_fid))) {
        return fail("this name is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... SECURITY.SYS): it gets no new version");
    }
    return ok();
}

/* The number of the new version: 0 asks for one above the highest (1 for a name that is new); n for that version. *exists_out: the explicit version is there already (the caller words
   the refusal: COPY %RMS-E-FEX, RENAME ...DUPFILENAME). A version above 32767 is refused. */
static ods2_result_t next_version(const dir_name_info_t *info, int version_req, unsigned *v_out, bool *exists_out)
{
    unsigned i;
    *exists_out = false;
    if (version_req == 0) {
        if (!info->exists) { *v_out = 1; return ok(); }
        if (info->highest >= ODS2_MAX_VERSION) {
            return fail("%RMS-E-CRE, ACP file create failed; -SYSTEM-W-BADFILEVER, bad file version number (the highest version is 32767)");
        }
        *v_out = (unsigned) info->highest + 1;
        return ok();
    }
    *v_out = (unsigned) version_req;
    for (i = 0; i < info->count; i++) {
        if (info->versions[i] == *v_out) *exists_out = true;
    }
    return ok();
}

/* The version the name's version limit removes when version `v` is added: none (*drop_out 0) unless the limit is 1 to 32766 and the count would pass it; exactly ONE goes, the lowest, even
   when the name is over its limit (CORPUS 49.9, 66.1). Refused: a version below all the kept ones (it would go at once). */
static ods2_result_t version_limit_drop(const dir_name_info_t *info, unsigned v, unsigned *drop_out)
{
    *drop_out = 0;
    if (info->exists && info->verlimit >= 1 && info->verlimit < ODS2_MAX_VERSION && info->count + 1 > info->verlimit) {
        unsigned lowest = 0, k;
        /* H1 (review 66 F1): the version the limit drops is chosen by NUMBER; on a name stored out of order VMS goes by the stored order (CORPUS 66.4), so a number could be the wrong file: refused,
           even with an explicit version (X stored ;1 ;2 ;3 with limit 3, COPY f X.TXT;5 deleted ;1 silently). The one place for COPY and RENAME (review 67 S2). */
        for (k = 1; k < info->count; k++) {
            if (info->versions[k] >= info->versions[k - 1]) {
                return fail("the versions of this name are stored out of order (BAD_VERSORDER) and its version limit would delete one: no new version is made");
            }
        }
        for (k = 0; k < info->count; k++) if (lowest == 0 || info->versions[k] < lowest) lowest = info->versions[k];
        if (v < lowest) return fail("the version limit of the name would delete this version at once (it is below all the versions kept): not made");
        *drop_out = lowest;
    }
    return ok();
}

static ods2_result_t choose_version(ods2_volume_t *vol, const uint8_t *parent_header, char *name, size_t name_cap, int version_req,
                                    unsigned special, unsigned *version_out, bool *existing_out, unsigned *drop_out, ods2_fid_t *prev_out)
{
    dir_name_info_t info;
    unsigned dir_number = ods2_fid_number(((const ods2_head_core_t *) parent_header)->fid), v;
    ods2_result_t r;

    (void) special;
    *existing_out = false;
    *drop_out = 0;
    if (version_req < 0 || version_req > ODS2_MAX_VERSION) {
        return fail("%RMS-E-CRE, ACP file create failed; -SYSTEM-W-BADFILEVER, bad file version number");
    }
    r = dir_name_info(vol, parent_header, name, &info);
    if (!r.ok) return r;
    if (!info.exists) {
        *version_out = version_req == 0 ? 1u : (unsigned) version_req;
        free(info.versions);
        return ok();
    }
    if (strlen(info.stored) >= name_cap) { free(info.versions); return fail("file name too long"); }
    memcpy(name, info.stored, strlen(info.stored) + 1);
    *existing_out = true;
    *prev_out = info.first_fid;
    /* a directory, or one of the volume's own files, is not given a new version (COPY used to refuse the name altogether) */
    r = version_target_ok(vol, dir_number, name, &info);
    if (!r.ok) { free(info.versions); return r; }
    if (version_req == 0) {
        /* H1 (CORPUS 66.4): a name whose versions are stored out of order gets no new version by "highest + 1" (VMS makes a second ;2 beside the stored ;1 ;2 ;3; here the highest number
           and the lowest number that a limit would drop would be taken by number, against the stored order that VMS follows): refused, changing nothing; ;n names the version */
        unsigned k;
        for (k = 1; k < info.count; k++) {
            if (info.versions[k] >= info.versions[k - 1]) {
                free(info.versions);
                return fail("the versions of this name are stored out of order (BAD_VERSORDER): no new version is made; name a version (;n)");
            }
        }
    }
    {
        bool exists;
        r = next_version(&info, version_req, &v, &exists);
        if (!r.ok) { free(info.versions); return r; }
        if (exists) {
            static char msg[ODS5_MAX_LITERAL_NAME + 96];     /* static: the message outlives this call */
            free(info.versions);
            snprintf(msg, sizeof(msg), "%%RMS-E-FEX, file already exists, not superseded: %s;%u", name, v);
            return fail(msg);
        }
    }
    /* the version limit of the entry: 0 and 32767 are no limit */
    r = version_limit_drop(&info, v, drop_out);
    if (!r.ok) { free(info.versions); return r; }
    free(info.versions);
    *version_out = v;
    return ok();
}

static ods2_result_t create_file_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, ods2_content_source_t *src,
                                      const ods2_record_attrs_t *attrs, unsigned special,
                                      ods2_fid_t *new_fid_out, create_scratch_t *sc,
                                      int version_req, uint16_t *version_out, char *name_out);

static ods2_result_t create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                 const char *name, ods2_content_source_t *src,
                                 const ods2_record_attrs_t *attrs, unsigned special,
                                 ods2_fid_t *new_fid_out, int version_req, uint16_t *version_out, char *name_out)
{
    create_scratch_t sc = { NULL, 0, NULL, NULL, NULL, NULL, 0 };
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = create_file_impl(vol, parent_header, name, src, attrs, special, new_fid_out, &sc, version_req, version_out, name_out);
    free(sc.extents); free(sc.seg_first); free(sc.seg_count); free(sc.seg_file_number); free(sc.seg_seq);
    return txn_end(vol, r);
}

static ods2_result_t create_file_impl(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, ods2_content_source_t *src,
                                      const ods2_record_attrs_t *attrs, unsigned special,
                                      ods2_fid_t *new_fid_out, create_scratch_t *sc,
                                      int version_req, uint16_t *version_out, char *name_out)
{
    unsigned new_version = 1;
    bool version_of_existing = false;
    unsigned drop_version = 0;
    ods2_fid_t prev_fid;
    bool have_prev = false;
    uint64_t content_len = src->length;
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    uint64_t blocks_needed;
    size_t blocks_remaining;
    int extent_count = 0;
    size_t total_allocated = 0;
    unsigned max_request;
    int segments = 0, numbered = 0, pos, seg;
    int lowest_written = 0x7fffffff; /* headers seg.. on disk */
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
    if (version_req == ODS2_NEWVER_REFUSE) {
        r = name_available(vol, parent_header, name,
                           "a file or directory with this name already exists in the parent");
        if (!r.ok) return r;
    } else {
        r = choose_version(vol, parent_header, upper_name, sizeof(upper_name), version_req, special, &new_version, &version_of_existing, &drop_version, &prev_fid);
        have_prev = version_of_existing;
        if (!r.ok) return r;
        name = upper_name;       /* the stored case of an existing name, as the directory spells it */
    }

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
    if (!scratch_seg_room(sc, 1)) return fail("out of memory");
    r = allocate_file_number(vol, &sc->seg_file_number[0], &sc->seg_seq[0]);
    if (!r.ok) return r;
    numbered = 1;

    max_request = (1u << 30) - ((1u << 30) % vol->home.cluster);
    blocks_remaining = blocks_needed;
    while (blocks_remaining > 0) {
        uint32_t chunk_lbn;
        unsigned chunk_allocated;
        unsigned chunk_request = (blocks_remaining < max_request)
                                     ? (unsigned) blocks_remaining : max_request;

        if (!scratch_extent_room(sc, (size_t) extent_count + 1)) {
            r = fail("out of memory allocating the file's pieces");
            goto undo;
        }
        r = ods2_allocate_extent(vol, chunk_request, &chunk_lbn, &chunk_allocated);
        if (!r.ok) {
            r = fail("not enough free space on the volume for this file");
            goto undo;
        }
        sc->extents[extent_count].lbn = chunk_lbn;
        sc->extents[extent_count].block_count = chunk_allocated;
        extent_count++;
        total_allocated += chunk_allocated;
        blocks_remaining = (chunk_allocated >= blocks_remaining) ? 0
                                                                 : blocks_remaining - chunk_allocated;
    }

    /* Spread the retrieval pointers over as many headers as they need:
       the primary header holds what fits in its map area, the rest go
       in extension headers chained through FH2$W_EXT_FID (spec 3.3,
       3.5.2.5, 3.5.2.8). */
    snprintf(ident, sizeof(ident), "%s;%u", name, new_version);
    memset(&spec, 0, sizeof(spec));
    spec.level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
    spec.ident_name = ident;
    for (pos = 0; pos < extent_count; ) {
        int fit;
        spec.seg_num = (uint16_t) segments; /* where the Map Area starts depends on it at level 5 */
        fit = ods2_header_extents_that_fit_at(sc->extents + pos, extent_count - pos,
                                              ods2_header_map_offset(&spec));
        if (fit <= 0) {
            r = fail("internal error: an allocated extent cannot be described by "
                     "any retrieval pointer format (should be unreachable)");
            goto undo;
        }
        if (segments >= ODS2_SEGMENT_LIMIT) {
            r = fail("the volume's free space is too fragmented to hold this file "
                     "(it would need more than 65,535 file headers)");
            goto undo;
        }
        if (!scratch_seg_room(sc, (size_t) segments + 1)) {
            r = fail("out of memory planning the file's headers");
            goto undo;
        }
        sc->seg_first[segments] = pos;
        sc->seg_count[segments] = fit;
        segments++;
        pos += fit;
    }

    /* A file number - and its header slot inside INDEXF.SYS's
       end-of-file, its bit marked in use - for every extension header
       too. */
    while (numbered < segments) {
        r = allocate_file_number(vol, &sc->seg_file_number[numbered], &sc->seg_seq[numbered]);
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
    if (have_prev) {
        /* CORPUS 49.9 (VAX V7.3, V8.4 ODS-2 and ODS-5): the protection of a new version is that of the highest version before it, whatever
           the process default or the directory's default-protection ACE (which applies to a new name only); this process acts as a privileged
           one, whose new version keeps the previous version's owner too (an unprivileged VMS user who is not the volume owner would get
           their own UIC). No %RMS-E-PRV refusal: a privileged process is not refused. */
        uint8_t ph[512];
        if (ods2_check_fid(prev_fid).ok && ods2_read_header_fid(vol, prev_fid, ph).ok && ods2_header_is_file(ph, prev_fid)) {
            const ods2_head_core_t *pc = (const ods2_head_core_t *) ph;
            spec.use_owner_prot = true;
            spec.owner_uic[0] = pc->fileowner_uic[0];
            spec.owner_uic[1] = pc->fileowner_uic[1];
            spec.fileprot = pc->fileprot;
        }
    }
    /* Records the whole file's size in every header, as the record
       attributes are copied into extension headers too. */
    spec.hiblk = (uint32_t) total_allocated;
    /* every allocated block is written (zero-filled past the data, write_extents), and the stored HIGHWATER is HIBLK + 1, as the files VMS's DCL makes have it
       at both levels (follow-up b; CORPUS 49 to 56: all 2,644 user files of the VMS-made ODS-2 volumes; DUMP/HEADER prints the stored value minus 1). ods2v2 wrote 0 at
       level 2 before. */
    spec.highwater = (uint32_t) total_allocated + 1;
    /* End-of-file mark per spec 6.1.5/6.1.6, in the preferred form
       VMS itself writes: EFBLK is the VBN holding the EOF position,
       FFBYTE the bytes in use there. Content ending exactly on a
       block boundary (including an empty file) gets EFBLK = blocks+1
       and FFBYTE = 0. (An earlier version wrote EFBLK = blocks and
       FFBYTE = 0 for that case, which VMS reads as one block SHORTER
       than the real content, and an empty file as 512 bytes.) */
    spec.efblk = (uint32_t) (content_len / 512) + 1;
    spec.ffbyte = (uint16_t) (content_len % 512);
    /* No characteristics on a file at either level: the files VMS makes (DCL, COPY of a file that does not have it) never carry FH2$M_CONTIG, even those in one piece
       (the VMS-made ODS-2 volumes; CORPUS 56.4: a CONTIG file makes VMS's COPY and BACKUP fail or warn (%COPY-W-CBT, %BACKUP-E-NONCONTIG) where free space is
       fragmented, and APPEND clears the flag). ods2v2 set it on a file of one piece at level 2 before (follow-up b). */
    spec.filechar = 0;
    snprintf(ident, sizeof(ident), "%s;%u", name, new_version);
    spec.ident_name = ident;

    /* Extension headers first, last to first, then the primary, so
       every header written only ever points at headers already on
       disk. Each extension header has its own FID, its segment
       number, and a back link to the primary header (spec 3.5.2.16). */
    {
    /* The record attributes of an extension header (follow-up d item 6; CORPUS 61: ods5-vms3's COPY into mixed-size holes, V8.4 and VAX V7.3, ODS-2 and ODS-5, byte-identical, and the
       APPEND that grows only the last extension header): the primary's RTYPE, RATTRIB, RSIZE and MAXREC, and HIBLK 0, EFBLK 1, FFBYTE 0, stored HIGHWATER 1; the primary header alone
       holds the file's size. (The extension headers of a file VMS GREW by one-block extends hold RSIZE 0 and the blocks the primary maps as HIBLK: ods2v2 makes its headers in one go,
       as COPY does.) ods2v2 wrote the whole file's size in every header before. */
    const uint32_t file_hiblk = spec.hiblk, file_efblk = spec.efblk, file_highwater = spec.highwater;
    const uint16_t file_ffbyte = spec.ffbyte;
    for (seg = segments - 1; seg >= 0; seg--) {
        ods2_fid_set_number(&spec.fid, sc->seg_file_number[seg]);
        spec.fid.fid_seq = sc->seg_seq[seg];
        spec.seg_num = (uint16_t) seg;
        spec.hiblk = seg != 0 ? 0 : file_hiblk;
        spec.efblk = seg != 0 ? 1 : file_efblk;
        spec.ffbyte = seg != 0 ? 0 : file_ffbyte;
        spec.highwater = seg != 0 ? 1 : file_highwater;
        memset(&spec.ext_fid, 0, sizeof(spec.ext_fid));
        if (seg + 1 < segments) {
            ods2_fid_set_number(&spec.ext_fid, sc->seg_file_number[seg + 1]);
            spec.ext_fid.fid_seq = sc->seg_seq[seg + 1];
        }
        memset(&spec.backlink, 0, sizeof(spec.backlink));
        if (seg == 0) {
            spec.backlink = parent_core->fid;
        } else {
            ods2_fid_set_number(&spec.backlink, sc->seg_file_number[0]);
            spec.backlink.fid_seq = sc->seg_seq[0];
        }
        spec.extents = sc->extents + sc->seg_first[seg];
        spec.extent_count = sc->seg_count[seg];
        if (!ods2_build_file_header(new_header, &spec)) {
            r = fail("could not construct new file's header");
            goto undo;
        }
        r = ods2_write_header(vol, sc->seg_file_number[seg], new_header);
        if (!r.ok) goto undo;
        lowest_written = seg;
    }
    }
    /* spec.fid is now the primary header's, for the directory entry. */

    /* Insert an entry for the new file into the parent - growing the
       parent's own allocation automatically if none of its existing
       content blocks have room. */
    if (version_of_existing) {
        r = insert_new_version(vol, ods2_fid_number(parent_core->fid), name, (uint16_t) new_version, spec.fid,
                               special != 0 ? ODS5_DIR_SPECIAL : 0);
    } else {
        r = insert_into_directory(vol, ods2_fid_number(parent_core->fid), name, (uint16_t) new_version, spec.fid,
                                  special != 0 ? ODS5_DIR_SPECIAL : 0);
    }
    if (!r.ok) goto undo;   /* no directory entry: give the headers, blocks and file numbers back */

    /* The content goes to the free clusters last, when nothing else can fail: the structure (bitmaps, headers,
       entry) is still only in memory, and reaches the image after this, in the order the crash-safety rules
       need - so the content is on the disk before any header refers to it, and a refused copy has not
       written a byte. */
    r = write_extents(vol, sc->extents, extent_count, src);
    if (!r.ok) goto undo;

    /* the version the limit removes goes last, in this same change (the new file has its own clusters and header slot, taken before this) */
    if (drop_version != 0) {
        r = ods2_delete_version(vol, ods2_fid_number(parent_core->fid), name, (int) drop_version, NULL);
        if (!r.ok) goto undo;
    }

    *new_fid_out = spec.fid;
    if (version_out != NULL) *version_out = (uint16_t) new_version;
    if (name_out != NULL) snprintf(name_out, ODS5_MAX_LITERAL_NAME + 8, "%s", name);
    return ok();

undo:
    /* Give back what was taken before the failure, so a refused or
       failed copy leaves no blocks or file numbers allocated to
       nothing. Any header already written is marked deleted (as
       ods2_delete_version() does), since a slot holding a valid header
       is never reused (spec 5.1.7). */
    for (seg = lowest_written; seg < segments; seg++) {
        memset(new_header, 0, sizeof(new_header));
        if (ods2_read_header(vol, sc->seg_file_number[seg], new_header).ok) {
            ods2_head_core_t *core = (ods2_head_core_t *) new_header;
            core->filechar |= 0x8000u; /* FH2$M_MARKDEL */
            core->fid.fid_num = 0;
            core->fid.fid_nmx = 0;
            core->fid.fid_rvn = 0;
            new_header[510] = 0;
            new_header[511] = 0;
            ods2_write_header(vol, sc->seg_file_number[seg], new_header);
        }
    }
    for (pos = 0; pos < extent_count; pos++) {
        ods2_free_blocks(vol, sc->extents[pos].lbn, sc->extents[pos].block_count);
    }
    for (seg = 0; seg < numbered; seg++) {
        mark_index_bitmap(vol, sc->seg_file_number[seg], false);
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
    unsigned drop_from = 0, drop_to = 0;       /* the blocks a shrink drops (cleared after the end of file has moved back) */
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
        /* the blocks no longer in use are cleared AFTER the header moves the end of file back (below): cleared first, an interruption left the last block
           inside the end of file all zeros (review 48 N10). Until the header is written they hold copies of names the blocks before them hold too (or, when no
           block moved, the names of the entries that are gone: still listed once, in the block that is still in use). */
        drop_from = used;
        drop_to = old_used;
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

    for (v = drop_from; v < drop_to; v++) {      /* blocks no longer in use: past the end of file now */
        r = ods2_write_file_block(vol, dir_header, v + 1, zero);
        if (!r.ok) return r;
    }

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

/* What the directory held in `blocks` has of `name`: *dup says it has `version` already, *lowest is the lowest version of the name
   there (0 when it has none). A directory of ODS2_DIR_BSEARCH_MIN blocks or more is searched by halving, which finds nothing in
   one that is not in the order the search expects (damage, another tool): a lookup that missed would let a create add a second
   entry of the name (review 27 N5). The insert holds the whole directory already, so this costs one more pass over it. */
static void loaded_directory_name_state(const ods2_volume_t *vol, const uint8_t *blocks, unsigned used, const char *name,
                                        uint16_t version, bool *dup, unsigned *lowest)
{
    unsigned b;
    *dup = false;
    *lowest = 0;
    for (b = 0; b < used; b++) {
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        unsigned bad = 0;
        int n = ods2_parse_directory_records(blocks + (size_t) b * 512, 512, vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2, e,
                                             ODS2_DIR_MAX_ENTRIES_PER_BLOCK, &bad), i;
        for (i = 0; i < n && i < ODS2_DIR_MAX_ENTRIES_PER_BLOCK; i++) {
            if (ods2_dir_name_compare(e[i].name, strlen(e[i].name), name, strlen(name)) != 0) continue;
            if (e[i].version == version) *dup = true;
            if (*lowest == 0 || e[i].version < *lowest) *lowest = e[i].version;
        }
    }
}

static ods2_result_t insert_into_directory_vl(ods2_volume_t *vol, unsigned dir_file_number,
                                              const char *name, uint16_t version, ods2_fid_t fid,
                                              uint8_t extra_flags, uint16_t verlimit_override);

static ods2_result_t insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                           const char *name, uint16_t version, ods2_fid_t fid,
                                           uint8_t extra_flags)
{
    return insert_into_directory_vl(vol, dir_file_number, name, version, fid, extra_flags, 0);
}

static ods2_result_t insert_into_directory_vl(ods2_volume_t *vol, unsigned dir_file_number,
                                              const char *name, uint16_t version, ods2_fid_t fid,
                                              uint8_t extra_flags, uint16_t verlimit_override)
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

    {
        bool dup;
        unsigned lowest;
        loaded_directory_name_state(vol, blocks, used, name, version, &dup, &lowest);
        if (dup && used >= ODS2_DIR_BSEARCH_MIN) {
            free(blocks);
            return fail("a directory entry with this name and version exists already: not added (a lookup did not find it: is "
                        "the directory out of order?)");
        }
        /* The record goes after the records of its name, so it is stored last: that is newest first only when its version
           is lower than every version there (review 28 b). A directory with the versions the other way round makes VMS and
           this program's own lookup of the highest version read the wrong file. */
        if (lowest != 0 && version >= lowest) {
            free(blocks);
            return fail("a version of this name that is not lower than every version there would be stored out of order "
                        "(versions are kept newest first): not added");
        }
    }
    t = ods2_dir_choose_block(blocks, used, name);
    verlimit = verlimit_override != 0 ? verlimit_override : ((const ods2_head_core_t *) dir_header)->recattr.versions;   /* the directory's default version limit */
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

/* ---- versions of a name that already has entries: COPY to a new version (PLAN_WRITE_OPS item 1) ---- */

/* What a directory holds of one name: its versions newest first (as stored), the version limit of its first record, and the name as the
   first record stores it (on an ODS-5 volume the first stored case wins: Abc.txt stays Abc.txt when ABC.TXT is created, CORPUS 49.8 C9).
   `versions` is allocated here; free it. */
static ods2_result_t dir_name_info(ods2_volume_t *vol, const uint8_t *dir_header, const char *name, dir_name_info_t *info)
{
    uint8_t *blocks = NULL;
    unsigned used, b;
    size_t cap = 0;
    ods2_result_t r;

    memset(info, 0, sizeof(*info));
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    if (!r.ok) return r;
    for (b = 0; b < used; b++) {
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        unsigned bad = 0;
        int n = ods2_parse_directory_records(blocks + (size_t) b * 512, 512, vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2, e,
                                             ODS2_DIR_MAX_ENTRIES_PER_BLOCK, &bad), i;
        for (i = 0; i < n && i < ODS2_DIR_MAX_ENTRIES_PER_BLOCK; i++) {
            if (ods2_dir_name_compare(e[i].name, strlen(e[i].name), name, strlen(name)) != 0) continue;
            if (info->count == cap) {
                uint16_t *nv = realloc(info->versions, (cap ? cap * 2 : 16) * sizeof(*nv));
                if (nv == NULL) {
                    free(info->versions);
                    free(blocks);
                    memset(info, 0, sizeof(*info));
                    return fail("out of memory");
                }
                info->versions = nv;
                cap = cap ? cap * 2 : 16;
            }
            if (!info->exists) {
                info->exists = true;
                info->verlimit = e[i].verlimit;
                info->first_fid = e[i].fid;
                snprintf(info->stored, sizeof(info->stored), "%s", e[i].name);
            }
            info->versions[info->count++] = e[i].version;
            if (e[i].version > info->highest) info->highest = e[i].version;
        }
    }
    free(blocks);
    return ok();
}

/* Adds version `version` of a name that has records already, in the place the order wants: in front of the first record when it is the
   highest (the usual COPY: a pair at the front of the existing record, as VMS does, CORPUS 49 C1), between or after the others otherwise;
   a block with no room is split. `name` is the name as stored. Fails, changing nothing, when the version exists. */
static ods2_result_t insert_new_version(ods2_volume_t *vol, unsigned dir_file_number, const char *name, uint16_t version,
                                        ods2_fid_t fid, uint8_t extra_flags)
{
    uint8_t dir_header[512];
    uint8_t *blocks = NULL;
    unsigned used, b, level = vol->ods5 ? ODS5_LEVEL_5 : ODS5_LEVEL_2;
    ods2_dir_namerec_t *recs = NULL;
    unsigned *rblock = NULL;
    size_t nrec = 0, cap = 0, j;
    size_t target = (size_t) -1;
    unsigned idx = 0, tb, t;
    ods2_result_t r;
    int rc;

    if (!vol->writable) return not_writable(vol);
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_header_ok(dir_header, dir_file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 1, &blocks, &used);
    if (!r.ok) return r;
    for (b = 0; b < used; b++) {
        ods2_dir_namerec_t tmp[256];
        int n = ods2_dir_name_records(blocks + (size_t) b * 512, 512, name, tmp, 256), i;
        if (n < 0) { free(blocks); free(recs); free(rblock); return fail("a directory block is damaged: not changed"); }
        for (i = 0; i < n && i < 256; i++) {
            if (nrec == cap) {
                ods2_dir_namerec_t *nr = realloc(recs, (cap ? cap * 2 : 8) * sizeof(*nr));
                unsigned *nb;
                if (nr == NULL) { free(blocks); free(recs); free(rblock); return fail("out of memory"); }
                recs = nr;
                nb = realloc(rblock, (cap ? cap * 2 : 8) * sizeof(*nb));
                if (nb == NULL) { free(blocks); free(recs); free(rblock); return fail("out of memory"); }
                rblock = nb;
                cap = cap ? cap * 2 : 8;
            }
            recs[nrec] = tmp[i];
            rblock[nrec] = b;
            nrec++;
        }
    }
    if (nrec == 0) { free(blocks); free(recs); free(rblock); return fail("internal error: a new version of a name the directory does not hold"); }

    for (j = 0; j < nrec; j++) {
        if (version == recs[j].first || version == recs[j].last) {
            free(blocks); free(recs); free(rblock);
            return fail("a directory entry with this name and version exists already: not added");
        }
        if (version > recs[j].first) {
            if (j == 0) { target = 0; idx = 0; }                      /* the new highest: in front of the first record */
            else { target = j - 1; idx = recs[j - 1].nentries; }       /* between two records: at the end of the one above */
            break;
        }
        if (version > recs[j].last) {                                  /* inside this record: after the pairs above it */
            const uint8_t *rb = blocks + (size_t) rblock[j] * 512 + recs[j].first_pair;
            unsigned k;
            target = j;
            idx = 0;
            for (k = 0; k < recs[j].nentries; k++) {
                uint16_t v = (uint16_t) (rb[k * 8] | (rb[k * 8 + 1] << 8));
                if (v == version) { free(blocks); free(recs); free(rblock); return fail("a directory entry with this name and version exists already: not added"); }
                if (v > version) idx = k + 1;
            }
            break;
        }
    }
    if (target == (size_t) -1) { target = nrec - 1; idx = recs[nrec - 1].nentries; }   /* below all: at the end of the last record */
    tb = rblock[target];
    t = tb;

    rc = ods2_dir_record_add_version(blocks + (size_t) tb * 512, 512, recs[target].pos, idx, version, fid);
    if (rc == -2) { free(blocks); free(recs); free(rblock); return fail("a directory block is damaged: not changed"); }
    if (rc == -1) {
        /* the record is full (510 bytes): a record of its own, before it or after it; not into the middle of it */
        size_t pos;
        if (idx == 0) pos = recs[target].pos;
        else if (idx == recs[target].nentries) pos = recs[target].pos + recs[target].total;
        else { free(blocks); free(recs); free(rblock);
               return fail("the record that holds the neighbouring versions is full and this version belongs inside it: not added "
                           "(a full record is not split)"); }
        if (ods2_dir_insert_record_at(blocks + (size_t) tb * 512, 512, pos, name, version, fid, level, extra_flags, recs[target].verlimit)) {
            rc = 1;
        } else {
            uint8_t combined[1024];
            memcpy(combined, blocks + (size_t) tb * 512, 512);
            memset(combined + 512, 0, 512);
            if (!ods2_dir_insert_record_at(combined, sizeof(combined), pos, name, version, fid, level, extra_flags, recs[target].verlimit)) {
                free(blocks); free(recs); free(rblock);
                return fail("internal error: a block plus one record overflowed 1024 bytes (should be unreachable)");
            }
            rc = 2;
            memmove(blocks + (size_t) (tb + 2) * 512, blocks + (size_t) (tb + 1) * 512, (size_t) (used - tb - 1) * 512);
            if (!ods2_dir_split_block(combined, sizeof(combined), blocks + (size_t) tb * 512, blocks + (size_t) (tb + 1) * 512)) {
                free(blocks); free(recs); free(rblock);
                return fail("internal error: could not split a full directory block (should be unreachable)");
            }
        }
    } else if (rc == 0) {
        uint8_t combined[1024];
        memcpy(combined, blocks + (size_t) tb * 512, 512);
        memset(combined + 512, 0, 512);
        if (ods2_dir_record_add_version(combined, sizeof(combined), recs[target].pos, idx, version, fid) != 1) {
            free(blocks); free(recs); free(rblock);
            return fail("internal error: a block plus one pair overflowed 1024 bytes (should be unreachable)");
        }
        rc = 2;
        memmove(blocks + (size_t) (tb + 2) * 512, blocks + (size_t) (tb + 1) * 512, (size_t) (used - tb - 1) * 512);
        if (!ods2_dir_split_block(combined, sizeof(combined), blocks + (size_t) tb * 512, blocks + (size_t) (tb + 1) * 512)) {
            free(blocks); free(recs); free(rblock);
            return fail("internal error: could not split a full directory block (should be unreachable)");
        }
    }
    free(recs);
    free(rblock);
    if (rc == 2) {
        r = store_directory(vol, dir_file_number, dir_header, blocks, used + 1, used, t);
    } else if (ods2_file_content_length(dir_header) == 0 || !dir_eof_standard(dir_header, used) || !below_highwater(dir_header, t + 1)) {
        r = store_directory(vol, dir_file_number, dir_header, blocks, used, used, t);
    } else {
        r = ods2_write_file_block(vol, dir_header, t + 1, blocks + (size_t) t * 512);
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

/* SET FILE/ATTRIBUTES=(MRS:0): repairs a stream file made with a maximum record size (ods2v2 gave every explicit-format
   stream file MRS 512 whatever its lines, and RMS refuses to read such a file with a longer line: %RMS-W-RTB). MRS becomes 0
   and LRL the file's own longest record, as VMS makes such files; the file's data is not touched. Only a sound header of a
   sequential Stream, Stream_LF or Stream_CR file is changed, and only if it is not already as VMS leaves such a file: MRS 0
   and an LRL of at least the longest record, the record taken as at most 32767 bytes (a binary file with a longer "record" is
   left by VMS with MRS 0 and LRL 32767, and is skipped silently in that state; with a non-zero MRS it gets MRS 0 and LRL
   32767). *changed is 1 if the header was written. */
static ods2_result_t set_stream_record_sizes_impl(ods2_volume_t *vol, ods2_fid_t fid, unsigned *changed)
{
    uint8_t h[512], blk[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint64_t bytes, done = 0;
    unsigned vbn, rfm;
    size_t longest;
    ods2_reclen_t st;
    uint16_t sum;
    ods2_result_t r;

    *changed = 0;
    if (!vol->writable) return not_writable(vol);
    r = ods2_read_header_fid(vol, fid, h);
    if (!r.ok) return r;
    r = header_ok_for_write(h, ods2_fid_number(fid));
    if (!r.ok) return r;
    rfm = core->recattr.rtype & 0x0Fu;
    if ((core->recattr.rtype >> 4) != 0 || (rfm != ODS2_RFM_STM && rfm != ODS2_RFM_STMLF && rfm != ODS2_RFM_STMCR) ||
        (core->filechar & 0x2000u)) {
        return ok();
    }
    bytes = ((uint64_t) ods2_word_swap32(core->recattr.efblk) - 1) * 512u + core->recattr.ffbyte;
    if (ods2_word_swap32(core->recattr.efblk) == 0) bytes = 0;
    ods2_reclen_init(&st, (uint8_t) rfm);
    for (vbn = 1; done < bytes; vbn++) {
        size_t n = (bytes - done) < 512u ? (size_t) (bytes - done) : 512u;
        r = ods2_read_file_block(vol, h, vbn, blk);
        if (!r.ok) return r;
        ods2_reclen_feed(&st, blk, n);
        done += n;
    }
    longest = ods2_reclen_longest(&st);
    if (longest > 32767) longest = 32767;        /* no record length can be more: VMS's state for such a binary file */
    /* needs repair: a maximum record size (RMS reads with a buffer of that size), or a longest record length (LRL) below
       the longest record (COPY and CHECKSUM then fail too, even with MRS 0: %RMS-W-RTB) */
    if (core->recattr.maxrec == 0 && core->recattr.rsize >= longest) return ok();
    core->recattr.maxrec = 0;
    core->recattr.rsize = (uint16_t) longest;
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    r = ods2_write_header(vol, ods2_fid_number(fid), h);
    if (r.ok) *changed = 1;
    return r;
}

/* SET FILE/PROTECTION and /OWNER_UIC (PLAN_WRITE_OPS item 4; CORPUS 49.4, 49.7, 49.8 on VAX V7.3 and V8.4 ODS-2 and ODS-5): the primary header is
   changed, in one block write, and an owner change also goes into every extension header (CORPUS 56.2; see set_file_security_impl). FH2$W_FILEPROT and FH2$L_FILEOWNER (member, group) are the named fields only. Every successful change adds 1 to the
   revision number, even when the value is the one the file had; the date that moves is the revision date in a level 2 header (also the reserved files'
   on an ODS-5 volume) and the attribute change date in a level 5 header (its revision date, the data modification date, stays); the access date
   is not touched. The header's checksum is made again. The reserved files take a protection change (measured: INDEXF.SYS, BITMAP.SYS and the master
   directory accepted it, the revision number going up by 1); an owner change on one is refused (measured only for INDEXF.SYS, where VMS refuses it
   because the file is open). */
/* A header that was revised (SET FILE, RENAME): the revision number +1 and the date the level sets: ODS-2 the revision date, ODS-5 the last attribute change date (the ODS-5 revision date
   stays: CORPUS 65.1). The caller recomputes the checksum. */
static void header_revise(uint8_t *h)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    uint8_t *ident = h + (size_t) core->idoffset * 2;
    uint16_t revision;
    if ((core->struclev >> 8) == ODS5_LEVEL_5) {
        revision = (uint16_t) (ident[ODS5_FI5_REVISION] | (ident[ODS5_FI5_REVISION + 1] << 8));
        revision = (uint16_t) (revision + 1u);
        ident[ODS5_FI5_REVISION] = (uint8_t) (revision & 0xff);
        ident[ODS5_FI5_REVISION + 1] = (uint8_t) (revision >> 8);
        ods2_store_vms_time(ident + ODS5_FI5_ATTDATE, ods2_vms_time_now());
    } else {
        revision = (uint16_t) (ident[20] | (ident[21] << 8));
        revision = (uint16_t) (revision + 1u);
        ident[20] = (uint8_t) (revision & 0xff);
        ident[21] = (uint8_t) (revision >> 8);
        ods2_store_vms_time(ident + 30, ods2_vms_time_now());
    }
}

static ods2_result_t set_file_security_impl(ods2_volume_t *vol, ods2_fid_t fid, unsigned what, uint16_t prot, uint16_t group, uint16_t member)
{
    uint8_t h[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint16_t sum;
    unsigned number = ods2_fid_number(fid);
    ods2_result_t r;

    if (!vol->writable) return not_writable(vol);
    r = ods2_check_fid(fid);
    if (!r.ok) return r;
    r = ods2_read_header_fid(vol, fid, h);
    if (!r.ok) return r;
    r = header_ok_for_write(h, number);
    if (!r.ok) return r;
    if (!ods2_header_is_file(h, fid)) return fail("the entry's file no longer exists (a dangling entry): nothing is changed");
    if ((what & ODS2_SETSEC_OWNER) && number >= 1 && number <= 10 && fid.fid_nmx == 0) {
        return fail("the owner of a reserved file (INDEXF.SYS, BITMAP.SYS, ... SECURITY.SYS) is not changed: not measured on VMS except for "
                    "INDEXF.SYS, where it is refused (the file is open)");
    }
    if (what & ODS2_SETSEC_OWNER) {
        /* CORPUS 56.2 (OpenVMS Alpha V8.4, ODS-2 and ODS-5): VMS writes the new owner into EVERY extension header of the file too, and leaves their FILEPROT alone
           (a protection change touches the primary header only). Each is checked as the next segment of this file (number, sequence, segment number, back link,
           checksum) before it is changed, and a chain that cannot be followed refuses the whole change: nothing is written (the writes are held in memory until the
           change ends). The extension headers go first and the primary header, written last, is the one the owner is read from: an interrupted change shows the old
           owner or the new one in the primary header, and an extension header never lags behind a primary header that shows the new owner. Their revision and
           dates are not touched (not measured). */
        ods2_fid_t link = core->ext_fid;
        unsigned segment;
        for (segment = 1; ods2_fid_number(link) != 0; segment++) {
            uint8_t x[512];
            ods2_head_core_t *xcore = (ods2_head_core_t *) x;
            unsigned xnum = ods2_fid_number(link);
            uint16_t xsum;
            if (segment >= ODS2_SEGMENT_LIMIT) return fail("the extension header chain does not end: nothing is changed");
            r = ods2_read_header(vol, xnum, x);
            if (!r.ok) return r;
            {
                const char *why = extension_header_why(x, link, number, fid.fid_seq, segment);
                if (why != NULL) {
                    static char msg[300];
                    snprintf(msg, sizeof(msg), "%s: damaged, nothing is changed", why);          /* nothing is written: the change is only in memory until it ends */
                    return fail(msg);
                }
            }
            xcore->fileowner_uic[0] = member;
            xcore->fileowner_uic[1] = group;
            xsum = ods2_checksum(x, 255);
            x[510] = (uint8_t) (xsum & 0xff);
            x[511] = (uint8_t) (xsum >> 8);
            r = ods2_write_header(vol, xnum, x);
            if (!r.ok) return r;
            link = xcore->ext_fid;
        }
    }
    if (what & ODS2_SETSEC_PROT) core->fileprot = prot;
    if (what & ODS2_SETSEC_OWNER) {
        core->fileowner_uic[0] = member;
        core->fileowner_uic[1] = group;
    }
    header_revise(h);
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    if (number == 1 && fid.fid_nmx == 0) return write_indexf_header(vol, h);
    return ods2_write_header(vol, number, h);
}

ods2_result_t ods2_set_file_security(ods2_volume_t *vol, ods2_fid_t fid, unsigned what, uint16_t prot, uint16_t group, uint16_t member)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = set_file_security_impl(vol, fid, what, prot, group, member);
    return txn_end(vol, r);
}

ods2_result_t ods2_set_stream_record_sizes(ods2_volume_t *vol, ods2_fid_t fid, unsigned *changed)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = set_stream_record_sizes_impl(vol, fid, changed);
    return txn_end(vol, r);
}

/* SET VOLUME/PARTIAL_CLUSTER: gives the blocks past the volume's last whole cluster to BADBLK.SYS, as VMS INITIALIZE
   does (ods2_init does since this was found: V8.4 ANALYZE called that cluster, allocated to nobody in an older ods2_init
   volume, ALLOCCLR and the free count FREESPADRIFT). Only BADBLK.SYS's header (file 3) changes: one extent at
   floor(VOLSIZE / cluster) * cluster, a whole cluster, HIBLK = cluster, EFBLK = cluster + 1, HIGHWATER = cluster + 1. Done only
   when the size is not a multiple of the cluster, BADBLK.SYS is empty and the cluster's bit is not set (it is not free). */
static ods2_result_t assign_partial_cluster_impl(ods2_volume_t *vol, bool *changed)
{
    uint8_t h[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint32_t c = vol->home.cluster, part;
    size_t ptr;
    uint16_t sum;
    ods2_result_t r;

    *changed = false;
    if (!vol->writable) return not_writable(vol);
    if (vol->volsize == 0 || c == 0) return fail("the storage control block's volume size is unreadable");
    {
        /* VOLSIZE is trusted only from a storage control block that is one: the starter volume's placeholder (all ones,
           0xAAAA) or a damaged block would put an extent far outside the volume into BADBLK.SYS */
        uint8_t bh0[512], scb[512];
        off_t here = ftello(vol->fp), size;
        r = ods2_read_header(vol, 2, bh0);
        if (!r.ok) return r;
        r = ods2_read_file_block(vol, bh0, 1, scb);
        if (!r.ok) return r;
        if ((scb[0] | (scb[1] << 8)) != (vol->ods5 ? 0x0501 : 0x0201)) {
            return fail("BITMAP.SYS's storage control block is not one (its structure level is not the volume's): the "
                        "volume size it gives is not believed, nothing is changed");
        }
        /* and the partial cluster must start inside the image */
        if (fseeko(vol->fp, 0, SEEK_END) != 0) return fail("could not size the image");
        size = ftello(vol->fp);
        (void) fseeko(vol->fp, here, SEEK_SET);
        if (vol->volsize % c != 0 && (uint64_t) (vol->volsize / c * c) >= (uint64_t) size / 512) {
            return fail("the blocks past the last whole cluster lie outside the image: nothing is changed");
        }
    }
    if (vol->volsize % c == 0) return ok();                          /* nothing past the last whole cluster */
    part = vol->volsize / c * c;
    r = ods2_read_header(vol, 3, h);
    if (!r.ok) return r;
    r = header_ok_for_write(h, 3);
    if (!r.ok) return r;
    if (core->map_inuse != 0 || ods2_word_swap32(core->recattr.hiblk) != 0) return ok();   /* BADBLK.SYS already owns blocks */
    /* the cluster must not be free (its bit is clear, or lies beyond the bitmap) */
    {
        uint8_t bh[512], bb[512];
        unsigned bit = part / c;
        r = ods2_read_header(vol, 2, bh);
        if (!r.ok) return r;
        r = ods2_read_file_block(vol, bh, 2 + bit / 4096, bb);
        if (!r.ok) {
            /* the bitmap does not reach the cluster's bit: the SCB and the bitmap disagree about the volume */
            return fail("the bitmap does not cover the cluster past the last whole one (the storage control block and "
                        "BITMAP.SYS disagree): not assigned to BADBLK.SYS");
        }
        if ((bb[(bit % 4096) / 8] >> (bit % 8)) & 1) {
            return fail("the cluster past the last whole one is marked free in the bitmap: not assigned to BADBLK.SYS");
        }
    }
    ptr = ods2_retrieval_pointer_size(part, c);
    if (ptr == 0 || (size_t) core->mpoffset * 2 + ptr > (size_t) core->acoffset * 2) {
        return fail("BADBLK.SYS's header has no room for the retrieval pointer");
    }
    ods2_encode_retrieval_pointer(h + (size_t) core->mpoffset * 2, part, c);
    core->map_inuse = (uint8_t) (ptr / 2);
    core->recattr.hiblk = ods2_word_swap32(c);
    core->recattr.efblk = ods2_word_swap32(c + 1);
    core->recattr.ffbyte = 0;
    core->highwater = c + 1;
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    r = ods2_write_header(vol, 3, h);
    if (!r.ok) return r;
    *changed = true;
    return ok();
}

ods2_result_t ods2_assign_partial_cluster(ods2_volume_t *vol, bool *changed)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = assign_partial_cluster_impl(vol, changed);
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
    /* remembered for write_extents(): see struct ods2_txn. The guard must never silently stop remembering: when the list cannot grow the change
       is refused here, before anything is freed (review 42: fail closed) */
    if (vol->txn != NULL && !txn_note_free(vol->txn, lbn, block_count)) {
        return fail("out of memory remembering the blocks freed in this change: the change is refused, nothing is written");
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
    /* the blocks are free now: a write kept for them is not made at commit (review 29 N1: after the checks and the bits) */
    txn_forget_blocks(vol, lbn, block_count);
    /* the cache of the bitmap follows (the hint moves down to the first cluster freed) */
    if (vol->bm_valid) {
        if ((size_t) (last_block + 1) * 512 <= vol->bm_bytes) {
            if (!ods2_bitmap_mark(vol->bm_data, vol->bm_bytes * 8, start_cluster, clusters_to_free, false)) {
                bitmap_cache_drop(vol);
            } else if (start_cluster < vol->bm_hint) {
                vol->bm_hint = start_cluster;
            }
        } else {
            bitmap_cache_drop(vol);
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

/* ---- RENAME in one directory (PLAN_WRITE_OPS item 5, step 1; CORPUS 65, 66; RENAME-PROPOSAL.md, review 65) ---- */

static bool fid_equal(ods2_fid_t a, ods2_fid_t b)
{
    return a.fid_num == b.fid_num && a.fid_seq == b.fid_seq && a.fid_rvn == b.fid_rvn && a.fid_nmx == b.fid_nmx;
}

/* the header's checksum, then the write (INDEXF.SYS's own header goes through its own writer) */
static ods2_result_t rename_write_header(ods2_volume_t *vol, unsigned number, ods2_fid_t fid, uint8_t *h)
{
    uint16_t sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    if (number == 1 && fid.fid_nmx == 0) return write_indexf_header(vol, h);
    return ods2_write_header(vol, number, h);
}

static ods2_result_t rename_impl(ods2_volume_t *vol, unsigned src_dir_number, const char *src_name, int src_version,
                                 unsigned dst_dir_number, const char *dst_name, int dst_version, ods2_rename_info_t *out)
{
    ods2_rename_info_t inf;
    uint8_t dir_header[512], h[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    const ods2_head_core_t *dcore = (const ods2_head_core_t *) dir_header;
    dir_name_info_t si, di;
    char dst_norm[ODS5_MAX_LITERAL_NAME + 8], ident_name[ODS5_MAX_LITERAL_NAME + 16], src_stored[ODS5_MAX_LITERAL_NAME + 8];
    ods2_fid_t src_fid, dir_fid;
    uint16_t sv = 0;
    unsigned number, v = 0, drop_version = 0, i;
    bool resumed = false, same_name, hardlinks, primary = false, adopt = false, header_names_target = false;
    ods2_result_t r;

    memset(&inf, 0, sizeof(inf));
    memset(&si, 0, sizeof(si));
    memset(&di, 0, sizeof(di));
    if (!vol->writable) return not_writable(vol);
    if (dst_dir_number != src_dir_number) return fail("RENAME into another directory is not available yet (step 2): the target must be in the source's directory");
    if (dst_version < 0 || dst_version > ODS2_MAX_VERSION) return fail("%RMS-E-CRE, ACP file create failed; -SYSTEM-W-BADFILEVER, bad file version number");
    r = ods2_read_header(vol, src_dir_number, dir_header);
    if (!r.ok) return r;
    r = directory_header_ok(dir_header, src_dir_number);
    if (!r.ok) return r;
    r = directory_ready(vol, dir_header, src_dir_number);
    if (!r.ok) return r;
    dir_fid = dcore->fid;

    /* the target name as a new file's name is made (the same checks, the on-disk form) */
    {
        static char nmsg[400];            /* static: the message outlives this call; it says whose fault it is: the NEW name's (review 67 N2) */
        const char *why = NULL;
        if (vol->ods5) {
            why = ods5_check_new_name(dst_name);
            if (why == NULL && !ods5_complete_name(dst_name, dst_norm, sizeof(dst_norm))) why = "file name too long";
        } else if (strchr(dst_name, ';') != NULL || strchr(dst_name, '*') != NULL || strchr(dst_name, '%') != NULL) {
            why = "the new name cannot contain ';', '*' or '%': pass the version apart (RENAME wildcards are not available yet)";
        } else if (!ods2_make_file_name(dst_name, dst_norm, sizeof(dst_norm), &why)) {
            /* why set by the call */
        } else {
            why = NULL;
        }
        if (why != NULL) {
            snprintf(nmsg, sizeof(nmsg), "the new name is not valid: %s", why);
            return fail(nmsg);
        }
    }

    /* the source: the entry as the directory stores it; the stored order is followed (H1, CORPUS 66.4): a version selector that is not explicit on a name out of order is refused */
    r = dir_name_info(vol, dir_header, src_name, &si);
    if (!r.ok) return r;
    if (!si.exists) {
        free(si.versions);
        return fail(ODS2_NAME_NOT_FOUND);
    }
    if (strlen(si.stored) >= sizeof(src_stored)) { free(si.versions); return fail("file name too long"); }
    memcpy(src_stored, si.stored, strlen(si.stored) + 1);
    if (src_version <= 0) {
        for (i = 1; i < si.count; i++) {
            if (si.versions[i] >= si.versions[i - 1]) {
                free(si.versions);
                return fail("the versions of this name are stored out of order (BAD_VERSORDER): not renamed; name a version (;n)");
            }
        }
    }
    free(si.versions);
    si.versions = NULL;
    r = ods2_lookup_name_version(vol, dir_header, src_stored, src_version, &src_fid, &sv);
    if (!r.ok) return r;
    number = ods2_fid_number(src_fid);
    r = ods2_check_fid(src_fid);
    if (!r.ok) return r;
    r = ods2_read_header_fid(vol, src_fid, h);
    if (!r.ok) return r;
    if (!ods2_header_is_file(h, src_fid)) return fail("the entry's file header is not a file header: damaged, not renamed");
    if (is_reserved_file(vol, src_dir_number, src_stored, number)) {
        return fail("%RENAME-E-OPENIN, error opening the file as input; -RMS-F-RMV, ACP remove function failed; -SYSTEM-F-NOPRIV, insufficient privilege or object protection violation "
                    "(a reserved file: V8.4 refuses it; the VAX allows the volume owner)");
    }
    if (core->filechar & 0x2000u) return fail("RENAME of a directory is not available yet (step 3)");
    if (core->seg_num != 0) return fail("the entry names an extension header: damaged, not renamed");
    hardlinks = vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS);

    same_name = ods2_dir_name_compare(src_stored, strlen(src_stored), dst_norm, strlen(dst_norm)) == 0;
    if (same_name && (dst_version == 0 || dst_version == (int) sv)) {
        /* the target is the source entry itself */
        inf.version = sv;
        if (strcmp(src_stored, dst_norm) == 0) {
            /* A.TXT to A.TXT: nothing to write (VMS bumps the revision and the date: an intended difference). One thing is finished if it was left undone: a case-only rename
               stopped after the record's name was replaced and before the header followed: the entry has the new spelling, the primary header the old one */
            ods5_ident_t id;
            const char *why;
            char a[ODS5_MAX_LITERAL_NAME + 8], bn[ODS5_MAX_LITERAL_NAME + 8], t[ODS5_MAX_LITERAL_NAME + 16];
            inf.same = true;
            snprintf(ident_name, sizeof(ident_name), "%s;%u", dst_norm, (unsigned) sv);
            if (ods5_read_ident(h, &id, &why) && ods5_complete_name(id.name, a, sizeof(a)) && ods5_complete_name(src_stored, bn, sizeof(bn)) &&
                ods2_dir_name_compare(a, strlen(a), bn, strlen(bn)) == 0 && fid_equal(core->backlink, dir_fid)) {
                snprintf(t, sizeof(t), "%s;%u", id.name, id.version);
                if (strcmp(t, ident_name) != 0) {
                    if (!ods2_header_set_name(h, ident_name)) return fail("the new name does not fit the header's name area: not renamed");
                    header_revise(h);
                    r = rename_write_header(vol, number, src_fid, h);
                    if (!r.ok) return r;
                    inf.same = false;
                    inf.resumed = true;
                    inf.case_only = true;
                    inf.header_changed = true;
                }
            }
            if (out != NULL) *out = inf;
            return ok();
        }
        /* only the case differs (ODS-5): the record's name bytes are replaced in place, then the header */
        {
            uint8_t *blocks = NULL;
            unsigned used, b;
            bool done = false;
            if (!vol->ods5) return fail("a rename that changes only the case is an ODS-5 matter (an ODS-2 name is upper case)");
            r = dir_name_info(vol, dir_header, src_stored, &si);
            if (!r.ok) return r;
            if (si.count != 1) {
                free(si.versions);
                return fail("a rename that changes only the case of a name with several versions is not available: the spelling belongs to the whole name");
            }
            free(si.versions);
            r = load_directory(vol, dir_header, 0, &blocks, &used);
            if (!r.ok) return r;
            for (b = 0; b < used && !done; b++) {
                ods2_dir_namerec_t rec[8];
                int n = ods2_dir_name_records(blocks + (size_t) b * 512, 512, src_stored, rec, 8);
                if (n == 1) {
                    if (!ods2_dir_record_set_name_same_length(blocks + (size_t) b * 512, 512, rec[0].pos, dst_norm, ODS5_LEVEL_5)) {
                        free(blocks);
                        return fail("the new spelling does not fit the record (another length or name type): not renamed");
                    }
                    r = ods2_write_file_block(vol, dir_header, b + 1, blocks + (size_t) b * 512);        /* one block write: the old spelling or the new */
                    done = true;
                } else if (n > 1) {
                    free(blocks);
                    return fail("the name is held in more than one record: not renamed");
                }
            }
            free(blocks);
            if (!r.ok) return r;
            if (!done) return fail("internal error: the entry was not found in its directory block");
            inf.case_only = true;
        }
        /* the header follows when this is the file's primary entry */
        {
            ods5_ident_t id;
            const char *why;
            char a[ODS5_MAX_LITERAL_NAME + 8], bn[ODS5_MAX_LITERAL_NAME + 8];
            if (ods5_read_ident(h, &id, &why) && ods5_complete_name(id.name, a, sizeof(a)) && ods5_complete_name(src_stored, bn, sizeof(bn)) &&
                ods2_dir_name_compare(a, strlen(a), bn, strlen(bn)) == 0 && fid_equal(core->backlink, dir_fid)) {
                snprintf(ident_name, sizeof(ident_name), "%s;%u", dst_norm, (unsigned) sv);
                if (!ods2_header_set_name(h, ident_name)) return fail("the new name does not fit the header's name area: not renamed");
                header_revise(h);
                r = rename_write_header(vol, number, src_fid, h);
                if (!r.ok) return r;
                inf.header_changed = true;
            }
        }
        if (out != NULL) *out = inf;
        return ok();
    }

    /* a different name (or another version): the target name as the directory has it */
    r = dir_name_info(vol, dir_header, dst_norm, &di);
    if (!r.ok) return r;
    if (di.exists) {
        if (strlen(di.stored) >= sizeof(dst_norm)) { free(di.versions); return fail("file name too long"); }
        memcpy(dst_norm, di.stored, strlen(di.stored) + 1);             /* the stored case of an existing name wins, as for COPY */
    }
    if (dst_version == 0 && di.exists) {
        for (i = 1; i < di.count; i++) {
            if (di.versions[i] >= di.versions[i - 1]) {
                free(di.versions);
                return fail("the versions of the target name are stored out of order (BAD_VERSORDER): not renamed; name a version (;n)");
            }
        }
    }
    if (di.exists) {
        /* an entry of the target name that already names this file: an interrupted rename (the new entry was added, the old one is still there): finish it, make no new version */
        for (i = 0; i < di.count; i++) {
            ods2_fid_t f;
            if (dst_version != 0 && (unsigned) dst_version != di.versions[i]) continue;
            if (ods2_lookup_name_version(vol, dir_header, dst_norm, (int) di.versions[i], &f, NULL).ok && fid_equal(f, src_fid)) {
                resumed = true;
                v = di.versions[i];
                break;
            }
        }
    }
    if (!resumed) {
        bool exists;
        /* the target name is held by a directory or is a reserved file: no new version of it (review 67 F1; the same two refusals as COPY) */
        r = version_target_ok(vol, src_dir_number, dst_norm, &di);
        if (!r.ok) { free(di.versions); return r; }
        r = next_version(&di, dst_version, &v, &exists);               /* the one rule of COPY: 1 for a new name, highest + 1, a typed version kept */
        if (!r.ok) { free(di.versions); return r; }
        if (exists) {
            static char msg[ODS5_MAX_LITERAL_NAME + 160];       /* static: the message outlives this call */
            free(di.versions);
            snprintf(msg, sizeof(msg), "%%RENAME-E-OPENOUT, error opening the target as output; -RMS-E-ENT, ACP enter function failed; -SYSTEM-W-DUPFILENAME, duplicate file name: %s;%u", dst_norm, v);
            return fail(msg);
        }
        if (same_name) {
            /* another version of the SAME name: one entry goes out and one comes in, the count does not grow, so a version limit prunes nothing. VMS has not been measured for it (ods5-vms3 is asked):
               refused while a limit applies, instead of pruning a version the count never called for (review 67 F2) */
            if (di.exists && di.verlimit >= 1 && di.verlimit < ODS2_MAX_VERSION) {
                free(di.versions);
                return fail("a rename to another version of the same name under a version limit is not available (VMS was not measured for it): not renamed");
            }
        } else {
            r = version_limit_drop(&di, v, &drop_version);           /* the one rule of COPY: exactly one version, the lowest, goes (CORPUS 66.1) */
            if (!r.ok) { free(di.versions); return r; }
        }
    }
    free(di.versions);
    di.versions = NULL;

    /* primary entry, alias, or a header that names no entry (CORPUS 65.5, 66.2) */
    {
        ods5_ident_t id;
        const char *why;
        char a[ODS5_MAX_LITERAL_NAME + 8], bn[ODS5_MAX_LITERAL_NAME + 8];
        bool no_primary = (core->backlink.fid_num == 0 && core->backlink.fid_nmx == 0) || (core->backlink.fid_num == 0xFFFF && core->backlink.fid_nmx == 0xFF);
        if (ods5_read_ident(h, &id, &why) && ods5_complete_name(id.name, a, sizeof(a))) {
            if (ods5_complete_name(src_stored, bn, sizeof(bn)) && ods2_dir_name_compare(a, strlen(a), bn, strlen(bn)) == 0 && fid_equal(core->backlink, dir_fid)) primary = true;
            snprintf(ident_name, sizeof(ident_name), "%s;%u", dst_norm, v);
            {
                char t[ODS5_MAX_LITERAL_NAME + 8];
                snprintf(t, sizeof(t), "%s;%u", id.name, id.version);
                header_names_target = ods2_dir_name_compare(t, strlen(t), ident_name, strlen(ident_name)) == 0 && fid_equal(core->backlink, dir_fid);
            }
        }
        adopt = no_primary;
        if (adopt) primary = false;
    }
    snprintf(ident_name, sizeof(ident_name), "%s;%u", dst_norm, v);

    /* 1 the link count (a HARDLINKS volume), 2 the new entry, 3 the header, 4 the old entry out, 5 the link count back, 6 the version the limit removes */
    if (hardlinks && !resumed) {
        unsigned links = h[ODS5_FH2_LINKCOUNT] | (h[ODS5_FH2_LINKCOUNT + 1] << 8);
        links++;
        h[ODS5_FH2_LINKCOUNT] = (uint8_t) (links & 0xff);
        h[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) (links >> 8);
        r = rename_write_header(vol, number, src_fid, h);
        if (!r.ok) return r;
    }
    if (!resumed) {
        dir_name_info_t chk;
        r = dir_name_info(vol, dir_header, dst_norm, &chk);
        if (!r.ok) return r;
        free(chk.versions);
        if (chk.exists) r = insert_new_version(vol, src_dir_number, dst_norm, (uint16_t) v, src_fid, 0);
        else r = insert_into_directory(vol, src_dir_number, dst_norm, (uint16_t) v, src_fid, 0);
        if (!r.ok) return r;
    }
    if ((primary || adopt) && !header_names_target) {
        if (!ods2_header_set_name(h, ident_name)) return fail("the new name does not fit the header's name area (the map would have to move): not renamed");
        if (adopt) core->backlink = dir_fid;
        header_revise(h);
        r = rename_write_header(vol, number, src_fid, h);
        if (!r.ok) return r;
        inf.header_changed = true;
        inf.adopted = adopt;
    }
    r = ods2_read_header(vol, src_dir_number, dir_header);          /* the insert may have changed the directory's header (a split, a larger area) */
    if (!r.ok) return r;
    r = remove_directory_entry(vol, src_dir_number, dir_header, src_stored, sv, src_fid);
    if (!r.ok) return r;
    if (hardlinks) {                       /* also when an interrupted rename is finished: its first run counted the new entry (step 1), the old entry goes now */
        unsigned links = h[ODS5_FH2_LINKCOUNT] | (h[ODS5_FH2_LINKCOUNT + 1] << 8);
        if (links > 0) links--;
        h[ODS5_FH2_LINKCOUNT] = (uint8_t) (links & 0xff);
        h[ODS5_FH2_LINKCOUNT + 1] = (uint8_t) (links >> 8);
        r = rename_write_header(vol, number, src_fid, h);
        if (!r.ok) return r;
    }
    if (drop_version != 0) {
        r = ods2_delete_version(vol, src_dir_number, dst_norm, (int) drop_version, NULL);
        if (!r.ok) return r;
        inf.pruned_version = drop_version;
    }
    inf.version = (uint16_t) v;
    inf.resumed = resumed;
    if (out != NULL) *out = inf;
    return ok();
}

ods2_result_t ods2_rename(ods2_volume_t *vol, unsigned src_dir_number, const char *src_name, int src_version,
                          unsigned dst_dir_number, const char *dst_name, int dst_version, ods2_rename_info_t *info)
{
    ods2_result_t r;
    TXN_BEGIN_OR_RETURN(vol);
    r = rename_impl(vol, src_dir_number, src_name, src_version, dst_dir_number, dst_name, dst_version, info);
    return txn_end(vol, r);
}

ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name)
{
    return ods2_delete_version(vol, dir_file_number, name, ODS2_VERSION_HIGHEST, NULL);
}

/* The disk geometry (sectors x tracks) a device type reports for a volume of `blocks` blocks, which the storage
   control block must record for OpenVMS VAX V7.3's ANALYZE (cylinders = ceil(blocks / (sectors * tracks)) always).
   Measured on VMS (CORPUS section 16) and in the corpus; `*measured` is false when this size was not one of
   the sizes measured (or accepted by ANALYZE) for the geometry returned, and the answer is a prediction (confirm with SHOW DEVICE/FULL).
     ld      the LD driver's containers (LDDRIVER V8.3, VAX V7.3): measured at 29 sizes (CORPUS 16 and 30). CORPUS 31: the
             driver gives the same geometry every time for a size, and ANALYZE draws CHKSCB for a volume whose SCB has the
             same product and cylinders in the other order. The product
             sectors x tracks is the largest k*k or k*(k+1) not above the square root of the size (fits all 29) and
             cylinders = ceil(blocks / product); the ORDER of the two (11 x 12 or 12 x 11) follows no rule found in the
             29 sizes (20,000 to 20,002 give 11 x 12, 18,000 and 20,479 to 20,481 give 12 x 11), and ANALYZE draws CHKSCB
             if it is the wrong way round (B325: 20,001 and 262,144 blocks). Only the table sizes are certain; any
             other size gets the product and a guessed order (sectors first), reported as a prediction
     ra      SIMH MSCP units: RA82 57 x 15 (measured to 4,194,304 blocks), RA92 73 x 13 (measured at 8,388,608 and
             33,554,432); the type changes somewhere between 4 and 8 Mi blocks (taken at 4,194,304 here)
     scsi    a physical SCSI disk: 63 x 255 (AXP1SYS)
     axpbox  AXPbox SCSI units: 96 x 96 (the blank-volume library)
     rz      Alpha virtual disks on ASIM5 (RZ58): 32 x 16 */
bool ods2_target_geometry(const char *target, uint64_t blocks, uint32_t *sectors, uint32_t *tracks, bool *measured)
{
    static const struct { uint64_t n; uint32_t s, t; } ld[] = {
        { 2048, 7, 6 },
        { 10000, 10, 10 },
        { 16384, 11, 11 },      /* this and 131072: ANALYZE on VAX96 accepted these (B325), SHOW DEVICE not run */
        { 16385, 11, 11 },
        { 18000, 12, 11 },
        { 20000, 11, 12 },
        { 20001, 11, 12 },
        { 20002, 11, 12 },
        { 20479, 12, 11 },
        { 20480, 12, 11 },
        { 20481, 12, 11 },
        { 65536, 16, 16 },
        { 100000, 18, 17 },
        { 131072, 19, 19 },
        { 131073, 19, 19 },
        { 200000, 21, 21 },
        { 204800, 21, 21 },
        { 250000, 22, 22 },
        { 262143, 22, 23 },
        { 262144, 22, 23 },
        { 262145, 22, 23 },
        { 300000, 23, 23 },
        { 500000, 26, 27 },
        { 524288, 27, 26 },
        { 1000000, 32, 31 },
        { 1000001, 32, 31 },
        { 1024000, 32, 31 },
        { 2097152, 38, 38 },
        { 4000000, 44, 45 },
        { 4000001, 44, 45 },
        { 4190000, 45, 45 },
    };
    /* the sizes each fixed geometry was measured at (the geometry of these types is believed to be the same at
       other sizes, but that is a belief, and a size outside the list is called a prediction) */
    static const uint64_t ra82_measured[] = { 131072, 1049298, 2097152, 4194304 };
    static const uint64_t ra92_measured[] = { 8388608, 33554432 };
    static const uint64_t scsi_measured[] = { 17773524 };                                    /* AXP1SYS */
    static const uint64_t axpbox_measured[] = { 67108864, 134217728, 268435456 };           /* blank-volume library, 32-128 GiB */
    static const uint64_t rz_measured[] = { 8192, 131072, 262144, 2097152, 8388608, 16777216 };
    const uint64_t *list = NULL;
    size_t i, n = 0;
    bool m = false;

#define SET_LIST(a) do { list = (a); n = sizeof(a) / sizeof((a)[0]); } while (0)
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
            SET_LIST(ra92_measured);
        } else {
            *sectors = 57; *tracks = 15;
            SET_LIST(ra82_measured);
        }
    } else if (strcasecmp(target, "scsi") == 0) {
        *sectors = 63; *tracks = 255; SET_LIST(scsi_measured);
    } else if (strcasecmp(target, "axpbox") == 0) {
        *sectors = 96; *tracks = 96; SET_LIST(axpbox_measured);
    } else if (strcasecmp(target, "rz") == 0) {
        *sectors = 32; *tracks = 16; SET_LIST(rz_measured);
    } else {
        return false;
    }
#undef SET_LIST
    for (i = 0; i < n; i++) if (list[i] == blocks) m = true;
    if (measured) *measured = m;
    return true;
}

/* The warning a caller prints when ods2_target_geometry reported a prediction (measured false): what is predicted and what
   to do about it. Returns false (and writes nothing) when `measured` is true. For the LD driver at a size that is not in
   the table and whose sectors and tracks differ, the order is a guess (see ods2_target_geometry): VAX V7.3 ANALYZE
   compares sectors and tracks one by one. */
bool ods2_target_geometry_warning(const char *target, uint64_t blocks, uint32_t sectors, uint32_t tracks, bool measured,
                                  char *msg, size_t cap)
{
    if (measured || msg == NULL || cap == 0) return false;
    if (strcasecmp(target, "ld") == 0 && sectors != tracks) {
        snprintf(msg, cap, "the ld geometry for %llu blocks is a prediction (not a measured size): the product %u x %u comes from "
                 "the rule that fits every measured size, but whether the LD driver gives %u sectors x %u tracks or %u x %u follows no rule "
                 "found (20,000 blocks: 11 x 12; 20,479: 12 x 11), and VAX V7.3 ANALYZE reports CHKSCB for the wrong order. "
                 "Check with LD CONNECT and SHOW DEVICE/FULL, and give the answer as --geometry=SECTORS,TRACKS "
                 "(SET VOLUME/GEOMETRY=SECTORS,TRACKS)", (unsigned long long) blocks, sectors, tracks, sectors, tracks,
                 tracks, sectors);
    } else {
        snprintf(msg, cap, "the %s geometry for %llu blocks is a prediction (not a measured size): confirm with SHOW "
                 "DEVICE/FULL on the target", target, (unsigned long long) blocks);
    }
    return true;
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

/* What a delete keeps in memory for the file it removes: the extent list and the extension headers, sized by the file (a file
   may have thousands of extents and, up to FH2$W_SEG_NUM, 65,535 headers). Freed by delete_version_impl on every path. */
typedef struct {
    ods2_extent_t *extents;
    uint8_t (*ext_headers)[512];
    unsigned *ext_numbers;
} delete_scratch_t;

static ods2_result_t delete_version_body(ods2_volume_t *vol, unsigned dir_file_number,
                                         const char *name, int version, uint16_t *version_out,
                                         ods2_delete_info_t *info, delete_scratch_t *sc);

static ods2_result_t delete_version_impl(ods2_volume_t *vol, unsigned dir_file_number,
                                         const char *name, int version, uint16_t *version_out,
                                         ods2_delete_info_t *info)
{
    delete_scratch_t sc = { NULL, NULL, NULL };
    ods2_result_t r = delete_version_body(vol, dir_file_number, name, version, version_out, info, &sc);
    free(sc.extents);
    free(sc.ext_headers);
    free(sc.ext_numbers);
    return r;
}

ods2_result_t ods2_delete_version_info(ods2_volume_t *vol, unsigned dir_file_number,
                                       const char *name, int version, uint16_t *version_out,
                                       ods2_delete_info_t *info)
{
    TXN_BEGIN_OR_RETURN(vol);
    return txn_end(vol, delete_version_impl(vol, dir_file_number, name, version, version_out, info));
}

static ods2_result_t delete_version_body(ods2_volume_t *vol, unsigned dir_file_number,
                                         const char *name, int version, uint16_t *version_out,
                                         ods2_delete_info_t *info, delete_scratch_t *sc)
{
    uint16_t target_version;
    uint8_t dir_header[512];
    ods2_fid_t target_fid;
    uint8_t target_header[512];
    ods2_head_core_t *target_core;
    ods2_extent_t *extents = NULL;
    int extent_count;
    int i;
    uint8_t (*ext_headers)[512] = NULL;
    unsigned *ext_numbers = NULL;
    size_t ext_cap = 0;
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
            if (info != NULL) info->link_removed = true;     /* the file stays for its other links (PURGE does not say it freed blocks) */
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
    r = ods2_decode_all_extents_alloc(vol, target_header, &sc->extents, &extent_count);
    if (!r.ok) return r;
    extents = sc->extents;
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
            if (ext_count >= ODS2_SEGMENT_LIMIT) {
                return fail("extension header chain exceeded maximum segment limit "
                            "(possibly corrupted or circular ext_fid chain)");
            }
            if ((size_t) ext_count >= ext_cap) {
                size_t ncap = ext_cap == 0 ? 16 : ext_cap * 2;
                uint8_t (*nh)[512] = realloc(sc->ext_headers, ncap * sizeof(*nh));
                unsigned *nn;
                if (nh == NULL) return fail("out of memory reading the file's extension headers");
                sc->ext_headers = nh;
                nn = realloc(sc->ext_numbers, ncap * sizeof(*nn));
                if (nn == NULL) return fail("out of memory reading the file's extension headers");
                sc->ext_numbers = nn;
                ext_headers = nh;
                ext_numbers = nn;
                ext_cap = ncap;
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
    uint32_t *lbns = NULL;
    unsigned v = vol->home.cluster, count, i, primary_index = 0;
    bool have_primary = false, have_backup = false;
    ods2_result_t r = ok();

    if (copies_out) *copies_out = 0;
    if (vol->writable) {
        r = first_change(vol);                     /* a relabel streams the home block copies without a buffer */
        if (!r.ok) return r;
    }
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
    /* One block at a time (a cluster factor of 16,383 means 49,148 copies: 24 MiB): only the LBNs are kept. */
    lbns = malloc((size_t) count * sizeof(*lbns));
    if (lbns == NULL) {
        return fail("could not allocate memory for the home block copies");
    }

    /* Read and check every copy first; write nothing unless all of
       them are what they should be. */
    for (i = 0; i < count && r.ok; i++) {
        uint32_t vbn = i + 2, lbn;
        uint8_t copy[512];
        if (!vbn_to_lbn(vol->indexf_extents, vol->indexf_extent_count, vbn, &lbn)) {
            r = fail("INDEXF.SYS doesn't map all of the home block's clusters");
            break;
        }
        lbns[i] = lbn;
        if (fseeko(vol->fp, (off_t) lbn * 512, SEEK_SET) != 0 ||
            fread(copy, 1, 512, vol->fp) != 512) {
            r = fail("could not read a home block copy");
            break;
        }
        if (!is_home_copy(copy, primary, lbn, vbn)) {
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
       (old) label, and running this again finishes the job. Each copy is read again, changed, written and read back
       before the next. */
    for (i = 0; r.ok && i <= count; i++) {
        unsigned k = (i < count) ? i : primary_index;
        uint8_t copy[512], back[512];
        if (i < count && i == primary_index) continue;
        if (fseeko(vol->fp, (off_t) lbns[k] * 512, SEEK_SET) != 0 || fread(copy, 1, 512, vol->fp) != 512) {
            r = fail("could not read a home block copy");
            break;
        }
        memcpy(((ods2_home_t *) copy)->volname, name, 12);
        set_home_checksums(copy);
        if (!ods2_validate_home((const ods2_home_t *) copy).ok) {
            r = fail("internal error: a relabelled home block doesn't validate");
            break;
        }
        r = ods2_write_block(vol, lbns[k], copy);
        if (!r.ok) break;
        if (fflush(vol->fp) != 0) { r = fail_io("could not flush the home blocks"); break; }
        if (fseeko(vol->fp, (off_t) lbns[k] * 512, SEEK_SET) != 0 ||
            fread(back, 1, 512, vol->fp) != 512 || memcmp(back, copy, 512) != 0) {
            r = fail("a home block copy didn't read back as written");
            break;
        }
        if (k == primary_index) {
            memcpy(&vol->home, copy, 512);
            if (copies_out) *copies_out = count;
        }
    }
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
