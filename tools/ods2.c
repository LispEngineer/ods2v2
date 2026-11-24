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
 *   COPY <local-file> <path>      write a local file onto the disk; the
 *                                 record format is chosen per file: text
 *                                 as Stream_LF (Stream for CR LF, Stream_CR
 *                                 for CR-only), anything else as fixed
 *                                 512-byte records (see choose_format());
 *                                 /FORMAT= overrides
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
 *                                 empty directory; '*', '%' and '?' in
 *                                 the file name delete every match in
 *                                 that one directory (highest version
 *                                 unless ;n, ;-n or ;* is given)
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

#define MAX_LINE 8192   /* a command line; a longer one is refused, never cut (a cut path names another file) */
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
    fflush(stdout);              /* what came before comes first when both go to one pipe */
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
    return ods2_read_header_fid(vol, *dir_fid_out, dir_header_out);
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

/* H1 (CORPUS 66.4). Versions of a name stored out of order (;1 ;2 ;3 in the record: written by another tool, or damaged): VMS follows the STORED order for every version form (TYPE
   with no version, ;0 and ;-1 take the first and the second entry stored; PURGE keeps the first stored one; SET FILE with no version changes it; DELETE with no version is
   refused with DELVER, and an explicit ;3 that is listed says "file not found"; ANALYZE: BAD_VERSORDER). ods2v2 reads the same way, and a command that changes something
   refuses such a name so that no file is lost on a damaged directory (a deliberate divergence from VMS for PURGE and SET FILE, which act on the first stored): unless it names an
   explicit version (;n) or all of them (;*). g_disorder_mode: 0 reads, 1 a change that takes versions by position (SET FILE), 2 PURGE (always refuses such a name). */
static int g_disorder_mode;
static void display_name(const char *name, char *out, size_t cap);

/* Are the entries of the name that entry `i` belongs to (neighbours with the same name) in descending order of version? */
static bool group_in_order(const ods2_dir_entry_t *entries, int count, int i)
{
    int a = i, b = i, k;
    while (a > 0 && same_name(entries[a].name, entries[a - 1].name)) a--;
    while (b + 1 < count && same_name(entries[b].name, entries[b + 1].name)) b++;
    for (k = a + 1; k <= b; k++) if (entries[k].version >= entries[k - 1].version) return false;
    return true;
}

/* Are the version numbers of the name of entry `i` all usable (1 to 32767, no two alike)? PURGE refuses a name with unusable numbers on its own, with the reason (review 49 S1). */
static bool group_usable(const ods2_dir_entry_t *entries, int count, int i)
{
    int a = i, b = i, k, m;
    while (a > 0 && same_name(entries[a].name, entries[a - 1].name)) a--;
    while (b + 1 < count && same_name(entries[b].name, entries[b + 1].name)) b++;
    for (k = a; k <= b; k++) {
        if (entries[k].version < 1 || entries[k].version > 32767) return false;
        for (m = a; m < k; m++) if (entries[m].version == entries[k].version) return false;
    }
    return true;
}

/* How many names a walk refused for their order: the commands that go on to say "nothing found" must not, when the name was found and refused (review 66 S1) */
static int g_disorder_refused;

static void report_version_order(const char *dir_path, const char *name, const char *what)
{
    char shown[ODS2_PATH_MAX];
    display_name(name, shown, sizeof(shown));
    if (strcmp(what, "purged") == 0) {
        /* PURGE takes no version (review 66 S2): the way out is DELETE with the version number of each version to go */
        report_error("%%ODS2-E-VERSORDER, [%s%s]%s: the versions of this name are stored out of order (BAD_VERSORDER): not purged; PURGE takes no version: DELETE name;n removes a version by its number\n",
                     dir_path[0] == '\0' ? "000000" : "", dir_path, shown);
        return;
    }
    report_error("%%ODS2-E-VERSORDER, [%s%s]%s: the versions of this name are stored out of order (BAD_VERSORDER): not %s; name a version (;n) or all of them (;*)\n",
                 dir_path[0] == '\0' ? "000000" : "", dir_path, shown, what);
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
static char *spelling_to_slash_path(const char *spelling)
{
    const char *p = spelling;
    size_t len = 0, cap = strlen(spelling) * 3 + 8;   /* a name grows at most threefold (%XX) */
    char *out = malloc(cap);

    if (out == NULL) return NULL;
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
        if (len >= cap) { len = cap - 1; out[len] = '\0'; break; }   /* cannot happen: cap covers the longest result */
        p = end;
        if (*p == '.') p++;
    }
    return out;
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
    ods2_result_t r = ods2_read_header_fid(vol, e->fid, header);

    display_name(e->name, shown, sizeof(shown));
    printf("\n%s;%u\n", shown, e->version);
    if (!r.ok) {
        printf("  (header not readable: %s)\n", r.problem);
        return;
    }
    if (!ods2_header_is_file(header, e->fid)) {
        printf("  no such file (the entry's file, ID (%u,%u,%u), no longer exists)\n",
               ods2_fid_number(e->fid), (unsigned) e->fid.fid_seq, (unsigned) e->fid.fid_rvn);
        return;
    }
    printf("  File ID:            (%u,%u,%u)\n", ods2_fid_number(e->fid),
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
    printf("  Size:               %llu/%u blocks, %llu bytes\n",
           (unsigned long long) ((ods2_file_content_length(header) + 511) / 512), (unsigned) hiblk,
           (unsigned long long) ods2_file_content_length(header));
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
            unsigned next = ods2_fid_number(hc->ext_fid);
            ods2_acl_walk(h, acl_print_line, &ap);
            if (next == 0 || !ods2_read_header(vol, next, ext).ok ||
                ods2_fid_number(((const ods2_head_core_t *) ext)->fid) != next) break;
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
    printf("  Backup:             %s\n", id.bakdate ? t : "<No backup recorded>");
    if (id.has_ods5_dates) {
        ods5_format_vms_time(id.accdate, t, sizeof(t));
        printf("  Accessed:           %s\n", t);
        ods5_format_vms_time(id.attdate, t, sizeof(t));
        printf("  Attributes changed: %s\n", t);
        ods5_format_vms_time(id.revdate, t, sizeof(t));
        printf("  Data modified:      %s\n", t);
    } else if (vol->ods5) {
        /* a level 2 header on an ODS-5 volume (the reserved files): VMS prints no access or attribute-change
           date, and the revision date as the data modification date (AXP1SYS, 10 files) */
        ods5_format_vms_time(0, t, sizeof(t));
        printf("  Accessed:           %s\n", t);
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
            size_t got = 0;
            uint64_t len = ods2_file_content_length(header);
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
    char refused_name[ODS2_DIR_MAX_NAME + 1] = "";
    bool g_ok = true, g_use = true;        /* the verdict on the name of the current group, worked out once at its first entry (review 66 S3: not once per entry) */

    for (i = 0; i < count; i++) {
        rank = (i > 0 && same_name(entries[i].name, entries[i - 1].name)) ? rank + 1 : 0;
        if (rank == 0 && g_disorder_mode != 0 && g_entry_hook != NULL) {
            g_ok = group_in_order(entries, count, i);
            g_use = g_ok ? true : group_usable(entries, count, i);
        }
        if (ods2_wildcard_match(pattern, entries[i].name) &&
            version_selected(p, &entries[i], rank)) {
            if (g_disorder_mode != 0 && g_entry_hook != NULL && !g_ok &&
                (g_disorder_mode == 1 ? !(p->has_version && (p->version_wildcard || p->version > 0)) : g_use)) {
                if (strcmp(refused_name, entries[i].name) != 0) {                 /* once for each name; the other names go on */
                    report_version_order(dir_path, entries[i].name, g_disorder_mode == 2 ? "purged" : "changed");
                    snprintf(refused_name, sizeof(refused_name), "%s", entries[i].name);
                    g_disorder_refused++;
                }
                continue;
            }
            if (g_entry_hook != NULL) {
                g_entry_hook(g_entry_hook_ctx, vol, dir_path, &entries[i]);
                shown++;
                continue;
            }
            if (g_paths_mode) {
                char *slash = spelling_to_slash_path(dir_path);
                if (slash == NULL) { report_error("%%ODS2-E-NOMEM, out of memory\n"); return shown; }
                {
                    /* a '/' inside a name would read as a separator, a control character
                       would reach the terminal: %XX, as EXPORT does */
                    char safe[ODS2_DIR_MAX_NAME * 3 + 8];
                    if (ods5_escape_for_linux(entries[i].name, safe, sizeof(safe)) == (size_t) -1) {
                        snprintf(safe, sizeof(safe), "%s", "(name too long)");
                    }
                    printf("%s/%s;%u", slash, safe, entries[i].version);
                }
                free(slash);
                if (g_paths_fid) {
                    printf("\t%u,%u,%u", ods2_fid_number(entries[i].fid),
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
        if (!ods2_read_header_fid(vol, fid, header).ok ||
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

/* A tree as deep as the volume holds is walked: OpenVMS V7.3-2 made 400 levels, and its own wildcard walk stops
   at 256, so a VMS-made volume can hold more than ods2v2 creates (255) and more than VMS lists. The walk keeps
   the path in one growing string and a small frame per level, so that the depth is limited by memory and by
   this bound on the C stack only; a tree deeper than the bound is reported, never cut off silently. */
#define WALK_MAX_LEVELS 2048     /* levels below the directory the walk starts at (the creation cap is the same) */
#define MAX_DIR_VISITS 1000000

/* A growing string. */
typedef struct { char *s; size_t len, cap; } gstr_t;

static bool gstr_append(gstr_t *g, const char *add, size_t n)
{
    if (g->len + n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap : 256;
        char *ns;
        while (cap < g->len + n + 1) cap *= 2;
        ns = realloc(g->s, cap);
        if (ns == NULL) return false;
        g->s = ns;
        g->cap = cap;
    }
    memcpy(g->s + g->len, add, n);
    g->len += n;
    g->s[g->len] = '\0';
    return true;
}

static void gstr_truncate(gstr_t *g, size_t len)
{
    g->len = len;
    if (g->s != NULL) g->s[len] = '\0';
}

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
    gstr_t abs;                    /* the absolute path of the directory being walked, as a spelling */
    gstr_t rel;                    /* the same relative to where the walk began */
    /* The directories on the current recursion path, [0..depth]: their FIDs and the length of their absolute
       path in `abs`, to refuse an entry that points back at one of them (a loop). Aliases that are not
       ancestors are still followed, as VMS does. */
    ods2_fid_t *path_fid;
    size_t *path_len;
    size_t path_cap;
} dir_walk_t;

/* Walks the tree below the start directory depth first, in directory
 * order (alphabetical, as VMS stores them), listing every directory
 * whose path relative to the start matches w->spec. The directory is
 * given by its header and FID; w->abs holds its absolute path, w->rel the
 * same directory relative to where the walk began ("" for the start
 * itself). Subtrees that cannot contain a match are never read. This one
 * walk serves every DIR: "[A]" (spec "" - just the start), "[A...]" (spec
 * "..."), "[*]", "[*...]", "[A...B]" and so on. */
static void walk_directories(dir_walk_t *w, ods2_fid_t dir_fid, int depth)
{
    ods2_dir_entry_t *entries = NULL;
    uint8_t dir_header[512];
    int count = 0, i;
    ods2_result_t r;
    bool here = ods2_dir_spec_match(w->spec, w->rel.s);
    bool deeper = ods2_dir_spec_could_descend(w->spec, w->rel.s);
    char *bufs;
    size_t abs_len = w->abs.len, rel_len = w->rel.len;

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
    r = ods2_read_header_fid(w->vol, dir_fid, dir_header);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, [%s]: %s\n", w->abs.s, r.problem);
        w->errors++;
        return;
    }
    r = ods2_list_directory_alloc(w->vol, dir_header, &entries, &count);
    if (!r.ok) {
        report_error("%%ODS2-E-LISTERR, [%s]: %s\n", w->abs.s, r.problem);
        w->errors++;
        return;
    }
    if (depth > WALK_MAX_LEVELS) {
        report_error("%%ODS2-E-TOODEEP, [%s] is more than %d levels deep: this directory (%d entries) and "
                     "everything below it are skipped\n", w->abs.s, WALK_MAX_LEVELS, count);
        w->errors++;
        free(entries);
        return;
    }
    if ((size_t) depth >= w->path_cap) {
        size_t nc = w->path_cap ? w->path_cap * 2 : 64;
        ods2_fid_t *nf = realloc(w->path_fid, nc * sizeof(*nf));
        size_t *nl;
        if (nf != NULL) w->path_fid = nf;
        nl = nf != NULL ? realloc(w->path_len, nc * sizeof(*nl)) : NULL;
        if (nl != NULL) w->path_len = nl;
        if (nf == NULL || nl == NULL) {
            report_error("%%ODS2-E-NOMEM, out of memory walking [%s]\n", w->abs.s);
            w->errors++;
            free(entries);
            return;
        }
        w->path_cap = nc;
    }
    w->path_fid[depth] = dir_fid;
    w->path_len[depth] = abs_len;
    bufs = malloc(2 * (size_t) ODS2_PATH_MAX);
    if (bufs == NULL) {
        report_error("%%ODS2-E-NOMEM, out of memory listing [%s]\n", w->abs.s);
        w->errors++;
        free(entries);
        return;
    }

    if (here) {
        int shown = show_directory(w->vol, w->abs.s, entries, count, w->pattern, w->p);
        if (shown > 0) {
            w->dirs_shown++;
            w->files_shown += shown;
        }
    }

    for (i = 0; deeper && i < count; i++) {
        uint8_t sub_header[512];
        char *name = bufs, *shown = bufs + ODS2_PATH_MAX;
        size_t len;
        bool ok_path;
        int d, loop = -1;

        /* Skip self-referential entries - root's own directory always
           contains a "000000.DIR" entry pointing straight back at
           itself (FID (4,4), present on every ODS-2 volume). This is
           also why "[*]" never matches [000000], as on VMS. */
        if (ods2_fid_number(entries[i].fid) == ods2_fid_number(dir_fid) &&
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
        if (ods5_escape_name(name, 0, shown, ODS2_PATH_MAX) == (size_t) -1) {
            report_error("%%ODS2-E-PATHTOOLONG, directory name too long, "
                            "skipping subtree under [%s]\n", w->abs.s);
            w->errors++;
            continue;
        }
        /* the child's path: appended to the one string, taken off again after the walk below it */
        ok_path = (w->abs.len == 0 || gstr_append(&w->abs, ".", 1)) &&
                  gstr_append(&w->abs, shown, strlen(shown)) &&
                  (w->rel.len == 0 || gstr_append(&w->rel, ".", 1)) &&
                  gstr_append(&w->rel, shown, strlen(shown));
        if (!ok_path) {
            report_error("%%ODS2-E-NOMEM, out of memory walking [%s]\n", w->abs.s);
            w->errors++;
            gstr_truncate(&w->abs, abs_len);
            gstr_truncate(&w->rel, rel_len);
            continue;
        }
        /* Cheap name test before reading the entry's header. */
        if (!ods2_dir_spec_match(w->spec, w->rel.s) &&
            !ods2_dir_spec_could_descend(w->spec, w->rel.s)) {
            goto next;
        }
        /* Is it really a directory? Checked via its own header's
           FH2$M_DIRECTORY bit (0x2000), not just the .DIR suffix. */
        r = ods2_read_header_fid(w->vol, entries[i].fid, sub_header);
        if (!r.ok && r.problem != ods2_stale_entry) {
            /* not a stale alias (a VMS state: the name is listed, nothing is below it) but a header that cannot
               be read: a whole subtree would be missing, so it is said (and the exit status is 1) */
            report_error("%%ODS2-E-DIRERR, [%s]: %s\n", w->abs.s, r.problem);
            w->errors++;
            goto next;
        }
        if (!r.ok || !(((ods2_head_core_t *) sub_header)->filechar & 0x2000u)) {
            goto next;
        }
        /* An entry naming a directory already on the path is a loop:
           warn instead of descending until the depth limit. */
        for (d = 0; d <= depth; d++) {
            if (ods2_fid_number(w->path_fid[d]) == ods2_fid_number(entries[i].fid) &&
                w->path_fid[d].fid_seq == entries[i].fid.fid_seq) {
                loop = d;
                break;
            }
        }
        if (loop >= 0) {
            fprintf(stderr, "%%ODS2-W-DIRLOOP, [%s] is the directory [%.*s] "
                            "again; not descended into\n",
                    w->abs.s, w->path_len[loop] ? (int) w->path_len[loop] : 6,
                    w->path_len[loop] ? w->abs.s : "000000");
            goto next;
        }
        walk_directories(w, entries[i].fid, depth + 1);
next:
        gstr_truncate(&w->abs, abs_len);
        gstr_truncate(&w->rel, rel_len);
    }
    free(bufs);
    free(entries);
}

/* Starts a walk at the directory `start` (a path spelling, "" for [000000]): resolved once, then walked by FID.
   Returns false (and reports) if the start cannot be found. */
static bool walk_from(dir_walk_t *w, const char *start)
{
    uint8_t header[512];
    ods2_fid_t fid;
    ods2_result_t r = resolve_dir(w->vol, start, header, &fid);

    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, [%s]: %s\n", start, r.problem);
        w->errors++;
        return false;
    }
    if (!gstr_append(&w->abs, start, strlen(start)) || !gstr_append(&w->rel, "", 0)) {
        report_error("%%ODS2-E-NOMEM, out of memory\n");
        w->errors++;
        return false;
    }
    walk_directories(w, fid, 0);
    free(w->abs.s);
    free(w->rel.s);
    free(w->path_fid);
    free(w->path_len);
    w->abs.s = w->rel.s = NULL;
    w->path_fid = NULL;
    w->path_len = NULL;
    return true;
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
    walk_from(&w, start);

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
    struct seen_fid { unsigned num; uint16_t seq; char *path; } *seen;
    size_t seen_count, seen_cap;
    char *long_dir;           /* the first directory whose Linux path was too long: [long_dir] and below are not made */
    int long_dirs;            /* directories not made for that reason (the first is named, the rest counted) */
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
                                       uint64_t *bytes_out, bool *sink_failed_out, const char **note)
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
    if (!(rec_options & ODS2_REC_LINES)) {
        const char *bad = ods2_rec_check_attrs(&a);
        if (bad != NULL) {
            if (sink_failed_out) *sink_failed_out = false;
            if (bytes_out) *bytes_out = 0;
            return (ods2_result_t) { .ok = false, .problem = bad };
        }
    }
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

/* The Linux directory for a VMS directory spelling below the export's start: x->local plus one component per level.
   False when the path would be 4,000 characters or more (the host's limit is 4,096): the caller reports it for
   the file or directory concerned, and nothing is ever cut short. */
#define EXPORT_PATH_LIMIT 4000
static bool linux_dir_for(const export_ctx_t *x, const char *dir_spelling, char *out, size_t cap)
{
    const char *rel = dir_spelling + strlen(x->start);
    size_t len;

    if (*rel == '.' && x->start[0] != '\0') rel++;
    len = (size_t) snprintf(out, cap, "%s", x->local);
    if (len >= cap || len >= EXPORT_PATH_LIMIT) return false;
    if (!x->tree) return true;
    while (*rel) {
        const char *end = rel;
        char lit[ODS2_PATH_MAX], comp[ODS2_DIR_MAX_NAME * 3 + 16];
        size_t ul;
        while (*end && *end != '.') { if (*end == '^' && end[1]) end++; end++; }
        ul = ods5_unescape(rel, (size_t) (end - rel), lit, sizeof(lit));
        if (ul == (size_t) -1) snprintf(lit, sizeof(lit), "%.*s", (int) (end - rel), rel);
        linux_component(lit, comp, sizeof(comp));
        if (len + strlen(comp) + 2 >= EXPORT_PATH_LIMIT || len + strlen(comp) + 2 >= cap) return false;
        out[len++] = '/';
        memcpy(out + len, comp, strlen(comp) + 1);
        len += strlen(comp);
        rel = end;
        if (*rel == '.') rel++;
    }
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
    ods2_result_t r = ods2_read_header_fid(vol, e->fid, header);
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
            if (nl > 4 && ods2_dir_name_compare(e->name + nl - 4, 4, ".DIR", 4) == 0) {
                char parent[ODS2_PATH_MAX * 2];
                snprintf(dname, sizeof(dname), "%.*s", (int) (nl - 4), e->name);
                linux_component(dname, dcomp, sizeof(dcomp));
                if (!linux_dir_for(x, dir_spelling, parent, sizeof(parent)) ||
                    strlen(parent) + strlen(dcomp) + 2 >= EXPORT_PATH_LIMIT) {
                    /* the first such directory is named; those below it (every one of them is too long too) are
                       counted and summarised at the end, so that one deep tree does not print hundreds of lines */
                    x->errors++;
                    x->long_dirs++;
                    if (x->long_dir == NULL) {
                        size_t ln = strlen(dir_spelling) + strlen(shown) + 3;
                        x->long_dir = malloc(ln);
                        if (x->long_dir != NULL) snprintf(x->long_dir, ln, "%s%s%s", dir_spelling, dir_spelling[0] ? "." : "", dname);
                        report_error("%%ODS2-E-PATHTOOLONG, [%s]%s: the Linux path would be too long; this directory "
                                     "and everything below it are not copied (the files in it are named one by one)\n",
                                     dir_spelling, shown);
                    }
                    return;
                }
                snprintf(dpath, sizeof(dpath), "%s/%s", parent, dcomp);
                if (make_local_dirs(dpath, strlen(x->local))) return;
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
    if (!linux_dir_for(x, dir_spelling, dirpath, sizeof(dirpath))) {
        report_error("%%ODS2-E-PATHTOOLONG, [%s]%s: the Linux path would be too long; not copied\n", dir_spelling, shown);
        x->errors++;
        return;
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
        if (strlen(dirpath) + strlen(comp) + 2 > 4095) {
            report_error("%%ODS2-E-PATHTOOLONG, [%s]%s: the Linux path would be too long; not copied\n", dir_spelling, shown);
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
        size_t got = 0;
        uint64_t len = ods2_file_content_length(header);
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
            if (x->seen[i].num == ods2_fid_number(e->fid) && x->seen[i].seq == e->fid.fid_seq) {
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
        uint64_t bytes = 0;
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
            if (note != NULL && r.ok) fprintf(stderr, "%%ODS2-W-RECORDS, %s: %s\n", shown, note);   /* a note: the exit status stays 0 */
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
            x->seen[x->seen_count].num = ods2_fid_number(e->fid);
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
    walk_from(&w, start);
    g_entry_hook = saved_hook;
    g_entry_hook_ctx = saved_ctx;
    g_paths_mode = saved_paths;

    if (x.long_dirs > 1) {
        report_error("%%ODS2-E-PATHTOOLONG, %d directories in all (from [%s] down) were not made: the Linux path "
                     "would be too long\n", x.long_dirs, x.long_dir != NULL ? x.long_dir : "?");
    }
    free(x.long_dir);
    for (i = 0; i < x.seen_count; i++) free(x.seen[i].path);
    free(x.seen);
    if (x.files + x.symlinks == 0 && x.errors == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else {
        printf("%%ODS2-I-EXPORTED, %d file%s, %d symbolic link%s copied to %s",
               x.files, x.files == 1 ? "" : "s", x.symlinks, x.symlinks == 1 ? "" : "s", local);
        if (x.dirs_skipped) printf("; %d director%s not copied", x.dirs_skipped, x.dirs_skipped == 1 ? "y" : "ies");
        if (x.errors + w.errors > 0) {
            /* every file or directory that was not copied was named on stderr, and the exit status is 1 */
            printf("; %d error%s: not everything was copied", x.errors + w.errors, x.errors + w.errors == 1 ? "" : "s");
        }
        printf("\n");
    }
}

/* SET FILE/VERSION_LIMIT=n <spec>: the version limit of the directory entries the spec names (wildcards and [dir...]
   work as in DIRECTORY). ods2v2 wrote 1 for the entries of ODS-2 volumes until it was fixed; VMS then purges version 1
   when it makes version 2 of such a file (COPY, EDIT). 32767 is "no limit", what VMS writes. */
typedef struct {
    ods2_volume_t *vol;
    uint16_t limit;
    unsigned entries, records;     /* names looked at, directory records changed */
    int errors;
    char last_dir[ODS2_PATH_MAX];
    char last_name[ODS2_DIR_MAX_NAME + 8];
    bool have_last;
} verlimit_ctx_t;

static void verlimit_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    verlimit_ctx_t *x = (verlimit_ctx_t *) ctx_;
    ods2_fid_t dfid;
    unsigned changed = 0;
    ods2_result_t r;

    /* the reserved files and the master directory's own entry keep the limit VMS gave them (1) */
    if (ods2_fid_number(e->fid) <= 10 && ods2_fid_number(e->fid) != 0 && e->fid.fid_nmx == 0) return;
    /* one call per name (the walk calls the hook once per version) */
    if (x->have_last && strcmp(x->last_dir, dir_spelling) == 0 && same_name(x->last_name, e->name)) return;
    snprintf(x->last_dir, sizeof(x->last_dir), "%s", dir_spelling);
    snprintf(x->last_name, sizeof(x->last_name), "%s", e->name);
    x->have_last = true;
    x->entries++;
    r = ods2_lookup_path(vol, dir_spelling, &dfid);
    if (r.ok) r = ods2_check_fid(dfid);
    if (r.ok) r = ods2_set_entry_version_limit(vol, ods2_fid_number(dfid), e->name, x->limit, &changed);
    if (!r.ok) {
        report_error("%%ODS2-E-VERLIMIT, [%s]%s: %s\n", dir_spelling, e->name, r.problem);
        x->errors++;
        return;
    }
    x->records += changed;
}

static void cmd_set_version_limit(ods2_volume_t *vol, const ods2_parsed_path_t *p, uint16_t limit)
{
    verlimit_ctx_t x;
    entry_hook_fn saved_hook = g_entry_hook;
    void *saved_ctx = g_entry_hook_ctx;
    bool saved_paths = g_paths_mode;
    char effective[ODS2_PATH_MAX], spec[ODS2_PATH_MAX + 4], start[ODS2_PATH_MAX], pattern[ODS2_PATH_MAX + 4];
    const char *rest;
    dir_walk_t w;

    memset(&x, 0, sizeof(x));
    x.vol = vol;
    x.limit = limit;
    resolve_effective_path(p, effective, sizeof(effective));
    snprintf(spec, sizeof(spec), "%s%s", effective, p->recursive ? "..." : "");
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }
    canonical_dir_path(vol, start, sizeof(start));
    if (p->filename[0] == '\0') snprintf(pattern, sizeof(pattern), "*.*");
    else if (ods5_find_unescaped(p->filename, '.') == NULL) snprintf(pattern, sizeof(pattern), "%s.*", p->filename);
    else snprintf(pattern, sizeof(pattern), "%s", p->filename);
    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = pattern;
    w.p = p;
    g_entry_hook = verlimit_entry;
    g_entry_hook_ctx = &x;
    g_paths_mode = false;
    walk_from(&w, start);
    g_entry_hook = saved_hook;
    g_entry_hook_ctx = saved_ctx;
    g_paths_mode = saved_paths;
    if (x.entries == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else {
        printf("%%ODS2-I-VERLIMIT, %u name%s looked at, %u directory record%s changed to version limit %u\n",
               x.entries, x.entries == 1 ? "" : "s", x.records, x.records == 1 ? "" : "s", (unsigned) limit);
    }
}

/* SET FILE/ATTRIBUTES=(MRS:0) <spec>: repairs Stream, Stream_LF and Stream_CR files whose MRS is not 0 or whose LRL is below
   their longest record (ods2v2 gave every explicit-format stream file MRS 512, and once gave a Stream file an LRL one short;
   record-mode reads then fail: %RMS-W-RTB): MRS 0 and LRL the file's own longest record (at most 32767), as VMS makes such
   files. A file already in that state is left alone, silently. The data is not touched. */
typedef struct {
    unsigned files, changed;
    int errors;
} recsize_ctx_t;

static void recsize_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    recsize_ctx_t *x = (recsize_ctx_t *) ctx_;
    unsigned changed = 0;
    ods2_result_t r;

    if (ods2_fid_number(e->fid) <= 10 && ods2_fid_number(e->fid) != 0 && e->fid.fid_nmx == 0) return;
    x->files++;
    r = ods2_set_stream_record_sizes(vol, e->fid, &changed);
    if (!r.ok) {
        report_error("%%ODS2-E-RECSIZES, [%s]%s;%u: %s\n", dir_spelling, e->name, (unsigned) e->version, r.problem);
        x->errors++;
        return;
    }
    x->changed += changed;
}

static void cmd_set_record_sizes(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    recsize_ctx_t x;
    entry_hook_fn saved_hook = g_entry_hook;
    void *saved_ctx = g_entry_hook_ctx;
    bool saved_paths = g_paths_mode;
    char effective[ODS2_PATH_MAX], spec[ODS2_PATH_MAX + 4], start[ODS2_PATH_MAX], pattern[ODS2_PATH_MAX + 4];
    const char *rest;
    dir_walk_t w;

    memset(&x, 0, sizeof(x));
    resolve_effective_path(p, effective, sizeof(effective));
    snprintf(spec, sizeof(spec), "%s%s", effective, p->recursive ? "..." : "");
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }
    canonical_dir_path(vol, start, sizeof(start));
    if (p->filename[0] == '\0') snprintf(pattern, sizeof(pattern), "*.*");
    else if (ods5_find_unescaped(p->filename, '.') == NULL) snprintf(pattern, sizeof(pattern), "%s.*", p->filename);
    else snprintf(pattern, sizeof(pattern), "%s", p->filename);
    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = pattern;
    w.p = p;
    g_entry_hook = recsize_entry;
    g_entry_hook_ctx = &x;
    g_paths_mode = false;
    g_disorder_mode = 1;                     /* SET FILE/ATTRIBUTES changes the header of the version the walk picks by position: refused on a name out of order (H1) */
    g_disorder_refused = 0;
    walk_from(&w, start);
    g_disorder_mode = 0;
    g_entry_hook = saved_hook;
    g_entry_hook_ctx = saved_ctx;
    g_paths_mode = saved_paths;
    if (x.files == 0 && w.errors == 0 && g_disorder_refused > 0) {
        return;
    }
    if (x.files == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else {
        printf("%%ODS2-I-RECSIZES, %u file%s looked at, %u stream file%s changed to MRS 0 and LRL the longest record\n",
               x.files, x.files == 1 ? "" : "s", x.changed, x.changed == 1 ? "" : "s");
    }
}

/* Warnings for a directory made deep. OpenVMS VAX V7.3 opens a path of 8 directory names on an ODS-2 volume and fails on the 9th (CORPUS 12.3 and 65.3: 8 names work; 66.3: a directory renamed to a 9th
   name cannot be opened, %RMS-F-DIR); the one constant below serves CREATE/DIRECTORY, IMPORT and RENAME of a directory (it was 7 for the first two, from an earlier measurement). Alpha V7.3-2 and later create
   10,000 levels without refusal. But VMS's own wildcard descent [...] stops at 256 directories on V7.3-2 and 512 on V8.4 and
   V8.4-2L2 (%RMS-F-DIR), and BACKUP cannot save files that deep (FTLNTSAVED). ods2v2 creates up to 2,048 levels (the depth
   its own DIR [...] and EXPORT read back) and says so past 256, and past 8 names on an ODS-2 volume. Counts what it warned
   about in *vax_warned and *deep_warned; prints when `print`. */
#define ODS2_VAX_V73_MAX_NAMES 8u
#define ODS2_VMS_WILDCARD_LEVELS 256u
static void warn_deep(ods2_volume_t *vol, const uint8_t *parent_header, const char *shown, bool print, int *vax_warned,
                      int *deep_warned)
{
    unsigned depth = 0;

    if (!ods2_directory_depth(vol, parent_header, &depth).ok) return;
    depth += 1;     /* the level of the directory being made */
    if (!vol->ods5 && depth > ODS2_VAX_V73_MAX_NAMES) {
        if (vax_warned != NULL) (*vax_warned)++;
        if (print) {
            fprintf(stderr, "%%ODS2-W-DEEPDIR, [%s] is %u levels below [000000]: OpenVMS VAX V7.3 cannot reach a "
                            "directory deeper than %u levels on an ODS-2 volume (%%RMS-F-DIR); Alpha V7.3-2 and "
                            "later can\n", shown, depth, ODS2_VAX_V73_MAX_NAMES);
        }
    }
    if (depth > ODS2_VMS_WILDCARD_LEVELS) {
        if (deep_warned != NULL) (*deep_warned)++;
        if (print) {
            fprintf(stderr, "%%ODS2-W-DEEPDIR256, [%s] is %u levels below [000000]: VMS's own wildcard [...] stops at 256 "
                            "directories (V7.3-2) or 512 (V8.4) with %%RMS-F-DIR, and BACKUP cannot save files in it "
                            "(FTLNTSAVED)\n", shown, depth);
        }
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
    warn_deep(vol, parent_header, effective_path, true, NULL, NULL);
}

/* The image being worked on (argv[1]): its directory is the second place a spool file may go. */
static const char *g_image_path = NULL;

/* An anonymous temporary file (made, then unlinked at once) for input that has no length to go by. It goes in $TMPDIR, else in
   the image's directory; never in /tmp by default (tmpfile() ignores TMPDIR and uses /tmp, a RAM disk on many hosts). On failure
   `*tried` says what was tried. */
static FILE *spool_create(char *tried, size_t tried_cap)
{
    const char *tmpdir = getenv("TMPDIR");
    char imgdir[ODS2_PATH_MAX];
    const char *dirs[2];
    size_t n = 0, i, used = 0;

    tried[0] = '\0';
    if (tmpdir != NULL && tmpdir[0] != '\0') dirs[n++] = tmpdir;
    if (g_image_path != NULL) {
        const char *slash = strrchr(g_image_path, '/');
        if (slash == NULL) snprintf(imgdir, sizeof(imgdir), ".");
        else if (slash == g_image_path) snprintf(imgdir, sizeof(imgdir), "/");
        else snprintf(imgdir, sizeof(imgdir), "%.*s", (int) (slash - g_image_path), g_image_path);
        dirs[n++] = imgdir;
    }
    for (i = 0; i < n; i++) {
        char path[ODS2_PATH_MAX + 32];
        int fd;
        FILE *f;
        snprintf(path, sizeof(path), "%s/ods2-spool-XXXXXX", dirs[i]);
        fd = mkstemp(path);
        if (fd < 0) {
            used += (size_t) snprintf(tried + used, used < tried_cap ? tried_cap - used : 0, "%s%s: %s", i ? "; " : "", dirs[i],
                                      strerror(errno));
            continue;
        }
        unlink(path);
        f = fdopen(fd, "w+b");
        if (f == NULL) {
            /* the directory is named in the message too (review 27 N9) */
            int saved = errno;
            close(fd);
            used += (size_t) snprintf(tried + used, used < tried_cap ? tried_cap - used : 0, "%s%s: %s", i ? "; " : "", dirs[i],
                                      strerror(saved));
            continue;
        }
        return f;
    }
    if (n == 0) snprintf(tried, tried_cap, "no directory to use (TMPDIR is not set)");
    return NULL;
}

/* A host file opened for COPY, read in order a chunk at a time
   rather than all at once (issue #16). `length` is known before
   anything is allocated on the volume: from the file itself if it is
   a regular file, otherwise (a pipe, /dev/stdin) by first copying it
   to a temporary file, which keeps memory use flat either way. */
typedef struct {
    FILE *f;
    const char *path;
    uint64_t length;
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
                 "%s became shorter while being copied (expected %llu bytes)",
                 in->path, (unsigned long long) in->length);
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
        in->length = (uint64_t) st.st_size;
        return true;
    }
    /* No length to go by: spool it to a temporary file first. */
    {
        static uint8_t buf[1 << 16];
        char tried[ODS2_PATH_MAX * 2 + 64];
        FILE *spool = spool_create(tried, sizeof(tried));
        size_t n;
        uint64_t total = 0;

        if (spool == NULL) {
            report_error("%%ODS2-E-OPENIN, could not create a temporary file to "
                         "hold %s (tried $TMPDIR, then the image's directory): %s\n", path, tried);
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
        if (fflush(spool) != 0 || fseeko(spool, 0, SEEK_SET) != 0) {
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
typedef enum { FMT_AUTO = 0, FMT_STMLF, FMT_STM, FMT_STMCR, FMT_FIX512, FMT_UDF } host_format_t;

typedef struct {
    bool convert;   /* /CONVERT: host text lines -> VAR records */
    bool mrs_given; /* MRS:n was given */
    bool lrl_given; /* LRL:n was given */
    bool attrs_given; /* /ATTRIBUTES was given: the record attributes are the user's, no choosing */
    host_format_t format; /* /FORMAT=: FMT_AUTO chooses per file (see choose_format) */
    bool new_version; /* IMPORT/NEW_VERSION: a name that exists gets a new version instead of being skipped */
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
 * /FORMAT=AUTO|STMLF|STM|STMCR|FIX512|UDF|VAR chooses the record format
 * by name (AUTO, the default: see choose_format(); VAR is /CONVERT) and
 * cannot be combined with /ATTRIBUTES or /CONVERT. With /ATTRIBUTES the
 * attributes are the user's. A fixed format with no size given is
 * 512-byte records. Returns false after reporting the problem. */
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
        if (keyword(name, name_len, "NEW_VERSION", 3)) {
            if (*q == '=') {
                report_error("%%ODS2-E-NOVALUE, /NEW_VERSION takes no value\n");
                return false;
            }
            opts->new_version = true;
            continue;
        }
        if (keyword(name, name_len, "FORMAT", 4)) {
            const char *v;
            size_t vl;
            if (*q != '=') {
                report_error("%%ODS2-E-VALREQ, /FORMAT needs a value: AUTO, STMLF, STM, STMCR, FIX512 (or BINARY), UDF or VAR\n");
                return false;
            }
            v = ++q;
            while (*q && *q != '/') q++;
            vl = (size_t) (q - v);
            if (keyword(v, vl, "AUTO", 2)) opts->format = FMT_AUTO;
            else if (keyword(v, vl, "STMLF", 5) || keyword(v, vl, "TEXT", 4)) opts->format = FMT_STMLF;
            else if (keyword(v, vl, "STMCR", 5)) opts->format = FMT_STMCR;
            else if (keyword(v, vl, "STM", 3)) opts->format = FMT_STM;
            else if (keyword(v, vl, "FIX512", 3) || keyword(v, vl, "BINARY", 3)) opts->format = FMT_FIX512;
            else if (keyword(v, vl, "UDF", 3)) opts->format = FMT_UDF;
            else if (keyword(v, vl, "VAR", 3)) opts->convert = true;
            else {
                report_error("%%ODS2-E-BADVALUE, /FORMAT=%.*s: use AUTO, STMLF (TEXT), STM, STMCR, FIX512 (BINARY), UDF or VAR\n", (int) vl, v);
                return false;
            }
            continue;
        }
        if (!keyword(name, name_len, "ATTRIBUTES", 4)) {
            report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - COPY takes "
                         "/ATTRIBUTES=(RFM:fmt,MRS:size,RAT:attr), /FORMAT=fmt and /CONVERT (IMPORT also /NEW_VERSION)\n",
                         (int) name_len, name);
            return false;
        }
        opts->attrs_given = true;
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
    opts->lrl_given = lrl_given;
    if (opts->attrs_given && opts->format != FMT_AUTO) {
        report_error("%%ODS2-E-CONFQUAL, /FORMAT and /ATTRIBUTES both choose the record format: give one\n");
        return false;
    }
    if (opts->convert && opts->format != FMT_AUTO) {
        report_error("%%ODS2-E-CONFQUAL, /FORMAT and /CONVERT both choose the record format: give one\n");
        return false;
    }
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
    /* A stream or undefined-format file has no maximum record size of its own: MRS 0, as VMS makes such files, unless MRS
       was given. (The default of 512 left in MRS made RMS refuse any such file with a longer line: %RMS-W-RTB.) The
       longest record (LRL) is the file's own, known when the host file is read: complete_stream_attrs(). */
    if (!opts->convert && (a->rtype == ODS2_RFM_STM || a->rtype == ODS2_RFM_STMLF || a->rtype == ODS2_RFM_STMCR ||
                           a->rtype == ODS2_RFM_UDF)) {
        if (!mrs_given) a->maxrec = 0;
    }
    return true;
}

/* Which record format a host file gets when COPY/IMPORT is not told (no /ATTRIBUTES, /CONVERT or /FORMAT):
 * text (no NUL byte, at most 1 percent control bytes other than TAB LF CR FF VT BS ESC, lines of at most
 * 32767 bytes) becomes a stream file with carriage-return carriage control, LRL the longest line and MRS 0, as
 * VMS's own text files are: Stream_LF for LF line ends (a CR before an LF stays part of the line, so mixed
 * LF and CR LF ends are Stream_LF too: the bytes are kept as they are), Stream for CR LF throughout, Stream_CR
 * for CR-only line ends (no LF at all); anything else becomes fixed 512-byte records with no carriage control
 * and an exact end-of-file byte, as VMS binary FTP makes it. (Before, every file was Stream_LF with MRS 512,
 * which VMS V7.3-2 cannot read when a "line" of a binary file is longer than 512 bytes: %RMS-W-RTB.) */
/* The longest record of the host file in each stream format, by the rules RMS applies (measured on V8.4 and VAX V7.3:
 * ODS5_FORMAT_NOTES 15.28, ods2_reclen_feed in the library): Stream_LF ends at LF (a CR before it stays in the record),
 * Stream_CR at CR, Stream (STM) at LF, FF or VT (that byte stays in the record) or at CR LF (stripped). `longest_crlf` is the
 * STM figure (the name is the old one). */
typedef struct {
    size_t total, nul, badctl, lf, crlf, cr, longest_lf, longest_crlf, longest_cr;
} host_stats_t;

static bool scan_host_file(FILE *f, host_stats_t *s)
{
    uint8_t buf[65536];
    size_t n, i, cur = 0, cur_cr = 0;
    bool prev_cr = false;

    ods2_reclen_t stm;

    memset(s, 0, sizeof(*s));
    ods2_reclen_init(&stm, ODS2_RFM_STM);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ods2_reclen_feed(&stm, buf, n);
        for (i = 0; i < n; i++) {
            uint8_t c = buf[i];
            s->total++;
            if (c == 0) s->nul++;
            else if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v' && c != 0x1b && c != '\b') ||
                     c == 0x7f) s->badctl++;
            if (c == '\r') {
                s->cr++;
                if (cur_cr > s->longest_cr) s->longest_cr = cur_cr;
                cur_cr = 0;
            } else {
                cur_cr++;
            }
            if (c == '\n') {
                s->lf++;
                if (prev_cr) s->crlf++;
                if (cur > s->longest_lf) s->longest_lf = cur;
                cur = 0;
                prev_cr = false;
            } else {
                cur++;
                prev_cr = (c == '\r');
            }
        }
    }
    if (ferror(f)) return false;
    if (cur > s->longest_lf) s->longest_lf = cur;
    s->longest_crlf = ods2_reclen_longest(&stm);
    if (cur_cr > s->longest_cr) s->longest_cr = cur_cr;
    return fseeko(f, 0, SEEK_SET) == 0;
}

/* Sets `attrs` for the host file `f` of `length` bytes as described above, or as /FORMAT says. */
static bool choose_format(const copy_options_t *opts, FILE *f, uint64_t length, ods2_record_attrs_t *attrs,
                          bool *partial_fix, const char *path)
{
    host_stats_t s;
    host_format_t fmt = opts->format;
    size_t longest;

    *partial_fix = false;
    if (!scan_host_file(f, &s)) {
        report_error("%%ODS2-E-READERR, error reading %s\n", path);
        return false;
    }
    if (fmt == FMT_AUTO) {
        bool crlf_file = s.lf > 0 && s.crlf == s.lf;
        bool cr_file = s.lf == 0 && s.cr > 0;
        size_t l = cr_file ? s.longest_cr : (crlf_file ? s.longest_crlf : s.longest_lf);
        bool text = s.nul == 0 && s.badctl * 100 <= s.total && l <= 32767;
        fmt = !text ? FMT_FIX512 : (cr_file ? FMT_STMCR : (crlf_file ? FMT_STM : FMT_STMLF));
    }
    longest = (fmt == FMT_STMCR) ? s.longest_cr : (fmt == FMT_STM ? s.longest_crlf : s.longest_lf);
    memset(attrs, 0, sizeof(*attrs));
    switch (fmt) {
    case FMT_STM:
    case FMT_STMLF:
    case FMT_STMCR:
        if (longest > 32767) {
            report_error("%%ODS2-E-LINETOOLONG, %s: a line of %zu bytes is longer than a stream record can be (32767)\n",
                         path, longest);
            return false;
        }
        attrs->rtype = (fmt == FMT_STM) ? ODS2_RFM_STM : (fmt == FMT_STMCR ? ODS2_RFM_STMCR : ODS2_RFM_STMLF);
        attrs->rattrib = ODS2_RAT_CR;
        attrs->rsize = (uint16_t) longest;
        attrs->maxrec = 0;
        break;
    case FMT_UDF:
        attrs->rtype = ODS2_RFM_UDF;
        attrs->rsize = (uint16_t) (length > 32767 ? 32767 : length);
        attrs->maxrec = 0;
        break;
    default:
        attrs->rtype = ODS2_RFM_FIX;
        attrs->rsize = attrs->maxrec = 512;
        *partial_fix = true;
        break;
    }
    return true;
}

/* /ATTRIBUTES names the record format but not the file's own record sizes: for a stream file the longest record (LRL)
 * is the longest line of the host file (LF, CR LF or CR ends as the format says) and MRS is 0 unless it was given; for an
 * undefined-format file LRL is the length. A size the user gave that is smaller than a line of the file is refused: RMS
 * would refuse to read such a file back (%RMS-W-RTB, record too large for the user's buffer). */
static bool complete_stream_attrs(const copy_options_t *opts, FILE *f, uint64_t length, ods2_record_attrs_t *attrs,
                                  const char *path)
{
    host_stats_t s;
    size_t longest;

    if (attrs->rtype == ODS2_RFM_UDF) {
        if (!opts->lrl_given) attrs->rsize = (uint16_t) (length > 32767 ? 32767 : length);
        return true;
    }
    if (attrs->rtype != ODS2_RFM_STM && attrs->rtype != ODS2_RFM_STMLF && attrs->rtype != ODS2_RFM_STMCR) return true;
    if (!scan_host_file(f, &s)) {
        report_error("%%ODS2-E-READERR, error reading %s\n", path);
        return false;
    }
    longest = attrs->rtype == ODS2_RFM_STMCR ? s.longest_cr : (attrs->rtype == ODS2_RFM_STM ? s.longest_crlf : s.longest_lf);
    if (longest > 32767) {
        report_error("%%ODS2-E-LINETOOLONG, %s: a line of %zu bytes is longer than a stream record can be (32767)\n",
                     path, longest);
        return false;
    }
    if (!opts->lrl_given) attrs->rsize = (uint16_t) longest;
    if (attrs->rsize < longest) {
        report_error("%%ODS2-E-RECTOOLONG, %s: a line of %zu bytes is longer than LRL:%u - RMS would refuse to read the file "
                     "(RMS-W-RTB)\n", path, longest, (unsigned) attrs->rsize);
        return false;
    }
    if (attrs->maxrec != 0 && attrs->maxrec < longest) {
        report_error("%%ODS2-E-RECTOOLONG, %s: a line of %zu bytes is longer than MRS:%u - RMS would refuse to read the file "
                     "(RMS-W-RTB)\n", path, longest, (unsigned) attrs->maxrec);
        return false;
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
    if (fseeko(in->f, 0, SEEK_SET) != 0) {
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
    ods2_content_source_t src = { 0 };
    ods2_record_attrs_t attrs = *attrs_in;
    ods2_text_var_scan_t scan;
    convert_ctx_t conv;
    ods2_result_t r;
    char literal_name[ODS2_PATH_MAX];
    const char *name;
    uint16_t made_version = 1;
    char made_name[ODS5_MAX_LITERAL_NAME + 8] = "";

    if (p->filename[0] == '\0' && !g_ods5) {
        report_error("%%ODS2-E-BADPATH, no destination filename given\n");
        return;
    }
    /* The version is NAME (or NAME; or NAME;0): the next one, highest + 1, or 1 for a new name; NAME;n: that version if it is free
       (CORPUS 49, 49.8). A wildcard and a relative version (;-1) name no one version: refused, rather than ever storing
       ";version" as part of the name. */
    if (p->has_version && (p->version_wildcard || p->version < 0)) {
        char spec[ODS2_PATH_MAX + 16];
        format_file_spec(p, spec, sizeof(spec));
        report_error("%%ODS2-E-BADVER, %s: COPY makes one version: give NAME, NAME; or NAME;n (n from 1 to 32767)\n", spec);
        return;
    }
    {
        /* The name is a spelling (^-escapes): store the characters it
           stands for. Without a name on an ODS-5 volume, the host file's own
           name is used (UTF-8, as the host has it). */
        if (p->filename[0] != '\0') {
            if (ods5_unescape_filename(p->filename, strlen(p->filename), literal_name,
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

    if (!opts->convert && !opts->attrs_given) {
        if (!choose_format(opts, in.f, in.length, &attrs, &src.partial_fix_ok, local_file)) {
            fclose(in.f);
            return;
        }
    } else if (!opts->convert && !complete_stream_attrs(opts, in.f, in.length, &attrs, local_file)) {
        fclose(in.f);
        return;
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
    r = ods2_create_file_source_version(vol, dir_header, name, p->has_version ? p->version : 0, &src, &attrs, &new_fid, &made_version, made_name);
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
                    "first %llu bytes were written - copy it again\n",
                    local_file, (unsigned long long) in.length);
        }
    }
    ods2_text_var_encoder_free(conv.enc);
    fclose(in.f);
    if (!r.ok) {
        report_error("%%ODS2-E-COPYERR, %s\n", r.problem);
        return;
    }
    {
        /* Report the name as stored: uppercase, with its dot (an existing name: as the directory spells it). */
        char stored[ODS2_PATH_MAX];
        const char *why;
        if (made_name[0] != '\0') name = made_name;
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
            printf("%%ODS2-I-COPIED, %llu bytes to %s;%u as %zu variable-length "
                   "record%s (longest %zu)\n", (unsigned long long) in.length, stored, made_version, scan.records,
                   scan.records == 1 ? "" : "s", scan.longest);
        } else {
            printf("%%ODS2-I-COPIED, %llu bytes to %s;%u\n", (unsigned long long) in.length, stored, made_version);
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
    if (ods5_unescape_filename(p->filename, strlen(p->filename), literal, sizeof(literal)) == (size_t) -1) {
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
/* RENAME <old> <new>: one file in one directory (PLAN_WRITE_OPS item 5, step 1; CORPUS 65 and 66). The new name without a directory is in the old one's directory. Wildcards, another
   directory and a directory file are later steps and are refused, saying so. */
/* RENAME's two specifications: the device part (before the first colon that is not ^-escaped and not inside the brackets) is taken off and kept apart, so that it is not part of a name (review 69 S2). `bad` is
   set for a device that does not fit `dev`, one with a quote in it, or a second colon before the directory or name (a node, MOV:MOV:): refused by the caller. */
static void rename_split_device(const char *arg, char *spec, size_t spec_n, char *dev, size_t dev_n, bool *bad, bool *junk)
{
    const char *br = ods5_find_unescaped(arg, '['), *c1 = ods5_find_unescaped(arg, ':'), *p;
    size_t len;
    dev[0] = '\0';
    *bad = false;
    *junk = false;
    if (c1 != NULL && br != NULL && c1 > br) c1 = NULL;                       /* a colon inside or after the brackets is no device */
    if (c1 == NULL) {
        if (br != NULL && br > arg) *junk = true;                                 /* junk[Y]N.TXT: text in front of the directory (review 69 recheck R3) */
        snprintf(spec, spec_n, "%s", arg);
        return;
    }
    len = (size_t) (c1 - arg);
    if (len == 0 || len >= dev_n || memchr(arg, '"', len) != NULL) *bad = true;
    else { memcpy(dev, arg, len); dev[len] = '\0'; }
    for (p = ods5_find_unescaped(c1 + 1, ':'); p != NULL && (br == NULL || p < br); p = ods5_find_unescaped(p + 1, ':')) *bad = true;      /* MOV:MOV:, MOV:: */
    if (br != NULL && br > c1 + 1) *junk = true;                                  /* MOV:junk[Y]N.TXT: text between the device and the directory (review 69 recheck R3) */
    snprintf(spec, spec_n, "%s", br != NULL ? br : c1 + 1);
}

/* the path parser, plus the one thing RENAME needs of it: a version and no name (`[Y];7`, `;7`): the name is then the input's */
static bool rename_parse_path(const char *spec, ods2_parsed_path_t *out)
{
    const char *close = ods5_find_unescaped(spec, ']'), *name = close != NULL ? close + 1 : spec;
    char tmp[ODS2_PATH_MAX + 8];
    if (*name == ';') {
        snprintf(tmp, sizeof(tmp), "%.*sZZRENAMEZZ%s", (int) (name - spec), spec, name);
        if (!ods2_parse_path(tmp, out)) return false;
        out->filename[0] = '\0';
        return true;
    }
    return ods2_parse_path(spec, out);
}

static void rename_wild(ods2_volume_t *vol, const ods2_parsed_path_t *sp, const ods2_parsed_path_t *dp, const char *src_arg, const char *dst_arg, bool device_only);
static bool rename_has_wild(const char *filename);

static void cmd_rename(ods2_volume_t *vol, const ods2_parsed_path_t *sp, const ods2_parsed_path_t *dp, const char *src_arg, const char *dst_arg, bool device_only)
{
    uint8_t sdir[512], ddir[512];
    ods2_fid_t sfid, dfid;
    char sdir_path[ODS2_PATH_MAX], ddir_path[ODS2_PATH_MAX], sname[ODS2_PATH_MAX], dname[ODS2_PATH_MAX];
    ods2_rename_info_t info;
    ods2_result_t r;
    char what[ODS2_PATH_MAX * 2 + 8];

    if (sp->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, RENAME needs the name of the file to rename (a directory is renamed by its name.DIR)\n");
        return;
    }
    if (rename_has_wild(sp->filename) || sp->version_wildcard || rename_has_wild(dp->filename) || dp->version_wildcard) {
        rename_wild(vol, sp, dp, src_arg, dst_arg, device_only);                    /* step 4: every file the old name matches, each its own transaction */
        return;
    }
    if (dp->filename[0] == '\0' && !dp->had_brackets && !device_only && !dp->has_version) {
        report_error("%%ODS2-E-BADPATH, %s: RENAME needs a new name or a directory for the file\n", dst_arg);
        return;
    }
    if (ods5_unescape_filename(sp->filename, strlen(sp->filename), sname, sizeof(sname)) == (size_t) -1 ||
        ods5_unescape_filename(dp->filename, strlen(dp->filename), dname, sizeof(dname)) == (size_t) -1) {
        report_error("%%ODS2-E-BADNAME, malformed ^ escape in a file name\n");
        return;
    }
    resolve_effective_path(sp, sdir_path, sizeof(sdir_path));
    r = resolve_dir(vol, sdir_path, sdir, &sfid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    if (dp->had_brackets) {
        resolve_effective_path(dp, ddir_path, sizeof(ddir_path));
        r = resolve_dir(vol, ddir_path, ddir, &dfid);
        if (!r.ok) {
            report_error("%%RENAME-E-OPENOUT, error opening %s as output\n-RMS-E-DNF, directory not found\n-SYSTEM-W-NOSUCHFILE, no such file\n", dst_arg);
            return;
        }
    } else {
        dfid = sfid;                                  /* no directory in the new name: the old file's directory */
    }
    {
        /* what the new name leaves out comes from the STORED name of the file (review 69 recheck R2: on ODS-5 lower.txt stays lower; the typed text may spell it otherwise). dname holds the typed new
           name as a literal; the escaped form (dp->filename) tells whether it has a type separator of its own (a ^. is no separator) */
        char stored[ODS2_DIR_MAX_NAME + 8], full[ODS2_PATH_MAX * 2];
        const char *sfull = sname, *sdot;
        if (ods2_stored_name(vol, ods2_fid_number(sfid), sname, stored, sizeof(stored)).ok) sfull = stored;
        sdot = strrchr(sfull, '.');
        if (dp->filename[0] == '\0') {
            snprintf(dname, sizeof(dname), "%s", sfull);          /* RENAME [X]C.TXT [Y]: the same name in the other directory (CORPUS 65.1 b1); [Y];7: the same name, version 7 */
        } else {
            /* DCL fills what the new name leaves out from the input (review 69 S3): [Y]N has no type: N.TXT; [Y].DAT has no name: A.DAT */
            const char *dot = ods5_find_unescaped(dp->filename, '.');
            if (dot == NULL && sdot != NULL) {                                                                                              /* the type of the input */
                size_t dl = strlen(dname);
                if (dl > 0 && dname[dl - 1] == '.') dname[dl - 1] = '\0';          /* an escaped dot gives the literal a trailing no-type dot (n^.m is n.m.): it goes before the type is added (review 70 F1) */
                snprintf(full, sizeof(full), "%s%s", dname, sdot);
            }
            else if (dot == dp->filename && sdot != NULL) snprintf(full, sizeof(full), "%.*s%s", (int) (sdot - sfull), sfull, dname);      /* the name of the input */
            else if (dot == dp->filename) snprintf(full, sizeof(full), "%s%s", sfull, dname);
            else snprintf(full, sizeof(full), "%s", dname);
            snprintf(dname, sizeof(dname), "%s", full);
        }
    }
    memset(&info, 0, sizeof(info));
    {
        /* the directories the typed target path passes through, by whole file ID (a directory is not moved under itself by a path that goes through a second entry of it: review 71 C1) */
        ods2_fid_t *via = NULL;
        unsigned nvia = 0;
        if (dp->had_brackets && ddir_path[0] != '\0') {
            char pre[ODS2_PATH_MAX];
            const char *dot = ddir_path;
            size_t cap = 0;
            for (;;) {
                const char *nx = ods5_find_unescaped(dot, '.');
                size_t len = nx != NULL ? (size_t) (nx - ddir_path) : strlen(ddir_path);
                ods2_fid_t f;
                memcpy(pre, ddir_path, len);
                pre[len] = '\0';
                if (ods2_lookup_path(vol, pre, &f).ok) {
                    if (nvia == cap) {
                        ods2_fid_t *nv = realloc(via, (cap ? cap * 2 : 16) * sizeof(*nv));
                        if (nv == NULL) break;
                        via = nv;
                        cap = cap ? cap * 2 : 16;
                    }
                    via[nvia++] = f;
                }
                if (nx == NULL) break;
                dot = nx + 1;
            }
        }
        r = ods2_rename_ex(vol, ods2_fid_number(sfid), sname, sp->has_version ? sp->version : 0, ods2_fid_number(dfid), dname, dp->has_version ? dp->version : 0, via, nvia, &info);
        free(via);
    }
    snprintf(what, sizeof(what), "%s", src_arg);
    if (!r.ok) {
        report_error("%%ODS2-E-RENAME, %s: %s\n", what, r.problem);
        return;
    }
    {
        char shown[ODS2_PATH_MAX];
        display_name(dname, shown, sizeof(shown));
        if (info.same) {
            printf("%%ODS2-I-RENAMED, %s is already named %s: nothing changed\n", src_arg, dst_arg);
        } else if (info.case_only) {
            printf("%%ODS2-I-RENAMED, %s renamed to %s (the spelling only)\n", src_arg, dst_arg);
        } else {
            printf("%%ODS2-I-RENAMED, %s renamed to %s;%u%s%s\n", src_arg, shown, (unsigned) info.version, info.resumed ? " (the target name already named this file: the old entry was removed and the rename finished)" : "",
                   (info.header_changed || info.header_current) ? "" : " (an alias entry: the file's header is not changed)");
        }
        if (info.pruned_version != 0) printf("%%ODS2-I-DELETED, deleted %s;%u (the version limit)\n", shown, info.pruned_version);
        if (info.is_dir && !vol->ods5 && info.deepest_names > ODS2_VAX_V73_MAX_NAMES) {
            fprintf(stderr, "%%ODS2-W-DEEPDIR, the directory is now %u names deep below [000000]: OpenVMS VAX V7.3 cannot open a path of more than %u names on an ODS-2 volume (%%RMS-F-DIR); Alpha V7.3-2 and later can\n",
                    info.deepest_names, ODS2_VAX_V73_MAX_NAMES);
        }
    }
}

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
    if (ods5_unescape_filename(np->filename, strlen(np->filename), literal, sizeof(literal)) == (size_t) -1 ||
        ods5_unescape_filename(ep->filename, strlen(ep->filename), tliteral, sizeof(tliteral)) == (size_t) -1) {
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
    r = ods2_create_link(vol, ods2_fid_number(dir_fid), literal, target_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-LINK, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-LINKED, %s;1 is %s file (%u,%u,%u)\n", np->filename,
           (vol->ods5 && (vol->home.volchar & ODS5_VC_HARDLINKS)) ? "a link to" : "an alias of",
           ods2_fid_number(target_fid), (unsigned) target_fid.fid_seq, (unsigned) target_fid.fid_rvn);
}

/* IMPORT <linux-directory> <path>: copies a Linux directory tree onto the
   volume as EXPORT writes one: UTF-8 names (a "%2F" in a Linux name is a '/'
   in the VMS name), directories made as needed, regular files in the record format
   choose_format() picks for each (text as Stream_LF, anything else as fixed 512-byte records),
   Linux symbolic links as VMS symbolic links (ODS-5). Names VMS would refuse
   and files that already exist are reported and skipped. */
typedef struct {
    ods2_volume_t *vol;
    int files, dirs, links, errors, skipped;
    int deep, deep256;                   /* directories deeper than OpenVMS VAX V7.3 reaches (ODS-2) / than VMS's [...] walks */
    unsigned long long bytes;
    const copy_options_t *opts;          /* /FORMAT, /ATTRIBUTES */
    const ods2_record_attrs_t *attrs_in; /* the /ATTRIBUTES given */
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

#define IMPORT_MAX_DEPTH 2048   /* the creation cap; the host's 4,096-character path limit stops a tree sooner */

static void import_tree(import_stats_t *st, const char *local_dir, unsigned dir_file_number, int depth)
{
    struct dirent **list = NULL;
    int count, i;
    struct import_bufs {
        char path[ODS2_PATH_MAX * 2], literal[ODS2_PATH_MAX], shown[ODS2_PATH_MAX], dirname[ODS2_PATH_MAX + 8];
        uint8_t dir_header[512], existing[512];
        char target[ODS5_SYMLINK_MAX + 2];
    } *b;

    /* a bounded C stack: the big buffers of a level are on the heap (one block per directory, freed when it is done), so
       a frame is small and 2,048 levels fit the stack with room to spare */
    if (depth > IMPORT_MAX_DEPTH) {
        report_error("%%ODS2-E-DEEP, %s: directories nested more than %d levels: this directory and "
                     "everything below it are skipped\n", local_dir, IMPORT_MAX_DEPTH);
        st->errors++;
        return;
    }
    b = malloc(sizeof(*b));
    if (b == NULL) {
        report_error("%%ODS2-E-NOMEM, out of memory importing %s\n", local_dir);
        st->errors++;
        return;
    }
    count = scandir(local_dir, &list, NULL, import_name_cmp);
    if (count < 0) {
        free(b);
        report_error("%%ODS2-E-OPENIN, could not read directory %s: %s\n", local_dir, strerror(errno));
        st->errors++;
        return;
    }
    for (i = 0; i < count; i++) {
        const char *entry = list[i]->d_name;
        char *path = b->path, *literal = b->literal, *shown = b->shown;
        uint8_t *dir_header = b->dir_header;
        struct stat sb;
        ods2_result_t r;
        ods2_fid_t fid;

        if (strcmp(entry, ".") == 0 || strcmp(entry, "..") == 0) continue;
        if (snprintf(path, sizeof(b->path), "%s/%s", local_dir, entry) >= (int) sizeof(b->path)) {
            report_error("%%ODS2-E-PATHTOOLONG, %s/%s: the path is too long: skipped\n", local_dir, entry);
            st->errors++;
            continue;
        }
        import_decode_name(entry, literal, sizeof(b->literal));
        display_name(literal, shown, sizeof(b->shown));
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
            char *dirname = b->dirname;
            snprintf(dirname, sizeof(b->dirname), "%s.DIR", literal);
            if (ods2_lookup_name(st->vol, dir_header, dirname, &fid).ok) {
                /* already there: put the files into it, if it is a directory */
                uint8_t *existing = b->existing;
                ods2_result_t fr = ods2_check_fid(fid);
                if (!fr.ok) {
                    report_error("%%ODS2-E-DIRERR, %s: %s\n", shown, fr.problem);
                    st->errors++;
                    continue;
                }
                if (!ods2_read_header_fid(st->vol, fid, existing).ok ||
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
                warn_deep(st->vol, dir_header, shown, false, &st->deep, &st->deep256);
            }
            import_tree(st, path, ods2_fid_number(fid), depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            host_input_t in;
            ods2_content_source_t src = { 0 };
            ods2_record_attrs_t attrs;
            if (!st->opts->new_version && ods2_lookup_name(st->vol, dir_header, literal, &fid).ok) {
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
            if (st->opts->attrs_given) {
                attrs = *st->attrs_in;
                if (!complete_stream_attrs(st->opts, in.f, in.length, &attrs, path)) {
                    fclose(in.f);
                    st->errors++;
                    continue;
                }
            } else if (!choose_format(st->opts, in.f, in.length, &attrs, &src.partial_fix_ok, path)) {
                fclose(in.f);
                st->errors++;
                continue;
            }
            r = st->opts->new_version ? ods2_create_file_source_version(st->vol, dir_header, literal, 0, &src, &attrs, &fid, NULL, NULL)
                                      : ods2_create_file_source(st->vol, dir_header, literal, &src, &attrs, &fid);
            fclose(in.f);
            if (!r.ok) {
                report_error("%%ODS2-E-COPYERR, %s: %s\n", shown, r.problem);
                st->errors++;
                continue;
            }
            st->files++;
            st->bytes += in.length;
        } else if (S_ISLNK(sb.st_mode)) {
            char *target = b->target;
            ssize_t n = readlink(path, target, sizeof(b->target) - 1);
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
    free(b);
}

static void cmd_import_tree(ods2_volume_t *vol, const char *local_dir, const ods2_parsed_path_t *p,
                            const copy_options_t *opts, const ods2_record_attrs_t *attrs)
{
    import_stats_t st;
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_result_t r;
    char effective_dir[ODS2_PATH_MAX];

    memset(&st, 0, sizeof(st));
    st.vol = vol;
    st.opts = opts;
    st.attrs_in = attrs;
    resolve_effective_path(p, effective_dir, sizeof(effective_dir));
    r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    import_tree(&st, local_dir, ods2_fid_number(dir_fid), 0);
    if (st.deep > 0) {
        fprintf(stderr, "%%ODS2-W-DEEPDIR, %d director%s deeper than %u levels below [000000] created: OpenVMS "
                        "VAX V7.3 cannot reach them on an ODS-2 volume (%%RMS-F-DIR); Alpha V7.3-2 and "
                        "later can\n", st.deep, st.deep == 1 ? "y" : "ies", ODS2_VAX_V73_MAX_NAMES);
    }
    if (st.deep256 > 0) {
        fprintf(stderr, "%%ODS2-W-DEEPDIR256, %d director%s deeper than %u levels below [000000] created: VMS's own wildcard "
                        "[...] stops at 256 directories (V7.3-2) or 512 (V8.4) with %%RMS-F-DIR, and BACKUP cannot save "
                        "files in them\n", st.deep256, st.deep256 == 1 ? "y" : "ies", ODS2_VMS_WILDCARD_LEVELS);
    }
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
    unsigned geo_s = 0, geo_t = 0;
    bool partial = false;
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
            if (keyword(name, name_len, "PARTIAL_CLUSTER", 3)) {
                /* /PARTIAL_CLUSTER: give the blocks past the last whole cluster to BADBLK.SYS, as VMS INITIALIZE does */
                partial = true;
                continue;
            }
            if (keyword(name, name_len, "GEOMETRY", 3)) {
                /* /GEOMETRY=sectors,tracks: the geometry the SCB records (the device's own: VAX V7.3 compares) */
                char nm[16];
                if (value != NULL && value != q && !isdigit((unsigned char) *value) && (size_t) (q - value) < sizeof(nm)) {
                    /* a device type: the geometry it reports for this volume's size */
                    bool measured = false;
                    uint32_t ps = 0, pt = 0;
                    memcpy(nm, value, (size_t) (q - value));
                    nm[q - value] = '\0';
                    if (vol->volsize == 0) {
                        report_error("%%ODS2-E-GEOMERR, the volume size of the storage control block is unreadable or 0: the "
                                     "geometry of a device type cannot be worked out (give sectors,tracks)\n");
                        return;
                    }
                    if (!ods2_target_geometry(nm, vol->volsize, &ps, &pt, &measured)) {
                        report_error("%%ODS2-E-VALREQ, /GEOMETRY=%s: unknown device type (ld, ra, ra82, ra92, rq, scsi, axpbox, "
                                     "rz) or sectors,tracks\n", nm);
                        return;
                    }
                    geo_s = ps; geo_t = pt;
                    {
                        char warn[900];
                        if (ods2_target_geometry_warning(nm, vol->volsize, ps, pt, measured, warn, sizeof(warn))) {
                            fprintf(stderr, "%%ODS2-W-PREDICTED, %s\n", warn);
                        }
                    }
                    continue;
                }
                char *end1 = NULL, *end2 = NULL;
                unsigned long a = value ? strtoul(value, &end1, 10) : 0, b = (end1 && *end1 == ',') ? strtoul(end1 + 1, &end2, 10) : 0;
                if (value == NULL || value == q || end1 == NULL || *end1 != ',' || end2 == NULL || end2 != q ||
                    a == 0 || b == 0 || a > 65535 || b > 65535) {
                    report_error("%%ODS2-E-VALREQ, /GEOMETRY needs sectors,tracks, e.g. SET VOLUME/GEOMETRY=57,15\n");
                    return;
                }
                geo_s = (unsigned) a;
                geo_t = (unsigned) b;
                continue;
            }
            if (!keyword(name, name_len, "LABEL", 3)) {
                report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - SET VOLUME "
                             "takes /LABEL=name, /GEOMETRY=sectors,tracks or /PARTIAL_CLUSTER\n", (int) name_len, name);
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
    if (partial) {
        bool changed = false;
        ods2_result_t pr = ods2_assign_partial_cluster(vol, &changed);
        if (!pr.ok) {
            report_error("%%ODS2-E-PARTIAL, %s\n", pr.problem);
            return;
        }
        printf(changed ? "%%ODS2-I-PARTIAL, the blocks past the last whole cluster now belong to BADBLK.SYS\n"
                       : "%%ODS2-I-PARTIAL, nothing to do (the size is a whole number of clusters, or BADBLK.SYS owns them)\n");
        if (geo_s == 0 && label == NULL) return;
    }
    if (geo_s != 0) {
        ods2_result_t gr = ods2_set_volume_geometry(vol, geo_s, geo_t);
        if (!gr.ok) {
            report_error("%%ODS2-E-GEOMERR, %s\n", gr.problem);
            return;
        }
        printf("%%ODS2-I-GEOMETRY, the storage control block now records %u sectors x %u tracks\n", geo_s, geo_t);
        if (vol->volsize != 0 && ((uint64_t) vol->volsize + (uint64_t) geo_s * geo_t - 1) / ((uint64_t) geo_s * geo_t) > 65535) {
            fprintf(stderr, "%%ODS2-W-CYLWRAP, that is more than 65,535 cylinders: a device with a 16-bit cylinder count (SIMH RQ) "
                            "wraps it and then draws CHKSCB (or INTDIV) on VAX V7.3; measure the device's own geometry\n");
        }
        if (label == NULL) return;
    }
    if (label == NULL) {
        report_error("%%ODS2-E-NOPARM, usage: SET VOLUME/LABEL=name, /GEOMETRY=sectors,tracks or /PARTIAL_CLUSTER\n");
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

/* The records of one file to stdout (streamed a chunk at a time). `spec` names it in messages. */
static void type_one_file(ods2_volume_t *vol, ods2_fid_t file_fid, const char *spec, bool raw, bool lines)
{
    uint8_t file_header[512];
    ods2_result_t r;

    r = ods2_read_header_fid(vol, file_fid, file_header);
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
        uint64_t bytes = 0;
        const char *note = NULL;
        r = read_file_content(vol, file_header, !raw, lines ? ODS2_REC_LINES : 0, stdout_sink, NULL, &bytes, &sink_failed, &note);
        if (r.ok && fflush(stdout) != 0) sink_failed = true;
        if (note != NULL && r.ok && !sink_failed) fprintf(stderr, "%%ODS2-W-RECORDS, %s: %s\n", spec, note);
        if (sink_failed) {
            report_error("%%ODS2-E-WRITEERR, %s: could not write all %llu bytes to output\n",
                         spec, (unsigned long long) ods2_file_content_length(file_header));
        } else if (!r.ok) {
            report_error("%%ODS2-E-READERR, %s: %s\n", spec, r.problem);
        }
    }
}

static void cmd_type(ods2_volume_t *vol, const ods2_parsed_path_t *p, bool raw, bool lines)
{
    uint8_t dir_header[512];
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
        if (ods5_unescape_filename(p->filename, strlen(p->filename), literal, sizeof(literal)) == (size_t) -1) {
            report_error("%%ODS2-E-BADNAME, %s: malformed ^ escape in the file name\n", spec);
            return;
        }
        r = ods2_lookup_name_version(vol, dir_header, literal, p->version, &file_fid, NULL);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-FNF, %s not found\n", spec);
        return;
    }
    type_one_file(vol, file_fid, spec, raw, lines);
}

/* The message for one entry DELETE removed: deleted, or only the entry removed (an alias, or one whose file is
   gone). */
static void report_deletion(const char *name, uint16_t version, const ods2_delete_info_t *info)
{
    if (info->no_entry_left) {
        /* the first state of a stopped RENAME (CORPUS 69): not "deleted": the entry went, the file did not */
        printf("%%ODS2-W-FILEKEPT, removed the entry %s;%u only: the file stays allocated with no entry left in its directory (its header names a name or a directory that has no entry for it: a RENAME that was stopped; another directory may still enter it). "
               "ANALYZE/DISK_STRUCTURE/REPAIR on VMS puts it in SYSLOST; it is not deleted\n", name, version);
    } else if (info->link_removed && info->dir_link) {
        /* a directory taken off a HARDLINKS count that was above 1 (review 73 S3): not plainly "deleted": the entry went, the directory file stays; with no other entry it is in no directory */
        printf("%%ODS2-W-FILEKEPT, removed the entry %s;%u only (the directory's link count was above 1): the directory file stays allocated; if no other entry names it, it is now in no directory (ANALYZE/DISK_STRUCTURE/REPAIR on VMS); it is not deleted\n", name, version);
    } else if (info->kept == 1 && info->name_differs) {
        char shown[ODS2_PATH_MAX];
        display_name(info->primary_name, shown, sizeof(shown));
        printf("%%ODS2-I-ALIASREMOVED, removed %s;%u; the file stays: this entry is an alias, "
               "its header names %s;%u as the primary entry\n", name, version, shown,
               (unsigned) info->primary_version);
    } else if (info->kept == 1) {
        printf("%%ODS2-I-ALIASREMOVED, removed %s;%u; the file stays: its header's back link names "
               "another directory (file number %u), so this entry is taken for an alias\n", name,
               version, info->backlink_dir);
    } else if (info->kept == 2) {
        printf("%%ODS2-I-DANGLING, removed %s;%u; its file no longer exists (nothing else changed)\n",
               name, version);
    } else {
        printf("%%ODS2-I-DELETED, deleted %s;%u\n", name, version);
    }
}

/* DELETE with '*', '%' or '?' (not ^-escaped) in the file name (upstream #24, be4e467): every entry of the one
   directory whose name and type match, each matched apart (case-blind, ^-escapes literal; no type given is the
   null type). The versions follow the rules for one file: NAME;* every version, NAME;n version n, NAME;-n
   the n-th lower one, and no version (or ;0) the highest. The matches are listed first and then deleted by their
   exact version numbers, so deleting never disturbs the list being worked through. An entry that cannot be
   deleted (a reserved file, a directory that is not empty) is reported and the rest are still deleted. */
static void delete_wildcard(ods2_volume_t *vol, const uint8_t *dir_header, ods2_fid_t dir_fid,
                            const ods2_parsed_path_t *p)
{
    ods2_dir_entry_t *entries = NULL;
    int count = 0, i, rank = 0, matched = 0, deleted = 0, failed = 0;
    ods2_result_t r;
    char pat_name[ODS2_PATH_MAX + 4], pat_type[ODS2_PATH_MAX + 4];
    const char *dot;
    struct target { char name[ODS2_DIR_MAX_NAME + 1]; uint16_t version; } *targets;
    char last_refused[ODS2_DIR_MAX_NAME + 1] = "", dir_path_for_msg[ODS2_PATH_MAX];

    resolve_effective_path(p, dir_path_for_msg, sizeof(dir_path_for_msg));
    if (!vol->writable) {
        report_error("%%ODS2-E-DELETEERR, %s: %s\n", p->filename, vol->write_refusal[0] != '\0' ? vol->write_refusal : "the volume is mounted read-only");
        return;
    }
    /* The name and the type are matched apart (review 26 F2): on ODS-5 a name may hold dots, and '*' must not
       reach across the type delimiter, or FOO.*;* would take FOO.TAR.GZ (name FOO.TAR). With no type given the
       type is the null type, as for one file (DELETE README is README.) and as VMS applies no default type
       (review 26 F1): READM%;* is README.;*, not README.*;*. In the spelling the one unescaped dot is the
       delimiter (the last one if more are typed, as the one-file path reads it); in the stored literal name, the
       last dot. */
    dot = ods5_rfind_unescaped(p->filename, '.');
    if (dot == NULL) {
        snprintf(pat_name, sizeof(pat_name), "%s", p->filename);
        pat_type[0] = '\0';
    } else {
        snprintf(pat_name, sizeof(pat_name), "%.*s", (int) (dot - p->filename), p->filename);
        snprintf(pat_type, sizeof(pat_type), "%s", dot + 1);
    }

    r = ods2_list_directory_alloc(vol, dir_header, &entries, &count);
    if (!r.ok) {
        report_error("%%ODS2-E-DELETEERR, %s: %s\n", p->filename, r.problem);
        return;
    }
    targets = malloc(sizeof(*targets) * (size_t) (count > 0 ? count : 1));
    if (targets == NULL) {
        free(entries);
        report_error("%%ODS2-E-DELETEERR, %s: out of memory\n", p->filename);
        return;
    }
    bool g_ok = true;                       /* the order of the current name, judged once at its first entry (review 66 S3) */
    for (i = 0; i < count; i++) {
        bool take;
        rank = (i > 0 && same_name(entries[i].name, entries[i - 1].name)) ? rank + 1 : 0;
        if (rank == 0 && !p->version_wildcard && !(p->has_version && p->version > 0)) g_ok = group_in_order(entries, count, i);
        {
            char lit_name[ODS2_DIR_MAX_NAME + 1];
            const char *ldot = strrchr(entries[i].name, '.');
            const char *lit_type = ldot != NULL ? ldot + 1 : "";
            size_t nlen = ldot != NULL ? (size_t) (ldot - entries[i].name) : strlen(entries[i].name);
            memcpy(lit_name, entries[i].name, nlen);
            lit_name[nlen] = '\0';
            if (!ods2_wildcard_match(pat_name, lit_name) || !ods2_wildcard_match(pat_type, lit_type)) continue;
        }
        if (p->version_wildcard) take = true;                                   /* ;*  every version */
        else if (!p->has_version || p->version == 0) take = (rank == 0);        /* the highest */
        else if (p->version > 0) take = (entries[i].version == (unsigned) p->version);
        else take = (rank == -p->version);                                       /* ;-n */
        if (!take) continue;
        if (!p->version_wildcard && !(p->has_version && p->version > 0) && !g_ok) {
            /* H1: a name whose versions are stored out of order: refused (once), the others go on */
            if (strcmp(last_refused, entries[i].name) != 0) {
                report_version_order(dir_path_for_msg, entries[i].name, "deleted");
                snprintf(last_refused, sizeof(last_refused), "%s", entries[i].name);
            }
            continue;
        }
        snprintf(targets[matched].name, sizeof(targets[matched].name), "%s", entries[i].name);
        targets[matched].version = entries[i].version;
        matched++;
    }
    free(entries);

    for (i = 0; i < matched; i++) {
        uint16_t done = 0;
        ods2_delete_info_t info;
        r = ods2_delete_version_info(vol, ods2_fid_number(dir_fid), targets[i].name, targets[i].version, &done,
                                     &info);
        if (r.ok) {
            report_deletion(targets[i].name, done, &info);
            deleted++;
        } else {
            char shown[ODS2_PATH_MAX];
            display_name(targets[i].name, shown, sizeof(shown));
            report_error("%%ODS2-E-DELETEERR, %s;%u: %s\n", shown, targets[i].version, r.problem);
            failed++;
        }
    }
    free(targets);

    if (matched == 0 && last_refused[0] != '\0') {
        /* a name was found and refused for its order (H1): no "no files match" for it (review 66 S1) */
    } else if (matched == 0) {
        char spec[ODS2_PATH_MAX + 16];
        format_file_spec(p, spec, sizeof(spec));
        report_error("%%ODS2-E-DELETEERR, %s: no files match\n", spec);
    } else if (deleted > 1 || failed > 0) {
        printf("%%ODS2-I-TOTAL, %d entr%s deleted%s\n", deleted, deleted == 1 ? "y" : "ies",
               failed > 0 ? ", some could not be (see above)" : "");
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
        if (ods5_unescape_filename(p->filename, strlen(p->filename), name_buf, sizeof(name_buf)) ==
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

    if (p->filename[0] != '\0' && (ods5_find_unescaped(p->filename, '*') != NULL ||
                                   ods5_find_unescaped(p->filename, '%') != NULL ||
                                   ods5_find_unescaped(p->filename, '?') != NULL)) {
        delete_wildcard(vol, dir_header, dir_fid, p);
        return;
    }

    /* H1 (CORPUS 66.4): no version (or ;0, ;-n) on a name whose versions are stored out of order: VMS acts on the entry stored first (DELETE with no version it refuses: DELVER);
       ods2v2 refuses and names the reason; ;n and ;* act as told. */
    if (!p->version_wildcard && version <= 0) {
        bool in_order = true;
        r = ods2_name_versions_in_order(vol, dir_header, name, &in_order);
        if (r.ok && !in_order) {
            report_version_order(dir_path, name, "deleted");
            return;
        }
    }
    /* One version (the one selected, the highest by default), or with
       ";*" every version - highest first, until none remain. */
    do {
        ods2_delete_info_t info;
        r = ods2_delete_version_info(vol, ods2_fid_number(dir_fid), name,
                                     p->version_wildcard ? ODS2_VERSION_HIGHEST : version,
                                     &deleted_version, &info);
        if (!r.ok) break;
        report_deletion(name, deleted_version, &info);
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

static void spec_entry_text(const char *dir_spelling, const char *literal_name, unsigned version, char *out, size_t cap);

/* ---- RENAME with wildcards (step 4; CORPUS 65.4 e1 and e2, 70.8; reviews 75) ----
   The old name's wildcards (`*`, `%`, `?` in the name and the type; the version `;*`, `;n`, `;-n`, none) pick files in ONE directory. The match list is made FIRST, from the directory as it is before the command: a list of
   names (as the directory stores them) and version NUMBERS, "highest" and ;-n already turned into numbers. Each file is then renamed by the step 1 to 3 code (one ods2_rename_ex: its own transaction, its own write
   order), so a failure of one file leaves the others done or untouched, never half done; the command as a whole is not atomic. What is not measured (U1 to U16 of RENAME-STEP4-PROPOSAL.md) is refused, with the volume
   unchanged: a partial wildcard in the new name, a directory wildcard, several sources for one target name, a target that is also a source, a `;*` source other than the measured shape. */
static bool rename_has_wild(const char *filename)
{
    return ods5_find_unescaped(filename, '*') != NULL || ods5_find_unescaped(filename, '%') != NULL || ods5_find_unescaped(filename, '?') != NULL;
}

typedef struct {
    char name[ODS2_DIR_MAX_NAME + 1];       /* as the directory stores it */
    uint16_t version;
    ods2_fid_t fid;
} rw_match_t;

/* a field of the new name: 0 literal (or empty), 1 exactly '*', -1 a partial wildcard */
static int rw_field_kind(const char *f, size_t n)
{
    char buf[ODS2_PATH_MAX];
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, f, n);
    buf[n] = '\0';
    if (strcmp(buf, "*") == 0) return 1;
    return rename_has_wild(buf) ? -1 : 0;
}

/* The new name for a file whose stored name is `sfull`: each field of the typed new name is literal, or exactly '*' (the stored field), or left out (the stored field too: CORPUS 70). Filled field by field
   from the PARSED name, never as text parsed again (review 75 C5): the stored name may hold ^. ^_ ; * % as name characters. A new name with only a directory keeps the stored name. false: no room or a bad escape. */
static bool rw_fill(const char *sfull, const ods2_parsed_path_t *dp, char *out, size_t cap)
{
    const char *fn = dp->filename, *dot = ods5_rfind_unescaped(fn, '.'), *sdot = strrchr(sfull, '.');
    size_t sname_len = sdot != NULL ? (size_t) (sdot - sfull) : strlen(sfull), nlen = dot != NULL ? (size_t) (dot - fn) : strlen(fn), k;
    char name[ODS2_PATH_MAX], type[ODS2_PATH_MAX], tmp[ODS2_PATH_MAX];

    if (fn[0] == '\0') {
        if (strlen(sfull) >= cap) return false;
        snprintf(out, cap, "%s", sfull);
        return true;
    }
    if (nlen == 0 || (nlen == 1 && fn[0] == '*')) {
        if (sname_len >= sizeof(name)) return false;
        memcpy(name, sfull, sname_len);
        name[sname_len] = '\0';
    } else {
        k = ods5_unescape(fn, nlen, name, sizeof(name));
        if (k == (size_t) -1) return false;
    }
    if (dot == NULL) {
        snprintf(type, sizeof(type), "%s", sdot != NULL ? sdot : "");                     /* a new name with no type: the stored type */
    } else if (dot[1] == '\0') {
        snprintf(type, sizeof(type), ".");                                                /* "name." : the null type */
    } else if (dot[1] == '*' && dot[2] == '\0') {
        snprintf(type, sizeof(type), "%s", sdot != NULL ? sdot : ".");
    } else {
        k = ods5_unescape(dot + 1, strlen(dot + 1), tmp, sizeof(tmp));
        if (k == (size_t) -1 || k + 2 > sizeof(type)) return false;
        snprintf(type, sizeof(type), ".%s", tmp);
    }
    if (strlen(name) + strlen(type) >= cap) return false;
    snprintf(out, cap, "%s%s", name, type);
    return true;
}

typedef struct { rw_match_t *v; size_t n, cap; } rw_list_t;

static bool rw_add(rw_list_t *l, const ods2_dir_entry_t *e)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        rw_match_t *nv = realloc(l->v, nc * sizeof(*nv));
        if (nv == NULL) return false;
        l->v = nv;
        l->cap = nc;
    }
    snprintf(l->v[l->n].name, sizeof(l->v[l->n].name), "%s", e->name);
    l->v[l->n].version = e->version;
    l->v[l->n].fid = e->fid;
    l->n++;
    return true;
}

/* does the directory `entries` hold an entry of this name (any version)? *fid gets the file of the first one found */
static bool rw_has_name(const ods2_dir_entry_t *entries, int count, const char *name, ods2_fid_t *fid)
{
    int i;
    for (i = 0; i < count; i++) {
        if (same_name(entries[i].name, name)) {
            if (fid != NULL) *fid = entries[i].fid;
            return true;
        }
    }
    return false;
}

static bool rw_same_file(ods2_fid_t a, ods2_fid_t b)
{
    return ods2_fid_number(a) == ods2_fid_number(b) && a.fid_seq == b.fid_seq;
}

typedef struct { size_t g; const char *key; int kind; } rw_key_t;     /* kind 0: a group's new name, 1: a group's own name (a source name) */

static int rw_key_cmp(const void *x, const void *y)
{
    const rw_key_t *a = (const rw_key_t *) x, *b = (const rw_key_t *) y;
    int c = ods2_dir_name_compare(a->key, strlen(a->key), b->key, strlen(b->key));
    if (c != 0) return c;
    if (a->kind != b->kind) return a->kind - b->kind;
    return a->g < b->g ? -1 : a->g > b->g ? 1 : 0;
}

typedef enum { RW_FAIL, RW_DONE, RW_SAME } rw_result_t;

/* One file: the step 1 to 3 call and the report (the same lines as the single-file RENAME). */
static rw_result_t rw_apply(ods2_volume_t *vol, unsigned sdir_num, const rw_match_t *m, unsigned ddir_num, const char *dname, int dver, const ods2_fid_t *via, unsigned nvia,
                            const ods2_fid_t *protect, unsigned nprotect, const char *sdir_path, const char *ddir_path)
{
    ods2_rename_info_t info;
    ods2_result_t r;
    char src[ODS2_PATH_MAX * 2 + 16], shown[ODS2_PATH_MAX], dst[ODS2_PATH_MAX * 2 + 16];

    memset(&info, 0, sizeof(info));
    spec_entry_text(sdir_path, m->name, m->version, src, sizeof(src));
    r = ods2_rename_protect(vol, sdir_num, m->name, (int) m->version, ddir_num, dname, dver, via, nvia, protect, nprotect, &info);     /* review 77: no file of the match list is ever the victim of a version limit's prune */
    if (!r.ok) {
        report_error("%%ODS2-E-RENAME, %s: %s\n", src, r.problem);
        return RW_FAIL;
    }
    display_name(dname, shown, sizeof(shown));
    if (!vol->ods5) {
        size_t q;
        for (q = 0; shown[q] != '\0'; q++) shown[q] = (char) toupper((unsigned char) shown[q]);            /* an ODS-2 name is stored in upper case (review 76 N3) */
    }
    snprintf(dst, sizeof(dst), "[%s%s]%s", ddir_path[0] == '\0' ? "000000" : "", ddir_path, shown);
    if (info.same) {
        printf("%%ODS2-I-RENAMED, %s is already named %s: nothing changed\n", src, dst);
        return RW_SAME;
    }
    if (info.case_only) {
        printf("%%ODS2-I-RENAMED, %s renamed to %s (the spelling only)\n", src, dst);
    } else {
        printf("%%ODS2-I-RENAMED, %s renamed to %s;%u%s%s\n", src, dst, (unsigned) info.version, info.resumed ? " (the target name already named this file: the old entry was removed and the rename finished)" : "",
               (info.header_changed || info.header_current) ? "" : " (an alias entry: the file's header is not changed)");
    }
    if (info.pruned_version != 0) printf("%%ODS2-I-DELETED, deleted %s;%u (the version limit)\n", dst, info.pruned_version);
    if (info.is_dir && !vol->ods5 && info.deepest_names > ODS2_VAX_V73_MAX_NAMES) {
        fprintf(stderr, "%%ODS2-W-DEEPDIR, the directory is now %u names deep below [000000]: OpenVMS VAX V7.3 cannot open a path of more than %u names on an ODS-2 volume (%%RMS-F-DIR); Alpha V7.3-2 and later can\n",
                info.deepest_names, ODS2_VAX_V73_MAX_NAMES);
    }
    return RW_DONE;
}

static void rename_wild(ods2_volume_t *vol, const ods2_parsed_path_t *sp, const ods2_parsed_path_t *dp, const char *src_arg, const char *dst_arg, bool device_only)
{
    uint8_t sdir[512], ddir[512];
    ods2_fid_t sfid, dfid, *via = NULL, *protect = NULL;
    char sdir_path[ODS2_PATH_MAX], ddir_path[ODS2_PATH_MAX], pat_name[ODS2_PATH_MAX + 4], pat_type[ODS2_PATH_MAX + 4], last_refused[ODS2_DIR_MAX_NAME + 1] = "", msg_dir[ODS2_PATH_MAX];
    ods2_dir_entry_t *entries = NULL, *dentries = NULL;
    int count = 0, dcount = 0, i, rank = 0, nk, tk, refused_order = 0, skipped = 0, renamed = 0, same = 0, failed = 0;
    const char *dot;
    bool dir_type, kept_versions, refuse = false, same_dir, g_ok = true;
    unsigned nvia = 0;
    rw_list_t list = { NULL, 0, 0 };
    ods2_result_t r;
    size_t gi, ngroups = 0;
    size_t *gstart = NULL;
    char (*dnames)[ODS2_DIR_MAX_NAME + 1] = NULL;
    bool *unchanged = NULL;
    rw_key_t *keys = NULL;
    size_t nkeys = 0;
    (void) device_only;                                          /* (the device of the new name was checked by the caller) */

    if (!vol->writable) {
        report_error("%%ODS2-E-RENAME, %s: %s\n", src_arg, vol->write_refusal[0] != '\0' ? vol->write_refusal : "the volume is mounted read-only");
        return;
    }
    if (sp->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, RENAME needs the name of the file to rename (a directory is renamed by its name.DIR)\n");
        return;
    }
    /* what the new name may be: each field literal or exactly '*' (review 75 D1); the version none, ;n, or ;* with a ;* source */
    {
        const char *fn = dp->filename;
        const char *ndot = ods5_rfind_unescaped(fn, '.');
        nk = rw_field_kind(fn, ndot != NULL ? (size_t) (ndot - fn) : strlen(fn));
        tk = ndot != NULL ? rw_field_kind(ndot + 1, strlen(ndot + 1)) : 0;
    }
    if (nk < 0 || tk < 0) {
        report_error("%%ODS2-E-RENAME, %s: a wildcard in the new name other than a whole name or type field (*) is refused as VMS refuses it (%%RENAME-E-OPENOUT, -RMS-F-FNM, error in file name; CORPUS 76.5): nothing renamed\n", dst_arg);
        return;
    }
    if ((dp->version_wildcard && !sp->version_wildcard) || dp->version < 0) {
        report_error("%%ODS2-E-RENAME, %s: a ;* in the new name with a one-version old name (and a negative new version) is not measured on VMS: nothing renamed\n", dst_arg);
        return;
    }
    if (dp->filename[0] == '\0' && !dp->had_brackets) {
        report_error("%%ODS2-E-BADPATH, %s: RENAME needs a new name or a directory for the files\n", dst_arg);
        return;
    }
    resolve_effective_path(sp, sdir_path, sizeof(sdir_path));
    r = resolve_dir(vol, sdir_path, sdir, &sfid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    if (dp->had_brackets) {
        resolve_effective_path(dp, ddir_path, sizeof(ddir_path));
        r = resolve_dir(vol, ddir_path, ddir, &dfid);
        if (!r.ok) {
            report_error("%%RENAME-E-OPENOUT, error opening %s as output\n-RMS-E-DNF, directory not found\n-SYSTEM-W-NOSUCHFILE, no such file\n", dst_arg);
            return;
        }
    } else {
        snprintf(ddir_path, sizeof(ddir_path), "%s", sdir_path);
        memcpy(ddir, sdir, sizeof(ddir));
        dfid = sfid;
    }
    same_dir = rw_same_file(sfid, dfid);                         /* directories are compared by file ID, not by their typed text (review 75 C4) */
    snprintf(msg_dir, sizeof(msg_dir), "%s", sdir_path);

    /* the match list: names and version numbers, the directory's own order (names ascending, a name's versions highest first), H1 by the rule of DELETE */
    dot = ods5_rfind_unescaped(sp->filename, '.');
    if (dot == NULL) {
        snprintf(pat_name, sizeof(pat_name), "%s", sp->filename);
        pat_type[0] = '\0';
    } else {
        snprintf(pat_name, sizeof(pat_name), "%.*s", (int) (dot - sp->filename), sp->filename);
        snprintf(pat_type, sizeof(pat_type), "%s", dot + 1);
    }
    dir_type = strcasecmp(pat_type, "DIR") == 0;                  /* directories take part only for a type that is exactly DIR (review 75 D3) */
    r = ods2_list_directory_alloc(vol, sdir, &entries, &count);
    if (!r.ok) {
        report_error("%%ODS2-E-RENAME, %s: %s\n", src_arg, r.problem);
        return;
    }
    for (i = 0; i < count; i++) {
        bool take;
        char lit_name[ODS2_DIR_MAX_NAME + 1];
        const char *ldot = strrchr(entries[i].name, '.');
        const char *lit_type = ldot != NULL ? ldot + 1 : "";
        size_t nlen = ldot != NULL ? (size_t) (ldot - entries[i].name) : strlen(entries[i].name);
        rank = (i > 0 && same_name(entries[i].name, entries[i - 1].name)) ? rank + 1 : 0;
        if (rank == 0 && !sp->version_wildcard && !(sp->has_version && sp->version > 0)) g_ok = group_in_order(entries, count, i);
        memcpy(lit_name, entries[i].name, nlen);
        lit_name[nlen] = '\0';
        if (!ods2_wildcard_match(pat_name, lit_name) || !ods2_wildcard_match(pat_type, lit_type)) continue;
        if (sp->version_wildcard) take = true;
        else if (!sp->has_version || sp->version == 0) take = (rank == 0);
        else if (sp->version > 0) take = (entries[i].version == (unsigned) sp->version);
        else take = (rank == -sp->version);
        if (!take) continue;
        if (!sp->version_wildcard && !(sp->has_version && sp->version > 0) && !g_ok) {
            if (strcmp(last_refused, entries[i].name) != 0) {
                report_version_order(msg_dir, entries[i].name, "renamed");
                snprintf(last_refused, sizeof(last_refused), "%s", entries[i].name);
                refused_order++;
            }
            continue;
        }
        {
            uint8_t h[512];
            bool is_dir = ods2_read_header_fid(vol, entries[i].fid, h).ok && ods2_header_is_file(h, entries[i].fid) && (((const ods2_head_core_t *) h)->filechar & 0x2000u) != 0 &&
                          ((const ods2_head_core_t *) h)->seg_num == 0;
            if (is_dir && !dir_type) {
                char t[ODS2_PATH_MAX * 2 + 16];
                spec_entry_text(sdir_path, entries[i].name, entries[i].version, t, sizeof(t));
                printf("%%ODS2-I-SKIPDIR, %s is a directory: a wildcard whose type is not exactly DIR does not rename it (RENAME it by its own name)\n", t);
                skipped++;
                continue;
            }
        }
        if (!rw_add(&list, &entries[i])) {
            report_error("%%ODS2-E-NOMEM, out of memory\n");
            goto out;
        }
    }
    if (list.n == 0) {
        if (refused_order == 0 && skipped == 0) {
            char spec[ODS2_PATH_MAX + 16], full[ODS2_PATH_MAX * 2 + 40];
            format_file_spec(sp, spec, sizeof(spec));
            snprintf(full, sizeof(full), "[%s%s]%s%s", sdir_path[0] == '\0' ? "000000" : "", sdir_path, spec, sp->has_version ? "" : ";");
            report_error("%%RENAME-E-SEARCHFAIL, error searching for %s\n-RMS-E-FNF, file not found\n", full);          /* CORPUS 76.9: VMS's words */
        } else if (skipped > 0) {
            char spec[ODS2_PATH_MAX + 16];
            format_file_spec(sp, spec, sizeof(spec));
            printf("%%ODS2-I-TOTAL, 0 files renamed, %d director%s skipped\n", skipped, skipped == 1 ? "y" : "ies");
            report_error("%%ODS2-E-RENAME, %s: no files renamed: the only matches were directories (a wildcard whose type is not exactly DIR does not rename them)\n", spec);     /* review 76 N1: exit 1 */
        }
        goto out;
    }

    protect = malloc((list.n + 1) * sizeof(*protect));
    if (protect == NULL) {
        report_error("%%ODS2-E-NOMEM, out of memory\n");
        goto out;
    }
    for (gi = 0; gi < list.n; gi++) protect[gi] = list.v[gi].fid;
    /* the groups (the versions of one name that follow each other) and their new names */
    gstart = malloc((list.n + 1) * sizeof(*gstart));
    dnames = malloc((list.n + 1) * sizeof(*dnames));
    unchanged = calloc(list.n + 1, sizeof(*unchanged));
    keys = malloc(2 * (list.n + 1) * sizeof(*keys));
    if (gstart == NULL || dnames == NULL || unchanged == NULL || keys == NULL) {
        report_error("%%ODS2-E-NOMEM, out of memory\n");
        goto out;
    }
    for (gi = 0; gi < list.n; gi++) {
        if (gi == 0 || !same_name(list.v[gi].name, list.v[gi - 1].name)) gstart[ngroups++] = gi;
    }
    gstart[ngroups] = list.n;
    for (gi = 0; gi < ngroups; gi++) {
        if (!rw_fill(list.v[gstart[gi]].name, dp, dnames[gi], sizeof(dnames[gi]))) {
            report_error("%%ODS2-E-BADNAME, %s: the new name is malformed or too long\n", dst_arg);
            goto out;
        }
        /* the new name is the file's own name in its own directory and no new version was typed: "already named" (review 76 S1: with a version typed the file's version is renamed, as the single-file RENAME does, CORPUS 67.4) */
        unchanged[gi] = same_dir && strcmp(dnames[gi], list.v[gstart[gi]].name) == 0 && !(dp->has_version && !dp->version_wildcard && dp->version != 0);
    }
    /* CORPUS 76.1, 76.6: a ;* source keeps its version numbers when the new name has no version or ;*; a ;n asks every file for ;n, an empty ; renumbers (the next version for each, highest first) */
    kept_versions = sp->version_wildcard && (!dp->has_version || dp->version_wildcard);
    if (sp->version_wildcard) {
        /* the versions of a name highest first, whatever order the directory stores them in (the last rename of a name is the lowest: a version limit prunes the lowest) */
        for (gi = 0; gi < ngroups; gi++) {
            size_t x, y;
            for (x = gstart[gi] + 1; x < gstart[gi + 1]; x++) {
                rw_match_t t = list.v[x];
                for (y = x; y > gstart[gi] && list.v[y - 1].version < t.version; y--) list.v[y] = list.v[y - 1];
                list.v[y] = t;
            }
        }
    }
    if (!same_dir) {
        r = ods2_list_directory_alloc(vol, ddir, &dentries, &dcount);
        if (!r.ok) {
            report_error("%%ODS2-E-RENAME, %s: %s\n", dst_arg, r.problem);
            goto out;
        }
    }
    {
        const ods2_dir_entry_t *tgt = same_dir ? entries : dentries;
        int tcount = same_dir ? count : dcount;
        /* (a) a target name that is also a name this command renames (CORPUS 76.7: VMS's result depends on the order): refused for the whole command before the first write, by name, any version, as the directory
           compares names; an entry that is the same file as its source (an alias, the second stopped state) is no collision (review 75 C3). Several sources for ONE new name are fine (76.4: consecutive versions). */
        for (gi = 0; gi < ngroups; gi++) {
            if (!unchanged[gi]) { keys[nkeys].g = gi; keys[nkeys].key = dnames[gi]; keys[nkeys].kind = 0; nkeys++; }
            if (same_dir) { keys[nkeys].g = gi; keys[nkeys].key = list.v[gstart[gi]].name; keys[nkeys].kind = 1; nkeys++; }
        }
        qsort(keys, nkeys, sizeof(*keys), rw_key_cmp);
        {
            size_t a = 0;
            while (a < nkeys) {
                size_t b = a + 1, x;
                size_t own = (size_t) -1, nconf_o = 0, first_o = (size_t) -1;
                while (b < nkeys && ods2_dir_name_compare(keys[a].key, strlen(keys[a].key), keys[b].key, strlen(keys[b].key)) == 0) b++;
                for (x = a; x < b; x++) if (keys[x].kind == 1) own = keys[x].g;
                for (x = a; x < b; x++) {
                    size_t g = keys[x].g;
                    if (keys[x].kind != 0 || own == (size_t) -1 || own == g || rw_same_file(list.v[gstart[g]].fid, list.v[gstart[own]].fid)) continue;
                    if (nconf_o == 0) first_o = g;
                    nconf_o++;
                }
                if (nconf_o >= 1) {
                    char s1[ODS2_PATH_MAX * 2 + 16], s2[ODS2_PATH_MAX * 2 + 16], dn[ODS2_PATH_MAX];
                    spec_entry_text(sdir_path, list.v[gstart[first_o]].name, 0, s1, sizeof(s1));
                    spec_entry_text(sdir_path, list.v[gstart[own]].name, 0, s2, sizeof(s2));
                    s1[strlen(s1) - 2] = '\0';                                                                    /* (the ";0" of the helper) */
                    s2[strlen(s2) - 2] = '\0';
                    display_name(dnames[first_o], dn, sizeof(dn));
                    if (nconf_o == 1) report_error("%%ODS2-E-RENAME, %s would become %s, which this command also renames (%s): nothing renamed\n", s1, dn, s2);
                    else report_error("%%ODS2-E-RENAME, %s and %zu more would become %s, which this command also renames (%s): nothing renamed\n", s1, nconf_o - 1, dn, s2);
                    refuse = true;
                }
                a = b;
            }
        }
        /* (b) the version limit (CORPUS 76.4 U4B: VMS silently DELETES a file this same command renamed when a later file of the command makes the limit prune it): per target name, in the order the files are
           renamed, the versions the name has and the versions this command makes, by the library's rule (a version above the highest, or the explicit one; with the count at or above the limit ONE lowest
           version goes; an explicit version that exists, or that would itself be the lowest, is refused for that file alone). A pruned version that this command made refuses the whole command (F1 class). */
        if (!refuse) {
            size_t a = 0;
            while (a < nkeys) {
                size_t b = a + 1, x;
                while (b < nkeys && ods2_dir_name_compare(keys[a].key, strlen(keys[a].key), keys[b].key, strlen(keys[b].key)) == 0) b++;
                if (keys[a].kind == 0) {
                    struct { unsigned v; bool moved; size_t src; } *vs = NULL;
                    size_t nv = 0, cap = 0, y, z;
                    unsigned limit = 0;
                    int ti;
                    bool first_entry = true;
                    for (ti = 0; ti < tcount; ti++) {
                        size_t li2;
                        if (!same_name(tgt[ti].name, keys[a].key)) continue;
                        if (nv == cap) {
                            void *nvp = realloc(vs, (cap ? cap * 2 : 8) * sizeof(*vs));
                            if (nvp == NULL) { report_error("%%ODS2-E-NOMEM, out of memory\n"); refuse = true; break; }
                            vs = nvp;
                            cap = cap ? cap * 2 : 8;
                        }
                        vs[nv].v = tgt[ti].version;
                        vs[nv].moved = false;
                        vs[nv].src = 0;
                        /* an existing version whose file is in the match list is ours (review 77 F2: an alias entry of a matched file already at the target; the library finishes that file, so the entry is its only name) */
                        for (li2 = 0; li2 < list.n; li2++) {
                            if (rw_same_file(list.v[li2].fid, tgt[ti].fid)) { vs[nv].moved = true; vs[nv].src = li2; break; }
                        }
                        nv++;
                        if (first_entry) limit = tgt[ti].verlimit;                  /* the library reads the limit of the FIRST record of the name (review 77 N1) */
                        first_entry = false;
                    }
                    if (first_entry) limit = ((const ods2_head_core_t *) ddir)->recattr.versions;       /* a new name takes the target directory's default limit (review 77 F1) */
                    if (limit >= 32767) limit = 0;                           /* no limit */
                    for (x = a; x < b && !refuse && keys[x].kind == 0; x++) {
                        size_t g = keys[x].g, k;
                        for (k = gstart[g]; k < gstart[g + 1] && !refuse; k++) {
                            unsigned v = 0, lowest, before;
                            unsigned explicit_v = kept_versions ? (unsigned) list.v[k].version : (dp->has_version && !dp->version_wildcard && dp->version > 0 ? (unsigned) dp->version : 0);
                            bool taken = false, resumed = false;
                            size_t li = 0;
                            /* review 77 recheck S2: a file that already has an entry of the new name (an alias, the second stopped state) is finished into it by the library: no new version is made */
                            for (y = 0; y < nv; y++) {
                                if (vs[y].moved && rw_same_file(list.v[vs[y].src].fid, list.v[k].fid) && (explicit_v == 0 || vs[y].v == explicit_v)) { vs[y].src = k; resumed = true; break; }
                            }
                            if (resumed) continue;
                            /* review 77 recheck S1: a new name that is the file's own name (a new version number on it): the source's own version goes out as the new one comes in, the count does not grow (the library's has_removed) */
                            if (same_dir && same_name(dnames[g], list.v[gstart[g]].name)) {
                                for (y = 0; y < nv; y++) {
                                    if (vs[y].v == list.v[k].version) {
                                        for (z = y; z + 1 < nv; z++) vs[z] = vs[z + 1];
                                        nv--;
                                        break;
                                    }
                                }
                            }
                            before = (unsigned) nv;
                            if (explicit_v != 0) v = explicit_v;
                            else { for (y = 0; y < nv; y++) if (vs[y].v > v) v = vs[y].v; v++; }
                            for (y = 0; y < nv; y++) if (vs[y].v == v) taken = true;
                            if (taken || v > 32767) continue;                  /* this file fails alone: nothing is made */
                            if (nv == cap) {
                                void *nvp = realloc(vs, (cap ? cap * 2 : 8) * sizeof(*vs));
                                if (nvp == NULL) { report_error("%%ODS2-E-NOMEM, out of memory\n"); refuse = true; break; }
                                vs = nvp;
                                cap = cap ? cap * 2 : 8;
                            }
                            vs[nv].v = v;
                            vs[nv].moved = true;
                            vs[nv].src = k;
                            nv++;
                            if (limit == 0 || before < limit) continue;
                            lowest = vs[0].v;
                            li = 0;
                            for (y = 1; y < nv; y++) if (vs[y].v < lowest) { lowest = vs[y].v; li = y; }
                            if (lowest == v) { nv--; continue; }                /* the library refuses this file: the version would be deleted at once */
                            if (vs[li].moved) {
                                char s1[ODS2_PATH_MAX * 2 + 16], s2[ODS2_PATH_MAX * 2 + 16], dn[ODS2_PATH_MAX];
                                spec_entry_text(sdir_path, list.v[k].name, list.v[k].version, s1, sizeof(s1));
                                spec_entry_text(sdir_path, list.v[vs[li].src].name, list.v[vs[li].src].version, s2, sizeof(s2));
                                display_name(dnames[g], dn, sizeof(dn));
                                report_error("%%ODS2-E-RENAME, %s: the version limit (%u) of %s would delete %s, which this command renames (VMS deletes it without a word, CORPUS 77): nothing renamed\n", s1, limit, dn, s2);
                                refuse = true;
                                break;
                            }
                            for (z = li; z + 1 < nv; z++) vs[z] = vs[z + 1];
                            nv--;
                        }
                    }
                    free(vs);
                }
                a = b;
            }
        }
    }
    if (refuse) goto out;

    /* the directories the typed new directory passes through, by whole file ID (a directory is not moved under itself by a path through a second entry of it: review 71 C1) */
    if (dp->had_brackets && ddir_path[0] != '\0') {
        char pre[ODS2_PATH_MAX];
        const char *d = ddir_path;
        size_t cap = 0;
        for (;;) {
            const char *nx = ods5_find_unescaped(d, '.');
            size_t len = nx != NULL ? (size_t) (nx - ddir_path) : strlen(ddir_path);
            ods2_fid_t f;
            memcpy(pre, ddir_path, len);
            pre[len] = '\0';
            if (ods2_lookup_path(vol, pre, &f).ok) {
                if (nvia == cap) {
                    ods2_fid_t *nv = realloc(via, (cap ? cap * 2 : 16) * sizeof(*nv));
                    if (nv == NULL) break;
                    via = nv;
                    cap = cap ? cap * 2 : 16;
                }
                via[nvia++] = f;
            }
            if (nx == NULL) break;
            d = nx + 1;
        }
    }

    /* each file its own transaction, in the list's order (highest version first within a name: a version limit prunes the lowest) */
    for (gi = 0; gi < ngroups; gi++) {
        size_t k;
        for (k = gstart[gi]; k < gstart[gi + 1]; k++) {
            int dver = (kept_versions && !unchanged[gi]) ? (int) list.v[k].version : (dp->has_version && !dp->version_wildcard ? dp->version : 0);
            rw_result_t rr;
            if (unchanged[gi]) dver = 0;
            rr = rw_apply(vol, ods2_fid_number(sfid), &list.v[k], ods2_fid_number(dfid), dnames[gi], dver, via, nvia, protect, (unsigned) list.n, sdir_path, ddir_path);
            if (rr == RW_DONE) renamed++;
            else if (rr == RW_SAME) same++;
            else failed++;
            if (vol->write_failed) {
                report_error("%%ODS2-E-WRITEFAIL, a write to the disk image failed; the rest of the command is not run\n");
                goto total;
            }
        }
    }
total:
    if (list.n > 1 || failed > 0 || skipped > 0) {
        char part[160] = "";
        size_t l = 0;
        if (same > 0) l += (size_t) snprintf(part + l, sizeof(part) - l, ", %d already named", same);
        if (failed > 0) l += (size_t) snprintf(part + l, sizeof(part) - l, ", %d not renamed (see above)", failed);
        if (skipped > 0) l += (size_t) snprintf(part + l, sizeof(part) - l, ", %d director%s skipped", skipped, skipped == 1 ? "y" : "ies");
        printf("%%ODS2-I-TOTAL, %d file%s renamed%s\n", renamed, renamed == 1 ? "" : "s", part);
    }
out:
    free(via);
    free(protect);
    free(list.v);
    free(gstart);
    free(dnames);
    free(unchanged);
    free(keys);
    free(entries);
    free(dentries);
}

/* ---- shared by PURGE, TYPE with wildcards and SET FILE/PROTECTION, /OWNER_UIC (PLAN_WRITE_OPS items 2 to 4; CORPUS 49.2 to 49.4, 49.7 to 49.9) ---- */

/* "[T]" for the directory spelling the walk gives ("" is the master directory). */
static void spec_dir_text(const char *dir_spelling, char *out, size_t cap)
{
    snprintf(out, cap, "[%s%s]", dir_spelling[0] == '\0' ? "000000" : "", dir_spelling);
}

/* "[T]A1.TXT;3": what VMS prints with its device in front. */
static void spec_entry_text(const char *dir_spelling, const char *literal_name, unsigned version, char *out, size_t cap)
{
    char d[ODS2_PATH_MAX + 4], n[ODS2_PATH_MAX];
    size_t l;
    spec_dir_text(dir_spelling, d, sizeof(d));
    display_name(literal_name, n, sizeof(n));
    l = (size_t) snprintf(out, cap, "%s", d);
    if (l < cap) snprintf(out + l, cap - l, "%s;%u", n, version);
}

/* The parsed path as the walk wants it: no version means the highest of each name (as TYPE, SET FILE and DELETE read it), `all_versions` every version. */
static void walk_path_for(const ods2_parsed_path_t *p, bool all_versions, ods2_parsed_path_t *out)
{
    *out = *p;
    if (all_versions) {
        out->has_version = true;
        out->version_wildcard = true;
    } else if (!out->has_version) {
        out->has_version = true;
        out->version = 0;
        out->version_wildcard = false;
    }
}

/* Runs the walk of DIR over the files `p` names, calling `hook` for each entry (directory order, a name's versions highest first). Returns the
   number of walk errors; *start_ok says whether the starting directory was found. */
static int walk_matches(ods2_volume_t *vol, const ods2_parsed_path_t *p, entry_hook_fn hook, void *ctx, bool *start_ok)
{
    entry_hook_fn saved_hook = g_entry_hook;
    void *saved_ctx = g_entry_hook_ctx;
    bool saved_paths = g_paths_mode;
    char effective[ODS2_PATH_MAX], spec[ODS2_PATH_MAX + 4], start[ODS2_PATH_MAX], pattern[ODS2_PATH_MAX + 4];
    const char *rest;
    dir_walk_t w;
    bool ok;

    *start_ok = false;
    resolve_effective_path(p, effective, sizeof(effective));
    snprintf(spec, sizeof(spec), "%s%s", effective, p->recursive ? "..." : "");
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return 1;
    }
    canonical_dir_path(vol, start, sizeof(start));
    {
        uint8_t sh[512];
        ods2_fid_t sf;
        if (!resolve_dir(vol, start, sh, &sf).ok) return 0;      /* no such directory: the caller says so in VMS's words */
    }
    if (p->filename[0] == '\0') snprintf(pattern, sizeof(pattern), "*.*");
    else if (ods5_find_unescaped(p->filename, '.') == NULL) snprintf(pattern, sizeof(pattern), "%s.*", p->filename);
    else snprintf(pattern, sizeof(pattern), "%s", p->filename);
    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = pattern;
    w.p = p;
    g_entry_hook = hook;
    g_entry_hook_ctx = ctx;
    g_paths_mode = false;
    ok = walk_from(&w, start);
    g_entry_hook = saved_hook;
    g_entry_hook_ctx = saved_ctx;
    g_paths_mode = saved_paths;
    *start_ok = ok;
    return w.errors;
}

/* The three-line message VMS gives when a search finds nothing; `dir_missing` for a directory that does not exist. */
static void search_failed(const char *command, const char *spec, bool dir_missing)
{
    report_error("%%%s-W-SEARCHFAIL, error searching for %s\n", command, spec);
    if (dir_missing) {
        report_error("-RMS-E-DNF, directory not found\n-SYSTEM-W-NOSUCHFILE, no such file\n");
    } else {
        report_error("-RMS-E-FNF, file not found\n");
    }
}

/* The text of a parsed path as VMS echoes it in SEARCHFAIL: the directory, the name, and the version (";" when none was typed). */
static void echo_spec(const ods2_parsed_path_t *p, char *out, size_t cap)
{
    char eff[ODS2_PATH_MAX];
    resolve_effective_path(p, eff, sizeof(eff));
    snprintf(out, cap, "[%s%s]%s;", eff[0] == '\0' ? "000000" : "", eff, p->filename);
    if (p->has_version && p->version_wildcard) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "*");
    } else if (p->has_version && p->version != 0) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%d", p->version);
    }
}

/* ---- PURGE [/KEEP=n] [/LOG] [spec] ----
   CORPUS 49.2: the default keep is 1 and /KEEP=0 is %PURGE-F-BADVALUE; the lowest versions go and the highest `keep` stay, in name order, each name's
   deleted versions highest first; /LOG prints one %PURGE-I-FILPURG line per version ("[dir]NAME;v deleted (n blocks)") and, when more than one went,
   %PURGE-I-TOTAL; nothing purged prints %PURGE-I-NOFILPURG; a name that does not exist is SEARCHFAIL plus NOFILPURG; on a read-only volume each version
   to delete is %PURGE-W-FILNOTPUR with -RMS-E-MKD and -SYSTEM-F-WRITLCK. Deletes go through the DELETE path (aliases and hard links are reported as DELETE
   reports them). ods2v2 acts as a privileged process, so a version whose protection forbids its owner to delete is purged too (VMS refuses it with
   -SYSTEM-F-NOPRIV to an unprivileged process that is not the volume owner: CORPUS 49.7). */
typedef struct {
    char dir[ODS2_PATH_MAX];
    char name[ODS2_DIR_MAX_NAME + 1];
    unsigned version;
    uint64_t blocks;
} purge_item_t;
typedef struct {
    purge_item_t *items;
    size_t count, cap;
    int errors;
} purge_ctx_t;

static void purge_collect(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    purge_ctx_t *x = (purge_ctx_t *) ctx_;
    uint8_t h[512];
    purge_item_t *it;

    if (ods2_fid_number(e->fid) <= 10 && ods2_fid_number(e->fid) != 0 && e->fid.fid_nmx == 0) return;     /* the reserved files are never purged */
    if (x->count == x->cap) {
        size_t nc = x->cap ? x->cap * 2 : 64;
        purge_item_t *ni = realloc(x->items, nc * sizeof(*ni));
        if (ni == NULL) { report_error("%%ODS2-E-NOMEM, out of memory\n"); x->errors++; return; }
        x->items = ni;
        x->cap = nc;
    }
    it = &x->items[x->count++];
    snprintf(it->dir, sizeof(it->dir), "%s", dir_spelling);
    snprintf(it->name, sizeof(it->name), "%s", e->name);
    it->version = e->version;
    it->blocks = 0;
    if (ods2_read_header_fid(vol, e->fid, h).ok && ods2_header_is_file(h, e->fid)) it->blocks = (ods2_file_allocated_bytes(h) + 511) / 512;
}

static int purge_item_cmp(const void *a, const void *b)
{
    unsigned va = ((const purge_item_t *) a)->version, vb = ((const purge_item_t *) b)->version;
    return va < vb ? 1 : va > vb ? -1 : 0;           /* highest version first */
}

/* The groups of purge items (one name's versions that follow each other) and the check that no name is in two of them (review 50 F1). */
typedef struct { size_t start, end; bool dup, first; } purge_group_t;
static const purge_item_t *g_purge_items;
static const purge_group_t *g_purge_groups;

static int purge_group_cmp(const void *a, const void *b)
{
    const purge_item_t *x = &g_purge_items[g_purge_groups[*(const size_t *) a].start], *y = &g_purge_items[g_purge_groups[*(const size_t *) b].start];
    int c = strcmp(x->dir, y->dir);
    if (c != 0) return c;
    c = ods2_dir_name_compare(x->name, strlen(x->name), y->name, strlen(y->name));
    if (c != 0) return c;
    return *(const size_t *) a < *(const size_t *) b ? -1 : 1;      /* the same name: in the order the groups were found */
}

/* Fills `groups` (capacity x->count) and returns how many, or (size_t) -1 when memory for the duplicate check is not to be had (the caller then refuses the whole PURGE: a
   check that did not run must not be taken as a check that passed; review 51); marks a group `dup` when another group has the same directory and name, `first` for the first of them. */
static size_t purge_find_groups(const purge_ctx_t *x, purge_group_t *groups)
{
    size_t i = 0, n = 0, a;
    size_t *order;
    while (i < x->count) {
        size_t j = i;
        while (j < x->count && strcmp(x->items[j].dir, x->items[i].dir) == 0 && same_name(x->items[j].name, x->items[i].name)) j++;
        groups[n].start = i;
        groups[n].end = j;
        groups[n].dup = groups[n].first = false;
        n++;
        i = j;
    }
    order = malloc((n > 0 ? n : 1) * sizeof(*order));
    if (order == NULL) return (size_t) -1;
    for (a = 0; a < n; a++) order[a] = a;
    g_purge_items = x->items;
    g_purge_groups = groups;
    qsort(order, n, sizeof(*order), purge_group_cmp);
    for (a = 1; a < n; a++) {
        const purge_item_t *p = &x->items[groups[order[a - 1]].start], *q = &x->items[groups[order[a]].start];
        if (strcmp(p->dir, q->dir) == 0 && ods2_dir_name_compare(p->name, strlen(p->name), q->name, strlen(q->name)) == 0) {
            if (!groups[order[a - 1]].dup) groups[order[a - 1]].first = true;       /* the first group of the name in the order found (the sort kept that order) */
            groups[order[a - 1]].dup = groups[order[a]].dup = true;
        }
    }
    free(order);
    return n;
}

static void cmd_purge(ods2_volume_t *vol, const ods2_parsed_path_t *p, long keep, bool log)
{
    purge_ctx_t x;
    ods2_parsed_path_t wp;
    bool start_ok;
    int werrs;
    size_t i = 0, gi, ngroups;
    purge_group_t *groups;
    unsigned deleted = 0;
    uint64_t blocks = 0;
    char echo[ODS2_PATH_MAX + 32];

    memset(&x, 0, sizeof(x));
    walk_path_for(p, true, &wp);
    g_disorder_mode = 2;
    g_disorder_refused = 0;
    werrs = walk_matches(vol, &wp, purge_collect, &x, &start_ok);
    g_disorder_mode = 0;
    if (!start_ok) {
        echo_spec(&wp, echo, sizeof(echo));
        search_failed("PURGE", echo, true);
        printf("%%PURGE-I-NOFILPURG, no files purged\n");
        free(x.items);
        return;
    }
    if (x.count == 0 && werrs == 0 && g_disorder_refused > 0) {
        free(x.items);                       /* the name was found and refused for its order: no "not found" and no "no files purged" for it (review 66 S1) */
        return;
    }
    if (x.count == 0 && werrs == 0) {
        echo_spec(&wp, echo, sizeof(echo));
        search_failed("PURGE", echo, false);
        printf("%%PURGE-I-NOFILPURG, no files purged\n");
        free(x.items);
        return;
    }
    groups = malloc((x.count > 0 ? x.count : 1) * sizeof(*groups));
    if (groups == NULL) { report_error("%%ODS2-E-NOMEM, out of memory\n"); free(x.items); return; }
    ngroups = purge_find_groups(&x, groups);
    if (ngroups == (size_t) -1) { report_error("%%ODS2-E-NOMEM, out of memory\n"); free(groups); free(x.items); return; }
    for (gi = 0; gi < ngroups; gi++) {
        size_t j = groups[gi].end, k;
        i = groups[gi].start;
        if (groups[gi].dup) {
            /* Review 50 F1: grouping is by neighbours, and a name that a damaged directory holds in two records with another name between them is two groups: each
               is healthy by itself, and the second one's deletes by number reach the version the first one kept (and an entry taken for an alias leaves a file in no
               directory). Every group of such a name is refused, once; the other names go on. */
            if (groups[gi].first) {
                char nm[ODS2_PATH_MAX], what[ODS2_PATH_MAX * 2 + 8];
                display_name(x.items[i].name, nm, sizeof(nm));
                spec_dir_text(x.items[i].dir, what, sizeof(what));
                report_error("%%PURGE-W-FILNOTPUR, error deleting %s%s: the name is listed in more than one place in the directory (the directory entry is damaged): the name is not purged\n", what, nm);
            }
            continue;
        }
        /* [i, j) are the versions of one name. They are kept by version number, never by the order the directory stores them in (review 48 F3: a
           directory damaged or written by another tool can hold them out of order, and "the first `keep` stay" would then delete the newest data):
           sorted here, highest first, the first `keep` stay. Sorting and not refusing the name: the deletes name each version, so they are right
           whatever the stored order, and a refusal would leave the volume's owner no way to purge it. */
        qsort(x.items + i, j - i, sizeof(*x.items), purge_item_cmp);
        /* Review 49 S1: the deletes name a version by its number, so a number that is not a usable one makes the delete reach another file: 0 is read as "the
           highest", a number over 32767 sorts above every real one, and two entries with one number delete the first match. A name like that is not purged (the
           other names go on); ods5check warns of it. Valid, different numbers in the wrong order are sorted and purged as above. */
        {
            bool bad = false;
            const char *why = NULL;
            for (k = i; k < j; k++) {
                if (x.items[k].version == 0) { bad = true; why = "a version number is 0"; }
                else if (x.items[k].version > 32767) { bad = true; why = "a version number is over 32767"; }
                else if (k > i && x.items[k].version == x.items[k - 1].version) { bad = true; why = "two entries have the same version number"; }
            }
            if (bad) {
                char nm[ODS2_PATH_MAX], what[ODS2_PATH_MAX * 2 + 8];
                display_name(x.items[i].name, nm, sizeof(nm));
                spec_dir_text(x.items[i].dir, what, sizeof(what));
                report_error("%%PURGE-W-FILNOTPUR, error deleting %s%s: %s (the directory entry is damaged): the name is not purged\n", what, nm, why);
                continue;
            }
        }
        for (k = i + (size_t) keep; k < j; k++) {
            const purge_item_t *it = &x.items[k];
            char what[ODS2_PATH_MAX * 2 + 16];
            spec_entry_text(it->dir, it->name, it->version, what, sizeof(what));
            if (!vol->writable) {
                report_error("%%PURGE-W-FILNOTPUR, error deleting %s\n-RMS-E-MKD, ACP could not mark file for deletion\n-SYSTEM-F-WRITLCK, write lock error\n", what);
                if (vol->write_refusal[0] != '\0') report_error("-ODS2-I-NOWRITE, %s\n", vol->write_refusal);
                continue;
            }
            {
                ods2_fid_t dfid;
                ods2_delete_info_t info;
                uint16_t done = 0;
                ods2_result_t r = ods2_lookup_path(vol, it->dir, &dfid);
                if (r.ok) r = ods2_check_fid(dfid);
                if (r.ok) r = ods2_delete_version_info(vol, ods2_fid_number(dfid), it->name, (int) it->version, &done, &info);
                if (!r.ok) {
                    report_error("%%PURGE-W-FILNOTPUR, error deleting %s\n-ODS2-E-DELETEERR, %s\n", what, r.problem);
                    continue;
                }
                deleted++;
                if (info.kept != 0 || info.link_removed) {
                    report_deletion(it->name, done, &info);         /* as DELETE says it: an alias or a link was removed, the file stays, no blocks came back */
                    continue;
                }
                blocks += it->blocks;
                if (log) printf("%%PURGE-I-FILPURG, %s deleted (%llu block%s)\n", what, (unsigned long long) it->blocks, it->blocks == 1 ? "" : "s");
            }
        }
    }
    free(groups);
    if (deleted == 0) printf("%%PURGE-I-NOFILPURG, no files purged\n");
    else if (log && deleted > 1) printf("%%PURGE-I-TOTAL, %u files deleted (%llu blocks)\n", deleted, (unsigned long long) blocks);
    free(x.items);
}

/* ---- TYPE with wildcards and comma lists (CORPUS 49.3) ----
   For each file found by a wildcard, and for every file of a list but the first one named without a wildcard: a line with one space, a line with the full
   name (here without the device VMS prints in front: "[T]A1.TXT;1"), a line with one space, then the records. With no wildcard and no list there is no
   header (the old single-file TYPE). A wildcard without a version shows the highest version of each name in name order; ;* every version, highest first.
   Nothing found: %TYPE-W-SEARCHFAIL, error searching for [T]*.NOSUCH; and -RMS-E-FNF (or -RMS-E-DNF and -SYSTEM-W-NOSUCHFILE for a missing directory);
   the messages of a list come after what was typed. */
typedef struct {
    bool raw, lines, header;
    unsigned found;
} typewild_ctx_t;

static void typewild_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    typewild_ctx_t *x = (typewild_ctx_t *) ctx_;
    char what[ODS2_PATH_MAX * 2 + 16];
    uint8_t h[512];

    if (!ods2_read_header_fid(vol, e->fid, h).ok || !ods2_header_is_file(h, e->fid)) return;     /* a dangling alias: not a file to type */
    spec_entry_text(dir_spelling, e->name, e->version, what, sizeof(what));
    if (x->header) printf(" \n%s\n \n", what);
    fflush(stdout);
    x->found++;
    type_one_file(vol, e->fid, what, x->raw, x->lines);
}

static bool spec_has_wildcard(const ods2_parsed_path_t *p)
{
    size_t i;
    if (p->version_wildcard) return true;
    for (i = 0; p->filename[i]; i++) {
        if (p->filename[i] == '^') { if (p->filename[i + 1]) i++; continue; }
        if (p->filename[i] == '*' || p->filename[i] == '%' || p->filename[i] == '?') return true;
    }
    return false;
}

/* Splits "[T]A1.TXT,A3.TXT" at the commas outside brackets; an element with no directory takes the previous element's. Returns the count (0 on error). */
static int split_list(const char *arg, char elems[][ODS2_PATH_MAX + 16], int max)
{
    int n = 0, depth = 0;
    const char *s = arg, *start = arg;
    char prev_dir[ODS2_PATH_MAX] = "";
    for (;; s++) {
        /* a ^ escapes the character after it (^, is a comma in a name, ^^ a caret, ^[ and ^] brackets): it is a character of the name, never a separator or a bracket (review 66: TYPE
           [D]sp^,c.txt was split at the escaped comma and typed [D]c.txt). The tokenizer drops quotes, so a comma that is not escaped is a separator even
           inside quotes: VMS wants ^, for a comma in an ODS-5 name. */
        if (*s == '^' && s[1] != '\0') { s++; continue; }
        if (*s == '[' || *s == '<') depth++;
        else if (*s == ']' || *s == '>') { if (depth > 0) depth--; }
        if ((*s == ',' && depth == 0) || *s == '\0') {
            size_t len = (size_t) (s - start);
            char one[ODS2_PATH_MAX + 16];
            if (n >= max || len == 0 || len >= sizeof(one)) return 0;
            memcpy(one, start, len);
            one[len] = '\0';
            if (ods5_find_unescaped(one, '[') == NULL && ods5_find_unescaped(one, '<') == NULL && ods5_find_unescaped(one, ':') == NULL && prev_dir[0] != '\0') {
                snprintf(elems[n], ODS2_PATH_MAX + 16, "%s%s", prev_dir, one);
            } else {
                snprintf(elems[n], ODS2_PATH_MAX + 16, "%s", one);
                {
                    const char *close = ods5_find_unescaped(one, ']');
                    if (close != NULL) { size_t dl = (size_t) (close - one) + 1; if (dl >= sizeof(prev_dir)) return 0; memcpy(prev_dir, one, dl); prev_dir[dl] = '\0'; }
                }
            }
            n++;
            start = s + 1;
            if (*s == '\0') break;
        }
    }
    return n;
}

/* TYPE of a wildcard or a list. Returns true when it handled the argument (false: a plain single file, the old TYPE). */
static bool cmd_type_list(ods2_volume_t *vol, const char *arg, bool raw, bool lines)
{
    static char elems[64][ODS2_PATH_MAX + 16];
    int n, i;
    bool any_wild = false, listed;
    ods2_parsed_path_t ps[64];
    char missing[64][ODS2_PATH_MAX + 32];
    bool dnf[64];
    int nmissing = 0;

    n = split_list(arg, elems, 64);
    if (n == 0) return false;                   /* not a list we can read: the plain parser reports it */
    for (i = 0; i < n; i++) {
        if (!ods2_parse_path(elems[i], &ps[i])) { report_error("%%ODS2-E-BADPATH, could not parse %s\n", elems[i]); return true; }
        if (refuse_wild_dir(&ps[i], "TYPE", elems[i])) return true;
        if (spec_has_wildcard(&ps[i])) any_wild = true;
    }
    if (n == 1 && !any_wild) return false;
    listed = n > 1;
    for (i = 0; i < n; i++) {
        bool wild = spec_has_wildcard(&ps[i]);
        typewild_ctx_t x;
        ods2_parsed_path_t wp;
        bool start_ok;
        int werrs;
        memset(&x, 0, sizeof(x));
        x.raw = raw;
        x.lines = lines;
        x.header = wild || (listed && i > 0);
        if (ps[i].filename[0] == '\0') { report_error("%%ODS2-E-BADPATH, no filename given\n"); return true; }
        walk_path_for(&ps[i], false, &wp);
        werrs = walk_matches(vol, &wp, typewild_entry, &x, &start_ok);
        if (!start_ok && nmissing < 64) {
            echo_spec(&ps[i], missing[nmissing], sizeof(missing[nmissing]));
            dnf[nmissing++] = true;
        } else if (x.found == 0 && werrs == 0 && nmissing < 64) {
            echo_spec(&ps[i], missing[nmissing], sizeof(missing[nmissing]));
            dnf[nmissing++] = false;
        }
    }
    for (i = 0; i < nmissing; i++) search_failed("TYPE", missing[i], dnf[i]);
    return true;
}

/* ---- SET FILE/PROTECTION=(...) and SET FILE/OWNER_UIC=[g,m] (CORPUS 49.4, 49.7, 49.8) ---- */

/* A protection specification: the fields named, each with its four denial bits. mask/value are the whole FH2$W_FILEPROT word's parts. */
static bool parse_protection_spec(const char *text, uint16_t *mask, uint16_t *value, char *bad, size_t badcap)
{
    char buf[128];
    char *tok, *save = NULL;
    size_t n = strlen(text);
    *mask = 0;
    *value = 0;
    if (n >= 2 && text[0] == '(' && text[n - 1] == ')') {
        if (n - 2 >= sizeof(buf)) { snprintf(bad, badcap, "TOOLONG"); return false; }      /* review 48 F1: a list that does not fit is refused */
        memcpy(buf, text + 1, n - 2);
        buf[n - 2] = '\0';
    }
    else if (n < sizeof(buf)) snprintf(buf, sizeof(buf), "%s", text);
    else { snprintf(bad, badcap, "TOOLONG"); return false; }
    if (buf[0] == '\0') { snprintf(bad, badcap, "%s", text); return false; }
    for (tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':'), *letters;
        char field[32];
        int shift, k;
        size_t fl = colon ? (size_t) (colon - tok) : strlen(tok);
        uint16_t deny = 0xF;
        if (fl == 0 || fl >= sizeof(field)) { snprintf(bad, badcap, "%s", tok); return false; }
        memcpy(field, tok, fl);
        field[fl] = '\0';
        for (k = 0; field[k]; k++) field[k] = (char) toupper((unsigned char) field[k]);
        if (strncmp("SYSTEM", field, fl) == 0) shift = 0;
        else if (strncmp("OWNER", field, fl) == 0) shift = 4;
        else if (strncmp("GROUP", field, fl) == 0) shift = 8;
        else if (strncmp("WORLD", field, fl) == 0) shift = 12;
        else { snprintf(bad, badcap, "%s", field); return false; }
        if (colon != NULL) {
            letters = colon + 1;
            if (*letters == '\0') { snprintf(bad, badcap, "%s", "VALREQ"); return false; }
            for (k = 0; letters[k]; k++) {
                switch (toupper((unsigned char) letters[k])) {
                case 'R': deny &= (uint16_t) ~1u; break;
                case 'W': deny &= (uint16_t) ~2u; break;
                case 'E': deny &= (uint16_t) ~4u; break;
                case 'D': deny &= (uint16_t) ~8u; break;
                default: snprintf(bad, badcap, "%s", letters); return false;
                }
            }
        }
        *mask = (uint16_t) (*mask | (0xFu << shift));
        *value = (uint16_t) ((*value & ~(0xFu << shift)) | (deny << shift));      /* a repeated field takes the last */
    }
    return *mask != 0;
}

static void protection_word_text(uint16_t word, char *out, size_t cap)
{
    static const char *names[4] = { "S", "O", "G", "W" };
    size_t n = 0;
    int f, b;
    out[0] = '\0';
    for (f = 0; f < 4; f++) {
        n += (size_t) snprintf(out + n, cap - n, "%s%s:", f ? "," : "", names[f]);
        for (b = 0; b < 4; b++) {
            if (!((word >> (f * 4)) & (1u << b)) && n + 2 < cap) { out[n++] = "RWED"[b]; out[n] = '\0'; }
        }
    }
}

/* [g,m] in octal (up to 11 digits each, as DCL prints a numeric UIC) or [SYSTEM]; names that need the rights database are refused. */
static bool parse_uic_spec(const char *text, uint16_t *group, uint16_t *member, char *msg, size_t msgcap)
{
    size_t n = strlen(text);
    char buf[64], *comma;
    char *end;
    unsigned long g, m;
    if (n < 3 || n >= sizeof(buf) || text[0] != '[' || text[n - 1] != ']') { snprintf(msg, msgcap, "%%SET-E-SYNTAX, error parsing '%s'", text); return false; }
    memcpy(buf, text + 1, n - 2);
    buf[n - 2] = '\0';
    comma = strchr(buf, ',');
    if (comma == NULL) {
        if (strcasecmp(buf, "SYSTEM") == 0) { *group = 1; *member = 4; return true; }
        snprintf(msg, msgcap, "%%SET-E-SYNTAX, error parsing '[%s]'\n-ODS2-W-NORIGHTS, an identifier name needs the rights database, which ods2v2 does not have: give [group,member] in octal (or [SYSTEM])", buf);
        return false;
    }
    *comma = '\0';
    if (buf[0] == '\0' || comma[1] == '\0' || strspn(buf, "01234567") != strlen(buf) || strspn(comma + 1, "01234567") != strlen(comma + 1)) {
        if (isalpha((unsigned char) buf[0]) || isalpha((unsigned char) comma[1])) {
            snprintf(msg, msgcap, "%%SET-E-SYNTAX, error parsing '%s'\n-ODS2-W-NORIGHTS, an identifier name needs the rights database, which ods2v2 does not have: give [group,member] in octal (or [SYSTEM])",
                     isalpha((unsigned char) buf[0]) ? buf : comma + 1);
        } else {
            snprintf(msg, msgcap, "%%SET-E-SYNTAX, error parsing '[%s,%s]'", buf, comma + 1);
        }
        return false;
    }
    g = strtoul(buf, &end, 8);
    m = strtoul(comma + 1, &end, 8);
    if (strlen(buf) > 11 || strlen(comma + 1) > 11 || g > 0xFFFFu || m > 0xFFFFu) { snprintf(msg, msgcap, "%%SET-E-SYNTAX, error parsing '[%s,%s]'", buf, comma + 1); return false; }
    *group = (uint16_t) g;
    *member = (uint16_t) m;
    return true;
}

typedef struct {
    unsigned what;
    uint16_t mask, value, group, member;
    bool log;
    unsigned found;
    int errors;
} setsec_ctx_t;

static void setsec_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    setsec_ctx_t *x = (setsec_ctx_t *) ctx_;
    char what[ODS2_PATH_MAX * 2 + 16];
    uint8_t h[512];
    uint16_t prot = 0;
    ods2_result_t r;

    x->found++;
    spec_entry_text(dir_spelling, e->name, e->version, what, sizeof(what));
    if (x->what & ODS2_SETSEC_PROT) {
        r = ods2_read_header_fid(vol, e->fid, h);
        if (!r.ok) { report_error("%%SET-E-READERR, error reading %s\n-ODS2-E-READERR, %s\n", what, r.problem); x->errors++; return; }
        prot = (uint16_t) ((((const ods2_head_core_t *) h)->fileprot & ~x->mask) | x->value);
    }
    r = ods2_set_file_security(vol, e->fid, x->what, prot, x->group, x->member);
    if (!r.ok) {
        report_error("%%SET-E-NOTSET, error modifying %s\n-ODS2-E-SETERR, %s\n", what, r.problem);
        x->errors++;
        return;
    }
    if (x->log) {
        if (x->what & ODS2_SETSEC_PROT) {
            char t[64];
            protection_word_text(prot, t, sizeof(t));
            printf("%%SET-I-PROTECTED, %s file protection changed to %s\n", what, t);
        } else {
            printf("%%SET-I-MODIFIED, %s modified\n", what);
        }
    }
}

/* Appends `add` to buf (capacity cap, `*used` characters in it); false, nothing appended, when it does not fit. */
static bool qual_append(char *buf, size_t cap, size_t *used, const char *add)
{
    size_t n = strlen(add);
    if (*used + n + 1 > cap) return false;
    memcpy(buf + *used, add, n + 1);
    *used += n;
    return true;
}

/* tokens[1] is "FILE/PROTECTION=(...)" or "FILE/OWNER_UIC=[g,m]", with /LOG anywhere among the qualifiers; a parenthesis list may have been split at its spaces. */
static void cmd_set_file_security(ods2_volume_t *vol, const char *quals, const char *spec)
{
    setsec_ctx_t x;
    ods2_parsed_path_t p, wp;
    char msg[400], bad[128];
    bool have_prot = false, have_owner = false, start_ok;
    char protv[192] = "", ownv[192] = "";
    const char *q = quals;
    int werrs;

    memset(&x, 0, sizeof(x));
    while (*q != '\0') {
        const char *eq, *end;
        size_t ql;
        int depth = 0;
        if (*q == '/') q++;
        end = q;
        while (*end != '\0' && !(*end == '/' && depth == 0)) {
            if (*end == '(' || *end == '[') depth++;
            else if (*end == ')' || *end == ']') { if (depth > 0) depth--; }
            end++;
        }
        ql = (size_t) (end - q);
        eq = memchr(q, '=', ql);
        {
            char name[32], val[160];
            size_t nl = eq ? (size_t) (eq - q) : ql;
            size_t vl = eq ? ql - nl - 1 : 0;
            if (nl >= sizeof(name) || vl >= sizeof(val)) { report_error("%%ODS2-E-IVQUAL, a qualifier is too long\n"); return; }
            memcpy(name, q, nl); name[nl] = '\0';
            if (vl) memcpy(val, eq + 1, vl);
            val[vl] = '\0';
            if (nl > 0 && strncasecmp("PROTECTION", name, nl) == 0 && nl >= 3) { have_prot = true; snprintf(protv, sizeof(protv), "%s", val); }
            else if (nl > 0 && strncasecmp("OWNER_UIC", name, nl) == 0 && nl >= 3) { have_owner = true; snprintf(ownv, sizeof(ownv), "%s", val); }
            else if (nl > 0 && strncasecmp("LOG", name, nl) == 0 && vl == 0) x.log = true;
            else if (nl > 0 && strncasecmp("NOLOG", name, nl) == 0 && nl >= 3 && vl == 0) x.log = false;
            else { report_error("%%SET-F-IVQUAL, unrecognized qualifier - /%s\n", name); return; }
        }
        q = end;
    }
    if (have_prot == have_owner) {
        report_error("%%ODS2-E-NOPARM, usage: SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) <spec>  or  SET FILE/OWNER_UIC=[group,member] <spec>   (one of them per command: both at "
                     "once was not measured on VMS)\n");
        return;
    }
    if (have_prot) {
        x.what = ODS2_SETSEC_PROT;
        if (!parse_protection_spec(protv, &x.mask, &x.value, bad, sizeof(bad))) {
            if (strcmp(bad, "VALREQ") == 0) report_error("%%DCL-W-VALREQ, missing qualifier or keyword value\n");
            else if (strcmp(bad, "TOOLONG") == 0) report_error("%%SET-F-SYNTAX, the protection list is too long (more than 127 characters): not run\n");
            else report_error("%%SET-F-SYNTAX, error parsing '%s'\n", bad);
            return;
        }
    } else {
        x.what = ODS2_SETSEC_OWNER;
        if (!parse_uic_spec(ownv, &x.group, &x.member, msg, sizeof(msg))) { report_error("%s\n", msg); return; }
    }
    if (!ods2_parse_path(spec, &p)) { report_error("%%ODS2-E-BADPATH, could not parse %s\n", spec); return; }
    walk_path_for(&p, false, &wp);
    g_disorder_mode = 1;
    g_disorder_refused = 0;
    werrs = walk_matches(vol, &wp, setsec_entry, &x, &start_ok);
    g_disorder_mode = 0;
    if (x.found == 0 && werrs == 0 && g_disorder_refused > 0) {
        return;                              /* found and refused for its order: no SEARCHFAIL for it (review 66 S1) */
    }
    if (x.found == 0 && werrs == 0) {
        char echo[ODS2_PATH_MAX + 32];
        echo_spec(&p, echo, sizeof(echo));
        search_failed("SET", echo, false);
    } else if (!start_ok) {
        char echo[ODS2_PATH_MAX + 32];
        echo_spec(&p, echo, sizeof(echo));
        search_failed("SET", echo, true);
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
           "  COPY <local-file> <path>   write a local file onto the disk: text as Stream_LF (CR LF: Stream,\n"
           "                             CR only: Stream_CR), anything else as fixed 512-byte records.\n"
           "                             A name that exists gets a new version (highest + 1; NAME;n makes\n"
           "                             version n if it is free: %%RMS-E-FEX if it exists); the entry's\n"
           "                             version limit deletes the oldest versions as part of the COPY\n"
           "  COPY/FORMAT=fmt <local-file> <path>\n"
           "                             choose the record format: AUTO (the default), STMLF (TEXT), STM,\n"
           "                             STMCR, FIX512 (BINARY), UDF or VAR (= /CONVERT)\n"
           "  COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) <local-file> <path>\n"
           "                             ... with chosen record attributes: RFM: FIX,\n"
           "                             STMLF, STM, STMCR or UDF; MRS: record size;\n"
           "                             RAT: NONE, CR, FTN, PRN, BLK (one per RAT:)\n"
           "  COPY/CONVERT <local-file> <path>\n"
           "                             a text file as variable-length records with\n"
           "                             CR carriage control (VMS's native text), one\n"
           "                             per line; with /ATTRIBUTES for other RAT/MRS\n"
           "  IMPORT <linux-dir> <path>  copy a Linux directory tree onto the disk (UTF-8 names,\n"
           "                             directories made, symbolic links kept, ODS-5; record format per\n"
           "                             file as COPY; /FORMAT= and /ATTRIBUTES apply to every file;\n"
           "                             a name that exists is skipped, or with /NEW_VERSION gets a new version)\n"
           "  SYMLINK <link-path> <target>   make a symbolic link (ODS-5; as CREATE/SYMLINK=target)\n"
           "  LINK <new-path> <existing-path> another directory entry for a file (as SET\n"
           "                             FILE/ENTER=new existing): a hard link with a link count on\n"
           "                             a HARDLINKS volume, otherwise a plain alias (count unchanged)\n"
           "  TYPE[/RAW|/LINES] path     print a sequential file as text the way VMS TYPE shows its\n"
           "                             records (VAR, VFC, FIX, STM*; carriage control applied:\n"
           "                             none = records run together); /LINES: always one line\n"
           "                             per record; /RAW: the stored bytes\n"
           "  TYPE [dir]wild[;ver] or a,b  wildcards and comma lists as VMS TYPE: a header (a space line, the\n"
           "                             name, a space line) before each file, none for the first file of a\n"
           "                             list named without a wildcard; no match: %%TYPE-W-SEARCHFAIL\n"
           "  RENAME old new             rename a file in its directory (a new name, type or version; the target\n"
           "                             name that exists gets the next version); aliases and other directories\n"
           "                             and wildcards: later steps\n"
           "  DELETE path                delete a file (one version) or empty directory\n"
           "  DELETE [dir]wild[;ver]     every match: F*.COM (highest), *.*;* (all versions)\n"
           "  PURGE[/KEEP=n][/LOG] [spec]  keep the n highest versions (default 1) of each name, delete the rest\n"
           "                             (wildcards; no spec: the default directory); /LOG lists what went\n"
           "  SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) spec   only the fields named change; /LOG prints them\n"
           "  SET FILE/OWNER_UIC=[group,member] spec   octal numbers or [SYSTEM]; revision +1 for each change\n"
           "  SET DEFAULT path           set the current default directory\n"
           "  SHOW DEFAULT               show the current default directory\n"
           "  SET VOLUME/LABEL=name      relabel the volume (all home block copies)\n"
           "  SET VOLUME/PARTIAL_CLUSTER   give the blocks past the last whole cluster to BADBLK.SYS, as VMS INITIALIZE\n"
           "                             does (volumes made by an older ods2_init: V8.4 ANALYZE says ALLOCCLR)\n"
           "  SET VOLUME/GEOMETRY=sectors,tracks   record the disk geometry of the device the volume will be used on\n"
           "                             (OpenVMS VAX V7.3 ANALYZE compares it with the device: CHKSCB)\n"
           "  SET FILE/VERSION_LIMIT=n spec   the version limit of the entries a spec names (wildcards, [dir...]);\n"
           "                             32767 or 0 = no limit, what VMS writes (volumes written by ods2v2 before\n"
           "                             the fix carry 1: VMS purges version 1 when it makes version 2)\n"
           "  SET FILE/ATTRIBUTES=(MRS:0) spec   stream files made with a maximum record size (ods2v2 once gave every\n"
           "                             Stream_LF file 512): MRS 0 and LRL the longest record, as VMS makes them\n"
           "                             (RMS refused to read such a file with a longer line: RMS-W-RTB)\n"
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
/* The tokens of a command line, or -1 for a line that cannot be taken exactly as typed: an unterminated quote
   or more tokens than there is room for (the rest would be dropped without a word: F4). */
static int tokenize(char *line, char *tokens[MAX_TOKENS])
{
    int count = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char) *p)) p++;
        if (!*p) break;
        if (count == MAX_TOKENS) return -1;
        tokens[count++] = p;
        {
            /* "..." keeps white space inside one token (a Linux path with a
               space, a name with a literal space); the quotes are dropped. A
               quote after a ^ is the ^" of a name, not a quote: it stays in the
               token and the name rules deal with it. */
            char *w = p;
            bool in_quote = false;
            while (*p && (in_quote || !isspace((unsigned char) *p))) {
                if (*p == '^' && p[1] == '"') { *w++ = *p++; *w++ = *p++; continue; }
                if (*p == '"') { in_quote = !in_quote; p++; continue; }
                *w++ = *p++;
            }
            if (in_quote) return -1;
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
    if (n < 0) {
        report_error("%%ODS2-E-SYNTAX, the command has an unterminated quote or too many words: not run\n");
        return true;
    }
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
        r = ods2_read_header_fid(vol, fid, header);
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
    if (matches(tokens[0], "SET") && n > 1 && strncasecmp(tokens[1], "FILE/VERSION_LIMIT=", 19) == 0) {
        /* SET FILE/VERSION_LIMIT=n <spec> */
        ods2_parsed_path_t p;
        char *end;
        long limit = strtol(tokens[1] + 19, &end, 10);
        if (n < 3 || *end != '\0' || limit < 0 || limit > 32767) {
            report_error("%%ODS2-E-NOPARM, usage: SET FILE/VERSION_LIMIT=n <spec>   (n 0 to 32767; 0 is no limit)\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        cmd_set_version_limit(vol, &p, (uint16_t) (limit == 0 ? 32767 : limit));
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && strncasecmp(tokens[1], "FILE/ATTRIBUTES=", 16) == 0) {
        /* SET FILE/ATTRIBUTES=(MRS:0) <spec>: the only attribute change implemented */
        ods2_parsed_path_t p;
        const char *v = tokens[1] + 16;
        char norm[32];
        size_t k = 0;
        for (; *v != '\0' && k + 1 < sizeof(norm); v++) {
            if (*v != '(' && *v != ')') norm[k++] = (char) toupper((unsigned char) *v);
        }
        norm[k] = '\0';
        if (n < 3 || (strcmp(norm, "MRS:0") != 0 && strcmp(norm, "MRS=0") != 0)) {
            report_error("%%ODS2-E-NOPARM, usage: SET FILE/ATTRIBUTES=(MRS:0) <spec>   (the only change implemented: it makes "
                         "stream files that were made with a maximum record size 512 readable by RMS again)\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        cmd_set_record_sizes(vol, &p);
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && (strncasecmp(tokens[1], "FILE/PROTECTION", 15) == 0 || strncasecmp(tokens[1], "FILE/OWNER_UIC", 14) == 0 ||
                                               strncasecmp(tokens[1], "FILE/LOG/PROTECTION", 19) == 0 || strncasecmp(tokens[1], "FILE/LOG/OWNER_UIC", 18) == 0)) {
        /* SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) <spec>, SET FILE/OWNER_UIC=[g,m] <spec>; the parenthesis list may have been split at its spaces */
        char quals_buf[256];
        int k = 2;
        size_t used = 0;
        bool fits = qual_append(quals_buf, sizeof(quals_buf), &used, tokens[1] + 4);
        while (fits && k < n - 1 && strchr(quals_buf, '(') != NULL && strrchr(quals_buf, ')') == NULL) {
            fits = qual_append(quals_buf, sizeof(quals_buf), &used, tokens[k]);
            k++;
        }
        while (fits && k < n - 1 && tokens[k][0] == '/') {          /* "SET FILE/PROTECTION=(...) /LOG spec" */
            fits = qual_append(quals_buf, sizeof(quals_buf), &used, tokens[k]);
            k++;
        }
        if (!fits) {
            report_error("%%ODS2-E-IVQUAL, the qualifiers are longer than %d characters: not run\n", (int) sizeof(quals_buf) - 1);     /* review 48 F2 */
            return true;
        }
        if (k >= n) {
            report_error("%%ODS2-E-NOPARM, usage: SET FILE/PROTECTION=(S:RWED,O:RWED,G:RE,W) <spec>  or  SET FILE/OWNER_UIC=[group,member] <spec>\n");
            return true;
        }
        if (k + 1 < n) {
            /* review 49 N2: a second spec was silently ignored */
            report_error("%%SET-F-TOOMANY, SET FILE takes one file specification, and \"%s\" follows \"%s\": nothing is changed (run SET FILE once for each%s)\n", tokens[k + 1], tokens[k],
                         tokens[k + 1][0] == '/' ? "; qualifiers go before the spec, SET FILE/LOG/PROTECTION=(...) spec" : "");
            return true;
        }
        cmd_set_file_security(vol, quals_buf, tokens[k]);
        return true;
    }
    if (matches(verb, "PURGE")) {
        /* PURGE[/KEEP=n][/LOG] [spec]: the default keep is 1, the default spec every file of the default directory */
        long keep = 1;
        bool log = false;
        const char *qs = quals;
        ods2_parsed_path_t p;
        while (qs != NULL && *qs == '/') {
            char q[64];
            size_t ql = strcspn(qs + 1, "/");
            if (ql >= sizeof(q)) {
                report_error("%%PURGE-F-IVQUAL, a qualifier is longer than %d characters: not run\n", (int) sizeof(q) - 1);      /* review 49 N1: refused, never cut */
                return true;
            }
            memcpy(q, qs + 1, ql);
            q[ql] = '\0';
            if (strncasecmp(q, "KEEP=", 5) == 0) {
                char *end;
                keep = strtol(q + 5, &end, 10);
                if (q[5] == '\0' || *end != '\0' || keep < 1) {
                    report_error("%%PURGE-F-BADVALUE, '%s' is an invalid keyword value\n", q + 5);
                    return true;
                }
            } else if (matches(q, "LOG")) {
                log = true;
            } else if (matches(q, "NOLOG")) {
                log = false;
            } else {
                report_error("%%PURGE-F-IVQUAL, unrecognized qualifier - /%s\n", q);
                return true;
            }
            qs += 1 + strcspn(qs + 1, "/");
        }
        if (n > 2) {
            /* review 49 / N7: a second spec was silently ignored; a user who types two could think both were purged */
            report_error("%%PURGE-F-TOOMANY, PURGE takes one file specification, and \"%s\" follows \"%s\": nothing is purged (run PURGE once for each%s)\n", tokens[2], tokens[1],
                         tokens[2][0] == '/' ? "; qualifiers go on the command word, PURGE/LOG spec" : "");
            return true;
        }
        if (n > 1) {
            int depth = 0;
            const char *c;
            for (c = tokens[1]; *c != '\0'; c++) {
                if (*c == '^' && c[1] != '\0') { c++; continue; }
                if (*c == '[' || *c == '<') depth++;
                else if ((*c == ']' || *c == '>') && depth > 0) depth--;
                else if (*c == ',' && depth == 0) {
                    report_error("%%PURGE-F-TOOMANY, PURGE takes one file specification, not a list (\"%s\"): nothing is purged (run PURGE once for each)\n", tokens[1]);
                    return true;
                }
            }
        }
        if (!ods2_parse_path(n > 1 ? tokens[1] : "*.*", &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", n > 1 ? tokens[1] : "*.*");
            return true;
        }
        if (p.has_version) {
            report_error("%%ODS2-E-BADVER, %s: PURGE takes no version (what VMS does with one was not measured)\n", tokens[1]);
            return true;
        }
        cmd_purge(vol, &p, keep, log);
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
                if (opts.convert) {
                    report_error("%%ODS2-E-CONFQUAL, IMPORT of a directory does not take /CONVERT (or /FORMAT=VAR)\n");
                    return true;
                }
                cmd_import_tree(vol, tokens[1], &p, &opts, &attrs);
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
            if (!cmd_type_list(vol, tokens[1], raw, lines)) cmd_type(vol, &p, raw, lines);
        }
        return true;
    }
    if (matches(tokens[0], "RENAME") || matches(tokens[0], "MV")) {
        ods2_parsed_path_t sp, dp;
        char s1[ODS2_PATH_MAX], s2[ODS2_PATH_MAX], dev1[64], dev2[64];
        bool bad1, bad2, junk1, junk2, device_only = false;
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: RENAME <old-path> <new-path>\n");
            return true;
        }
        if (strlen(tokens[1]) >= ODS2_PATH_MAX || strlen(tokens[2]) >= ODS2_PATH_MAX) {            /* review 69 recheck R1: never cut a specification to fit a buffer */
            report_error("%%ODS2-E-BADPATH, a file specification is too long (%d characters at most)\n", ODS2_PATH_MAX - 1);
            return true;
        }
        rename_split_device(tokens[1], s1, sizeof(s1), dev1, sizeof(dev1), &bad1, &junk1);
        rename_split_device(tokens[2], s2, sizeof(s2), dev2, sizeof(dev2), &bad2, &junk2);
        if ((junk1 && !bad1) || (junk2 && !bad2)) {
            report_error("%%ODS2-E-BADPATH, %s: text between the device and the directory is not part of a file specification\n", junk1 ? tokens[1] : tokens[2]);
            return true;
        }
        if (!rename_parse_path(s1, &sp)) {
            report_error("%%ODS2-E-BADPATH, could not parse the paths\n");
            return true;
        }
        if (s2[0] == '\0' && dev2[0] != '\0') {
            /* RENAME [X]A.TXT MOV: a device and nothing else: the same name in the same directory */
            memset(&dp, 0, sizeof(dp));
            dp.relative = true;                                                  /* no name, no directory: the stored name of the source, in its own directory (R2) */
            device_only = true;
        } else if (!rename_parse_path(s2, &dp)) {
            report_error("%%ODS2-E-BADPATH, could not parse the paths\n");
            return true;
        }
        /* another device: the one volume of this session is the only one a file can go to (VMS: NOTSAMEDEV, CORPUS 65.6, nothing changed). The device parts typed must be the same, or, when only the new
           name has one, name this volume by its label; a device name that does not fit, a node, a quoted or a doubled device is refused (review 69 S1, N1) */
        if (bad1 || bad2 || dev2[0] != '\0') {
            char label[13];
            size_t k, len = 12;
            memcpy(label, vol->home.volname, 12);
            label[12] = '\0';
            while (len > 0 && label[len - 1] == ' ') label[--len] = '\0';
            for (k = 0; dev1[k] != '\0'; k++) dev1[k] = (char) toupper((unsigned char) dev1[k]);
            for (k = 0; dev2[k] != '\0'; k++) dev2[k] = (char) toupper((unsigned char) dev2[k]);
            for (k = 0; label[k] != '\0'; k++) label[k] = (char) toupper((unsigned char) label[k]);
            if (bad1 || bad2 || (strcmp(dev2, dev1) != 0 && !(dev1[0] == '\0' && strcmp(dev2, label) == 0))) {
                report_error("%%RENAME-E-NOTRENAMED, %s not renamed\n-RENAME-E-NOTSAMEDEV, Cannot RENAME to a different device (this volume is %s%s)\n", tokens[1], label[0] != '\0' ? label : "unlabelled", label[0] != '\0' ? ":" : "");
                return true;
            }
        }
        if (refuse_wild_dir(&sp, "RENAME", tokens[1]) || refuse_wild_dir(&dp, "RENAME", tokens[2])) return true;
        cmd_rename(vol, &sp, &dp, tokens[1], tokens[2], device_only);
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
        if (strlen(line) == sizeof(line) - 1 && line[sizeof(line) - 2] != '\n') {
            report_error("%%ODS2-E-LINETOOLONG, a script line is longer than %d characters: not run, and the "
                         "rest of the script is not run\n", MAX_LINE - 1);
            break;
        }
        printf("ODS2> %s", line);
        if (line[strlen(line) - 1] != '\n') printf("\n");
        if (!execute_line(vol, line)) break;
        if (vol->write_failed) {
            /* a change failed part way through its write: the rest of the script is not run */
            report_error("%%ODS2-E-WRITEFAIL, a write to the disk image failed; the rest of the script is not run\n");
            break;
        }
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
    int footer_state = 0;
    bool image_short = false;

    unsigned mount_flags = 0;
    /* a leading option: --ignore-quota writes a volume that has active disk quotas ([000000]QUOTA.SYS exists) although ods2v2 does not keep the usage figures (run DISKQUOTA REBUILD on VMS afterwards) */
    if (argc >= 2 && strcmp(argv[1], "--ignore-quota") == 0) {
        mount_flags |= ODS2_MOUNT_IGNORE_QUOTA;
        argv[1] = argv[0];
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--ignore-quota] <disk-image> [command...]\n", argv[0]);
        fprintf(stderr, "       %s [--ignore-quota] <disk-image> @<script-file>\n", argv[0]);
        fprintf(stderr, "       %s [--ignore-quota] <disk-image>                (interactive)\n", argv[0]);
        fprintf(stderr, "  --ignore-quota: write a volume whose disk quotas are active ([000000]QUOTA.SYS exists); without it such a volume is read only\n");
        return 1;
    }

    /* A structure version 0 volume (e.g. the OpenVMS Alpha V8.4 CD)
       is mounted for reading only, so look before opening for write. */
    g_image_path = argv[1];
    r = ods2_mount(argv[1], &vol);
    if (r.ok) {
        char scb_msg[512];
        g_ods5 = vol.ods5;
        if (ods2_scb_problem(&vol, scb_msg, sizeof(scb_msg))) fprintf(stderr, "%%ODS2-W-CHKSCB, %s\n", scb_msg);
        footer_state = ods2_image_footer(&vol, scb_msg, sizeof(scb_msg));
        if (footer_state == 1) fprintf(stderr, "%%ODS2-I-SIMHFOOTER, %s\n", scb_msg);
        else if (footer_state == 2) fprintf(stderr, "%%ODS2-W-SIMHFOOTER, %s\n", scb_msg);
        image_short = ods2_image_short(&vol, scb_msg, sizeof(scb_msg));
        if (image_short) fprintf(stderr, "%%ODS2-W-IMAGESHORT, %s\n", scb_msg);
        if (ods2_indexf_ext_looks_ods2v2(&vol)) {
            fprintf(stderr, "%%ODS2-W-INDEXEXT, INDEXF.SYS has an extension header that looks like one ods2v2 made (map at word "
                            "100 on ODS-2, or level 5 behind a level 2.1 primary): V8.4 crashes (BUGCHECK UNXSIGNAL) on the "
                            "first file creation on this volume (CORPUS 37/38): mount it on VMS only with /NOWRITE. This may be "
                            "wrong: it is a guess at what VMS does not write\n");
        }
        if (ods2_indexf_growth_unfinished(&vol)) {
            fprintf(stderr, "%%ODS2-W-INDEXGROWTH, INDEXF.SYS's last growth was interrupted (its map holds more blocks than its "
                            "header says, or a header number is marked past it); nothing is lost, and the first change made "
                            "through a write mount repairs it\n");
        }
    }
    {
        const char *names = getenv("ODS2_NAMES");
        if (names != NULL && matches(names, "UTF8")) g_utf8_names = true;
    }
    if (r.ok && (footer_state == 2 || image_short)) {
        /* already told: read-only */
    } else if (r.ok && vol.volsize == 0) {
        /* without the volume size nothing bounds a write to the volume (upstream issue #23) */
        fprintf(stderr, "%%ODS2-W-READONLY, %s: the storage control block's volume size is 0 or unreadable; "
                        "mounted read-only\n", argv[1]);
    } else if (r.ok && !vol.indexf_chain_ok) {
        fprintf(stderr, "%%ODS2-W-READONLY, %s: INDEXF.SYS's extension header chain cannot be followed to its end; "
                        "mounted read-only\n", argv[1]);
    } else if (r.ok && vol.version0) {
        fprintf(stderr, "%%ODS2-W-STRUCLEV0, %s: HM2$W_STRUCLEV version is 0; "
                        "mounted read-only\n", argv[1]);
    } else if (r.ok && access(argv[1], W_OK) != 0) {
        /* an image the user cannot write (chmod 444): read it, and
           refuse every write, instead of failing to mount */
        fprintf(stderr, "%%ODS2-W-READONLY, %s: image is not writable; mounted "
                        "read-only\n", argv[1]);
    } else {
        if (r.ok) ods2_dismount(&vol);
        r = ods2_mount_write_opts(argv[1], &vol, mount_flags);
        if (!r.ok && (strstr(r.problem, ODS2_QUOTA_ACTIVE_TEXT) != NULL || strstr(r.problem, ODS2_QUOTA_UNKNOWN_TEXT) != NULL)) {
            /* active disk quotas (CORPUS 71/72): read the volume, refuse every write with the reason */
            char why[640];
            ods2_result_t r_read;
            snprintf(why, sizeof(why), "%s", r.problem);
            r_read = ods2_mount(argv[1], &vol);
            if (r_read.ok) {
                snprintf(vol.write_refusal, sizeof(vol.write_refusal), "%s", why);
                fprintf(stderr, "%%ODS2-W-READONLY, %s: %s; mounted read-only\n", argv[1], why);
                r = r_read;
            }
        } else if (!r.ok && strstr(r.problem, "locked") != NULL) {
            /* another process has the image open (one writer or many readers): read it, refuse every write */
            ods2_result_t r_read = ods2_mount(argv[1], &vol);
            if (r_read.ok) {
                fprintf(stderr, "%%ODS2-W-READONLY, %s: the image is locked by another process; mounted "
                                "read-only\n", argv[1]);
                r = r_read;
            }
        }
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
        /* commands fed on standard input (not a terminal) carry their status as a script does: 1 if any command failed (review 74 S1) */
        if (!isatty(STDIN_FILENO) && g_command_failed) exit_code = 1;
    } else if (argv[2][0] == '@') {
        exit_code = run_script(&vol, argv[2] + 1);
    } else {
        /* Single-shot: join all remaining argv into one command line. */
        char line[MAX_LINE];
        int i;
        size_t total = 0;
        line[0] = '\0';
        for (i = 2; i < argc; i++) total += strlen(argv[i]) + 1;
        if (total >= sizeof(line)) {
            report_error("%%ODS2-E-LINETOOLONG, the command is longer than %d characters: not run\n", MAX_LINE - 1);
        } else {
            for (i = 2; i < argc; i++) {
                if (i > 2) strcat(line, " ");
                strcat(line, argv[i]);
            }
            execute_line(&vol, line);
        }
        exit_code = g_command_failed ? 1 : 0;
    }

    {
        /* a write that did not reach the disk is a command that failed */
        ods2_result_t synced = ods2_sync(&vol);
        if (!synced.ok) {
            report_error("%%ODS2-E-SYNCERR, %s: %s\n", argv[1], synced.problem);
            exit_code = 1;
        }
    }
    ods2_dismount(&vol);
    return exit_code;
}
