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

#include "ods2_directory.h"
#include "ods5_ondisk.h"
#include <string.h>

static uint16_t read_word(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

int ods2_parse_directory(const uint8_t *data, size_t len,
                          ods2_dir_entry_t *entries_out, size_t max_entries)
{
    return ods2_parse_directory_level(data, len, ODS5_LEVEL_2, entries_out, max_entries);
}

int ods2_parse_directory_level(const uint8_t *data, size_t len, unsigned level,
                               ods2_dir_entry_t *entries_out, size_t max_entries)
{
    unsigned bad = 0;
    int n = ods2_parse_directory_records(data, len, level, entries_out, max_entries, &bad);
    return bad != 0 ? -1 : n;
}

int ods2_parse_directory_records(const uint8_t *data, size_t len, unsigned level,
                                 ods2_dir_entry_t *entries_out, size_t max_entries,
                                 unsigned *bad_records)
{
    size_t pos = 0;
    size_t count = 0;

    while (pos + 6 <= len) {
        uint16_t size = read_word(data + pos);
        uint16_t namecount;
        uint8_t flags;
        size_t name_start, name_end, entry_start, record_total;

        if (size == 0xffff) {
            /* Sentinel: end of records in this block. */
            break;
        }
        if (size == 0) {
            /* No real record can have dir$size 0 (the smallest is 12:
               header, empty name, one version entry). Zeroed bytes
               mean an unused block, or unused space after the last
               record of a block that has no sentinel - either way,
               the end of records, not a malformed block. Treating it
               as malformed would discard the records before it. */
            break;
        }

        /* dir$size excludes itself (verified against real data: a
           record with header(6)+name(10)+entry(8)=24 total bytes
           reported dir$size=22, i.e. total-2). */
        record_total = (size_t) size + 2;
        if (pos + record_total > len) {
            (*bad_records)++; /* declared size runs past the buffer: nothing after it can be found */
            break;
        }

        namecount = data[pos + 5];
        flags = data[pos + 4];
        if (level == ODS5_LEVEL_5 &&
            (flags & ODS5_DIR_TYPE_MASK) == ODS5_DIR_TYPE_LINKNAME) {
            /* a link name record holds a name, not version/FID pairs:
               not a file entry */
            pos += record_total;
            continue;
        }
        name_start = pos + 6;
        name_end = name_start + namecount;
        /* Name is padded to an even byte boundary before the
           per-version entries begin. */
        entry_start = name_end + (name_end % 2);
        if (name_end > pos + record_total || entry_start + 8 > len ||
            entry_start + 8 > pos + record_total) {
            /* this record has no room for its name and a version: skip it, the size
               word says where the next one starts */
            (*bad_records)++;
            pos += record_total;
            continue;
        }

        /* Every complete 8-byte (version, FID) pair up to the end of
           the record is a separate version of this name - the spec
           says their count is deduced from the record's byte count.
           Any trailing bytes too short to be a whole pair are
           ignored. */
        {
            size_t record_end = pos + record_total;
            size_t e;
            size_t copy_len = namecount;
            char converted[ODS2_DIR_MAX_NAME + 1];
            unsigned name_type = 0;
            const char *name_text = (const char *) (data + name_start);

            if (level == ODS5_LEVEL_5) {
                name_type = (flags & ODS5_DIR_NAMETYPE_MASK) >> ODS5_DIR_NAMETYPE_SHIFT;
                if (name_type == ODS5_NAME_UCS2) {
                    copy_len = ods5_ucs2_to_utf8(data + name_start, namecount,
                                                 converted, sizeof(converted));
                } else {
                    copy_len = ods5_isl1_to_utf8(data + name_start, namecount,
                                                 converted, sizeof(converted));
                }
                if (copy_len == (size_t) -1) {
                    /* a UCS-2 name of odd length, or one that does not fit: skip the
                       record, not the whole block */
                    (*bad_records)++;
                    pos += record_total;
                    continue;
                }
                name_text = converted;
            } else if (copy_len > ODS2_DIR_MAX_NAME) {
                copy_len = ODS2_DIR_MAX_NAME;
            }

            for (e = entry_start; e + 8 <= record_end; e += 8) {
                if (count < max_entries) {
                    memcpy(entries_out[count].name, name_text, copy_len);
                    entries_out[count].name[copy_len] = '\0';
                    entries_out[count].flags = (level == ODS5_LEVEL_5) ? flags : 0;
                    entries_out[count].name_type = (uint8_t) name_type;

                    entries_out[count].version = read_word(data + e);
                    entries_out[count].fid.fid_num = read_word(data + e + 2);
                    entries_out[count].fid.fid_seq = read_word(data + e + 4);
                    entries_out[count].fid.fid_rvn = data[e + 6];
                    entries_out[count].fid.fid_nmx = data[e + 7];
                }
                /* Counted even when not stored, so the caller can
                   detect truncation (see ods2_directory.h). */
                count++;
            }
        }

        pos += record_total;
    }

    return (int) count;
}
