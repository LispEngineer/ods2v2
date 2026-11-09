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

#include "ods5_header.h"
#include "ods5_ondisk.h"
#include <stdio.h>
#include <string.h>

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* Splits "NAME.EXT;3" into the name (copied to out) and the version. */
static void split_name_version(const uint8_t *text, size_t n, char *name, size_t cap,
                               unsigned *version, bool ucs2)
{
    char utf8[ODS5_MAX_LITERAL_NAME + 1];
    size_t len, k;
    char *semi;

    len = ucs2 ? ods5_ucs2_to_utf8(text, n, utf8, sizeof(utf8))
               : ods5_isl1_to_utf8(text, n, utf8, sizeof(utf8));
    if (len == (size_t) -1) len = 0, utf8[0] = '\0';
    *version = 0;
    semi = strrchr(utf8, ';');
    if (semi != NULL) {
        const char *d = semi + 1;
        unsigned v = 0;
        bool digits = *d != '\0';
        for (; *d; d++) {
            if (*d < '0' || *d > '9') { digits = false; break; }
            v = v * 10 + (unsigned) (*d - '0');
        }
        if (digits) {
            *version = v;
            *semi = '\0';
        }
    }
    k = strlen(utf8);
    if (k >= cap) k = cap - 1;
    memcpy(name, utf8, k);
    name[k] = '\0';
}

bool ods5_read_ident(const uint8_t *h, ods5_ident_t *out, const char **why)
{
    unsigned level = (unsigned) (h[7]);      /* FH2$B_STRUCLEV */
    size_t id = (size_t) h[0] * 2, mp = (size_t) h[1] * 2;

    memset(out, 0, sizeof(*out));
    out->level = level;
    if (level != ODS5_LEVEL_2 && level != ODS5_LEVEL_5) {
        *why = "header structure level is not 2 or 5";
        return false;
    }
    if (id < 80 || id > mp || mp > 512) {
        *why = "ident area offsets are inconsistent";
        return false;
    }
    if (level == ODS5_LEVEL_2) {
        uint8_t name[86];
        size_t i, n = 0;
        if (id + 120 > 512) { *why = "ident area runs past the header"; return false; }
        memcpy(name, h + id, 20);
        memcpy(name + 20, h + id + 54, 66);
        /* FI2: 20 bytes of name in FI2$T_FILENAME, 66 more in FI2$T_FILENAMEXT, space/NUL padded */
        for (i = 0; i < 86; i++) { if (name[i] == ' ' || name[i] == 0) break; n++; }
        split_name_version(name, n, out->name, sizeof(out->name), &out->version, false);
        out->revision = (uint16_t) (h[id + 20] | (h[id + 21] << 8));
        out->credate = rd64(h + id + 22);
        out->revdate = rd64(h + id + 30);
        out->expdate = rd64(h + id + 38);
        out->bakdate = rd64(h + id + 46);
        return true;
    }
    {
        unsigned control = h[id + ODS5_FI5_CONTROL], namelen = h[id + ODS5_FI5_NAMELEN];
        if (id + ODS5_FI5_FILENAME > 512 || id + ODS5_FI5_FILENAME + namelen > 512 ||
            id + ODS5_FI5_FILENAME + namelen > mp) {
            *why = "FI5 name runs past the ident area";
            return false;
        }
        out->name_type = control & ODS5_FI5_NAMETYPE_MASK;
        if (out->name_type == ODS5_NAME_UCS2 && (namelen & 1)) {
            *why = "UCS-2 name of odd length";
            return false;
        }
        split_name_version(h + id + ODS5_FI5_FILENAME, namelen, out->name, sizeof(out->name),
                           &out->version, out->name_type == ODS5_NAME_UCS2);
        out->revision = (uint16_t) (h[id + ODS5_FI5_REVISION] | (h[id + ODS5_FI5_REVISION + 1] << 8));
        out->credate = rd64(h + id + ODS5_FI5_CREDATE);
        out->revdate = rd64(h + id + ODS5_FI5_REVDATE);
        out->expdate = rd64(h + id + ODS5_FI5_EXPDATE);
        out->bakdate = rd64(h + id + ODS5_FI5_BAKDATE);
        out->accdate = rd64(h + id + ODS5_FI5_ACCDATE);
        out->attdate = rd64(h + id + ODS5_FI5_ATTDATE);
        out->has_ods5_dates = true;
        return true;
    }
}

unsigned ods5_link_count(const uint8_t *h)
{
    return (unsigned) (h[ODS5_FH2_LINKCOUNT] | (h[ODS5_FH2_LINKCOUNT + 1] << 8));
}

unsigned ods5_special_type(const uint8_t *h)
{
    /* FAT starts at byte 20: byte 0 = record type (low nibble) and file
       organization (high nibble), byte 1 = record attributes / special type */
    if ((h[20] >> 4) != ODS5_FAT_ORG_SPECIAL) return 0;
    return h[21];
}

void ods5_format_vms_time(uint64_t t, char *out, size_t cap)
{
    static const char *months[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    uint64_t secs, ticks;
    long z, era, doe, yoe, y, doy, mp, d, m;
    unsigned long rem;

    if (t == 0) { snprintf(out, cap, "<None specified>"); return; }
    secs = t / 10000000u;
    ticks = t % 10000000u;
    rem = (unsigned long) (secs % 86400u);
    /* days since 17-NOV-1858 -> days since 1-JAN-1970 (MJD 40587), then the
       civil date of that day (proleptic Gregorian) */
    z = (long) (secs / 86400u) - 40587 + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, cap, "%02ld-%s-%04ld %02lu:%02lu:%02lu.%02u", d, months[m - 1], y,
             rem / 3600, (rem / 60) % 60, rem % 60, (unsigned) (ticks / 100000u));
}
