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
 * ods2_records.h - an RMS sequential file's bytes, read as records and written
 * as Linux text (what VMS TYPE shows): every record followed by one LF.
 *
 * Record layouts (FAT$B_RTYPE, spec 6.2; FH2$W_RECATTR):
 *   FIX    records of FAT$W_RSIZE bytes, back to back
 *   VAR    a 16-bit little-endian byte count, the record, one pad byte if the
 *          count is odd; a count of 0xFFFF means "no more records in this
 *          block" (the next record starts at the next 512-byte boundary)
 *   VFC    as VAR, but the count includes the fixed control area of
 *          FAT$B_VFCSIZE bytes (default 2) that precedes the data
 *   STM    records end in CR LF;  STMLF  in LF;  STMCR  in CR
 *   UDF    no records: the bytes as they are
 * Only sequential files have this layout; relative and indexed files keep
 * buckets and cells that this module does not decode.
 */
#ifndef ODS2_RECORDS_H
#define ODS2_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  rfm;     /* ODS2_RFM_* */
    uint8_t  rat;     /* ODS2_RAT_* (FAT$B_RATTRIB) */
    uint16_t rsize;   /* FAT$W_RSIZE (the record size of a FIX file) */
    uint16_t maxrec;  /* FAT$W_MAXREC */
    uint8_t  vfcsize; /* FAT$B_VFCSIZE (0 means 2) */
} ods2_rec_attrs_t;

/* Options */
#define ODS2_REC_CARRIAGE 0x01u /* apply FORTRAN / print-file carriage control (see the .c) */

typedef struct ods2_rec_decoder ods2_rec_decoder_t;

/* The decoder hands text to `sink` (false = the sink failed). */
ods2_rec_decoder_t *ods2_rec_new(const ods2_rec_attrs_t *attrs, unsigned options,
                                 bool (*sink)(void *ctx, const uint8_t *data, size_t n), void *ctx);
void ods2_rec_free(ods2_rec_decoder_t *d);

/* Feed the file's bytes in order, in pieces of any size. False: the sink failed or the data is
 * not a record stream (ods2_rec_problem says which; what was decoded before is already out). */
bool ods2_rec_feed(ods2_rec_decoder_t *d, const uint8_t *data, size_t n);
/* After the last piece: flushes an unterminated last record. */
bool ods2_rec_finish(ods2_rec_decoder_t *d);
/* NULL, or why feed/finish returned false (or a non-fatal note, e.g. a short last record) */
const char *ods2_rec_problem(const ods2_rec_decoder_t *d);
/* true if feed returned false because the data is not a record stream (not because the sink failed) */
bool ods2_rec_bad_data(const ods2_rec_decoder_t *d);
/* true if the format is one this module decodes (sequential FIX/VAR/VFC/STM/STMLF/STMCR) */
bool ods2_rec_decodable(unsigned fat_rtype);

#endif
