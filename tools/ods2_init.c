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
 *     --cluster=N             cluster factor (default by size), 1 to 16380 as VMS INITIALIZE; --cluster-over-limit
 *                             also allows 16381 to 16383 (V8.4 and V7.3-2 INITIALIZE refuse them, and mount such a volume; the VAX
 *                             V7.3 INITIALIZE takes 16381 and 16382, CORPUS 40.2),
 *                             and a cluster factor below ceil(blocks / (65535 x 4096)), which INITIALIZE refuses (HP's formula: the
 *                             storage bitmap, one bit per cluster, is limited to 65,535 blocks; V8.4, V7.3-2 and VAX V7.3 refuse
 *                             to mount such a volume, %MOUNT-F-FILESTRUCT, CORPUS 41, 43; the same size with the minimum cluster
 *                             mounts, 41.1, 43)
 *     --maxfiles=N            maximum number of files (clipped to the number of blocks with a note; a count above blocks / (cluster + 1) is kept, with a note that VMS INITIALIZE may not allow it; --maxfiles-over-limit keeps even a count above the blocks)
 *     --geometry=SECTORS,TRACKS       the disk geometry the storage control block records (default 32,16; 63,255 above 16 GiB)
 *     --security=profile|empty|zero   what SECURITY.SYS holds (default: the profile record VMS writes)
 *
 * The image file must not exist yet; nothing is ever written to an existing file.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* localtime_r(), tzset() */
#endif
#include <stdio.h>
#include <time.h>
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
                        "[--access-dates] [--no-special-files] [--nohighwater] [--cluster=N] [--maxfiles=N] [--maxfiles-over-limit] [--geometry=SECTORS,TRACKS | --target=ld|ra|scsi|axpbox|rz]\n"
                        "       <size> is blocks, or a number with K, M or G (KiB, MiB, GiB); 8192 to 2^32-16 blocks, cluster factor at most 16380 and at least blocks / 268,431,360 (rounded up)\n", argv[0]);
        return 2;
    }
    const char *geo_source = NULL;
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
        else if (starts(argv[i], "--target=")) {
            /* --target=ld|ra|ra82|ra92|rq|scsi|axpbox|rz: the geometry that device type reports for this size */
            bool measured = false;
            if (!ods2_target_geometry(argv[i] + 9, opt.blocks, &opt.sectors, &opt.tracks, &measured)) {
                fprintf(stderr, "unknown target %s (ld, ra, ra82, ra92, rq, scsi, axpbox, rz)\n", argv[i] + 9);
                return 2;
            }
            geo_source = argv[i] + 9;
            {
                char warn[900];
                if (ods2_target_geometry_warning(argv[i] + 9, opt.blocks, opt.sectors, opt.tracks, measured, warn, sizeof(warn))) {
                    fprintf(stderr, "ods2_init: %s\n", warn);
                }
            }
        }
        else if (starts(argv[i], "--geometry=")) {
            /* --geometry=SECTORS,TRACKS: the geometry the SCB records (cylinders follow from the size) */
            unsigned se = 0, tr = 0;
            if (sscanf(argv[i] + 11, "%u,%u", &se, &tr) != 2 || se == 0 || tr == 0 || se > 65535 || tr > 65535) {
                fprintf(stderr, "bad geometry: %s (use --geometry=SECTORS,TRACKS)\n", argv[i] + 11);
                return 2;
            }
            opt.sectors = se; opt.tracks = tr;
            geo_source = "--geometry";
        }
        else if (starts(argv[i], "--cluster=")) opt.cluster = (unsigned) atoi(argv[i] + 10);
        else if (strcmp(argv[i], "--cluster-over-limit") == 0) opt.cluster_over_limit = true;
        else if (strcmp(argv[i], "--maxfiles-over-limit") == 0) opt.maxfiles_over_limit = true;
        else if (strcmp(argv[i], "--security=profile") == 0) opt.security = ODS2_INIT_SECURITY_PROFILE;
        else if (strcmp(argv[i], "--security=empty") == 0) opt.security = ODS2_INIT_SECURITY_EMPTY;
        else if (strcmp(argv[i], "--security=zero") == 0) opt.security = ODS2_INIT_SECURITY_ZERO;
        else if (starts(argv[i], "--security-lw1=")) { opt.security_lw1_given = true; opt.security_lw1 = (uint32_t) strtoul(argv[i] + 15, NULL, 0); }
        else if (starts(argv[i], "--maxfiles=")) opt.maxfiles = (uint32_t) strtoul(argv[i] + 11, NULL, 10);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    r = ods2_initialize(argv[1], &opt);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-INITERR, %s: %s\n", argv[1], r.problem);
        return 1;
    }
    printf("%%ODS2-I-INITIALIZED, %s: %llu blocks, ODS-%u, label %s\n", argv[1], size, opt.level, opt.label);
    if (opt.maxfiles != 0) {
        uint32_t written = ods2_init_max_files_written(&opt), vms = ods2_init_max_files_limit(opt.blocks, opt.cluster);
        if (written < opt.maxfiles) {
            if (written == 16711679u) {                  /* every clip to the file-number limit, whatever the blocks and the option */
                fprintf(stderr, "ods2_init: --maxfiles=%u clipped to %u (the most a file number allows: --maxfiles-over-limit does not lift this)\n", (unsigned) opt.maxfiles, (unsigned) written);
            } else {
                fprintf(stderr, "ods2_init: --maxfiles=%u clipped to %u (a volume cannot have more files than blocks); --maxfiles-over-limit keeps it\n", (unsigned) opt.maxfiles, (unsigned) written);
            }
        } else if (written > opt.maxfiles) {
            fprintf(stderr, "ods2_init: --maxfiles=%u raised to %u (the minimum)\n", (unsigned) opt.maxfiles, (unsigned) written);
        }
        if (written > vms) {
            fprintf(stderr, "ods2_init: %u maximum files written; VMS INITIALIZE would allow at most %u here (floor(blocks / (cluster + 1)): measured at clusters 1, 3, 8 and 16 on 8,192 and 16,384 blocks, on Alpha V8.4 (ODS-2 and ODS-5) and VAX V7.3 (ODS-2), ODS-5 on Alpha V8.4 only, CORPUS 62 and 64; VMS still reads and writes volumes above it)\n",
                    (unsigned) written, (unsigned) vms);
        }
    }
    /* warnings: only for a volume that was made (a refused command prints none) */
    if (opt.cluster > 16380 && opt.cluster <= 16383 && opt.cluster_over_limit) {
        fprintf(stderr, "ods2_init: cluster factor %u: Alpha V8.4 and V7.3-2 INITIALIZE take 16,380 at most (%%INIT-F-BADCLUSTER above; "
                "VAX V7.3 INITIALIZE took 16,381 and 16,382 and refused 16,383, CORPUS 40.2); a volume with cluster 16,383 was mounted "
                "and written by V8.4 and V7.3-2 (ODS-2 and ODS-5) and by VAX V7.3 (ODS-2; its ODS-5 one was mounted, not written) (CORPUS 36.1, "
                "39.1, 40.2), ods2v2-made volumes with 16,381 and 16,382 were not tried, no Alpha INITIALIZE makes one\n", opt.cluster);
    }
    if (opt.cluster != 0 && opt.cluster < ods2_init_min_cluster(opt.blocks) && opt.cluster_over_limit) {
        fprintf(stderr, "ods2_init: cluster factor %u on %llu blocks: VMS INITIALIZE refuses less than %u (HP's formula: the "
                "storage bitmap, one bit per cluster, is limited to 65,535 blocks; VAX "
                "V7.3 INITIALIZE of 2^31-16 blocks, of the values tried, 1, 2, 4 and 8 refused and 9-16 accepted (11 not tried) and 17 and up refused, CORPUS 40.1); "
                "V8.4, V7.3-2 and VAX V7.3 refuse to mount such a volume (%%MOUNT-F-FILESTRUCT, CORPUS 41, 43); the same size with the minimum cluster mounts (41.1, 43)\n",
                opt.cluster, (unsigned long long) opt.blocks, ods2_init_min_cluster(opt.blocks));
    }
    if (opt.blocks > 2147475456ULL) {
        /* measured: V7.3-2 mounts 2,147,475,456 blocks (2^31 - 8192) and refuses one block more, ODS-2 and ODS-5, whoever made
           the volume (CORPUS 35.3). V8.4 mounts up to 4,261,348,350 blocks = 65534 x 255 x 255 and refuses more (CORPUS 35.1;
           HP, "OpenVMS V8.4 New Features and Documentation Overview", 2.4: "The precise new maximum volume size is given by:
           (65534 * 255 * 255) blocks"). VAX V7.3 mounts 2,147,483,632 blocks (2^31 - 16) and refuses 2,147,483,664 with
           %MOUNT-F-UNSUPPORTED; its limit between them was not searched (CORPUS 40.1). */
        fprintf(stderr, "ods2_init: %llu blocks: OpenVMS Alpha V7.3-2 mounts a volume of at most 2,147,475,456 blocks and refuses a "
                "bigger one (%%MOUNT-F-FILESTRUCT)%s; VAX V7.3 mounts 2,147,483,632 blocks and refuses 2,147,483,664 "
                "(%%MOUNT-F-UNSUPPORTED; its limit between them is not searched, CORPUS 40.1)\n", (unsigned long long) opt.blocks,
                opt.blocks > 4261348350ULL ? "" : "; V8.4 mounts it (up to 4,261,348,350 blocks)");
        if (opt.blocks > 4261348350ULL) {
            fprintf(stderr, "ods2_init: V8.4 mounts at most 4,261,348,350 blocks (65534 x 255 x 255) and refuses a bigger volume "
                    "the same way (%%MOUNT-F-FILESTRUCT): no VMS version measured mounts this size\n");
        }
    }
    {
        /* VMS keeps file dates in local time: the zone of the system the volume is for. A volume stamped in another zone
           than the VMS machine's gets dates ahead of its clock (ANALYZE: FUTCREDAT, FUTREVDAT) when that is the one
           that is behind. */
        const char *oz = getenv("ODS2_TZ");
        if (oz != NULL && oz[0] != '\0') {
            printf("%%ODS2-I-TIME, file dates are local time in the zone ODS2_TZ=%s\n", oz);
        } else {
            time_t now = time(NULL);
            struct tm lt;
            char abbr[32] = "?", off[16] = "?";
            tzset();
            if (localtime_r(&now, &lt) != NULL) {
                strftime(abbr, sizeof(abbr), "%Z", &lt);
                strftime(off, sizeof(off), "%z", &lt);
            }
            printf("%%ODS2-I-TIME, file dates are local time in this host's zone (%s, UTC%s): ODS2_TZ names the VMS "
                   "system's zone if it is another\n", abbr, off);
        }
    }
    {
        /* what the storage control block records: OpenVMS VAX V7.3 ANALYZE/DISK_STRUCTURE compares it with the device's
           own geometry (measured: only the matching one is accepted) */
        uint32_t sec = opt.sectors, trk = opt.tracks;
        unsigned long long per = 0, cyl;
        ods2_init_geometry(opt.blocks, &sec, &trk);
        per = (unsigned long long) sec * trk;
        cyl = (opt.blocks + per - 1) / per;
        printf("%%ODS2-I-GEOMETRY, the storage control block records %u sectors x %u tracks x %llu cylinders (%s)\n",
               sec, trk, cyl, geo_source != NULL ? geo_source : "the default");
        if (cyl > 65535 && geo_source != NULL) {
            fprintf(stderr, "ods2_init: %llu cylinders is more than 16 bits hold: a device with a 16-bit cylinder count (SIMH RQ) "
                    "wraps it and then draws CHKSCB (or INTDIV) on VAX V7.3; measure the device's own geometry\n", cyl);
        }
        if (geo_source == NULL) {
            printf("-ODS2-I-VAX, for an OpenVMS VAX V7.3 machine use --target=ld (LD driver), ra (SIMH RA82/RA92) or scsi: "
                   "its ANALYZE/DISK_STRUCTURE compares this with the device's own geometry; an existing image is fitted "
                   "with SET VOLUME/GEOMETRY=ld|ra|scsi|sectors,tracks\n");
        }
    }
    return 0;
}
