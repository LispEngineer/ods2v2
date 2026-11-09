/*
 * MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
 *
 * ods2_filedump.c - every file of a disk image, by file header, as ods2v2 reads it: for comparison with another
 * system's listing of the same volume (tests/axp1_compare.py). One record per valid primary header, in file
 * number order, on stdout:
 *
 *   F <number> <seq> <level> <filechar hex> <rtype> <rattrib> <rsize> <maxrec> <vfcsize> <hiblk> <efblk>
 *     <ffbyte> <content bytes> <status>\n  followed by the content bytes (status OK only) and a newline.
 *
 * status: OK (content follows), DIR (a directory: no content), MARKED (marked for delete, FID intact),
 * ERR:<message> (the content could not be read: no content). Usage: ods2_filedump <image> [--no-content]
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ods2_volume.h"
#include "ods2_checksum.h"

static bool sink(void *ctx, const uint8_t *data, size_t n)
{
    (void) ctx;
    return fwrite(data, 1, n, stdout) == n;
}

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    uint8_t h1[512], h[512];
    ods2_result_t r;
    unsigned n, first, last;
    bool content = !(argc > 2 && strcmp(argv[2], "--no-content") == 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [--no-content]\n", argv[0]);
        return 2;
    }
    r = ods2_mount(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: %s\n", argv[1], r.problem);
        return 1;
    }
    r = ods2_read_header(&vol, 1, h1);
    if (!r.ok) {
        fprintf(stderr, "INDEXF.SYS header: %s\n", r.problem);
        return 1;
    }
    /* the headers INDEXF.SYS's end-of-file covers: VBN cluster*4 + IBMAPSIZE + n, n from 1 */
    first = 1;
    last = ods2_word_swap32(((const ods2_head_core_t *) h1)->recattr.efblk) - 1 - vol.home.cluster * 4 - vol.home.ibmapsize;
    for (n = first; n <= last; n++) {
        const ods2_head_core_t *c = (const ods2_head_core_t *) h;
        const char *status = "OK";
        size_t len = 0;
        uint16_t stored;
        bool is_dir;

        if (!ods2_read_header(&vol, n, h).ok) continue;
        stored = (uint16_t) (h[510] | (h[511] << 8));
        if (stored != ods2_checksum(h, 255) || ods2_fid_number(c->fid) != n || c->seg_num != 0) continue;
        is_dir = (c->filechar & 0x2000u) != 0;
        if (c->filechar & 0x8000u) status = "MARKED";
        else if (is_dir) status = "DIR";
        len = ods2_file_content_length(h);
        if (strcmp(status, "OK") == 0 && !content) status = "NOCONTENT";
        printf("F %u %u %u %x %u %u %u %u %u %u %u %u %zu ", n, (unsigned) c->fid.fid_seq, (unsigned) (c->struclev >> 8),
               (unsigned) c->filechar, (unsigned) c->recattr.rtype, (unsigned) c->recattr.rattrib,
               (unsigned) c->recattr.rsize, (unsigned) c->recattr.maxrec, (unsigned) c->recattr.vfcsize,
               (unsigned) ods2_word_swap32(c->recattr.hiblk), (unsigned) ods2_word_swap32(c->recattr.efblk),
               (unsigned) c->recattr.ffbyte, len);
        if (strcmp(status, "OK") == 0) {
            /* the length is known; the content follows only if it can be read whole */
            size_t got = 0;
            bool failed = false;
            fputs("OK\n", stdout);
            fflush(stdout);
            r = ods2_read_file_stream(&vol, h, sink, NULL, &got, &failed);
            if (!r.ok || got != len) {
                /* the header said OK and some bytes may be out: pad so the reader stays in step */
                size_t pad = len - got;
                static const uint8_t zero[4096];
                fprintf(stderr, "file %u: %s (%zu of %zu bytes)\n", n, r.ok ? "short" : r.problem, got, len);
                while (pad > 0) {
                    size_t k = pad < sizeof(zero) ? pad : sizeof(zero);
                    fwrite(zero, 1, k, stdout);
                    pad -= k;
                }
                printf("\nE %u\n", n);
                continue;
            }
            fputc('\n', stdout);
        } else {
            printf("%s\n", status);
        }
    }
    ods2_dismount(&vol);
    return 0;
}
