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

/* ods2.c - a unified, DCL-style command-line interface for reading
 * and writing ODS-2 disk images. Wraps everything built in ods2v2
 * into one easy-to-use tool, rather than the separate single-purpose
 * tools (ods2_ls, ods2_mkdir, ods2_put, ods2_cat, ods2_rm) used
 * during development - those remain as diagnostic building blocks,
 * this is the intended day-to-day interface.
 *
 * Usage:
 *   ods2 <disk-image>                       interactive shell
 *   ods2 <disk-image> <command...>          run one command, exit
 *   ods2 <disk-image> @<script-file>        run commands from a file,
 *                                            one per line
 *
 * Exit status: 0 on success; 1 if the disk image couldn't be mounted,
 * or if the command (single-shot) or any command in the script failed
 * - i.e. reported a %ODS2-E- error. A script runs to the end either
 * way. Warnings (%ODS2-W-, e.g. DIR finding no files) don't count as
 * failure.
 *
 * Commands (case-insensitive; VMS bracket-notation paths throughout,
 * e.g. [DECUS.NETLIB020]FILE.TXT):
 *   DIR [path] [wildcard]         list a directory
 *   DIR [path...] [wildcard]      list a directory and everything
 *                                 beneath it, recursively (VMS's
 *                                 "..." notation)
 *   DIR [*...] [wildcard]         directory wildcards: '*' and '%' in
 *                                 any directory name, "..." anywhere
 *                                 (e.g. [*], [*.SRC], [...SALES],
 *                                 [DECUS...SRC]) - as VMS DIRECTORY
 *   CREATE/DIRECTORY path         create a directory
 *   COPY <local-file> <path>      write a local file onto the disk,
 *                                 as a Stream_LF file
 *   COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) <local-file> <path>
 *                                 ... with chosen RMS record attributes,
 *                                 in SET FILE/ATTRIBUTES syntax (see
 *                                 parse_copy_qualifiers())
 *   COPY/CONVERT <local-file> <path>
 *                                 a text file as variable-length
 *                                 records (RFM:VAR, RAT:CR), one per
 *                                 line - VMS's native text format
 *   TYPE path                     print a file's content
 *   DELETE path                   delete a file (one version) or an
 *                                 empty directory
 *   SET DEFAULT path              set the current default directory
 *   SHOW DEFAULT                  show the current default directory
 *   SET VOLUME/LABEL=name         relabel the volume: every home block
 *                                 copy, checksums recomputed
 *   SHOW VOLUME                   label, owner, cluster size, maximum
 *                                 files, total and free blocks
 *   HELP                          show this command list
 *   EXIT / QUIT                   leave the interactive shell
 *
 * Paths starting with '.' or '-' inside brackets (e.g. [.NETLIB020],
 * [-], [-.SIBLING]) and bare filenames with no brackets at all are
 * relative to the current default directory (see SET DEFAULT); '-'
 * means "go up one level" (repeat as "-.-" for more); other
 * bracketed paths are always absolute from root, matching real VMS.
 *
 * A directory spec beginning with "..." is relative too: [...] is the
 * default directory and everything below it; [*...] is every
 * directory on the disk except root's own (use [000000...] for that).
 * Only DIR accepts wildcard directories; the other commands act on
 * one directory and refuse them.
 *
 * File versions follow VMS rules: NAME;3 is version 3, NAME; or NAME;0
 * or plain NAME is the highest version, NAME;-1 the next lower
 * existing version, and NAME;* every version (DIR and DELETE only).
 * DIR lists every version unless a version is given.
 *
 * Interactive mode uses a vendored copy of linenoise (BSD licensed,
 * single .c/.h file, see third_party/linenoise/) for line editing and
 * command history (up/down arrows, Ctrl-A/E/W, etc.) - chosen over
 * GNU Readline (GPL - licensing friction) and over libedit (needs a
 * separate system install: `apt install libedit-dev` on Debian/
 * Ubuntu, which isn't preinstalled everywhere and reintroduces a
 * build-environment dependency this project has otherwise avoided
 * completely). Vendoring the source directly keeps `make` working
 * with zero external dependencies on every platform, matching the
 * rest of this project - `gcc file.c -o binary` has always just
 * worked here, and this keeps that true. The one real trade-off:
 * vanilla linenoise does not support Ctrl-R reverse search (GNU
 * Readline and libedit both do) - accepted deliberately in exchange
 * for zero-dependency builds. */
#include "linenoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "ods2_volume.h"
#include "ods2_path.h"
#include "ods2_wildcard.h"
#include "ods2_directory_write.h"
#include "ods2_text_var.h"
#include "ods2_records.h"
#include "ods2_acl.h"
#include "ods2_validate.h"
#include "ods5_ondisk.h"
#include "ods5_names.h"
#include "ods5_header.h"

#define MAX_LINE 1024
#define MAX_TOKENS 8

/* Current default directory for this session (SET DEFAULT), dot-
   separated with no brackets - "" means root. Bare filenames (no
   brackets at all) and VMS's "[.SUBDIR]" relative notation both
   resolve against this. A plain global is fine here: this is a
   single-threaded, single-volume interactive tool - there is exactly
   one "current session" for it to belong to. */
static char g_default_dir[ODS2_PATH_MAX] = "";

/* ODS-5 volumes keep the case of names, and show names the way DCL does
   (^-escapes); g_ods5 is set when the mounted volume is ODS-5.
   g_utf8_names (DIR/UTF8, SET NAMES UTF8, ODS2_NAMES=utf8) shows the
   names as UTF-8 instead, for use on Linux. */
static bool g_ods5 = false;
static bool g_utf8_names = false;
/* DIR/PATHS: one "dir/dir/NAME.TYP;ver" line per file, UTF-8, '/' between
   the names, nothing else - for scripts and for comparing listings. */
static bool g_paths_mode = false;
static bool g_paths_fid = false;   /* DIR/PATHS/FID: "\t<file number>,<sequence>" after each line */
/* DIR/FULL: a block of details per file (id, size, owner, dates, the
   ODS-5 fields) instead of one line. */
static bool g_full_mode = false;
/* While EXPORT runs, each entry DIR would list is handed to this instead
   (and not printed). */
typedef void (*entry_hook_fn)(void *ctx, ods2_volume_t *vol, const char *dir_spelling,
                              const ods2_dir_entry_t *entry);
static entry_hook_fn g_entry_hook = NULL;
static void *g_entry_hook_ctx = NULL;

/* Set by report_error(): some command since the last reset failed.
   main() turns it into the exit status of a single-shot command or an
   @script, so scripts and automation can tell success from failure
   without parsing messages. */
static bool g_command_failed = false;

/* Prints an error message (%ODS2-E-...) to stderr and records that the
   current command failed. Every error a command reports goes through
   here; warnings (-W-) and informational messages (-I-) do not. */
static void report_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    g_command_failed = true;
}

/* Case-insensitive equality check. */
static bool matches(const char *token, const char *name)
{
    size_t i;
    for (i = 0; token[i] && name[i]; i++) {
        if (toupper((unsigned char) token[i]) != toupper((unsigned char) name[i])) {
            return false;
        }
    }
    return token[i] == '\0' && name[i] == '\0';
}

/* Equality of two directory entry names as the directory itself compares them: case-blind
   with the VMS folding of ISO Latin-1 and UCS-2 letters (ods2_dir_name_compare). */
static bool same_name(const char *a, const char *b)
{
    return ods2_dir_name_compare(a, strlen(a), b, strlen(b)) == 0;
}

/* Formats a parsed path's filename plus its ";version" exactly as
   typed (";", ";*", ";3", ";-1"), for messages. */
static void format_file_spec(const ods2_parsed_path_t *p, char *out, size_t out_size)
{
    if (!p->has_version) {
        snprintf(out, out_size, "%s", p->filename);
    } else if (p->version_wildcard) {
        snprintf(out, out_size, "%s;*", p->filename);
    } else if (p->version == 0) {
        snprintf(out, out_size, "%s;", p->filename);
    } else {
        snprintf(out, out_size, "%s;%d", p->filename, p->version);
    }
}

/* Commands that act on one directory refuse a wildcard or ellipsis
   in it with a clear message, rather than looking up "*" as a
   literal name or quietly ignoring a "...". Returns true if refused. */
static bool refuse_wild_dir(const ods2_parsed_path_t *p, const char *command, const char *arg)
{
    if (!p->dir_wildcard) return false;
    report_error("%%ODS2-E-WILDDIR, %s: %s does not accept a wildcard or \"...\" "
                    "in the directory\n", arg, command);
    return true;
}

/* Combines a parsed path's dir_path/relative flag with the current
 * session default into the actual, absolute (from-root) path to look
 * up - e.g. with g_default_dir="DECUS": a bare "FILE.TXT" (relative,
 * empty dir_path) resolves to "DECUS"; "[.NETLIB020]" (relative,
 * dir_path="NETLIB020") resolves to "DECUS.NETLIB020"; an explicit
 * "[SOMEWHERE]" (not relative) resolves to itself unchanged,
 * regardless of the current default - matching real VMS, where a
 * plain bracketed path is always absolute unless it starts with '.'.
 */
/* Uppercases a path string in place - real VMS storage is always
   uppercase, but a user can type a path in any case. Kept as a small,
   local duplicate of ods2_volume.c's own uppercase_str() rather than
   exported/shared across the library/CLI boundary for something this
   trivial. */
static void uppercase_display_path(char *s)
{
    for (; *s; s++) {
        *s = (char) toupper((unsigned char) *s);
    }
}

/* Combines a parsed path's dir_path/relative/up_levels flags with the
 * current session default into the actual, absolute (from-root) path
 * to look up - e.g. with g_default_dir="DECUS": a bare "FILE.TXT"
 * (relative, empty dir_path) resolves to "DECUS"; "[.NETLIB020]"
 * (relative, dir_path="NETLIB020") resolves to "DECUS.NETLIB020"; an
 * explicit "[SOMEWHERE]" (not relative) resolves to itself unchanged,
 * regardless of the current default - matching real VMS, where a
 * plain bracketed path is always absolute unless it starts with '.'
 * or '-'. VMS's "[-]" up-level notation strips that many trailing
 * dot-separated components from the current default first (going up
 * past root simply clamps at root).
 *
 * The result is always uppercased before returning: real VMS storage
 * is always uppercase (see uppercase_str() in ods2_volume.c), but a
 * user can type a path in any case - without this, a lowercase-typed
 * path would display and recurse in whatever case was typed instead
 * of the actual, canonical stored form. 
 */
static void resolve_effective_path(const ods2_parsed_path_t *p, char *out, size_t out_size)
{
    char base[ODS2_PATH_MAX];
    strncpy(base, g_default_dir, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    if (p->up_levels > 0) {
        int levels_to_strip = p->up_levels;
        while (levels_to_strip > 0 && base[0] != '\0') {
            char *last_dot = (char *) ods5_rfind_unescaped(base, '.');
            if (last_dot != NULL) {
                *last_dot = '\0';
            } else {
                base[0] = '\0'; /* only one component left - up from here is root */
            }
            levels_to_strip--;
        }
        /* levels_to_strip still > 0 here means "up" was requested
           past root - base is already "" (root), which is the
           correct, clamped result. */
    }

    if (p->relative && base[0] != '\0') {
        if (p->dir_path[0] != '\0') {
            snprintf(out, out_size, "%s.%s", base, p->dir_path);
        } else {
            snprintf(out, out_size, "%s", base);
        }
    } else {
        /* Either an explicit absolute path, or relative but the
           current default is already root - both cases just use
           dir_path directly. */
        snprintf(out, out_size, "%s", p->dir_path);
    }

    if (!g_ods5) uppercase_display_path(out);
}

/* Resolves a parsed path's directory component to a header. Every
 * command needs at least the directory part resolved. */
static ods2_result_t resolve_dir(ods2_volume_t *vol, const char *dir_path,
                                  uint8_t *dir_header_out, ods2_fid_t *dir_fid_out)
{
    ods2_result_t r = ods2_lookup_path(vol, dir_path, dir_fid_out);
    if (!r.ok) return r;
    return ods2_read_header(vol, dir_fid_out->fid_num, dir_header_out);
}

/* Splits a dot-separated dir_path into its parent path and its own
 * last component name - e.g. "DECUS.NETLIB020" -> parent="DECUS",
 * name="NETLIB020". Used both when creating a directory (its own
 * name is the last component of where it will live) and when
 * deleting one referenced as a bare path like "[TESTDIR]" (no
 * filename part - the directory itself is the target, one level up
 * from where ods2_lookup_path would otherwise resolve it). `out_buf`
 * must be at least ODS2_PATH_MAX bytes; `*parent_out` and `*name_out`
 * point into it after the call.
 */
static void split_last_component(const char *dir_path, char *out_buf,
                                  const char **parent_out, const char **name_out)
{
    char *last_dot;
    strcpy(out_buf, dir_path);
    last_dot = (char *) ods5_rfind_unescaped(out_buf, '.'); /* an escaped ^. is part of a name */
    if (last_dot != NULL) {
        *last_dot = '\0';
        *parent_out = out_buf;
        *name_out = last_dot + 1;
    } else {
        *parent_out = "";
        *name_out = out_buf;
    }
}

/* Does entry `i` of a directory listing pass a DIR version filter?
   `rank` is its position among its name's versions (0 = highest,
   since they are stored newest first). */
static bool version_selected(const ods2_parsed_path_t *p, const ods2_dir_entry_t *e, int rank)
{
    if (!p->has_version || p->version_wildcard) return true; /* all versions */
    if (p->version > 0) return e->version == (unsigned) p->version;
    return rank == -p->version; /* 0 = highest, -1 = next lower, ... */
}

/* A file name as shown to the user: with VMS's ^-escapes, as DCL
   writes it (a space is ^_, a dot that is not the type delimiter is ^.,
   a character above U+00FF is ^Uxxxx), or as plain UTF-8 under /UTF8. */
static void display_name(const char *literal, char *out, size_t cap)
{
    if (g_utf8_names ||
        ods5_escape_name(literal, ODS5_ESC_KEEP_LAST_DOT, out, cap) == (size_t) -1) {
        /* plain UTF-8 must not carry controls to the terminal: %XX (ods5_escape_for_linux) */
        if (ods5_escape_for_linux(literal, out, cap) == (size_t) -1) snprintf(out, cap, "%s", "(name too long)");
    }
}

/* The names of a bracketed path (dot-separated spelling) joined with '/',
   as UTF-8 literals: "Lots^.Of^.Periods.X" -> "Lots.Of.Periods/X". */
static void spelling_to_slash_path(const char *spelling, char *out, size_t cap)
{
    const char *p = spelling;
    size_t len = 0;

    out[0] = '\0';
    while (*p) {
        const char *end = p;
        char literal[ODS2_PATH_MAX];
        size_t ul;
        while (*end && *end != '.') {
            if (*end == '^' && end[1] != '\0') end++;
            end++;
        }
        ul = ods5_unescape(p, (size_t) (end - p), literal, sizeof(literal));
        if (ul == (size_t) -1) snprintf(literal, sizeof(literal), "%.*s", (int) (end - p), p);
        {   /* a '/' or a control character inside a name: %XX, as EXPORT does */
            char safe[ODS2_PATH_MAX];
            if (ods5_escape_for_linux(literal, safe, sizeof(safe)) != (size_t) -1) {
                snprintf(literal, sizeof(literal), "%s", safe);
            }
        }
        len += (size_t) snprintf(out + len, cap - len, "/%s", literal);
        if (len >= cap) { out[cap - 1] = '\0'; return; }
        p = end;
        if (*p == '.') p++;
    }
}

static const char *rfm_name(unsigned rtype)
{
    static const char *names[] = { "Undefined", "Fixed length", "Variable length",
                                   "VFC", "Stream", "Stream_LF", "Stream_CR" };
    return rtype < 7 ? names[rtype] : "(unknown)";
}

static const char *org_name(unsigned org)
{
    static const char *names[] = { "Sequential", "Relative", "Indexed", "Direct", "Special" };
    return org < 5 ? names[org] : "(unknown)";
}

static void protection_text(unsigned nibble, char *out)
{
    /* a set bit DENIES: bit 0 read, 1 write, 2 execute, 3 delete */
    size_t n = 0;
    if (!(nibble & 1)) out[n++] = 'R';
    if (!(nibble & 2)) out[n++] = 'W';
    if (!(nibble & 4)) out[n++] = 'E';
    if (!(nibble & 8)) out[n++] = 'D';
    out[n] = '\0';
}

/* The ACL lines of DIR/FULL: the first after the label, the rest under it, as VMS lays them out. */
typedef struct { int n; } acl_print_t;
static void acl_print_line(void *ctx, const char *text)
{
    acl_print_t *a = (acl_print_t *) ctx;
    printf("  %s%s\n", a->n == 0 ? "Access Cntrl List:  " : "                    ", text);
    a->n++;
}

/* DIR/FULL for one entry: what the file header says. */
static void print_full_entry(ods2_volume_t *vol, const ods2_dir_entry_t *e)
{
    uint8_t header[512];
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    ods5_ident_t id;
    const char *why = "";
    char shown[ODS2_PATH_MAX], t[64], s_[8], o_[8], g_[8], w_[8];
    uint32_t hiblk, efblk;
    unsigned special;
    ods2_result_t r = ods2_read_header(vol, e->fid.fid_num, header);

    display_name(e->name, shown, sizeof(shown));
    printf("\n%s;%u\n", shown, e->version);
    if (!r.ok) {
        printf("  (header not readable: %s)\n", r.problem);
        return;
    }
    if (!ods2_header_is_file(header, e->fid)) {
        printf("  no such file (the entry's file, ID (%u,%u,%u), no longer exists)\n",
               (unsigned) e->fid.fid_num, (unsigned) e->fid.fid_seq, (unsigned) e->fid.fid_rvn);
        return;
    }
    printf("  File ID:            (%u,%u,%u)\n", (unsigned) e->fid.fid_num,
           (unsigned) e->fid.fid_seq, (unsigned) e->fid.fid_rvn);
    if (!ods5_read_ident(header, &id, &why)) {
        printf("  (ident area not readable: %s)\n", why);
        return;
    }
    if (strcmp(id.name, e->name) != 0 && id.name[0] != '\0') {
        /* an alias or hard link: the header names the primary link */
        display_name(id.name, shown, sizeof(shown));
        printf("  Primary link name:  %s;%u\n", shown, id.version);
    }
    hiblk = ods2_word_swap32(core->recattr.hiblk);
    efblk = ods2_word_swap32(core->recattr.efblk);
    (void) efblk;
    printf("  Size:               %zu/%u blocks, %zu bytes\n",
           (ods2_file_content_length(header) + 511) / 512, (unsigned) hiblk,
           ods2_file_content_length(header));
    printf("  Owner:              [%o,%o]\n", core->fileowner_uic[1], core->fileowner_uic[0]);
    protection_text((core->fileprot >> 0) & 15, s_);
    protection_text((core->fileprot >> 4) & 15, o_);
    protection_text((core->fileprot >> 8) & 15, g_);
    protection_text((core->fileprot >> 12) & 15, w_);
    printf("  Protection:         (S:%s, O:%s, G:%s, W:%s)\n", s_, o_, g_, w_);
    {
        /* the ACEs lie in the primary header and, when they do not fit there, in its extension
           headers (the primary's first, as VMS lists them) */
        acl_print_t ap = { 0 };
        uint8_t ext[512];
        const uint8_t *h = header;
        int guard;
        for (guard = 0; guard < 32; guard++) {
            const ods2_head_core_t *hc = (const ods2_head_core_t *) h;
            unsigned next = hc->ext_fid.fid_num | ((unsigned) hc->ext_fid.fid_nmx << 16);
            ods2_acl_walk(h, acl_print_line, &ap);
            if (next == 0 || !ods2_read_header(vol, next, ext).ok ||
                ((const ods2_head_core_t *) ext)->fid.fid_num != hc->ext_fid.fid_num) break;
            h = ext;
        }
    }
    ods5_format_vms_time(id.credate, t, sizeof(t));
    printf("  Created:            %s\n", t);
    /* ODS-5: the header's revision date is the data modification date; what VMS
       shows as "Modified" is the later of it and the attribute-change date */
    ods5_format_vms_time(id.has_ods5_dates && id.attdate > id.revdate ? id.attdate : id.revdate,
                         t, sizeof(t));
    printf("  Revised:            %s (%u)\n", t, (unsigned) id.revision);
    ods5_format_vms_time(id.expdate, t, sizeof(t));
    printf("  Expires:            %s\n", t);
    ods5_format_vms_time(id.bakdate, t, sizeof(t));
    printf("  Backup:             %s\n", id.bakdate ? t : "<no backup recorded>");
    if (id.has_ods5_dates) {
        ods5_format_vms_time(id.accdate, t, sizeof(t));
        printf("  Accessed:           %s\n", t);
        ods5_format_vms_time(id.attdate, t, sizeof(t));
        printf("  Attributes changed: %s\n", t);
        ods5_format_vms_time(id.revdate, t, sizeof(t));
        printf("  Data modified:      %s\n", t);
    }
    /* VMS prints "Linkcount" for every file on every volume: the header's count on a HARDLINKS volume,
       1 elsewhere, aliases included (batch m1: header count 0, DIRECTORY/FULL says 1) */
    printf("  Link count:         %u\n", (vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS))
                                            ? ods5_link_count(header) : 1u);
    special = ods5_special_type(header);
    printf("  File organization:  %s\n", org_name(core->recattr.rtype >> 4));
    if (special == 0) {
        /* as VMS words them: Fixed length N byte records / VFC, N byte header, maximum M bytes,
           longest L bytes; then the record attributes */
        unsigned rfm = core->recattr.rtype & 15u, rat = core->recattr.rattrib;
        if (rfm == ODS2_RFM_FIX) {
            printf("  Record format:      Fixed length %u byte records\n",
                   (unsigned) (core->recattr.rsize ? core->recattr.rsize : core->recattr.maxrec));
        } else if (rfm == ODS2_RFM_VFC) {
            printf("  Record format:      VFC, %u byte header, maximum %u bytes, longest %u byte%s\n",
                   (unsigned) (core->recattr.vfcsize ? core->recattr.vfcsize : 2u),
                   (unsigned) core->recattr.maxrec, (unsigned) core->recattr.rsize,
                   core->recattr.rsize == 1 ? "" : "s");
        } else {
            printf("  Record format:      %s, maximum %u bytes, longest %u byte%s\n", rfm_name(rfm),
                   (unsigned) core->recattr.maxrec, (unsigned) core->recattr.rsize,
                   core->recattr.rsize == 1 ? "" : "s");
        }
        {
            const char *cc = (rat & ODS2_RAT_FTN) ? "Fortran carriage control"
                           : (rat & ODS2_RAT_CR) ? "Carriage return carriage control"
                           : (rat & ODS2_RAT_PRN) ? "Print file carriage control" : NULL;
            if (rat == 0) printf("  Record attributes:  None\n");
            else printf("  Record attributes:  %s%s\n", cc ? cc : "No carriage control",
                        (rat & ODS2_RAT_BLK) ? ", Non-spanned" : "");
        }
    } else {
        static const char *kinds[] = { "?", "FIFO", "character special", "block special",
                                       "symbolic link (before V8.2)", "symbolic link" };
        printf("  Special file:       %s\n", special < 6 ? kinds[special] : "(unknown type)");
        if (special == ODS5_FAT_SYMBOLIC_LINK || special == ODS5_FAT_SYMLINK) {
            uint8_t target[1100];
            size_t got = 0, len = ods2_file_content_length(header);
            if (len > 0 && len < sizeof(target) - 1 &&
                ods2_read_file(vol, header, target, sizeof(target) - 1, &got).ok) {
                char lit[2400];
                target[got] = '\0';
                if (ods5_isl1_to_utf8(target, got, lit, sizeof(lit)) != (size_t) -1) {
                    printf("  Link target:        %s\n", lit);
                }
            }
        }
    }
}

/* One DIR line: the name padded to 20 characters, then ";version". */
static void print_listed_name(const char *literal, unsigned version)
{
    char shown[ODS2_PATH_MAX];
    size_t width;

    display_name(literal, shown, sizeof(shown));
    width = ods5_utf8_length(shown);
    printf("%s%*s;%u\n", shown, (width < 20) ? (int) (20 - width) : 0, "", version);
}

/* Lists the entries of one directory that match `pattern` and the
 * version filter, VMS DIRECTORY style: the "Directory [path]" heading,
 * the entries and a "Total of N files." line. Like VMS, prints nothing
 * at all for a directory with no matching entries, so a listing across
 * many directories shows only the ones that have something to show.
 * `dir_path` is absolute (from root). Returns the number shown. */
static int show_directory(ods2_volume_t *vol, const char *dir_path, const ods2_dir_entry_t *entries, int count,
                          const char *pattern, const ods2_parsed_path_t *p)
{
    int i, shown = 0, rank = 0;

    /* Every version of a name is its own entry, newest first and
       contiguous (a name's versions may span several records). */
    for (i = 0; i < count; i++) {
        rank = (i > 0 && same_name(entries[i].name, entries[i - 1].name)) ? rank + 1 : 0;
        if (ods2_wildcard_match(pattern, entries[i].name) &&
            version_selected(p, &entries[i], rank)) {
            if (g_entry_hook != NULL) {
                g_entry_hook(g_entry_hook_ctx, vol, dir_path, &entries[i]);
                shown++;
                continue;
            }
            if (g_paths_mode) {
                char slash[ODS2_PATH_MAX];
                spelling_to_slash_path(dir_path, slash, sizeof(slash));
                {
                    /* a '/' inside a name would read as a separator, a control character
                       would reach the terminal: %XX, as EXPORT does */
                    char safe[ODS2_DIR_MAX_NAME * 3 + 8];
                    if (ods5_escape_for_linux(entries[i].name, safe, sizeof(safe)) == (size_t) -1) {
                        snprintf(safe, sizeof(safe), "%s", "(name too long)");
                    }
                    printf("%s/%s;%u", slash, safe, entries[i].version);
                }
                if (g_paths_fid) {
                    printf("\t%u,%u,%u", (unsigned) entries[i].fid.fid_num,
                           (unsigned) entries[i].fid.fid_seq, (unsigned) entries[i].fid.fid_rvn);
                }
                printf("\n");
                shown++;
                continue;
            }
            if (shown == 0) {
                printf("\nDirectory [%s%s]\n\n", (dir_path[0] == '\0') ? "000000" : "",
                       dir_path);
            }
            if (g_full_mode) {
                print_full_entry(vol, &entries[i]);
            } else {
                print_listed_name(entries[i].name, entries[i].version);
            }
            shown++;
        }
    }
    if (shown > 0 && !g_paths_mode && g_entry_hook == NULL) {
        printf("\nTotal of %d file%s.\n", shown, (shown == 1) ? "" : "s");
    }
    return shown;
}

/* Rewrites a directory path (dot-separated spelling, as typed) with the
 * names as they are stored: on an ODS-5 volume the case of each
 * directory is kept, and DCL shows it, whatever case was typed. Every
 * component is looked up in its parent; if one is not found the path is
 * left as it is (the lookup that follows reports the error). */
static void canonical_dir_path(ods2_volume_t *vol, char *path, size_t cap)
{
    char result[ODS2_PATH_MAX], piece[ODS2_PATH_MAX], literal[ODS2_DIR_MAX_NAME + 8];
    char wanted[ODS2_DIR_MAX_NAME + 16], shown[ODS2_PATH_MAX];
    const char *p = path;
    ods2_fid_t fid;
    uint8_t header[512];
    size_t rlen = 0;

    if (path[0] == '\0' || !g_ods5) return;
    fid.fid_num = 4; fid.fid_seq = 4; fid.fid_rvn = 0; fid.fid_nmx = 0;
    result[0] = '\0';
    while (*p) {
        const char *end = p;
        size_t plen, ulen;
        ods2_dir_entry_t *entries = NULL;
        int count = 0, i;
        bool found = false;

        while (*end && *end != '.') {
            if (*end == '^' && end[1] != '\0') end++;
            end++;
        }
        plen = (size_t) (end - p);
        if (plen >= sizeof(piece)) return;
        memcpy(piece, p, plen);
        piece[plen] = '\0';
        ulen = ods5_unescape(piece, plen, literal, sizeof(literal) - 5);
        if (ulen == (size_t) -1) return;
        snprintf(wanted, sizeof(wanted), "%s.DIR", literal);
        if (!ods2_read_header(vol, fid.fid_num, header).ok ||
            !ods2_list_directory_alloc(vol, header, &entries, &count).ok) {
            return;
        }
        for (i = 0; i < count && !found; i++) {
            if (same_name(entries[i].name, wanted)) {
                size_t nl = strlen(entries[i].name);
                fid = entries[i].fid;
                memcpy(literal, entries[i].name, nl - 4);
                literal[nl - 4] = '\0';
                found = true;
            }
        }
        free(entries);
        if (!found) return;
        if (ods5_escape_name(literal, 0, shown, sizeof(shown)) == (size_t) -1) return;
        if (rlen + strlen(shown) + 2 > sizeof(result)) return;
        rlen += (size_t) snprintf(result + rlen, sizeof(result) - rlen, "%s%s",
                                  rlen ? "." : "", shown);
        p = end;
        if (*p == '.') p++;
    }
    snprintf(path, cap, "%s", result);
}

/* VMS allows 255 directory levels; this is a finite backstop against a
   directory cycle on a corrupted disk image. */
#define MAX_RECURSION_DEPTH 255
#define MAX_DIR_VISITS 1000000

/* State for one DIR across a wildcard directory spec. */
typedef struct {
    ods2_volume_t *vol;
    const char *spec;              /* what remains to match below the start
                                      directory, e.g. "*...", "...SALES" */
    const char *pattern;           /* file name pattern, e.g. "*.TXT" */
    const ods2_parsed_path_t *p;   /* for the version filter */
    int dirs_shown;
    int files_shown;
    int errors;
    long visits;                   /* directories read so far, aliases counted each time */
    /* The directories on the current recursion path, [0..depth]: their
       FIDs and absolute paths, to refuse an entry that points back at
       one of them (a loop). Aliases that are not ancestors are still
       followed, as VMS does. */
    ods2_fid_t path_fid[MAX_RECURSION_DEPTH + 1];
    const char *path_name[MAX_RECURSION_DEPTH + 1];
} dir_walk_t;

/* Walks the tree below the start directory depth first, in directory
 * order (alphabetical, as VMS stores them), listing every directory
 * whose path relative to the start matches w->spec. `abs_path` is the
 * directory's absolute path, `rel_path` the same directory relative to
 * where the walk began ("" for the start itself). Subtrees that cannot
 * contain a match are never read. This one walk serves every DIR:
 * "[A]" (spec "" - just the start), "[A...]" (spec "..."), "[*]",
 * "[*...]", "[A...B]" and so on. */
static void walk_directories(dir_walk_t *w, const char *abs_path, const char *rel_path,
                             int depth)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_dir_entry_t *entries = NULL;
    int count = 0, i;
    ods2_result_t r;
    bool here = ods2_dir_spec_match(w->spec, rel_path);
    bool deeper = ods2_dir_spec_could_descend(w->spec, rel_path);
    char *bufs;

    if (!here && !deeper) return;
    /* A directory reached through aliases is read again each time, as on VMS (the V8.4
       CD's 256 SYSn aliases make 33,560 visits); a damaged or hostile volume whose
       aliases multiply could take forever, so a very large number of visits is refused. */
    if (++w->visits > MAX_DIR_VISITS) {
        if (w->visits == MAX_DIR_VISITS + 1) {
            report_error("%%ODS2-E-TOOMANYDIRS, more than %d directories visited; "
                         "stopping (aliases that multiply?)\n", MAX_DIR_VISITS);
            w->errors++;
        }
        return;
    }
    if (depth > MAX_RECURSION_DEPTH) {
        report_error("%%ODS2-E-TOODEEP, recursion limit reached, skipping "
                        "subtree under [%s]\n", abs_path);
        w->errors++;
        return;
    }

    r = resolve_dir(w->vol, abs_path, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, [%s]: %s\n", abs_path, r.problem);
        w->errors++;
        return;
    }
    r = ods2_list_directory_alloc(w->vol, dir_header, &entries, &count);
    if (!r.ok) {
        report_error("%%ODS2-E-LISTERR, [%s]: %s\n", abs_path, r.problem);
        w->errors++;
        return;
    }

    w->path_fid[depth] = dir_fid;
    w->path_name[depth] = abs_path;
    bufs = malloc(3 * (size_t) ODS2_PATH_MAX);
    if (bufs == NULL) {
        report_error("%%ODS2-E-NOMEM, out of memory listing [%s]\n", abs_path);
        w->errors++;
        free(entries);
        return;
    }

    if (here) {
        int shown = show_directory(w->vol, abs_path, entries, count, w->pattern, w->p);
        if (shown > 0) {
            w->dirs_shown++;
            w->files_shown += shown;
        }
    }

    for (i = 0; deeper && i < count; i++) {
        uint8_t sub_header[512];
        char *name = bufs, *child_abs = bufs + ODS2_PATH_MAX,
             *child_rel = bufs + 2 * ODS2_PATH_MAX;
        char shown[ODS2_PATH_MAX];
        size_t len;
        int a, b;

        /* Skip self-referential entries - root's own directory always
           contains a "000000.DIR" entry pointing straight back at
           itself (FID (4,4), present on every ODS-2 volume). This is
           also why "[*]" never matches [000000], as on VMS. */
        if (entries[i].fid.fid_num == dir_fid.fid_num &&
            entries[i].fid.fid_seq == dir_fid.fid_seq) {
            continue;
        }
        /* Only a name's highest version can be the subdirectory a
           path like [A.B] refers to. */
        if (i > 0 && same_name(entries[i].name, entries[i - 1].name)) {
            continue;
        }
        /* Directories are NAME.DIR; the name is the path component. */
        len = strlen(entries[i].name);
        if (len <= 4 || len >= (size_t) ODS2_PATH_MAX || !same_name(entries[i].name + len - 4, ".DIR")) {
            continue;
        }
        memcpy(name, entries[i].name, len - 4);
        name[len - 4] = '\0';
        /* the path component is written as a spelling: a dot in a
           directory name is ^. , a space ^_ ... (it is a bracketed path) */
        if (ods5_escape_name(name, 0, shown, sizeof(shown)) == (size_t) -1) {
            report_error("%%ODS2-E-PATHTOOLONG, directory name too long, "
                            "skipping subtree under [%s]\n", abs_path);
            w->errors++;
            continue;
        }
        snprintf(name, ODS2_PATH_MAX, "%s", shown);

        a = snprintf(child_abs, ODS2_PATH_MAX, "%s%s%s", abs_path,
                     abs_path[0] ? "." : "", name);
        b = snprintf(child_rel, ODS2_PATH_MAX, "%s%s%s", rel_path,
                     rel_path[0] ? "." : "", name);
        if (a < 0 || (size_t) a >= (size_t) ODS2_PATH_MAX || b < 0 ||
            (size_t) b >= (size_t) ODS2_PATH_MAX) {
            report_error("%%ODS2-E-PATHTOOLONG, path too deep to represent, "
                            "skipping subtree under [%s]\n", abs_path);
            w->errors++;
            continue;
        }
        /* Cheap name test before reading the entry's header. */
        if (!ods2_dir_spec_match(w->spec, child_rel) &&
            !ods2_dir_spec_could_descend(w->spec, child_rel)) {
            continue;
        }
        /* Is it really a directory? Checked via its own header's
           FH2$M_DIRECTORY bit (0x2000), not just the .DIR suffix. */
        r = ods2_read_header(w->vol, entries[i].fid.fid_num, sub_header);
        if (!r.ok || !(((ods2_head_core_t *) sub_header)->filechar & 0x2000u)) {
            continue;
        }
        /* An entry naming a directory already on the path is a loop:
           warn instead of descending until the depth limit. */
        {
            int d, loop = -1;
            for (d = 0; d <= depth; d++) {
                if (w->path_fid[d].fid_num == entries[i].fid.fid_num &&
                    w->path_fid[d].fid_seq == entries[i].fid.fid_seq) {
                    loop = d;
                    break;
                }
            }
            if (loop >= 0) {
                fprintf(stderr, "%%ODS2-W-DIRLOOP, [%s] is the directory [%s] "
                                "again; not descended into\n",
                        child_abs, w->path_name[loop][0] ? w->path_name[loop] : "000000");
                continue;
            }
        }
        walk_directories(w, child_abs, child_rel, depth + 1);
    }
    free(bufs);
    free(entries);
}

/* DIR, following VMS DIRECTORY: '*' and '%' may appear in any
 * directory name and the ellipsis anywhere in the directory spec, so
 * one command can list many directories. As on VMS, directories with
 * no matching files are not shown, a "Grand total" follows when more
 * than one directory was listed, and a listing that finds nothing at
 * all reports "no files found". */
static void cmd_dir(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    char effective_dir[ODS2_PATH_MAX];
    char spec[ODS2_PATH_MAX + 4];
    char start[ODS2_PATH_MAX];
    const char *rest;
    char file_pattern[ODS2_PATH_MAX + 4];
    dir_walk_t w;

    resolve_effective_path(p, effective_dir, sizeof(effective_dir));
    /* A trailing ellipsis was split off as `recursive`; put it back,
       so the spec says everything the walk has to match. */
    snprintf(spec, sizeof(spec), "%s%s", effective_dir, p->recursive ? "..." : "");

    /* Start at the longest plain prefix ("A.B" of "A.B.*...") - it is
       looked up directly, so a missing directory there is reported
       as such - and match only what follows it. */
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }
    canonical_dir_path(vol, start, sizeof(start));

    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    /* As in VMS DIRECTORY, a name with no type means any type:
       "MAKEFILE" is "MAKEFILE.*", "*" is "*.*". (Names are stored with
       their dot, "MAKEFILE.", so the bare pattern would match nothing.) */
    if (p->filename[0] == '\0') {
        snprintf(file_pattern, sizeof(file_pattern), "*.*");
    } else if (ods5_find_unescaped(p->filename, '.') == NULL) {
        snprintf(file_pattern, sizeof(file_pattern), "%s.*", p->filename);
    } else {
        snprintf(file_pattern, sizeof(file_pattern), "%s", p->filename);
    }
    w.pattern = file_pattern;
    w.p = p;
    walk_directories(&w, start, "", 0);

    if (g_paths_mode) {
        if (w.dirs_shown == 0 && w.errors == 0) fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else if (w.dirs_shown > 1) {
        printf("\nGrand total of %d director%s, %d file%s.\n",
               w.dirs_shown, (w.dirs_shown == 1) ? "y" : "ies",
               w.files_shown, (w.files_shown == 1) ? "" : "s");
    } else if (w.dirs_shown == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    }
}

/* --- EXPORT: copy files off the volume to Linux files ------------------ */

typedef struct {
    ods2_volume_t *vol;
    const char *local;        /* destination: a directory, or one file name */
    bool dest_is_dir;
    bool tree;                /* recreate the directory tree below `start` */
    bool versions;            /* name ends ";N" */
    bool overwrite;
    bool links;               /* repeat FIDs become hard links */
    bool records;             /* /RECORDS: sequential files as text, one line per record */
    bool lines;               /* /LINES: ignore the files' carriage control, one line per record */
    const char *start;        /* the start directory's spelling (tree mode) */
    int files, symlinks, dirs_skipped, specials_skipped, errors;
    struct seen_fid { uint16_t num, seq; char *path; } *seen;
    size_t seen_count, seen_cap;
} export_ctx_t;

/* A name made safe for one Linux path component. */
static void linux_component(const char *literal, char *out, size_t cap)
{
    if (literal[0] == '\0' || strcmp(literal, ".") == 0 || strcmp(literal, "..") == 0) {
        snprintf(out, cap, "%s", literal[0] == '\0' ? "%00" : (literal[1] == '\0' ? "%2E" : "%2E%2E"));
        return;
    }
    if (ods5_escape_for_linux(literal, out, cap) == (size_t) -1) {
        snprintf(out, cap, "%%FF"); /* too long to hold: cannot happen for a stored name */
    }
}

/* Makes the Linux directory `path`, and its parents as needed. The first `rootlen` bytes
   are the destination the user named and are used as they are (they may be a symbolic link
   to a directory, on purpose). Below that every component is checked with lstat(): a
   symbolic link or anything that is not a directory is refused, so that a name on the
   volume can never steer EXPORT's writes out of the destination (a directory `X` and a
   symbolic link `X` of the same Linux name, a link already lying in the destination). */
static bool make_local_dirs(const char *path, size_t rootlen)
{
    char buf[ODS2_PATH_MAX * 2];
    char *p;
    struct stat st;

    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if ((size_t) (p - buf) > rootlen) {
                if (lstat(buf, &st) == 0) {
                    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return false; }
                } else if (mkdir(buf, 0777) != 0) {
                    return false;
                }
            } else if (stat(buf, &st) != 0 && mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (strlen(buf) > rootlen) {
        if (lstat(buf, &st) == 0) {
            if (S_ISDIR(st.st_mode)) return true;
            errno = ENOTDIR;
            return false;
        }
        return mkdir(buf, 0777) == 0;
    }
    return stat(buf, &st) == 0 ? S_ISDIR(st.st_mode) : mkdir(buf, 0777) == 0;
}

/* A file's content for output: as the bytes it holds (records == false), or as Linux text, one
 * line per record, as VMS TYPE shows a sequential RMS file (records == true; relative and indexed
 * files, and special files, are still given as bytes: their records are not decoded). `note` gets
 * a non-fatal remark (or NULL). */
static bool rec_feed_sink(void *ctx, const uint8_t *data, size_t n)
{
    return ods2_rec_feed((ods2_rec_decoder_t *) ctx, data, n);
}

static ods2_result_t read_file_content(ods2_volume_t *vol, const uint8_t *header, bool records,
                                       unsigned rec_options,
                                       bool (*sink)(void *ctx, const uint8_t *data, size_t n), void *ctx,
                                       size_t *bytes_out, bool *sink_failed_out, const char **note)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    ods2_rec_decoder_t *d;
    ods2_rec_attrs_t a;
    ods2_result_t r;

    *note = NULL;
    if (!records || ods5_special_type(header) != 0 || !ods2_rec_decodable(core->recattr.rtype)) {
        if (records && ods5_special_type(header) == 0 && (core->recattr.rtype >> 4) != 0) {
            *note = "relative and indexed files are given as stored (their records are not decoded)";
        }
        return ods2_read_file_stream(vol, header, sink, ctx, bytes_out, sink_failed_out);
    }
    a.rfm = core->recattr.rtype & 15u;
    a.rat = core->recattr.rattrib;
    a.rsize = core->recattr.rsize;
    a.maxrec = core->recattr.maxrec;
    a.vfcsize = core->recattr.vfcsize;
    d = ods2_rec_new(&a, rec_options, sink, ctx);
    if (d == NULL) {
        if (sink_failed_out) *sink_failed_out = false;
        if (bytes_out) *bytes_out = 0;
        return (ods2_result_t) { .ok = false, .problem = "out of memory" };
    }
    r = ods2_read_file_stream(vol, header, rec_feed_sink, d, bytes_out, sink_failed_out);
    if (r.ok && !ods2_rec_bad_data(d) && !(sink_failed_out && *sink_failed_out)) {
        if (!ods2_rec_finish(d) && !ods2_rec_bad_data(d) && sink_failed_out) *sink_failed_out = true;
    }
    if (ods2_rec_bad_data(d)) {
        /* the decoder refused the data (the sink did not fail) */
        if (sink_failed_out) *sink_failed_out = false;
        r.ok = false;
        r.problem = ods2_rec_problem(d); /* a string constant */
    } else {
        *note = ods2_rec_problem(d);
    }
    ods2_rec_free(d);
    return r;
}

typedef struct { FILE *f; bool failed; } file_sink_t;

static bool file_sink(void *ctx, const uint8_t *data, size_t n)
{
    file_sink_t *s = (file_sink_t *) ctx;
    if (fwrite(data, 1, n, s->f) != n) { s->failed = true; return false; }
    return true;
}

static void export_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling,
                         const ods2_dir_entry_t *e)
{
    export_ctx_t *x = (export_ctx_t *) ctx_;
    uint8_t header[512];
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    char dirpath[ODS2_PATH_MAX * 2], name[ODS2_DIR_MAX_NAME + 16], comp[ODS2_DIR_MAX_NAME * 3 + 16];
    char path[ODS2_PATH_MAX * 3];
    ods2_result_t r = ods2_read_header(vol, e->fid.fid_num, header);
    unsigned special;
    size_t i;
    char shown[ODS2_DIR_MAX_NAME * 3 + 8]; /* the name, safe to print: %XX for controls */

    if (ods5_escape_for_linux(e->name, shown, sizeof(shown)) == (size_t) -1) snprintf(shown, sizeof(shown), "(name too long)");

    if (!r.ok) {
        report_error("%%ODS2-E-READERR, %s: %s\n", shown, r.problem);
        x->errors++;
        return;
    }
    if (!ods2_header_is_file(header, e->fid)) {
        report_error("%%ODS2-E-FNF, %s: no such file (a dangling entry: its file is gone)\n", shown);
        x->errors++;
        return;
    }
    if (core->filechar & 0x2000u) {
        if (x->tree) {
            /* tree mode: every directory is recreated, empty ones too */
            size_t nl = strlen(e->name);
            char dname[ODS2_DIR_MAX_NAME + 8], dcomp[ODS2_DIR_MAX_NAME * 3 + 16], dpath[ODS2_PATH_MAX * 3];
            const char *rel = dir_spelling + strlen(x->start);
            if (*rel == '.' && x->start[0] != '\0') rel++;
            if (nl > 4 && ods2_dir_name_compare(e->name + nl - 4, 4, ".DIR", 4) == 0) {
                snprintf(dname, sizeof(dname), "%.*s", (int) (nl - 4), e->name);
                linux_component(dname, dcomp, sizeof(dcomp));
                {
                    /* the Linux path of the directory it sits in, as for a file below */
                    char parent[ODS2_PATH_MAX * 2];
                    snprintf(parent, sizeof(parent), "%s", x->local);
                    while (*rel) {
                        const char *end = rel;
                        char lit[ODS2_PATH_MAX], comp2[ODS2_DIR_MAX_NAME * 3 + 16];
                        size_t ul;
                        while (*end && *end != '.') { if (*end == '^' && end[1]) end++; end++; }
                        ul = ods5_unescape(rel, (size_t) (end - rel), lit, sizeof(lit));
                        if (ul == (size_t) -1) snprintf(lit, sizeof(lit), "%.*s", (int) (end - rel), rel);
                        linux_component(lit, comp2, sizeof(comp2));
                        if (strlen(parent) + strlen(comp2) + 2 < sizeof(parent)) {
                            strcat(parent, "/");
                            strcat(parent, comp2);
                        }
                        rel = end;
                        if (*rel == '.') rel++;
                    }
                    snprintf(dpath, sizeof(dpath), "%s/%s", parent, dcomp);
                }
                if (strlen(dpath) < 4000 && make_local_dirs(dpath, strlen(x->local))) return;
                report_error("%%ODS2-E-MKDIR, cannot create %s: %s\n", dpath, strerror(errno));
                x->errors++;
                return;
            }
        }
        x->dirs_skipped++;
        return;
    }
    special = ods5_special_type(header);
    if (special != 0 && special != ODS5_FAT_SYMBOLIC_LINK && special != ODS5_FAT_SYMLINK) {
        fprintf(stderr, "%%ODS2-W-SPECIAL, %s: special file (type %u) skipped\n", shown, special);
        x->specials_skipped++;
        return;
    }

    /* the Linux directory */
    {
        bool too_long = false;
        snprintf(dirpath, sizeof(dirpath), "%s", x->local);
        if (x->tree) {
            const char *rel = dir_spelling + strlen(x->start);
            if (*rel == '.' && x->start[0] != '\0') rel++;
            while (*rel) {
                const char *end = rel;
                char lit[ODS2_PATH_MAX];
                size_t ul;
                while (*end && *end != '.') { if (*end == '^' && end[1]) end++; end++; }
                ul = ods5_unescape(rel, (size_t) (end - rel), lit, sizeof(lit));
                if (ul == (size_t) -1) snprintf(lit, sizeof(lit), "%.*s", (int) (end - rel), rel);
                linux_component(lit, comp, sizeof(comp));
                if (strlen(dirpath) + strlen(comp) + 2 >= 4000) { too_long = true; break; }
                strcat(dirpath, "/");
                strcat(dirpath, comp);
                rel = end;
                if (*rel == '.') rel++;
            }
            if (too_long) {
                report_error("%%ODS2-E-PATHTOOLONG, %s: the Linux path would be too long\n", shown);
                x->errors++;
                return;
            }
        }
    }
    /* the Linux file name */
    snprintf(name, sizeof(name), "%s", e->name);
    {
        /* a name stored as "README." (no type; VMS always stores the dot) is "README" on Linux:
           the one trailing dot that is the name's only dot is dropped, so that IMPORT and EXPORT
           give back the same names (documented: a Linux "x." comes back as "x") */
        size_t nl = strlen(name);
        if (nl > 1 && name[nl - 1] == '.' && strchr(name, '.') == name + nl - 1) name[nl - 1] = '\0';
    }
    linux_component(name, comp, sizeof(comp));
    if (x->versions) {
        size_t l = strlen(comp);
        snprintf(comp + l, sizeof(comp) - l, ";%u", (unsigned) e->version);
    }
    if (x->dest_is_dir || x->tree) {
        if (!make_local_dirs(dirpath, strlen(x->local))) {
            report_error("%%ODS2-E-MKDIR, cannot create %s: %s\n", dirpath, strerror(errno));
            x->errors++;
            return;
        }
        snprintf(path, sizeof(path), "%s/%s", dirpath, comp);
    } else {
        if (x->files + x->symlinks > 0) {
            report_error("%%ODS2-E-TOOMANY, more than one file matches; the destination %s "
                         "must be a directory\n", x->local);
            x->errors++;
            return;
        }
        snprintf(path, sizeof(path), "%s", x->local);
    }

    if (x->overwrite) {
        /* only a file or a link is replaced, never a directory (remove() would take an empty one) */
        struct stat old;
        if (lstat(path, &old) == 0 && !S_ISDIR(old.st_mode)) remove(path);
    }

    if (special != 0) {      /* a symbolic link: the data is the target text */
        uint8_t target[1100];
        char lit[2400];
        size_t got = 0, len = ods2_file_content_length(header);
        if (len == 0 || len >= sizeof(target) ||
            !ods2_read_file(vol, header, target, sizeof(target), &got).ok ||
            ods5_isl1_to_utf8(target, got, lit, sizeof(lit)) == (size_t) -1) {
            report_error("%%ODS2-E-SYMLINK, %s: cannot read the link target\n", shown);
            x->errors++;
            return;
        }
        if (symlink(lit, path) != 0) {
            report_error("%%ODS2-E-SYMLINK, %s: %s\n", path, strerror(errno));
            x->errors++;
            return;
        }
        x->symlinks++;
        return;
    }

    if (x->links) {
        for (i = 0; i < x->seen_count; i++) {
            if (x->seen[i].num == e->fid.fid_num && x->seen[i].seq == e->fid.fid_seq) {
                if (link(x->seen[i].path, path) == 0) { x->files++; return; }
                break;
            }
        }
    }
    {
        /* O_EXCL | O_NOFOLLOW: a new file only, never through a link */
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
        file_sink_t sink;
        size_t bytes = 0;
        bool sink_failed = false;

        if (f == NULL) {
            report_error("%%ODS2-E-OPENERR, cannot create %s: %s\n", path, strerror(errno));
            x->errors++;
            return;
        }
        sink.f = f;
        sink.failed = false;
        {
            const char *note = NULL;
            r = read_file_content(vol, header, x->records, x->lines ? ODS2_REC_LINES : 0, file_sink, &sink, &bytes, &sink_failed, &note);
            if (note != NULL && r.ok) report_error("%%ODS2-W-RECORDS, %s: %s\n", shown, note);
        }
        if (fclose(f) != 0) sink.failed = true;
        if (!r.ok || sink.failed) {
            report_error("%%ODS2-E-READERR, %s: %s\n", shown,
                         r.ok ? "could not write the Linux file" : r.problem);
            remove(path);
            x->errors++;
            return;
        }
        x->files++;
        if (x->links) {
            if (x->seen_count == x->seen_cap) {
                size_t nc = x->seen_cap ? x->seen_cap * 2 : 64;
                struct seen_fid *g = realloc(x->seen, nc * sizeof(*g));
                if (g == NULL) return;
                x->seen = g;
                x->seen_cap = nc;
            }
            x->seen[x->seen_count].num = e->fid.fid_num;
            x->seen[x->seen_count].seq = e->fid.fid_seq;
            x->seen[x->seen_count].path = strdup(path);
            if (x->seen[x->seen_count].path != NULL) x->seen_count++;
        }
    }
}

/* EXPORT[/VERSIONS][/OVERWRITE][/LINKS] <spec> <linux-path>: the files the spec names, to
 * Linux. The destination is a directory (made if needed) for a spec with wildcards, versions
 * or an ellipsis; a spec naming one file may also give the new file's name. An ellipsis
 * recreates the directory tree below the start directory. File contents are the bytes of the
 * file up to its end-of-file mark, as TYPE prints them; names are the UTF-8 names. */
static void cmd_export(ods2_volume_t *vol, const ods2_parsed_path_t *p, const char *local,
                       const char *quals)
{
    export_ctx_t x;
    struct stat st;
    entry_hook_fn saved_hook = g_entry_hook;
    void *saved_ctx = g_entry_hook_ctx;
    bool saved_paths = g_paths_mode;
    char effective[ODS2_PATH_MAX], spec[ODS2_PATH_MAX + 4], start[ODS2_PATH_MAX], pattern[ODS2_PATH_MAX + 4];
    const char *rest;
    dir_walk_t w;
    bool wild;
    size_t i;

    memset(&x, 0, sizeof(x));
    x.vol = vol;
    x.local = local;
    while (quals != NULL && *quals == '/') {
        char q[32];
        size_t ql = strcspn(quals + 1, "/");
        if (ql >= sizeof(q)) ql = sizeof(q) - 1;
        memcpy(q, quals + 1, ql);
        q[ql] = '\0';
        if (matches(q, "VERSIONS")) x.versions = true;
        else if (matches(q, "OVERWRITE")) x.overwrite = true;
        else if (matches(q, "LINKS")) x.links = true;
        else if (matches(q, "RECORDS")) x.records = true;
        else if (matches(q, "LINES")) { x.records = true; x.lines = true; }
        else if (matches(q, "RAW")) x.records = false;
        else {
            report_error("%%ODS2-E-BADQUAL, unrecognized EXPORT qualifier /%s "
                         "(use /VERSIONS, /OVERWRITE, /LINKS, /RECORDS, /LINES or /RAW)\n", q);
            return;
        }
        quals += 1 + strcspn(quals + 1, "/");
    }

    resolve_effective_path(p, effective, sizeof(effective));
    snprintf(spec, sizeof(spec), "%s%s", effective, p->recursive ? "..." : "");
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }
    canonical_dir_path(vol, start, sizeof(start));
    x.start = start;
    x.tree = p->dir_wildcard;
    if (p->filename[0] == '\0') snprintf(pattern, sizeof(pattern), "*.*");
    else if (ods5_find_unescaped(p->filename, '.') == NULL) snprintf(pattern, sizeof(pattern), "%s.*", p->filename);
    else snprintf(pattern, sizeof(pattern), "%s", p->filename);
    if (p->version_wildcard) x.versions = true;

    wild = p->dir_wildcard || p->version_wildcard || p->filename[0] == '\0' ||
           ods5_find_unescaped(p->filename, '.') == NULL;
    for (i = 0; !wild && p->filename[i]; i++) {
        if (p->filename[i] == '^') { if (p->filename[i + 1]) i++; continue; }
        if (p->filename[i] == '*' || p->filename[i] == '%' || p->filename[i] == '?') wild = true;
    }
    if (stat(local, &st) == 0 && S_ISDIR(st.st_mode)) x.dest_is_dir = true;
    else if (wild) {
        if (!make_local_dirs(local, strlen(local))) {
            report_error("%%ODS2-E-MKDIR, cannot create %s: %s\n", local, strerror(errno));
            return;
        }
        x.dest_is_dir = true;
    }

    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = pattern;
    w.p = p;
    g_entry_hook = export_entry;
    g_entry_hook_ctx = &x;
    g_paths_mode = false;
    walk_directories(&w, start, "", 0);
    g_entry_hook = saved_hook;
    g_entry_hook_ctx = saved_ctx;
    g_paths_mode = saved_paths;

    for (i = 0; i < x.seen_count; i++) free(x.seen[i].path);
    free(x.seen);
    if (x.files + x.symlinks == 0 && x.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else {
        printf("%%ODS2-I-EXPORTED, %d file%s, %d symbolic link%s copied to %s",
               x.files, x.files == 1 ? "" : "s", x.symlinks, x.symlinks == 1 ? "" : "s", local);
        if (x.dirs_skipped) printf("; %d director%s not copied", x.dirs_skipped, x.dirs_skipped == 1 ? "y" : "ies");
        printf("\n");
    }
}

static void cmd_create_directory(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    /* CREATE/DIRECTORY [PARENT.NEWNAME] - the new directory's own
       name is the LAST component of the (effective, SET-DEFAULT-
       resolved) bracketed path, its parent is everything before
       that. */
    char effective_path[ODS2_PATH_MAX];
    char dir_copy[ODS2_PATH_MAX];
    const char *parent_path;
    const char *new_name;
    uint8_t parent_header[512];
    ods2_fid_t parent_fid, new_fid;
    ods2_result_t r;

    resolve_effective_path(p, effective_path, sizeof(effective_path));
    if (effective_path[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no directory name given\n");
        return;
    }
    split_last_component(effective_path, dir_copy, &parent_path, &new_name);

    r = resolve_dir(vol, parent_path, parent_header, &parent_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    {
        /* the name is a spelling (^-escapes): create the characters it stands for */
        char literal[ODS2_PATH_MAX];
        if (ods5_unescape(new_name, strlen(new_name), literal, sizeof(literal)) == (size_t) -1) {
            report_error("%%ODS2-E-BADNAME, [%s]: malformed ^ escape in the directory name\n",
                         effective_path);
            return;
        }
        r = ods2_create_directory(vol, parent_header, literal, &new_fid);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-CREATEERR, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-CREATED, created [%s]\n", effective_path);
}

/* A host file opened for COPY, read in order a chunk at a time
   rather than all at once (issue #16). `length` is known before
   anything is allocated on the volume: from the file itself if it is
   a regular file, otherwise (a pipe, /dev/stdin) by first copying it
   to a temporary file, which keeps memory use flat either way. */
typedef struct {
    FILE *f;
    const char *path;
    size_t length;
    char problem[256];
} host_input_t;

static bool host_input_read(void *ctx_, uint8_t *buf, size_t n)
{
    host_input_t *in = (host_input_t *) ctx_;
    size_t got = fread(buf, 1, n, in->f);
    if (got == n) return true;
    if (ferror(in->f)) {
        snprintf(in->problem, sizeof(in->problem), "error reading %s", in->path);
    } else {
        snprintf(in->problem, sizeof(in->problem),
                 "%s became shorter while being copied (expected %zu bytes)",
                 in->path, in->length);
    }
    return false;
}

/* Opens `path` for COPY, finding its length. Returns false and prints
   why on failure. */
/* IMPORT reads a Linux tree: a file found there as a regular file must still be one when it
   is opened (no symbolic link swapped in under it that would lead out of the tree). */
static bool g_open_nofollow = false;

static bool host_input_open(const char *path, host_input_t *in)
{
    struct stat st;
    FILE *f;
    if (g_open_nofollow) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        f = fd >= 0 ? fdopen(fd, "rb") : NULL;
        if (fd >= 0 && f == NULL) close(fd);
    } else {
        f = fopen(path, "rb");
    }

    memset(in, 0, sizeof(*in));
    in->path = path;
    if (f == NULL) {
        report_error("%%ODS2-E-OPENIN, could not open %s\n", path);
        return false;
    }
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
        in->f = f;
        in->length = (size_t) st.st_size;
        return true;
    }
    /* No length to go by: spool it to a temporary file first. */
    {
        static uint8_t buf[1 << 16];
        FILE *spool = tmpfile();
        size_t n, total = 0;

        if (spool == NULL) {
            report_error("%%ODS2-E-OPENIN, could not create a temporary file to "
                         "hold %s: %s\n", path, strerror(errno));
            fclose(f);
            return false;
        }
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (fwrite(buf, 1, n, spool) != n) {
                report_error("%%ODS2-E-WRITEERR, could not write the temporary copy of "
                             "%s (is the temporary directory full?)\n", path);
                fclose(f);
                fclose(spool);
                return false;
            }
            total += n;
        }
        if (ferror(f)) {
            report_error("%%ODS2-E-READERR, error reading %s\n", path);
            fclose(f);
            fclose(spool);
            return false;
        }
        fclose(f);
        if (fflush(spool) != 0 || fseek(spool, 0, SEEK_SET) != 0) {
            report_error("%%ODS2-E-READERR, could not reread the temporary copy of %s\n",
                         path);
            fclose(spool);
            return false;
        }
        in->f = spool;
        in->length = total;
        return true;
    }
}

/* Case-insensitive: is `word` (length `len`) `name`, or an
   abbreviation of it at least `min` characters long, as VMS allows? */
static bool keyword(const char *word, size_t len, const char *name, size_t min)
{
    size_t i;
    if (len < min || len > strlen(name)) return false;
    for (i = 0; i < len; i++) {
        if (toupper((unsigned char) word[i]) != toupper((unsigned char) name[i])) return false;
    }
    return true;
}

/* COPY's options beyond the record attributes themselves. */
typedef struct {
    bool convert;   /* /CONVERT: host text lines -> VAR records */
    bool mrs_given; /* MRS:n was given */
} copy_options_t;

/* Parses COPY's qualifiers - everything after "COPY" in the command
 * word - into the record attributes of the new file. /ATTRIBUTES is in
 * the syntax of VMS's SET FILE/ATTRIBUTES:
 *
 *   COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) kit.pcsi [000000]KIT.PCSI
 *
 *   RFM:fmt   record format - FIX, STMLF, STM, STMCR or UDF, or VAR
 *             with /CONVERT
 *   MRS:n     maximum record size; for FIX, the record size
 *   LRL:n     longest record length (for FIX, the same as MRS)
 *   RAT:x     record attributes - NONE, CR, FTN, PRN, BLK, NOBLK;
 *             repeat RAT for more than one (RAT:CR,RAT:BLK)
 *
 * /CONVERT (issue #15) turns a host text file into variable-length
 * records, one per line (see ods2_text_var.h) - VMS's native text
 * format, as VMS FTP writes it in ASCII mode:
 *
 *   COPY/CONVERT notes.txt [DOC]NOTES.TXT
 *   COPY/ATTRIBUTES=(RFM:VAR,RAT:CR)/CONVERT notes.txt [DOC]NOTES.TXT
 *
 * With /CONVERT the format is VAR and, unless a RAT was given, RAT:CR;
 * the longest record length comes from the file, and MRS is 0 (no
 * limit) unless given.
 *
 * With no qualifier the file is Stream_LF, as always. A fixed format
 * with no size given is 512-byte records. Returns false after
 * reporting the problem. */
static bool parse_copy_qualifiers(const char *quals, ods2_record_attrs_t *a,
                                  copy_options_t *opts)
{
    bool mrs_given = false, lrl_given = false, rfm_given = false, rat_given = false;
    const char *q = quals;

    while (*q == '/') {
        const char *name = ++q;
        const char *value, *end;
        size_t name_len;

        while (*q && *q != '=' && *q != '/') q++;
        name_len = (size_t) (q - name);
        if (keyword(name, name_len, "CONVERT", 4)) {
            if (*q == '=') {
                report_error("%%ODS2-E-NOVALUE, /CONVERT takes no value\n");
                return false;
            }
            opts->convert = true;
            continue;
        }
        if (!keyword(name, name_len, "ATTRIBUTES", 4)) {
            report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - COPY takes "
                         "/ATTRIBUTES=(RFM:fmt,MRS:size,RAT:attr) and /CONVERT\n",
                         (int) name_len, name);
            return false;
        }
        if (*q != '=') {
            report_error("%%ODS2-E-VALREQ, /ATTRIBUTES needs a value, e.g. "
                         "/ATTRIBUTES=(RFM:FIX,MRS:8192)\n");
            return false;
        }
        value = ++q;
        if (*value == '(') {
            end = strchr(value, ')');
            if (end == NULL) {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: missing ')'\n");
                return false;
            }
            value++;
            q = end + 1;
        } else {
            end = value;
            while (*end && *end != '/') end++;
            q = end;
        }

        /* comma-separated KEY:VALUE items between value and end */
        while (value < end) {
            const char *item = value, *colon, *item_end = value;
            size_t key_len, val_len;
            while (item_end < end && *item_end != ',') item_end++;
            value = (item_end < end) ? item_end + 1 : end;
            colon = memchr(item, ':', (size_t) (item_end - item));
            if (colon == NULL) colon = memchr(item, '=', (size_t) (item_end - item));
            if (colon == NULL) {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: \"%.*s\" is not KEY:VALUE\n",
                             (int) (item_end - item), item);
                return false;
            }
            key_len = (size_t) (colon - item);
            val_len = (size_t) (item_end - colon - 1);

            if (keyword(item, key_len, "RFM", 3)) {
                const char *v = colon + 1;
                rfm_given = true;
                if (keyword(v, val_len, "FIXED", 3)) a->rtype = ODS2_RFM_FIX;
                else if (keyword(v, val_len, "STMLF", 5) || keyword(v, val_len, "STREAM_LF", 8))
                    a->rtype = ODS2_RFM_STMLF;
                else if (keyword(v, val_len, "STMCR", 5) || keyword(v, val_len, "STREAM_CR", 8))
                    a->rtype = ODS2_RFM_STMCR;
                else if (keyword(v, val_len, "STM", 3) || keyword(v, val_len, "STREAM", 6))
                    a->rtype = ODS2_RFM_STM;
                else if (keyword(v, val_len, "UDF", 3) || keyword(v, val_len, "UNDEFINED", 3))
                    a->rtype = ODS2_RFM_UDF;
                else if (keyword(v, val_len, "VARIABLE", 3)) a->rtype = ODS2_RFM_VAR;
                else if (keyword(v, val_len, "VFC", 3)) a->rtype = ODS2_RFM_VFC;
                else {
                    report_error("%%ODS2-E-BADVALUE, RFM:%.*s - record format must be FIX, "
                                 "STMLF, STM, STMCR or UDF, or VAR with /CONVERT\n",
                                 (int) val_len, v);
                    return false;
                }
            } else if (keyword(item, key_len, "MRS", 3) || keyword(item, key_len, "LRL", 3)) {
                char digits[16];
                char *stop;
                unsigned long n;
                if (val_len == 0 || val_len >= sizeof(digits)) {
                    n = 0;
                    stop = digits;
                    digits[0] = 'x';
                } else {
                    memcpy(digits, colon + 1, val_len);
                    digits[val_len] = '\0';
                    n = strtoul(digits, &stop, 10);
                }
                if (*stop != '\0' || digits[0] == '-' || n > 65535) {
                    report_error("%%ODS2-E-BADVALUE, %.*s needs a record size in bytes\n",
                                 (int) (item_end - item), item);
                    return false;
                }
                if (toupper((unsigned char) item[0]) == 'M') {
                    a->maxrec = (uint16_t) n;
                    mrs_given = true;
                } else {
                    a->rsize = (uint16_t) n;
                    lrl_given = true;
                }
            } else if (keyword(item, key_len, "RAT", 3)) {
                const char *v = colon + 1;
                rat_given = true;
                if (keyword(v, val_len, "NONE", 4)) a->rattrib &= (uint8_t) ~0x07u;
                else if (keyword(v, val_len, "CR", 2)) a->rattrib |= ODS2_RAT_CR;
                else if (keyword(v, val_len, "FTN", 3)) a->rattrib |= ODS2_RAT_FTN;
                else if (keyword(v, val_len, "PRN", 3)) a->rattrib |= ODS2_RAT_PRN;
                else if (keyword(v, val_len, "BLK", 3)) a->rattrib |= ODS2_RAT_BLK;
                else if (keyword(v, val_len, "NOBLK", 5)) a->rattrib &= (uint8_t) ~ODS2_RAT_BLK;
                else {
                    report_error("%%ODS2-E-BADVALUE, RAT:%.*s - record attributes are NONE, "
                                 "CR, FTN, PRN, BLK or NOBLK\n", (int) val_len, v);
                    return false;
                }
            } else {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: unknown keyword \"%.*s\" - "
                             "use RFM, MRS, LRL or RAT\n", (int) key_len, item);
                return false;
            }
        }
    }
    if (*q != '\0') {
        report_error("%%ODS2-E-IVQUAL, could not parse qualifiers \"%s\"\n", quals);
        return false;
    }

    opts->mrs_given = mrs_given;
    if (opts->convert) {
        if (!rfm_given) a->rtype = ODS2_RFM_VAR;
        if (a->rtype != ODS2_RFM_VAR) {
            report_error("%%ODS2-E-CONFQUAL, /CONVERT makes variable-length records: "
                         "use it with RFM:VAR, or no RFM at all\n");
            return false;
        }
        if (!rat_given) a->rattrib = ODS2_RAT_CR;
        if (lrl_given) {
            report_error("%%ODS2-E-CONFQUAL, with /CONVERT the longest record length "
                         "(LRL) comes from the file - leave LRL out\n");
            return false;
        }
    } else if (a->rtype == ODS2_RFM_VAR) {
        report_error("%%ODS2-E-CONFQUAL, RFM:VAR needs /CONVERT: each variable-length "
                     "record starts with a byte count the host file doesn't have, so "
                     "COPY/CONVERT makes one record per line\n");
        return false;
    }

    /* A fixed record's size is both RSIZE and MRS: either one sets
       both, as VMS keeps them equal; neither means 512. */
    if (a->rtype == ODS2_RFM_FIX) {
        if (mrs_given && !lrl_given) a->rsize = a->maxrec;
        else if (lrl_given && !mrs_given) a->maxrec = a->rsize;
        else if (!mrs_given && !lrl_given) a->rsize = a->maxrec = 512;
    }
    return true;
}

/* /CONVERT's second pass: the encoder reads the host file through
   this, and its problem is passed on to the content source. */
typedef struct {
    host_input_t *in;
    ods2_text_var_encoder_t *enc;
    ods2_content_source_t *src;
} convert_ctx_t;

static size_t convert_read_some(void *ctx_, uint8_t *buf, size_t n, bool *error_out)
{
    host_input_t *in = (host_input_t *) ctx_;
    size_t got = fread(buf, 1, n, in->f);
    if (got == 0 && ferror(in->f)) *error_out = true;
    return got;
}

static bool convert_read(void *ctx_, uint8_t *buf, size_t n)
{
    convert_ctx_t *c = (convert_ctx_t *) ctx_;
    if (ods2_text_var_read(c->enc, buf, n)) return true;
    snprintf(c->in->problem, sizeof(c->in->problem), "%s: %s", c->in->path,
             ods2_text_var_problem(c->enc));
    c->src->problem = c->in->problem;
    return false;
}

/* /CONVERT's first pass: reads the whole host file through the
   scanner, then rewinds it. Returns false after reporting a problem
   (a read error, or a line too long to be a record). */
static bool convert_scan(host_input_t *in, ods2_text_var_scan_t *scan)
{
    static uint8_t buf[1 << 16];
    size_t n;

    ods2_text_var_scan_init(scan);
    while ((n = fread(buf, 1, sizeof(buf), in->f)) > 0) {
        ods2_text_var_scan_feed(scan, buf, n);
    }
    if (ferror(in->f)) {
        report_error("%%ODS2-E-READERR, error reading %s\n", in->path);
        return false;
    }
    ods2_text_var_scan_finish(scan);
    if (scan->too_long_line != 0) {
        report_error("%%ODS2-E-RECTOOBIG, %s: line %zu is longer than a variable-length "
                     "record can be (%u bytes)\n", in->path, scan->too_long_line,
                     ODS2_VAR_MAX_RECORD);
        return false;
    }
    if (fseek(in->f, 0, SEEK_SET) != 0) {
        report_error("%%ODS2-E-READERR, could not reread %s\n", in->path);
        return false;
    }
    return true;
}

static void import_decode_name(const char *linux_name, char *out, size_t cap);

static void cmd_copy(ods2_volume_t *vol, const char *local_file, const ods2_parsed_path_t *p,
                     const ods2_record_attrs_t *attrs_in, const copy_options_t *opts)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid, new_fid;
    host_input_t in;
    ods2_content_source_t src;
    ods2_record_attrs_t attrs = *attrs_in;
    ods2_text_var_scan_t scan;
    convert_ctx_t conv;
    ods2_result_t r;
    char literal_name[ODS2_PATH_MAX];
    const char *name;

    if (p->filename[0] == '\0' && !g_ods5) {
        report_error("%%ODS2-E-BADPATH, no destination filename given\n");
        return;
    }
    /* A new file is always created as version 1 here. Accept the
       spellings that mean that on a new file ("NAME", "NAME;",
       "NAME;0", "NAME;1") and refuse the rest, rather than ever
       storing ";version" as part of the name. */
    if (p->has_version &&
        (p->version_wildcard || p->version < 0 || p->version > 1)) {
        char spec[ODS2_PATH_MAX + 16];
        format_file_spec(p, spec, sizeof(spec));
        report_error("%%ODS2-E-BADVER, %s: COPY creates version 1 only - "
                        "give NAME, NAME; or NAME;1\n", spec);
        return;
    }
    {
        /* The name is a spelling (^-escapes): store the characters it
           stands for. Without a name on an ODS-5 volume, the host file's own
           name is used (UTF-8, as the host has it). */
        if (p->filename[0] != '\0') {
            if (ods5_unescape(p->filename, strlen(p->filename), literal_name,
                              sizeof(literal_name)) == (size_t) -1) {
                report_error("%%ODS2-E-BADNAME, malformed ^ escape in the file name\n");
                return;
            }
        } else {
            const char *base = strrchr(local_file, '/');
            base = (base != NULL) ? base + 1 : local_file;
            if (strlen(base) >= sizeof(literal_name) || base[0] == '\0') {
                report_error("%%ODS2-E-BADPATH, no destination filename given\n");
                return;
            }
            import_decode_name(base, literal_name, sizeof(literal_name)); /* %XX as IMPORT reads it */
        }
        name = literal_name;
    }

    if (!host_input_open(local_file, &in)) {
        return;
    }
    memset(&src, 0, sizeof(src));
    memset(&conv, 0, sizeof(conv));
    if (opts->convert) {
        if (!convert_scan(&in, &scan)) {
            fclose(in.f);
            return;
        }
        attrs.rsize = (uint16_t) scan.longest;
        if (!opts->mrs_given) attrs.maxrec = 0;
        conv.in = &in;
        conv.src = &src;
        conv.enc = ods2_text_var_encoder_new(convert_read_some, &in);
        if (conv.enc == NULL) {
            report_error("%%ODS2-E-NOMEM, could not allocate the record converter\n");
            fclose(in.f);
            return;
        }
    }

    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        ods2_text_var_encoder_free(conv.enc);
        fclose(in.f);
        return;
    }
    if (opts->convert) {
        src.length = scan.converted_length;
        src.read = convert_read;
        src.ctx = &conv;
        src.var_records = true;
    } else {
        src.length = in.length;
        src.read = host_input_read;
        src.ctx = &in;
        src.problem = in.problem;
    }
    r = ods2_create_file_source(vol, dir_header, name, &src, &attrs, &new_fid);
    if (r.ok) {
        /* More content than was measured means the host file grew
           while it was copied: what's on the volume is its earlier
           length, and with /CONVERT may end part way through a record. */
        uint8_t extra;
        bool grew = opts->convert ? ods2_text_var_read(conv.enc, &extra, 1)
                                  : (fgetc(in.f) != EOF);
        if (grew && opts->convert) {
            report_error("%%ODS2-E-CHANGED, %s changed while being converted: its last "
                         "record may be cut short - delete it and copy again\n",
                         local_file);
        } else if (grew) {
            fprintf(stderr, "%%ODS2-W-CHANGED, %s grew while being copied; only its "
                    "first %zu bytes were written - copy it again\n",
                    local_file, in.length);
        }
    }
    ods2_text_var_encoder_free(conv.enc);
    fclose(in.f);
    if (!r.ok) {
        report_error("%%ODS2-E-COPYERR, %s\n", r.problem);
        return;
    }
    {
        /* Report the name as stored: uppercase, with its dot. */
        char stored[ODS2_PATH_MAX];
        const char *why;
        if (g_ods5) {
            char completed[ODS2_PATH_MAX];
            if (ods5_complete_name(name, completed, sizeof(completed))) {
                display_name(completed, stored, sizeof(stored));
            } else {
                snprintf(stored, sizeof(stored), "%s", name);
            }
        } else if (!ods2_make_file_name(name, stored, sizeof(stored), &why)) {
            snprintf(stored, sizeof(stored), "%s", name);
        }
        if (opts->convert) {
            printf("%%ODS2-I-COPIED, %zu bytes to %s;1 as %zu variable-length "
                   "record%s (longest %zu)\n", in.length, stored, scan.records,
                   scan.records == 1 ? "" : "s", scan.longest);
        } else {
            printf("%%ODS2-I-COPIED, %zu bytes to %s;1\n", in.length, stored);
        }
    }
}

/* A directory spelling's own header, with its literal name resolved: the
   directory part of a path as resolve_dir() wants it. Shared by the
   commands that make several things in one directory. */

/* CREATE/SYMLINK=<target> <link-path>   (also SYMLINK <link-path> <target>):
   a symbolic link as VMS V8.4 makes it. The target is literal text (as
   typed: UTF-8, ISO Latin-1 characters only), not a spelling. */
static void cmd_symlink(ods2_volume_t *vol, const ods2_parsed_path_t *p, const char *target)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid, new_fid;
    char literal[ODS2_PATH_MAX], shown[ODS2_PATH_MAX];
    ods2_result_t r;
    char effective_dir[ODS2_PATH_MAX];

    if (p->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no link name given\n");
        return;
    }
    if (ods5_unescape(p->filename, strlen(p->filename), literal, sizeof(literal)) == (size_t) -1) {
        report_error("%%ODS2-E-BADNAME, malformed ^ escape in the link name\n");
        return;
    }
    resolve_effective_path(p, effective_dir, sizeof(effective_dir));
    r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    r = ods2_create_symlink(vol, dir_header, literal, target, &new_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-SYMLINK, %s\n", r.problem);
        return;
    }
    {
        char completed[ODS2_PATH_MAX];
        if (ods5_complete_name(literal, completed, sizeof(completed))) {
            display_name(completed, shown, sizeof(shown));
        } else {
            snprintf(shown, sizeof(shown), "%s", literal);
        }
    }
    printf("%%ODS2-I-SYMLINK, %s;1 -> %s\n", shown, target);
}

/* LINK <new-path> <existing-path>   (also SET FILE/ENTER=<new-path> <existing-path>):
   another directory entry for an existing file, on a HARDLINKS volume. */
static void cmd_link(ods2_volume_t *vol, const ods2_parsed_path_t *np, const ods2_parsed_path_t *ep)
{
    uint8_t dir_header[512], target_dir[512];
    ods2_fid_t dir_fid, target_dir_fid, target_fid;
    char literal[ODS2_PATH_MAX], tliteral[ODS2_PATH_MAX], effective_dir[ODS2_PATH_MAX];
    ods2_result_t r;

    if (np->filename[0] == '\0' || ep->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, LINK needs a file name for the new entry and for the file\n");
        return;
    }
    if (ods5_unescape(np->filename, strlen(np->filename), literal, sizeof(literal)) == (size_t) -1 ||
        ods5_unescape(ep->filename, strlen(ep->filename), tliteral, sizeof(tliteral)) == (size_t) -1) {
        report_error("%%ODS2-E-BADNAME, malformed ^ escape in a file name\n");
        return;
    }
    resolve_effective_path(ep, effective_dir, sizeof(effective_dir));
    r = resolve_dir(vol, effective_dir, target_dir, &target_dir_fid);
    if (r.ok) r = ods2_lookup_name_version(vol, target_dir, tliteral, ep->version, &target_fid, NULL);
    if (!r.ok) {
        report_error("%%ODS2-E-FNF, the file to link to was not found\n");
        return;
    }
    resolve_effective_path(np, effective_dir, sizeof(effective_dir));
    r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    r = ods2_create_link(vol, dir_fid.fid_num, literal, target_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-LINK, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-LINKED, %s;1 is %s file (%u,%u,%u)\n", np->filename,
           (vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS)) ? "a link to" : "an alias of",
           (unsigned) target_fid.fid_num, (unsigned) target_fid.fid_seq, (unsigned) target_fid.fid_rvn);
}

/* IMPORT <linux-directory> <path>: copies a Linux directory tree onto the
   volume as EXPORT writes one: UTF-8 names (a "%2F" in a Linux name is a '/'
   in the VMS name), directories made as needed, regular files as Stream_LF,
   Linux symbolic links as VMS symbolic links (ODS-5). Names VMS would refuse
   and files that already exist are reported and skipped. */
typedef struct {
    ods2_volume_t *vol;
    int files, dirs, links, errors, skipped;
    unsigned long long bytes;
} import_stats_t;

static int import_name_cmp(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static void import_decode_name(const char *linux_name, char *out, size_t cap)
{
    /* the inverse of EXPORT's names: %2F is '/', %XX of a control character that character */
    if (ods5_unescape_from_linux(linux_name, out, cap) == (size_t) -1) snprintf(out, cap, "%s", linux_name);
}

static void import_tree(import_stats_t *st, const char *local_dir, unsigned dir_file_number, int depth)
{
    struct dirent **list = NULL;
    int count, i;

    if (depth > 200) {
        report_error("%%ODS2-E-DEEP, %s: directories nested more than 200 levels\n", local_dir);
        st->errors++;
        return;
    }
    count = scandir(local_dir, &list, NULL, import_name_cmp);
    if (count < 0) {
        report_error("%%ODS2-E-OPENIN, could not read directory %s: %s\n", local_dir, strerror(errno));
        st->errors++;
        return;
    }
    for (i = 0; i < count; i++) {
        const char *entry = list[i]->d_name;
        char path[ODS2_PATH_MAX * 2], literal[ODS2_PATH_MAX], shown[ODS2_PATH_MAX];
        uint8_t dir_header[512];
        struct stat sb;
        ods2_result_t r;
        ods2_fid_t fid;

        if (strcmp(entry, ".") == 0 || strcmp(entry, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", local_dir, entry);
        import_decode_name(entry, literal, sizeof(literal));
        display_name(literal, shown, sizeof(shown));
        if (lstat(path, &sb) != 0) {
            report_error("%%ODS2-E-OPENIN, %s: %s\n", path, strerror(errno));
            st->errors++;
            continue;
        }
        r = ods2_read_header(st->vol, dir_file_number, dir_header);
        if (!r.ok) {
            report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
            st->errors++;
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            char dirname[ODS2_PATH_MAX + 8];
            snprintf(dirname, sizeof(dirname), "%s.DIR", literal);
            if (ods2_lookup_name(st->vol, dir_header, dirname, &fid).ok) {
                /* already there: put the files into it, if it is a directory */
                uint8_t existing[512];
                if (!ods2_read_header(st->vol, fid.fid_num, existing).ok ||
                    !(((const ods2_head_core_t *) existing)->filechar & 0x2000u)) {
                    report_error("%%ODS2-E-NOTDIR, %s: a file of that name exists and is not a directory\n", shown);
                    st->errors++;
                    continue;
                }
            } else {
                r = ods2_create_directory(st->vol, dir_header, literal, &fid);
                if (!r.ok) {
                    report_error("%%ODS2-E-CREATEERR, %s: %s\n", shown, r.problem);
                    st->errors++;
                    continue;
                }
                st->dirs++;
            }
            import_tree(st, path, fid.fid_num, depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            host_input_t in;
            ods2_content_source_t src;
            ods2_record_attrs_t attrs;
            if (ods2_lookup_name(st->vol, dir_header, literal, &fid).ok) {
                fprintf(stderr, "%%ODS2-W-EXISTS, %s already exists: skipped\n", shown);
                st->skipped++;
                continue;
            }
            g_open_nofollow = true;
            {
                bool opened = host_input_open(path, &in);
                g_open_nofollow = false;
                if (!opened) {
                    st->errors++;
                    continue;
                }
            }
            memset(&src, 0, sizeof(src));
            src.length = in.length;
            src.read = host_input_read;
            src.ctx = &in;
            src.problem = in.problem;
            attrs.rtype = ODS2_RFM_STMLF;
            attrs.rattrib = 0;
            attrs.rsize = 512;
            attrs.maxrec = 512;
            r = ods2_create_file_source(st->vol, dir_header, literal, &src, &attrs, &fid);
            fclose(in.f);
            if (!r.ok) {
                report_error("%%ODS2-E-COPYERR, %s: %s\n", shown, r.problem);
                st->errors++;
                continue;
            }
            st->files++;
            st->bytes += in.length;
        } else if (S_ISLNK(sb.st_mode)) {
            char target[ODS5_SYMLINK_MAX + 2];
            ssize_t n = readlink(path, target, sizeof(target) - 1);
            if (n <= 0 || n > ODS5_SYMLINK_MAX) {
                report_error("%%ODS2-E-SYMLINK, %s: cannot read the link or its target is too long\n", shown);
                st->errors++;
                continue;
            }
            target[n] = '\0';
            r = ods2_create_symlink(st->vol, dir_header, literal, target, &fid);
            if (!r.ok) {
                report_error("%%ODS2-E-SYMLINK, %s: %s\n", shown, r.problem);
                st->errors++;
                continue;
            }
            st->links++;
        } else {
            fprintf(stderr, "%%ODS2-W-SPECIAL, %s is not a regular file, directory or link: skipped\n", shown);
            st->skipped++;
        }
    }
    for (i = 0; i < count; i++) free(list[i]);
    free(list);
}

static void cmd_import_tree(ods2_volume_t *vol, const char *local_dir, const ods2_parsed_path_t *p)
{
    import_stats_t st;
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_result_t r;
    char effective_dir[ODS2_PATH_MAX];

    memset(&st, 0, sizeof(st));
    st.vol = vol;
    resolve_effective_path(p, effective_dir, sizeof(effective_dir));
    r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    import_tree(&st, local_dir, dir_fid.fid_num, 0);
    printf("%%ODS2-I-IMPORTED, %d file%s (%llu bytes), %d director%s, %d symbolic link%s imported "
           "from %s; %d skipped, %d error%s\n", st.files, st.files == 1 ? "" : "s", st.bytes,
           st.dirs, st.dirs == 1 ? "y" : "ies", st.links, st.links == 1 ? "" : "s", local_dir,
           st.skipped, st.errors, st.errors == 1 ? "" : "s");
}

/* "VOLUME", abbreviated to at least "VOL", with any qualifiers after
   it ("VOLUME/LABEL=X"). */
static bool is_volume_word(const char *token)
{
    const char *slash = strchr(token, '/');
    size_t len = slash ? (size_t) (slash - token) : strlen(token);
    return keyword(token, len, "VOLUME", 3);
}

/* A 12-byte, space-padded name field, without its padding. */
static void print_name(const char *what, const char *field)
{
    int len = 12;
    while (len > 0 && (field[len - 1] == ' ' || field[len - 1] == '\0')) len--;
    printf("  %-20s%.*s\n", what, len, field);
}

/* SET VOLUME/LABEL=name (issue #13), as on VMS; the qualifier may also
   come as a separate word ("SET VOLUME /LABEL=name"). */
static void cmd_set_volume(ods2_volume_t *vol, char **tokens, int n)
{
    const char *label = NULL;
    int i;

    for (i = 1; i < n; i++) {
        const char *q = (i == 1) ? strchr(tokens[1], '/') : tokens[i];
        while (q != NULL && *q == '/') {
            const char *name = ++q, *value = NULL;
            size_t name_len;
            while (*q && *q != '=' && *q != '/') q++;
            name_len = (size_t) (q - name);
            if (*q == '=') {
                value = ++q;
                while (*q && *q != '/') q++;
            }
            if (!keyword(name, name_len, "LABEL", 3)) {
                report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - SET VOLUME "
                             "takes /LABEL=name\n", (int) name_len, name);
                return;
            }
            if (value == NULL || value == q) {
                report_error("%%ODS2-E-VALREQ, /LABEL needs a value, e.g. "
                             "SET VOLUME/LABEL=ITANKITS\n");
                return;
            }
            {
                static char buf[64];
                size_t len = (size_t) (q - value);
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, value, len);
                buf[len] = '\0';
                label = buf;
            }
        }
        if (i > 1 && (tokens[i][0] != '/')) {
            report_error("%%ODS2-E-MAXPARM, SET VOLUME takes no parameters here - the "
                         "volume is the disk image (unexpected \"%s\")\n", tokens[i]);
            return;
        }
    }
    if (label == NULL) {
        report_error("%%ODS2-E-NOPARM, usage: SET VOLUME/LABEL=name\n");
        return;
    }
    {
        unsigned copies = 0;
        char old[13];
        ods2_result_t r;
        memcpy(old, vol->home.volname, 12);
        old[12] = '\0';
        r = ods2_set_volume_label(vol, label, &copies);
        if (!r.ok) {
            report_error("%%ODS2-E-SETVOL, %s\n", r.problem);
            return;
        }
        {
            int len = 12;
            while (len > 0 && old[len - 1] == ' ') len--;
            old[len] = '\0';
        }
        {
            int len = 12;
            while (len > 0 && vol->home.volname[len - 1] == ' ') len--;
            printf("%%ODS2-I-RELABELED, volume label changed from %s to %.*s "
                   "(%u home block copies)\n", old, len, vol->home.volname, copies);
        }
    }
}

/* HM2$W_VOLCHAR as a list of keywords like DCL's /VOLUME_CHARACTERISTICS
   and SHOW DEVICE/FULL use them. Bits 5-7 mean something only on an
   ODS-5 volume. */
static void format_volchar(const ods2_home_t *h, char *out, size_t cap)
{
    static const struct { uint16_t bit; const char *name; bool ods5_only; } bits[] = {
        { ODS5_VC_READCHECK,        "READCHECK",        false },
        { ODS5_VC_WRITCHECK,        "WRITECHECK",       false },
        { ODS5_VC_ERASE,            "ERASE",            false },
        { ODS5_VC_NOHIGHWATER,      "NOHIGHWATER",      false },
        { ODS5_VC_CLASS_PROT,       "CLASS_PROTECTION", false },
        { ODS5_VC_ACCESSTIMES,      "ACCESS_DATES",     true  },
        { ODS5_VC_HARDLINKS,        "HARDLINKS",        true  },
        { ODS5_VC_NO_SPECIAL_FILES, "NO_SPECIAL_FILES", true  },
    };
    uint16_t known = 0;
    size_t i, len = 0;

    out[0] = '\0';
    for (i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (bits[i].ods5_only && !ods2_home_is_ods5(h)) continue;
        known |= bits[i].bit;
        if (h->volchar & bits[i].bit) {
            len += (size_t) snprintf(out + len, cap - len, "%s%s",
                                     len ? ", " : "", bits[i].name);
            if (len >= cap) { out[cap - 1] = '\0'; return; }
        }
    }
    if (h->volchar & ~known) {
        snprintf(out + len, cap - len, "%s0x%04X (unknown)",
                 len ? ", " : "", (unsigned) (h->volchar & ~known));
    }
    if (out[0] == '\0') snprintf(out, cap, "none");
}

/* SHOW VOLUME (issue #13): the home block's identification, and space. */
static void cmd_show_volume(ods2_volume_t *vol)
{
    const ods2_home_t *h = &vol->home;
    uint64_t total = 0, free_blocks = 0;
    ods2_result_t r = ods2_volume_space(vol, &total, &free_blocks);
    bool in_set = false;
    int i;

    for (i = 0; i < 12; i++) {
        if (h->strucname[i] != ' ' && h->strucname[i] != '\0') in_set = true;
    }
    print_name("Volume label:", h->volname);
    print_name("Owner name:", h->ownername);
    print_name("Format:", h->format);
    printf("  %-20sODS-%u, version %u\n", "Structure level:", h->struclev >> 8,
           h->struclev & 0xffu);
    {
        char vc[160];
        format_volchar(h, vc, sizeof(vc));
        printf("  %-20s%s\n", "Characteristics:", vc);
    }
    printf("  %-20s%u\n", "Cluster size:", h->cluster);
    printf("  %-20s%u\n", "Maximum files:", (unsigned) h->maxfiles);
    printf("  %-20s[%o,%o]\n", "Volume owner UIC:", h->volowner_uic[1], h->volowner_uic[0]);
    if (in_set || h->rvn != 0) {
        int len = 12;
        while (len > 0 && (h->strucname[len - 1] == ' ' || h->strucname[len - 1] == '\0')) len--;
        printf("  %-20s%.*s, volume %u of %u\n", "Volume set:", len, h->strucname,
               h->rvn, h->setcount);
    } else {
        printf("  %-20snone\n", "Volume set:");
    }
    if (r.ok) {
        printf("  %-20s%llu\n", "Total blocks:", (unsigned long long) total);
        printf("  %-20s%llu\n", "Free blocks:", (unsigned long long) free_blocks);
    } else {
        report_error("%%ODS2-E-READERR, could not count free space: %s\n", r.problem);
    }
}

static bool stdout_sink(void *ctx, const uint8_t *data, size_t n)
{
    (void) ctx;
    return fwrite(data, 1, n, stdout) == n;
}

static void cmd_type(ods2_volume_t *vol, const ods2_parsed_path_t *p, bool raw, bool lines)
{
    uint8_t dir_header[512], file_header[512];
    ods2_fid_t dir_fid, file_fid;
    ods2_result_t r;
    char spec[ODS2_PATH_MAX + 16];

    if (p->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no filename given\n");
        return;
    }
    format_file_spec(p, spec, sizeof(spec));
    if (p->version_wildcard) {
        report_error("%%ODS2-E-BADVER, %s: TYPE takes one version, not ;*\n", spec);
        return;
    }
    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    /* p->version is 0 (highest) when no version was given. The name is a
       spelling (^-escapes): look up the characters it stands for. */
    {
        char literal[ODS2_PATH_MAX];
        if (ods5_unescape(p->filename, strlen(p->filename), literal, sizeof(literal)) == (size_t) -1) {
            report_error("%%ODS2-E-BADNAME, %s: malformed ^ escape in the file name\n", spec);
            return;
        }
        r = ods2_lookup_name_version(vol, dir_header, literal, p->version, &file_fid, NULL);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-FNF, %s not found\n", spec);
        return;
    }
    r = ods2_read_header(vol, file_fid.fid_num, file_header);
    if (!r.ok) {
        report_error("%%ODS2-E-READERR, %s\n", r.problem);
        return;
    }
    if (!ods2_header_is_file(file_header, file_fid)) {
        report_error("%%ODS2-E-FNF, %s not found (a dangling entry: its file is gone)\n", spec);
        return;
    }
    /* Streamed a chunk at a time, so any size of file can be typed
       without holding it in memory (issue #16). A short write (a full
       disk, a closed pipe) is a failed TYPE: whatever was extracting
       the file got less than all of it. */
    {
        bool sink_failed = false;
        size_t bytes = 0;
        const char *note = NULL;
        r = read_file_content(vol, file_header, !raw, lines ? ODS2_REC_LINES : 0, stdout_sink, NULL, &bytes, &sink_failed, &note);
        if (r.ok && fflush(stdout) != 0) sink_failed = true;
        if (note != NULL && r.ok && !sink_failed) fprintf(stderr, "%%ODS2-W-RECORDS, %s: %s\n", spec, note);
        if (sink_failed) {
            report_error("%%ODS2-E-WRITEERR, %s: could not write all %zu bytes to output\n",
                         spec, ods2_file_content_length(file_header));
        } else if (!r.ok) {
            report_error("%%ODS2-E-READERR, %s: %s\n", spec, r.problem);
        }
    }
}

static void cmd_delete(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_result_t r;
    const char *dir_path;
    const char *name;
    char effective_path[ODS2_PATH_MAX];
    char split_buf[ODS2_PATH_MAX];
    char name_buf[ODS2_PATH_MAX];

    int version = p->version; /* 0 (highest) when none was given */
    uint16_t deleted_version = 0;
    int deleted = 0;

    resolve_effective_path(p, effective_path, sizeof(effective_path));

    if (p->filename[0] != '\0') {
        /* Normal case: "[SOMEDIR]FILE.TXT" - delete FILE.TXT from
           SOMEDIR directly. */
        dir_path = effective_path;
        if (ods5_unescape(p->filename, strlen(p->filename), name_buf, sizeof(name_buf)) ==
            (size_t) -1) {
            report_error("%%ODS2-E-BADNAME, malformed ^ escape in the file name\n");
            return;
        }
        name = name_buf;
    } else {
        /* Bare directory reference like "[TESTDIR]" - the directory
           itself is the target, one level up from where
           ods2_lookup_path would otherwise resolve it. Append .DIR,
           matching how it's actually stored as a directory entry. */
        const char *parent_path;
        const char *last_name;
        if (effective_path[0] == '\0') {
            report_error("%%ODS2-E-BADPATH, cannot delete the root directory\n");
            return;
        }
        split_last_component(effective_path, split_buf, &parent_path, &last_name);
        {
            char last_literal[ODS2_PATH_MAX - 8];
            if (ods5_unescape(last_name, strlen(last_name), last_literal, sizeof(last_literal)) ==
                (size_t) -1) {
                report_error("%%ODS2-E-BADNAME, malformed ^ escape in the directory name\n");
                return;
            }
            snprintf(name_buf, sizeof(name_buf), "%s.DIR", last_literal);
        }
        dir_path = parent_path;
        name = name_buf;
    }

    r = resolve_dir(vol, dir_path, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }

    /* One version (the one selected, the highest by default), or with
       ";*" every version - highest first, until none remain. */
    do {
        r = ods2_delete_version(vol, dir_fid.fid_num, name,
                                p->version_wildcard ? ODS2_VERSION_HIGHEST : version,
                                &deleted_version);
        if (!r.ok) break;
        printf("%%ODS2-I-DELETED, deleted %s;%u\n", name, deleted_version);
        deleted++;
    } while (p->version_wildcard);

    if (!r.ok && !(p->version_wildcard && deleted > 0)) {
        char spec[ODS2_PATH_MAX + 16];
        if (p->filename[0] != '\0') {
            format_file_spec(p, spec, sizeof(spec));
        } else {
            snprintf(spec, sizeof(spec), "%s", name);
        }
        report_error("%%ODS2-E-DELETEERR, %s: %s\n", spec, r.problem);
    }
}

static void print_help(void)
{
    printf("Commands (VMS bracket-notation paths, e.g. [DECUS.NETLIB020]FILE.TXT):\n"
           "  DIR [path] [wildcard]      list a directory\n"
           "  DIR [path...] [wildcard]   list a directory and everything beneath it\n"
           "  DIR [*...] [wildcard]      '*' '%%' in directory names, '...' anywhere\n"
           "                             ([*], [*.SRC], [...SALES], [DECUS...SRC])\n"
           "  DIR/UTF8 ...               show names as UTF-8 (default: with ^-escapes as DCL\n"
           "                             shows them: ^_ space, ^. dot, ^xx and ^Uxxxx)\n"
           "  DIR/FULL ...               per file: id, size, owner, protection, dates (also\n"
           "                             access and attribute dates on ODS-5), record format\n"
           "  SET NAMES UTF8|ESCAPED     the same for every command that shows names\n"
           "  EXPORT[/VERSIONS][/OVERWRITE][/LINKS][/RECORDS|/LINES] <path> <linux-path>\n"
           "                             copy files off the volume to Linux files (UTF-8 names;\n"
           "                             wildcards, [dir...] recreates the tree; symbolic links\n"
           "                             become Linux symbolic links); \"quotes\" keep a space;\n"
           "                             the bytes as stored, or with /RECORDS sequential files as\n"
           "                             text as TYPE shows them (/LINES: one line per record)\n"
           "  CREATE/DIRECTORY path      create a directory\n"
           "  COPY <local-file> <path>   write a local file onto the disk (Stream_LF)\n"
           "  COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) <local-file> <path>\n"
           "                             ... with chosen record attributes: RFM: FIX,\n"
           "                             STMLF, STM, STMCR or UDF; MRS: record size;\n"
           "                             RAT: NONE, CR, FTN, PRN, BLK (one per RAT:)\n"
           "  COPY/CONVERT <local-file> <path>\n"
           "                             a text file as variable-length records with\n"
           "                             CR carriage control (VMS's native text), one\n"
           "                             per line; with /ATTRIBUTES for other RAT/MRS\n"
           "  IMPORT <linux-dir> <path>  copy a Linux directory tree onto the disk (UTF-8 names,\n"
           "                             directories made, symbolic links kept, ODS-5)\n"
           "  SYMLINK <link-path> <target>   make a symbolic link (ODS-5; as CREATE/SYMLINK=target)\n"
           "  LINK <new-path> <existing-path> another directory entry for a file (as SET\n"
           "                             FILE/ENTER=new existing): a hard link with a link count on\n"
           "                             a HARDLINKS volume, otherwise a plain alias (count unchanged)\n"
           "  TYPE[/RAW|/LINES] path     print a sequential file as text the way VMS TYPE shows its\n"
           "                             records (VAR, VFC, FIX, STM*; carriage control applied:\n"
           "                             none = records run together); /LINES: always one line\n"
           "                             per record; /RAW: the stored bytes\n"
           "  DELETE path                delete a file (one version) or empty directory\n"
           "  SET DEFAULT path           set the current default directory\n"
           "  SHOW DEFAULT               show the current default directory\n"
           "  SET VOLUME/LABEL=name      relabel the volume (all home block copies)\n"
           "  SHOW VOLUME                label, owner, cluster size, files, free space\n"
           "  HELP                       show this command list\n"
           "  EXIT / QUIT                leave the interactive shell\n"
           "\n"
           "Paths starting with '.' or '-' inside brackets (e.g. [.NETLIB020],\n"
           "[-], [-.SIBLING]) and bare filenames with no brackets at all are\n"
           "relative to the current default directory (see SET DEFAULT); '-'\n"
           "means \"go up one level\" (repeat as \"-.-\" for more); other bracketed\n"
           "paths are always absolute from root, matching real VMS. [...] is the\n"
           "default directory and below; [*...] every directory on the disk.\n"
           "\n"
           "Versions: NAME;3 is version 3; NAME, NAME; or NAME;0 the highest;\n"
           "NAME;-1 the next lower existing version; NAME;* every version\n"
           "(DIR and DELETE). DIR lists every version unless one is given.\n");
}

/* Splits `line` into up to MAX_TOKENS whitespace-separated tokens,
   modifying `line` in place (inserting nuls) - tokens[] point into it. */
static int tokenize(char *line, char *tokens[MAX_TOKENS])
{
    int count = 0;
    char *p = line;
    while (*p && count < MAX_TOKENS) {
        while (*p && isspace((unsigned char) *p)) p++;
        if (!*p) break;
        tokens[count++] = p;
        {
            /* "..." keeps white space inside one token (a Linux path with a
               space, a name with a literal space); the quotes are dropped */
            char *w = p;
            bool in_quote = false;
            while (*p && (in_quote || !isspace((unsigned char) *p))) {
                if (*p == '"') { in_quote = !in_quote; p++; continue; }
                *w++ = *p++;
            }
            if (*p) p++;
            *w = '\0';
        }
    }
    return count;
}

/* Executes one command line against the mounted volume. Returns
   false if the command was EXIT/QUIT (caller should stop looping). */
static bool execute_line_inner(ods2_volume_t *vol, char *line);

/* Runs one command, then says so if directory records could not be read while it ran:
   they were skipped, so what the command shows may be incomplete. */
static bool execute_line(ods2_volume_t *vol, char *line)
{
    unsigned before = vol->dir_block_errors;
    bool keep_going = execute_line_inner(vol, line);
    if (vol->dir_block_errors != before) {
        fprintf(stderr, "%%ODS2-W-DIRBLOCK, %u directory record%s could not be read and %s skipped: "
                        "the directory is damaged, the listing may be incomplete\n",
                vol->dir_block_errors - before, vol->dir_block_errors - before == 1 ? "" : "s",
                vol->dir_block_errors - before == 1 ? "was" : "were");
    }
    return keep_going;
}

static bool execute_line_inner(ods2_volume_t *vol, char *line)
{
    char *tokens[MAX_TOKENS];
    char verb[32];          /* the command word without qualifiers */
    const char *quals;      /* its qualifiers ("/ATTRIBUTES=..."), or NULL */
    size_t verb_len;
    int n = tokenize(line, tokens);
    if (n == 0) return true; /* blank line */

    /* COPY takes qualifiers on the command word itself
       (COPY/ATTRIBUTES=...) - not on its arguments, since a host path
       contains '/' too. CREATE/DIRECTORY is matched whole, below. */
    quals = strchr(tokens[0], '/');
    verb_len = quals ? (size_t) (quals - tokens[0]) : strlen(tokens[0]);
    if (verb_len >= sizeof(verb)) verb_len = 0; /* too long to be any command */
    memcpy(verb, tokens[0], verb_len);
    verb[verb_len] = '\0';

    if (matches(tokens[0], "EXIT") || matches(tokens[0], "QUIT")) {
        return false;
    }
    if (matches(tokens[0], "HELP") || matches(tokens[0], "?")) {
        print_help();
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && matches(tokens[1], "NAMES")) {
        if (n > 2 && matches(tokens[2], "UTF8")) {
            g_utf8_names = true;
        } else if (n > 2 && (matches(tokens[2], "ESCAPED") || matches(tokens[2], "DCL"))) {
            g_utf8_names = false;
        } else {
            report_error("%%ODS2-E-NOPARM, usage: SET NAMES UTF8|ESCAPED\n");
        }
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && is_volume_word(tokens[1])) {
        cmd_set_volume(vol, tokens, n);
        return true;
    }
    if (matches(tokens[0], "SHOW") && n > 1 && is_volume_word(tokens[1])) {
        cmd_show_volume(vol);
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 &&
        (matches(tokens[1], "DEFAULT") || matches(tokens[1], "DEF"))) {
        ods2_parsed_path_t p;
        char effective[ODS2_PATH_MAX];
        uint8_t header[512];
        ods2_fid_t fid;
        ods2_result_t r;
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: SET DEFAULT <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "SET DEFAULT", tokens[2])) return true;
        if (p.filename[0] != '\0') {
            report_error("%%ODS2-E-BADPATH, SET DEFAULT takes a directory, "
                            "not a file (%s)\n", tokens[2]);
            return true;
        }
        resolve_effective_path(&p, effective, sizeof(effective));
        /* Validate the target actually exists before accepting it -
           an invalid SET DEFAULT should never silently leave the
           session pointed at a directory that doesn't exist. */
        r = ods2_lookup_path(vol, effective, &fid);
        if (!r.ok) {
            report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
            return true;
        }
        r = ods2_read_header(vol, fid.fid_num, header);
        if (r.ok && !(((ods2_head_core_t *) header)->filechar & 0x2000u)) {
            report_error("%%ODS2-E-NOTDIR, %s is not a directory\n", tokens[2]);
            return true;
        }
        canonical_dir_path(vol, effective, sizeof(effective));
        strncpy(g_default_dir, effective, sizeof(g_default_dir) - 1);
        g_default_dir[sizeof(g_default_dir) - 1] = '\0';
        printf("%%ODS2-I-DEFSET, default set to [%s%s]\n",
               (g_default_dir[0] == '\0') ? "000000" : "", g_default_dir);
        return true;
    }
    if (matches(tokens[0], "SHOW") && n > 1 &&
        (matches(tokens[1], "DEFAULT") || matches(tokens[1], "DEF"))) {
        printf("  [%s%s]\n", (g_default_dir[0] == '\0') ? "000000" : "", g_default_dir);
        return true;
    }
    if (matches(verb, "DIR") || matches(verb, "LS")) {
        ods2_parsed_path_t p;
        bool saved_utf8 = g_utf8_names, saved_paths = g_paths_mode,
             saved_full = g_full_mode;
        const char *arg = (n > 1) ? tokens[1] : ""; /* "" resolves to the
                                                         current default
                                                         directory, via the
                                                         same relative-path
                                                         logic as a bare
                                                         filename */
        if (!ods2_parse_path(arg, &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", arg);
            return true;
        }
        /* An explicit third token overrides any wildcard already
           parsed from the path itself (e.g. "DIR [000000] *.SYS"
           rather than "DIR [000000]*.SYS" in one token). */
        if (n > 2) {
            strncpy(p.filename, tokens[2], sizeof(p.filename) - 1);
            p.filename[sizeof(p.filename) - 1] = '\0';
            if (!ods2_split_version(p.filename, &p.has_version,
                                    &p.version_wildcard, &p.version)) {
                report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
                return true;
            }
        }
        /* qualifiers: /UTF8 shows names as UTF-8 instead of with ^-escapes */
        if (quals != NULL) {
            char q[64];
            const char *s = quals;
            while (*s == '/') {
                size_t ql = strcspn(s + 1, "/");
                if (ql >= sizeof(q)) ql = sizeof(q) - 1;
                memcpy(q, s + 1, ql);
                q[ql] = '\0';
                if (matches(q, "FID")) {
                    g_paths_fid = true;
                } else if (matches(q, "FULL")) {
                    g_full_mode = true;
                } else if (matches(q, "PATHS")) {
                    g_paths_mode = true;
                    g_utf8_names = true;
                } else if (matches(q, "UTF8")) {
                    g_utf8_names = true;
                } else if (matches(q, "ESCAPED")) {
                    g_utf8_names = false;
                } else {
                    report_error("%%ODS2-E-BADQUAL, unrecognized DIR qualifier /%s "
                                 "(use /FULL, /UTF8, /ESCAPED or /PATHS)\n", q);
                    g_utf8_names = saved_utf8;
                    g_paths_mode = saved_paths;
                    g_full_mode = saved_full;
                    g_paths_fid = false;
                    return true;
                }
                s += 1 + strcspn(s + 1, "/");
            }
        }
        cmd_dir(vol, &p);
        g_utf8_names = saved_utf8;
        g_paths_mode = saved_paths;
        g_full_mode = saved_full;
        g_paths_fid = false;
        return true;
    }
    if (matches(verb, "EXPORT") || matches(verb, "GET")) {
        ods2_parsed_path_t p;
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: EXPORT <path> <linux-file-or-directory>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        cmd_export(vol, &p, tokens[2], quals);
        return true;
    }
    if (matches(tokens[0], "CREATE/DIRECTORY") || matches(tokens[0], "CREATE/DIR") ||
        matches(tokens[0], "MKDIR")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: CREATE/DIRECTORY <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "CREATE/DIRECTORY", tokens[1])) return true;
        cmd_create_directory(vol, &p);
        return true;
    }
    if (matches(verb, "SYMLINK") || matches(tokens[0], "CREATE/SYMLINK") ||
        (quals != NULL && matches(verb, "CREATE") && matches(quals, "/SYMLINK"))) {
        /* SYMLINK <link-path> <target>; CREATE/SYMLINK=<target> <link-path> (VMS) */
        ods2_parsed_path_t p;
        const char *target = NULL, *link = NULL;
        if (quals != NULL && strchr(quals, '=') != NULL) {
            target = strchr(quals, '=') + 1;
            link = (n > 1) ? tokens[1] : NULL;
        } else if (n > 2) {
            link = tokens[1];
            target = tokens[2];
        }
        if (link == NULL || target == NULL || target[0] == '\0') {
            report_error("%%ODS2-E-NOPARM, usage: SYMLINK <link-path> <target>\n");
            return true;
        }
        if (!ods2_parse_path(link, &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", link);
            return true;
        }
        if (refuse_wild_dir(&p, "SYMLINK", link)) return true;
        cmd_symlink(vol, &p, target);
        return true;
    }
    if (matches(verb, "LINK") || (matches(tokens[0], "SET") && n > 1 && strncmp(tokens[1], "FILE/ENTER=", 11) == 0)) {
        /* LINK <new-path> <existing-path>; SET FILE/ENTER=<new-path> <existing-path> */
        ods2_parsed_path_t np, ep;
        const char *newp = NULL, *oldp = NULL;
        if (matches(verb, "LINK")) {
            if (n > 2) { newp = tokens[1]; oldp = tokens[2]; }
        } else {
            const char *eq = strchr(tokens[1], '=');
            if (eq != NULL && n > 2) { newp = eq + 1; oldp = tokens[2]; }
        }
        if (newp == NULL || oldp == NULL) {
            report_error("%%ODS2-E-NOPARM, usage: LINK <new-path> <existing-path>\n");
            return true;
        }
        if (!ods2_parse_path(newp, &np) || !ods2_parse_path(oldp, &ep)) {
            report_error("%%ODS2-E-BADPATH, could not parse the paths\n");
            return true;
        }
        if (refuse_wild_dir(&np, "LINK", newp) || refuse_wild_dir(&ep, "LINK", oldp)) return true;
        cmd_link(vol, &np, &ep);
        return true;
    }
    if (matches(verb, "COPY") || matches(verb, "PUT") || matches(verb, "IMPORT")) {
        ods2_parsed_path_t p;
        ods2_record_attrs_t attrs;
        copy_options_t opts;
        memset(&opts, 0, sizeof(opts));
        /* COPY's default: Stream_LF, as before qualifiers existed. */
        attrs.rtype = ODS2_RFM_STMLF;
        attrs.rattrib = 0;
        attrs.rsize = 512;
        attrs.maxrec = 512;
        if (quals != NULL && !parse_copy_qualifiers(quals, &attrs, &opts)) {
            return true;
        }
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: COPY <local-file> <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "COPY", tokens[2])) return true;
        {
            struct stat sb;
            if (matches(verb, "IMPORT") && stat(tokens[1], &sb) == 0 && S_ISDIR(sb.st_mode)) {
                if (p.filename[0] != '\0') {
                    report_error("%%ODS2-E-BADPATH, IMPORT of a directory needs a directory "
                                 "path, not a file name\n");
                    return true;
                }
                cmd_import_tree(vol, tokens[1], &p);
                return true;
            }
        }
        cmd_copy(vol, tokens[1], &p, &attrs, &opts);
        return true;
    }
    if (matches(verb, "TYPE") || matches(verb, "CAT")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: TYPE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "TYPE", tokens[1])) return true;
        {
            bool raw = false, lines = false;
            const char *s = quals;
            while (s != NULL && *s == '/') {
                char q[32];
                size_t ql = strcspn(s + 1, "/");
                if (ql >= sizeof(q)) ql = sizeof(q) - 1;
                memcpy(q, s + 1, ql);
                q[ql] = '\0';
                if (matches(q, "RAW")) raw = true;
                else if (matches(q, "LINES")) lines = true;
                else {
                    report_error("%%ODS2-E-BADQUAL, unrecognized TYPE qualifier /%s (use /RAW or /LINES)\n", q);
                    return true;
                }
                s += 1 + strcspn(s + 1, "/");
            }
            cmd_type(vol, &p, raw, lines);
        }
        return true;
    }
    if (matches(tokens[0], "DELETE") || matches(tokens[0], "RM")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: DELETE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "DELETE", tokens[1])) return true;
        cmd_delete(vol, &p);
        return true;
    }

    report_error("%%ODS2-E-ILLCMD, unrecognized command \"%s\" - try HELP\n", tokens[0]);
    return true;
}

static int run_script(ods2_volume_t *vol, const char *script_path)
{
    FILE *f = fopen(script_path, "r");
    char line[MAX_LINE];
    if (f == NULL) {
        report_error("%%ODS2-E-OPENIN, could not open script %s\n", script_path);
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        /* Skip comment/blank lines (VMS-ish convention: '!' starts a comment). */
        char *p = line;
        while (*p && isspace((unsigned char) *p)) p++;
        if (*p == '\0' || *p == '!') continue;
        printf("ODS2> %s", line);
        if (line[strlen(line) - 1] != '\n') printf("\n");
        if (!execute_line(vol, line)) break;
    }
    fclose(f);
    /* Every line runs, as before, but the script as a whole failed if
       any command in it did. */
    return g_command_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    int exit_code = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <disk-image> [command...]\n", argv[0]);
        fprintf(stderr, "       %s <disk-image> @<script-file>\n", argv[0]);
        fprintf(stderr, "       %s <disk-image>                (interactive)\n", argv[0]);
        return 1;
    }

    /* A structure version 0 volume (e.g. the OpenVMS Alpha V8.4 CD)
       is mounted for reading only, so look before opening for write. */
    r = ods2_mount(argv[1], &vol);
    if (r.ok) g_ods5 = vol.ods5;
    {
        const char *names = getenv("ODS2_NAMES");
        if (names != NULL && matches(names, "UTF8")) g_utf8_names = true;
    }
    if (r.ok && vol.version0) {
        fprintf(stderr, "%%ODS2-W-STRUCLEV0, %s: HM2$W_STRUCLEV version is 0; "
                        "mounted read-only\n", argv[1]);
    } else if (r.ok && access(argv[1], W_OK) != 0) {
        /* an image the user cannot write (chmod 444): read it, and
           refuse every write, instead of failing to mount */
        fprintf(stderr, "%%ODS2-W-READONLY, %s: image is not writable; mounted "
                        "read-only\n", argv[1]);
    } else {
        if (r.ok) ods2_dismount(&vol);
        r = ods2_mount_write(argv[1], &vol);
    }
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-F-MOUNTERR, %s: %s\n", argv[1], r.problem);
        return 1;
    }

    if (argc == 2) {
        /* Interactive shell - linenoise() returns a malloc'd line with
           no trailing newline, or NULL on EOF (Ctrl-D) or a read
           error. linenoiseHistoryAdd() copies the string internally,
           so it must run before execute_line() modifies the buffer
           in place via tokenization. Blank lines aren't added to
           history, matching typical shell behavior. */
        printf("ods2v2 - type HELP for commands, EXIT to leave\n");
        for (;;) {
            char *line = linenoise("ODS2> ");
            if (line == NULL) break; /* EOF (Ctrl-D) */
            if (line[0] != '\0') {
                linenoiseHistoryAdd(line);
            }
            {
                bool keep_going = execute_line(&vol, line);
                linenoiseFree(line);
                if (!keep_going) break;
            }
        }
    } else if (argv[2][0] == '@') {
        exit_code = run_script(&vol, argv[2] + 1);
    } else {
        /* Single-shot: join all remaining argv into one command line. */
        char line[MAX_LINE];
        int i;
        line[0] = '\0';
        for (i = 2; i < argc; i++) {
            if (i > 2) strncat(line, " ", sizeof(line) - strlen(line) - 1);
            strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
        }
        execute_line(&vol, line);
        exit_code = g_command_failed ? 1 : 0;
    }

    ods2_dismount(&vol);
    return exit_code;
}
