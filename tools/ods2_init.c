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

/* ods2_init.c - INITIALIZE for ods2v2: makes a new, empty ODS-2 or ODS-5 volume
 * in a new image file.
 *
 *   ods2_init <image> <size> [options]
 *     <size>                  blocks, or a number with K, M or G (KiB, MiB, GiB)
 *     --label=NAME            volume label (default: the file name, upper-cased, 12 characters)
 *     --structure=2|5         ODS-2 or ODS-5 (default 5)
 *     --hardlinks             HM2$M_HARDLINKS   (ODS-5)
 *     --access-dates          HM2$M_ACCESSTIMES (ODS-5)
 *     --no-special-files      HM2$M_NO_SPECIAL_FILES (ODS-5)
 *     --nohighwater           HM2$M_NOHIGHWATER
 *     --cluster=N             cluster factor (default by size)
 *     --maxfiles=N            maximum number of files
 *     --security=profile|empty|zero   what SECURITY.SYS holds (default: the profile record VMS writes)
 *
 * The image file must not exist yet; nothing is ever written to an existing file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ods2_init.h"
#include "ods5_ondisk.h"

static int starts(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

int main(int argc, char **argv)
{
    ods2_init_options_t opt;
    ods2_result_t r;
    char label[64];
    const char *base;
    char *end;
    int i;
    unsigned long long size;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <new-image> <size> [--label=NAME] [--structure=2|5] [--hardlinks] "
                        "[--access-dates] [--no-special-files] [--nohighwater] [--cluster=N] [--maxfiles=N]\n"
                        "       <size> is blocks, or a number with K, M or G (KiB, MiB, GiB)\n", argv[0]);
        return 2;
    }
    memset(&opt, 0, sizeof(opt));
    opt.level = ODS5_LEVEL_5;
    size = strtoull(argv[2], &end, 10);
    if (*end == 'K' || *end == 'k') { size *= 2; end++; }
    else if (*end == 'M' || *end == 'm') { size *= 2048; end++; }
    else if (*end == 'G' || *end == 'g') { size *= 2097152; end++; }
    if (*end != '\0' || size == 0) { fprintf(stderr, "bad size: %s\n", argv[2]); return 2; }
    opt.blocks = size;
    base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    {
        size_t k, n = 0;
        for (k = 0; base[k] && base[k] != '.' && n < 12; k++) {
            char ch = (char) toupper((unsigned char) base[k]);
            label[n++] = (isalnum((unsigned char) ch) || ch == '_' || ch == '-' || ch == '$') ? ch : '_';
        }
        label[n] = '\0';
    }
    opt.label = label;
    for (i = 3; i < argc; i++) {
        if (starts(argv[i], "--label=")) opt.label = argv[i] + 8;
        else if (starts(argv[i], "--structure=")) opt.level = (unsigned) atoi(argv[i] + 12);
        else if (strcmp(argv[i], "--hardlinks") == 0) opt.volchar |= ODS5_VC_HARDLINKS;
        else if (strcmp(argv[i], "--access-dates") == 0) opt.volchar |= ODS5_VC_ACCESSTIMES;
        else if (strcmp(argv[i], "--no-special-files") == 0) opt.volchar |= ODS5_VC_NO_SPECIAL_FILES;
        else if (strcmp(argv[i], "--nohighwater") == 0) opt.volchar |= ODS5_VC_NOHIGHWATER;
        else if (starts(argv[i], "--cluster=")) opt.cluster = (unsigned) atoi(argv[i] + 10);
        else if (strcmp(argv[i], "--security=profile") == 0) opt.security = ODS2_INIT_SECURITY_PROFILE;
        else if (strcmp(argv[i], "--security=empty") == 0) opt.security = ODS2_INIT_SECURITY_EMPTY;
        else if (strcmp(argv[i], "--security=zero") == 0) opt.security = ODS2_INIT_SECURITY_ZERO;
        else if (starts(argv[i], "--maxfiles=")) opt.maxfiles = (uint32_t) strtoul(argv[i] + 11, NULL, 10);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    r = ods2_initialize(argv[1], &opt);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-INITERR, %s: %s\n", argv[1], r.problem);
        return 1;
    }
    printf("%%ODS2-I-INITIALIZED, %s: %llu blocks, ODS-%u, label %s\n", argv[1], size, opt.level, opt.label);
    return 0;
}
