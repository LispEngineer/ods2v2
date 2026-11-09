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

/* ods2_records_selftest.c - the record decoder gives the same text whatever the size of the pieces
 * the file arrives in (1 byte, 3, 511, 512, 513, all at once). */
#include "ods2_records.h"
#include "ods2_volume.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } else { printf("PASS: " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t out[4096]; size_t n; } buf_t;
static bool sink(void *ctx, const uint8_t *d, size_t n)
{
    buf_t *b = ctx;
    if (b->n + n > sizeof(b->out)) return false;
    memcpy(b->out + b->n, d, n);
    b->n += n;
    return true;
}

static int run(const ods2_rec_attrs_t *a, unsigned opts, const uint8_t *in, size_t len, size_t piece, buf_t *b)
{
    ods2_rec_decoder_t *d = ods2_rec_new(a, opts, sink, b);
    size_t i;
    int ok = 1;
    b->n = 0;
    for (i = 0; i < len && ok; i += piece) ok = ods2_rec_feed(d, in + i, (len - i < piece) ? len - i : piece);
    if (ok) ok = ods2_rec_finish(d);
    ods2_rec_free(d);
    return ok;
}

static void same_for_all_pieces(const char *name, const ods2_rec_attrs_t *a, unsigned opts,
                                const uint8_t *in, size_t len, const char *expect, size_t elen)
{
    static const size_t pieces[] = { 1, 2, 3, 7, 511, 512, 513, 100000 };
    size_t k;
    int good = 1;
    for (k = 0; k < sizeof(pieces) / sizeof(pieces[0]); k++) {
        buf_t b;
        int ok = run(a, opts, in, len, pieces[k], &b);
        if (!ok || b.n != elen || memcmp(b.out, expect, elen) != 0) {
            printf("  piece %zu: ok=%d got %zu bytes\n", pieces[k], ok, b.n);
            good = 0;
        }
    }
    CHECK(good, "%s: same text for every piece size", name);
}

int main(void)
{
    ods2_rec_attrs_t a;
    uint8_t v[1024];
    size_t n = 0, i;
    char expect[1024];
    size_t en = 0;

    /* VAR: three records, an odd one (pad), an empty one, then 0xFFFF filler to the block end, one more in block 2 */
    memset(&a, 0, sizeof(a));
    a.rfm = ODS2_RFM_VAR;
    a.rat = ODS2_RAT_CR;
    memset(v, 0, sizeof(v));
    v[n++] = 3; v[n++] = 0; v[n++] = 'a'; v[n++] = 'b'; v[n++] = 'c'; v[n++] = 0;   /* "abc" + pad */
    v[n++] = 0; v[n++] = 0;                                                         /* empty */
    v[n++] = 4; v[n++] = 0; memcpy(v + n, "wxyz", 4); n += 4;
    while (n < 510) v[n++] = 0xFF;                                                  /* filler: 0xFFFF counts */
    v[n++] = 0xFF; v[n++] = 0xFF;
    v[n++] = 2; v[n++] = 0; v[n++] = 'o'; v[n++] = 'k';                             /* block 2 */
    en = (size_t) snprintf(expect, sizeof(expect), "abc\n\nwxyz\nok\n");
    same_for_all_pieces("VAR with filler", &a, 0, v, n, expect, en);

    /* STMLF without a final LF gets one; STM keeps a lone CR and LF; STMCR becomes LF */
    a.rfm = ODS2_RFM_STMLF;
    same_for_all_pieces("STMLF", &a, 0, (const uint8_t *) "a\nb", 3, "a\nb\n", 4);
    a.rfm = ODS2_RFM_STM;
    same_for_all_pieces("STM", &a, 0, (const uint8_t *) "a\r\nb\rc\nd\r", 9, "a\nb\rc\n\nd\r\n", 10);
    a.rfm = ODS2_RFM_STMCR;
    same_for_all_pieces("STMCR", &a, 0, (const uint8_t *) "a\rb\r", 4, "a\nb\n", 4);

    /* FIX of 4 bytes with CR carriage control, the last record short (completed with zeros, as VMS shows it) */
    a.rfm = ODS2_RFM_FIX; a.rsize = 4; a.maxrec = 4; a.rat = ODS2_RAT_CR;
    same_for_all_pieces("FIX", &a, 0, (const uint8_t *) "abcdefghij", 10, "abcd\nefgh\nij\0\0\n", 15);
    /* no carriage control: the records run together (VMS TYPE writes them with prefix and postfix 0) */
    a.rat = 0;
    same_for_all_pieces("FIX, no carriage control", &a, 0, (const uint8_t *) "abcdefgh", 8, "abcdefgh", 8);
    /* ... unless the caller wants one line per record */
    same_for_all_pieces("FIX, no carriage control, one line per record", &a, ODS2_REC_LINES, (const uint8_t *) "abcdefgh", 8, "abcd\nefgh\n", 10);
    /* FORTRAN carriage control: the first byte of each record is the control (' ' 0 1 + $; measured) */
    a.rfm = ODS2_RFM_FIX; a.rsize = 5; a.maxrec = 5; a.rat = ODS2_RAT_FTN;
    same_for_all_pieces("FIX FORTRAN", &a, 0, (const uint8_t *) " aaaa0bbbb1cccc+dddd$eeee ffff", 30,
                        "aaaa\n\nbbbb\n\x0c" "cccc\rdddd\neeee\nffff\n", 32);

    /* VFC with a 2-byte control area */
    a.rfm = ODS2_RFM_VFC; a.vfcsize = 2; a.rat = ODS2_RAT_PRN;
    {
        const uint8_t f[] = { 5, 0, 1, 0x8d, 'h', 'i', 'x', 0, 4, 0, 2, 0x8d, 'y', 'o' };
        same_for_all_pieces("VFC", &a, 0, f, sizeof(f), "hix\n\nyo\n", 8);
    }
    /* a record length that cannot be: reported */
    {
        ods2_rec_decoder_t *d;
        buf_t b = { {0}, 0 };
        const uint8_t bad[] = { 0x00, 0x80, 'a' };
        a.rfm = ODS2_RFM_VAR; a.vfcsize = 0;
        d = ods2_rec_new(&a, 0, sink, &b);
        CHECK(!ods2_rec_feed(d, bad, sizeof(bad)) && ods2_rec_bad_data(d), "record length over 32767 is refused");
        ods2_rec_free(d);
    }
    (void) i;
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all ods2_records checks passed\n");
    return 0;
}
