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

/* ods2_make_loop - test helper for cli_dir_loop_check.sh: adds a
 * directory entry that points at another existing directory, which
 * the ods2 CLI cannot do (it never creates aliases or loops).
 *
 *   ods2_make_loop <disk-image> <dir-path> <NAME> <target-dir-path>
 *
 * adds NAME.DIR;1 to [dir-path], with the FID of [target-dir-path].
 * Paths are VMS directory paths without brackets ("" is the root).
 */

#include <stdio.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid, target_fid;
    char name[96];

    if (argc != 5) {
        fprintf(stderr, "usage: %s <disk-image> <dir-path> <NAME> <target-dir-path>\n",
                argv[0]);
        return 2;
    }
    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) { fprintf(stderr, "mount: %s\n", r.problem); return 1; }
    r = ods2_lookup_path(&vol, argv[2], &dir_fid);
    if (!r.ok) { fprintf(stderr, "%s: %s\n", argv[2], r.problem); return 1; }
    r = ods2_lookup_path(&vol, argv[4], &target_fid);
    if (!r.ok) { fprintf(stderr, "%s: %s\n", argv[4], r.problem); return 1; }
    snprintf(name, sizeof(name), "%s.DIR", argv[3]);
    r = ods2_insert_into_directory(&vol, dir_fid.fid_num, name, 1, target_fid);
    if (!r.ok) { fprintf(stderr, "insert: %s\n", r.problem); return 1; }
    ods2_dismount(&vol);
    return 0;
}
