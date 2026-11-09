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

/*
 * ods2_records.c - an RMS sequential file's bytes as records, written as Linux text.
 * See ods2_records.h for the record layouts.
 */
#include "ods2_records.h"
#include "ods2_volume.h"
#include <stdlib.h>
#include <string.h>

#define VAR_MAX 32767u

struct ods2_rec_decoder {
    ods2_rec_attrs_t a;
    unsigned options;
    bool (*sink)(void *ctx, const uint8_t *data, size_t n);
    void *ctx;
    bool failed;
    bool bad_data;       /* failed because the data is not a record stream (not the sink) */
    const char *problem;

    size_t pos;          /* offset of the next input byte in the file */
    /* the record being collected (FIX, VAR, VFC) */
    uint8_t *rec;
    size_t rec_cap, rec_len, rec_need;
    int state;           /* VAR/VFC: 0 count low, 1 count high, 2 body, 3 pad, 4 skip to block end */
    uint8_t count_lo;
    /* STM: a CR waiting to see whether an LF follows */
    bool cr_pending;
    bool any_output;
    uint8_t last_out;
    int cc_state;        /* carriage control: CC_NONE, CC_EOL (the line ended with a carriage return, its newline not yet written), CC_MID */
};

bool ods2_rec_decodable(unsigned fat_rtype)
{
    unsigned rfm = fat_rtype & 15u;
    return (fat_rtype >> 4) == 0 && rfm >= ODS2_RFM_FIX && rfm <= ODS2_RFM_STMCR;
}

ods2_rec_decoder_t *ods2_rec_new(const ods2_rec_attrs_t *attrs, unsigned options,
                                 bool (*sink)(void *, const uint8_t *, size_t), void *ctx)
{
    ods2_rec_decoder_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->a = *attrs;
    if (d->a.vfcsize == 0) d->a.vfcsize = 2;
    d->options = options;
    d->sink = sink;
    d->ctx = ctx;
    if (d->a.rfm == ODS2_RFM_FIX) {
        d->rec_cap = d->a.rsize ? d->a.rsize : d->a.maxrec;
        if (d->rec_cap == 0) d->rec_cap = 512; /* no record size known: blocks */
        d->rec_need = d->rec_cap;
    } else if (d->a.rfm == ODS2_RFM_VAR || d->a.rfm == ODS2_RFM_VFC) {
        d->rec_cap = VAR_MAX + 2;
    }
    if (d->rec_cap && (d->rec = malloc(d->rec_cap)) == NULL) {
        free(d);
        return NULL;
    }
    return d;
}

void ods2_rec_free(ods2_rec_decoder_t *d)
{
    if (d != NULL) {
        free(d->rec);
        free(d);
    }
}

const char *ods2_rec_problem(const ods2_rec_decoder_t *d)
{
    return d->problem;
}

bool ods2_rec_bad_data(const ods2_rec_decoder_t *d)
{
    return d->bad_data;
}

static bool put(ods2_rec_decoder_t *d, const uint8_t *p, size_t n)
{
    if (n == 0) return true;
    if (!d->sink(d->ctx, p, n)) {
        d->failed = true;
        d->problem = "could not write the output";
        return false;
    }
    d->any_output = true;
    d->last_out = p[n - 1];
    return true;
}

/*
 * Carriage control, as VMS TYPE passes it on (measured on V8.4-2L2, batch m2: the records TYPE wrote with
 * their prefix/postfix bytes, and a terminal capture). Each record leaves TYPE as prefix, data, postfix:
 *   RAT CR (also every stream format): prefix 0x01 (one line feed), postfix 0x8D (a carriage return)
 *   print file (RAT PRN, VFC): the first two bytes of the record's fixed control area, as they are
 *   FORTRAN (RAT FTN): the first data byte is taken off and becomes the pair:
 *       ' ' 01 8D   '0' 02 8D   '1' 8C 8D   '+' 00 8D   '$' 01 00   any other byte 01 8D
 *   no carriage control (RAT none): 00 00, the records run together
 * A prefix byte below 0x80 is that many line feeds, one with bit 7 set is the character in its low 7 bits
 * (0x8C form feed, 0x8D carriage return); the same for a postfix byte.
 * As Linux text: a carriage return after the data is "the line ends": a newline, but written only when the
 * next record shows whether it was a newline or (prefix 0, overprint) a carriage return alone. A postfix of
 * 0 leaves the cursor in the line; the next prefix of one or more line feeds then ends that line.
 */
enum { CC_NONE = 0, CC_EOL = 1, CC_MID = 2 };

static bool lines_of(ods2_rec_decoder_t *d, unsigned n)
{
    static const uint8_t lf = '\n';
    while (n-- > 0) if (!put(d, &lf, 1)) return false;
    return true;
}

static bool apply_prefix(ods2_rec_decoder_t *d, uint8_t p)
{
    static const uint8_t cr = '\r';
    uint8_t ch;
    if (p & 0x80) {
        ch = p & 0x7f;
        if (d->cc_state == CC_EOL && !lines_of(d, 1)) return false;
        return put(d, &ch, 1);
    }
    if (p == 0) {                                   /* overprint: back to the start of the line */
        if (d->cc_state == CC_EOL) return put(d, &cr, 1);
        return true;
    }
    if (d->cc_state != CC_NONE) return lines_of(d, p);   /* the first line feed ends the line */
    return lines_of(d, p - 1u);                           /* the first record: it starts on the next line */
}

static bool apply_postfix(ods2_rec_decoder_t *d, uint8_t q)
{
    if (q == 0x8D) { d->cc_state = CC_EOL; return true; }
    d->cc_state = CC_MID;
    if (q & 0x80) {
        uint8_t ch = q & 0x7f;
        return put(d, &ch, 1);
    }
    return lines_of(d, q);
}

/* One finished record: `hdr` is the fixed control area (VFC), `data` the record. */
static bool emit_record(ods2_rec_decoder_t *d, const uint8_t *hdr, size_t hlen,
                        const uint8_t *data, size_t len)
{
    uint8_t p = 0x01, q = 0x8D;
    uint8_t rat = d->a.rat;

    if (!(d->options & ODS2_REC_LINES)) {
        if ((rat & ODS2_RAT_FTN) && d->a.rfm != ODS2_RFM_VFC) {
            uint8_t c = len > 0 ? data[0] : ' ';
            if (len > 0) { data++; len--; }
            switch (c) {
            case '0': p = 0x02; break;
            case '1': p = 0x8C; break;
            case '+': p = 0x00; break;
            case '$': q = 0x00; break;
            default: break;
            }
        } else if ((rat & ODS2_RAT_PRN) && d->a.rfm == ODS2_RFM_VFC && hlen >= 2) {
            p = hdr[0];
            q = hdr[1];
        } else if (!(rat & (ODS2_RAT_CR | ODS2_RAT_PRN | ODS2_RAT_FTN))) {
            p = 0x00;
            q = 0x00;
        }
        return apply_prefix(d, p) && put(d, data, len) && apply_postfix(d, q);
    }
    return put(d, data, len) && lines_of(d, 1);
}

static bool feed_stream(ods2_rec_decoder_t *d, const uint8_t *data, size_t n)
{
    size_t i, start = 0;
    static const uint8_t lf = '\n', cr = '\r';

    if (d->a.rfm == ODS2_RFM_STMLF) return put(d, data, n);
    if (d->a.rfm == ODS2_RFM_STMCR) {
        for (i = 0; i < n; i++) {
            if (data[i] == '\r') {
                if (!put(d, data + start, i - start) || !put(d, &lf, 1)) return false;
                start = i + 1;
            }
        }
        return put(d, data + start, n - start);
    }
    /* STM: CR LF ends a record; a CR or LF on its own is data */
    for (i = 0; i < n; i++) {
        if (d->cr_pending) {
            d->cr_pending = false;
            if (data[i] == '\n') {
                if (!put(d, &lf, 1)) return false;
                start = i + 1;
                continue;
            }
            if (!put(d, &cr, 1)) return false;
        }
        if (data[i] == '\r') {
            if (!put(d, data + start, i - start)) return false;
            d->cr_pending = true;
            start = i + 1;
        } else if (data[i] == '\n') {
            /* an LF without a CR also ends a record, and VMS TYPE keeps that LF in the record
               (measured, m2: "four has a lone LF" gives the line, an empty line, then the rest) */
            if (!put(d, data + start, i + 1 - start) || !put(d, &lf, 1)) return false;
            start = i + 1;
        }
    }
    return put(d, data + start, n - start);
}

static bool feed_var(ods2_rec_decoder_t *d, const uint8_t *data, size_t n)
{
    size_t i = 0;
    while (i < n) {
        switch (d->state) {
        case 0:
            d->count_lo = data[i++];
            d->pos++;
            d->state = 1;
            break;
        case 1: {
            unsigned count = d->count_lo | ((unsigned) data[i++] << 8);
            d->pos++;
            if (count == 0xFFFFu) {
                d->state = (d->pos % 512u) ? 4 : 0;
                break;
            }
            if (count > VAR_MAX) {
                d->failed = d->bad_data = true;
                d->problem = "not a variable-length record stream (record length over 32767)";
                return false;
            }
            if (d->a.rfm == ODS2_RFM_VFC && count < d->a.vfcsize) {
                d->failed = d->bad_data = true;
                d->problem = "bad VFC record (shorter than its control area)";
                return false;
            }
            d->rec_len = 0;
            d->rec_need = count;
            if (count == 0) {
                if (!emit_record(d, NULL, 0, d->rec, 0)) return false;
                d->state = 0;
            } else {
                d->state = 2;
            }
            break;
        }
        case 2: {
            size_t take = d->rec_need - d->rec_len;
            if (take > n - i) take = n - i;
            memcpy(d->rec + d->rec_len, data + i, take);
            d->rec_len += take;
            i += take;
            d->pos += take;
            if (d->rec_len == d->rec_need) {
                bool vfc = d->a.rfm == ODS2_RFM_VFC;
                size_t h = vfc ? d->a.vfcsize : 0;
                if (!emit_record(d, d->rec, h, d->rec + h, d->rec_len - h)) return false;
                d->state = (d->rec_need & 1u) ? 3 : 0;
            }
            break;
        }
        case 3:
            i++;
            d->pos++;
            d->state = 0;
            break;
        default: { /* 4: the rest of the block after a 0xFFFF count */
            size_t left = 512u - (d->pos % 512u);
            if (left > n - i) left = n - i;
            i += left;
            d->pos += left;
            if (d->pos % 512u == 0) d->state = 0;
            break;
        }
        }
    }
    return true;
}

static bool feed_fix(ods2_rec_decoder_t *d, const uint8_t *data, size_t n)
{
    while (n > 0) {
        size_t take = d->rec_cap - d->rec_len;
        if (take > n) take = n;
        memcpy(d->rec + d->rec_len, data, take);
        d->rec_len += take;
        data += take;
        n -= take;
        if (d->rec_len == d->rec_cap) {
            if (!emit_record(d, NULL, 0, d->rec, d->rec_len)) return false;
            d->rec_len = 0;
        }
    }
    return true;
}

bool ods2_rec_feed(ods2_rec_decoder_t *d, const uint8_t *data, size_t n)
{
    bool ok;
    if (d->failed) return false;
    switch (d->a.rfm) {
    case ODS2_RFM_FIX: ok = feed_fix(d, data, n); break;
    case ODS2_RFM_VAR:
    case ODS2_RFM_VFC: ok = feed_var(d, data, n); break;
    case ODS2_RFM_STM:
    case ODS2_RFM_STMLF:
    case ODS2_RFM_STMCR: ok = feed_stream(d, data, n); break;
    default: ok = put(d, data, n); break;   /* UDF: the bytes as they are */
    }
    return ok;
}

bool ods2_rec_finish(ods2_rec_decoder_t *d)
{
    static const uint8_t lf = '\n', cr = '\r';
    bool ok = true;
    if (d->failed) return false;
    switch (d->a.rfm) {
    case ODS2_RFM_FIX:
        if (d->rec_len > 0) {
            /* VMS TYPE gives the whole record: what follows the end-of-file mark in the block
               (zeros as ods2v2 and VMS write blocks) completes it (measured, m2) */
            d->problem = "the last fixed-length record is short (padded with zeros, as VMS shows it)";
            memset(d->rec + d->rec_len, 0, d->rec_cap - d->rec_len);
            ok = emit_record(d, NULL, 0, d->rec, d->rec_cap);
        }
        break;
    case ODS2_RFM_VAR:
    case ODS2_RFM_VFC:
        if (d->state == 2) {
            size_t h = d->a.rfm == ODS2_RFM_VFC ? d->a.vfcsize : 0;
            d->problem = "the last record is cut short by the end of the file";
            if (d->rec_len > h) ok = emit_record(d, d->rec, h, d->rec + h, d->rec_len - h);
        } else if (d->state == 1) {
            d->problem = "the file ends inside a record length";
        }
        break;
    case ODS2_RFM_STM:
        if (d->cr_pending) {
            d->cr_pending = false;
            ok = put(d, &cr, 1) && put(d, &lf, 1);
        } else if (d->any_output && d->last_out != '\n') {
            ok = put(d, &lf, 1);
        }
        break;
    case ODS2_RFM_STMLF:
        if (d->any_output && d->last_out != '\n') ok = put(d, &lf, 1);
        break;
    case ODS2_RFM_STMCR:
        if (d->any_output && d->last_out != '\n') ok = put(d, &lf, 1);
        break;
    default:
        break;
    }
    if (ok && d->cc_state == CC_EOL) ok = lines_of(d, 1);
    return ok;
}
