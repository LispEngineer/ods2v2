/*
 * MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
 *
 * ods2_deep_make - test tool: makes a tree of directories nested deeper than the 255 levels ods2v2 creates in
 * normal use (a VMS volume can hold more: V7.3-2 made 400), so that the readers (DIR [...], EXPORT, IMPORT) can be
 * tested on it. Built with -DODS2_FAULT_INJECTION, which lets ODS2_TEST_MAX_DIR_DEPTH raise the creation limit;
 * the ods2 that people run has no such hook.
 *
 *   ods2_deep_make <image> <levels> [file-level ...]
 *
 * makes [L1.L2. ... .Llevels] (each directory named L<n>) and, in each directory whose level is listed, a file
 * F<n>.TXT with the content "deep <n>\n". Paths this deep cannot be typed into the CLI (a command's path is limited
 * to 2,047 characters), which is why this tool works through the library.
 */
#include "ods2_volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    uint8_t header[512];
    ods2_result_t r;
    int levels, n, i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <levels> [file-level ...]\n", argv[0]);
        return 2;
    }
    levels = atoi(argv[2]);
    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) { fprintf(stderr, "mount: %s\n", r.problem); return 1; }
    r = ods2_read_header(&vol, 4, header);
    if (!r.ok) { fprintf(stderr, "root: %s\n", r.problem); return 1; }
    for (n = 1; n <= levels; n++) {
        char name[32], fname[32], content[32];
        ods2_fid_t fid;

        snprintf(name, sizeof(name), "L%d", n);
        r = ods2_create_directory(&vol, header, name, &fid);
        if (!r.ok) { fprintf(stderr, "level %d: %s\n", n, r.problem); return 1; }
        r = ods2_read_header_fid(&vol, fid, header);
        if (!r.ok) { fprintf(stderr, "level %d header: %s\n", n, r.problem); return 1; }
        for (i = 3; i < argc; i++) {
            if (atoi(argv[i]) == n) {
                ods2_fid_t ffid;
                snprintf(fname, sizeof(fname), "F%d.TXT", n);
                snprintf(content, sizeof(content), "deep %d\n", n);
                r = ods2_create_file(&vol, header, fname, (const uint8_t *) content, strlen(content), 5, &ffid);
                if (!r.ok) { fprintf(stderr, "file at level %d: %s\n", n, r.problem); return 1; }
            }
        }
    }
    ods2_sync(&vol);
    ods2_dismount(&vol);
    printf("made %d levels\n", levels);
    return 0;
}
