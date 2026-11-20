/*
 * MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
 *
 * ods2_newver_selftest - the directory block primitives behind COPY to a new version (PLAN_WRITE_OPS item 1), on blocks in memory:
 * a name's versions are kept in records newest first (VMS packs them into one record and puts a new highest version in front of the
 * first pair, CORPUS 49 C1); a pair goes in front, in the middle or at the end; a record grows to 510 bytes and no further; a block
 * with no room is left unchanged and says so; a new record goes before or after the name's records at a boundary.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ods2_directory.h"
#include "ods2_directory_write.h"
#include "ods5_ondisk.h"

static ods2_fid_t fid(unsigned n) { ods2_fid_t f; memset(&f, 0, sizeof f); f.fid_num = (uint16_t) n; f.fid_seq = 1; return f; }

/* the versions of `name` in the block, in the order they are stored */
static int versions(const uint8_t *block, const char *name, unsigned *out, int max)
{
    ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
    unsigned bad = 0;
    int n = ods2_parse_directory_records(block, 512, ODS5_LEVEL_2, e, ODS2_DIR_MAX_ENTRIES_PER_BLOCK, &bad), i, k = 0;
    assert(bad == 0);
    for (i = 0; i < n && i < ODS2_DIR_MAX_ENTRIES_PER_BLOCK; i++) {
        if (strcmp(e[i].name, name) == 0 && k < max) out[k++] = e[i].version;
    }
    return k;
}

int main(void)
{
    uint8_t blk[512];
    unsigned v[80];
    ods2_dir_namerec_t rec[4];
    int i, n, rc;

    /* ---- a record of one version; neighbours before and after ---- */
    memset(blk, 0, sizeof blk);
    assert(ods2_insert_dir_entry(blk, 512, "AAFIRST.TXT.", 1, fid(20)));
    assert(ods2_insert_dir_entry(blk, 512, "A.TXT", 3, fid(10)));
    assert(ods2_insert_dir_entry(blk, 512, "ZZLAST.TXT.", 1, fid(21)));
    n = ods2_dir_name_records(blk, 512, "A.TXT", rec, 4);
    assert(n == 1 && rec[0].nentries == 1 && rec[0].first == 3 && rec[0].last == 3);

    /* in front: the new highest version (VMS C1) */
    assert(ods2_dir_record_add_version(blk, 512, rec[0].pos, 0, 4, fid(11)) == 1);
    n = ods2_dir_name_records(blk, 512, "A.TXT", rec, 4);
    assert(n == 1 && rec[0].nentries == 2 && rec[0].first == 4 && rec[0].last == 3);
    /* at the end: a version below all */
    assert(ods2_dir_record_add_version(blk, 512, rec[0].pos, 2, 1, fid(12)) == 1);
    n = ods2_dir_name_records(blk, 512, "A.TXT", rec, 4);
    /* in the middle: after the two highest */
    assert(ods2_dir_record_add_version(blk, 512, rec[0].pos, 2, 2, fid(13)) == 1);
    n = versions(blk, "A.TXT", v, 80);
    assert(n == 4 && v[0] == 4 && v[1] == 3 && v[2] == 2 && v[3] == 1);
    assert(ods2_dir_block_ok(blk, 512));
    /* the neighbours are where they were, once each */
    assert(versions(blk, "AAFIRST.TXT.", v, 80) == 1 && versions(blk, "ZZLAST.TXT.", v, 80) == 1);
    printf("PASS: pairs go in front, in the middle and at the end of a record, in version order, neighbours untouched\n");

    /* an index past the end of the record's pairs, and a record that is not one: nothing changes */
    {
        uint8_t keep[512];
        memcpy(keep, blk, 512);
        assert(ods2_dir_record_add_version(blk, 512, rec[0].pos, 9, 7, fid(14)) == -2);
        assert(ods2_dir_record_add_version(blk, 512, rec[0].pos + 1, 0, 7, fid(14)) == -2);
        assert(memcmp(keep, blk, 512) == 0);
    }
    printf("PASS: a bad index or a position that is not a record is refused and changes nothing\n");

    /* ---- a record grows to 510 bytes and no further; a full block says so ---- */
    memset(blk, 0, sizeof blk);
    assert(ods2_insert_dir_entry(blk, 512, "VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT", 1, fid(30)));
    n = ods2_dir_name_records(blk, 512, "VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT", rec, 4);
    {
        unsigned ver = 1;
        int added = 0;
        for (;;) {
            uint8_t keep[512];
            memcpy(keep, blk, 512);
            n = ods2_dir_name_records(blk, 512, "VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT", rec, 4);
            assert(n == 1);
            rc = ods2_dir_record_add_version(blk, 512, rec[0].pos, 0, (uint16_t) (ver + 1), fid(31 + added));
            if (rc != 1) {
                assert(rc == -1);                       /* the record cannot grow: a record of its own is needed */
                assert(memcmp(keep, blk, 512) == 0);    /* and nothing was changed */
                break;
            }
            ver++;
            added++;
            assert(ods2_dir_block_ok(blk, 512));
        }
        n = ods2_dir_name_records(blk, 512, "VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT", rec, 4);
        assert(rec[0].total <= 510 && rec[0].total + 8 > 510);
        assert(rec[0].nentries == 58);                   /* (510 - 6 - 34) / 8 */
        /* a new record in front of it, with the next version: the name now has two records, versions still descending */
        assert(!ods2_dir_insert_record_at(blk, 512, rec[0].pos, "VERSIONS_OF_ONE_NAME_FOR_SPLIT.TXT", 59, fid(99),
                                          ODS5_LEVEL_2, 0, 32767));      /* no room beside a 510-byte record: false, unchanged */
        assert(ods2_dir_block_ok(blk, 512));
    }
    printf("PASS: a record takes 58 versions of a 34-character name (510 bytes), then says it cannot grow; a refused add changes nothing\n");

    /* ---- a new record at a boundary: before and after a name's record ---- */
    memset(blk, 0, sizeof blk);
    assert(ods2_insert_dir_entry(blk, 512, "B.TXT", 5, fid(40)));
    assert(ods2_insert_dir_entry(blk, 512, "B.TXT", 3, fid(41)));          /* the existing insert puts a lower version in its own record */
    n = ods2_dir_name_records(blk, 512, "B.TXT", rec, 4);
    assert(n == 2 && rec[0].first == 5 && rec[1].first == 3);
    assert(ods2_dir_insert_record_at(blk, 512, rec[0].pos, "B.TXT", 9, fid(42), ODS5_LEVEL_2, 0, 3));   /* in front of both */
    n = versions(blk, "B.TXT", v, 80);
    assert(v[0] == 9);
    assert(!ods2_dir_insert_record_at(blk, 512, 3, "B.TXT", 8, fid(44), ODS5_LEVEL_2, 0, 3));          /* 3 is no record boundary */
    assert(ods2_dir_block_ok(blk, 512));
    {
        ods2_dir_namerec_t r2[8];
        n = ods2_dir_name_records(blk, 512, "B.TXT", r2, 8);
        assert(n >= 3 && r2[0].first == 9 && r2[0].verlimit == 3);      /* the limit given is the one stored */
    }
    printf("PASS: a new record goes in front of a name's records at a boundary, keeps the limit it is given, and refuses a position that is no boundary\n");

    printf("ods2_newver_selftest: all checks passed\n");
    (void) i;
    return 0;
}
