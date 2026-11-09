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

#include "ods2_wildcard.h"
#include "ods5_names.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

bool ods2_wildcard_match(const char *pattern, const char *name)
{
    /* One matcher for ODS-2 and ODS-5 names: '*', '%' and '?' as wildcards,
       ^-escapes literal, case-blind, a character being a whole UTF-8
       character (ods5_names.h). */
    return ods5_wildcard_match(pattern, name);
}

/* --- Directory specifications (see ods2_wildcard.h) --- */

/* Deeper than any real volume: VMS allows 8 directory levels, and
   ellipses only shorten what has to be written down. */
#define MAX_DIR_TOKENS 256
#define MAX_DIR_NAME   768

typedef struct {
    bool ellipsis;              /* "..." rather than a name */
    const char *start;          /* where it begins in the source string */
    char name[MAX_DIR_NAME];    /* the name, nul-terminated (not for "...") */
} dir_token_t;

static bool starts_ellipsis(const char *s)
{
    return s[0] == '.' && s[1] == '.' && s[2] == '.';
}

/* Splits a spec (allow_ellipsis) or a concrete path (!allow_ellipsis)
   into tokens. Returns the count, or -1 if malformed or too long. */
static int tokenize_dir(const char *s, dir_token_t *toks, bool allow_ellipsis)
{
    int n = 0;
    size_t i = 0;

    while (s[i] != '\0') {
        if (n == MAX_DIR_TOKENS) return -1;
        if (allow_ellipsis && starts_ellipsis(s + i)) {
            toks[n].ellipsis = true;
            toks[n].start = s + i;
            toks[n].name[0] = '\0';
            n++;
            i += 3;
            /* After an ellipsis: the end, a name straight away
               ("A...B"), or one separating dot then a name
               ("A....B" - VMS accepts both). */
            if (s[i] == '.') {
                if (s[i + 1] == '\0' || s[i + 1] == '.') return -1;
                i++;
            }
            continue;
        }
        if (s[i] == '.') return -1; /* empty name */
        {
            size_t len = 0;
            toks[n].ellipsis = false;
            toks[n].start = s + i;
            /* a '.' ends the name unless escaped (^.); the character after
               a ^ is part of the name whatever it is */
            while (s[i + len] != '\0' && s[i + len] != '.') {
                if (s[i + len] == '^' && s[i + len + 1] != '\0') len++;
                len++;
            }
            if (len >= MAX_DIR_NAME) return -1;
            memcpy(toks[n].name, s + i, len);
            toks[n].name[len] = '\0';
            /* A concrete path's names are literal: the characters their
               spellings stand for, to be matched against a spec's pattern. */
            if (!allow_ellipsis) {
                char literal[MAX_DIR_NAME];
                size_t k = ods5_unescape(toks[n].name, len, literal, sizeof(literal));
                if (k != (size_t) -1) memcpy(toks[n].name, literal, k + 1);
            }
            n++;
            i += len;
        }
        if (s[i] == '.') {
            if (allow_ellipsis && starts_ellipsis(s + i)) continue;
            i++; /* separator */
            if (s[i] == '\0') return -1; /* trailing dot */
        }
    }
    return n;
}

static bool match_tokens(const dir_token_t *p, int np, const dir_token_t *c, int nc)
{
    if (np == 0) return nc == 0;
    if (p[0].ellipsis) {
        /* Zero more levels, or swallow one and try again. */
        return match_tokens(p + 1, np - 1, c, nc) ||
               (nc > 0 && match_tokens(p, np, c + 1, nc - 1));
    }
    return nc > 0 && ods2_wildcard_match(p[0].name, c[0].name) &&
           match_tokens(p + 1, np - 1, c + 1, nc - 1);
}

/* Could c plus one or more further names match p? */
static bool descend_tokens(const dir_token_t *p, int np, const dir_token_t *c, int nc)
{
    if (nc == 0) return np > 0; /* any remaining name or ellipsis can take more levels */
    if (np == 0) return false;
    if (p[0].ellipsis) {
        return descend_tokens(p + 1, np - 1, c, nc) ||
               descend_tokens(p, np, c + 1, nc - 1);
    }
    return ods2_wildcard_match(p[0].name, c[0].name) &&
           descend_tokens(p + 1, np - 1, c + 1, nc - 1);
}

bool ods2_dir_spec_valid(const char *spec)
{
    dir_token_t toks[MAX_DIR_TOKENS];
    return tokenize_dir(spec, toks, true) >= 0;
}

/* '*', '%' or '?' that is not escaped by a ^. */
static bool name_has_wildcard(const char *s)
{
    for (; *s; s++) {
        if (*s == '^' && s[1] != '\0') { s++; continue; }
        if (*s == '*' || *s == '%' || *s == '?') return true;
    }
    return false;
}

bool ods2_dir_spec_has_wildcard(const char *spec)
{
    /* an ellipsis is three dots; ^. is a literal one, and the
       character after a ^ never starts one */
    const char *s;
    for (s = spec; *s; s++) {
        if (*s == '^' && s[1] != '\0') { s++; continue; }
        if (s[0] == '.' && s[1] == '.' && s[2] == '.') return true;
    }
    return name_has_wildcard(spec);
}

bool ods2_dir_spec_match(const char *spec, const char *path)
{
    dir_token_t p[MAX_DIR_TOKENS], c[MAX_DIR_TOKENS];
    int np = tokenize_dir(spec, p, true);
    int nc = tokenize_dir(path, c, false);
    if (np < 0 || nc < 0) return false;
    return match_tokens(p, np, c, nc);
}

bool ods2_dir_spec_could_descend(const char *spec, const char *path)
{
    dir_token_t p[MAX_DIR_TOKENS], c[MAX_DIR_TOKENS];
    int np = tokenize_dir(spec, p, true);
    int nc = tokenize_dir(path, c, false);
    if (np < 0 || nc < 0) return false;
    return descend_tokens(p, np, c, nc);
}

const char *ods2_dir_spec_split_prefix(const char *spec, char *prefix_out,
                                       size_t prefix_out_size)
{
    dir_token_t toks[MAX_DIR_TOKENS];
    int n = tokenize_dir(spec, toks, true);
    int k = 0;
    size_t prefix_len = 0;

    if (n < 0 || prefix_out_size == 0) return NULL;
    while (k < n && !toks[k].ellipsis && !ods2_dir_spec_has_wildcard(toks[k].name)) {
        prefix_len = (size_t) (toks[k].start - spec) + strlen(toks[k].name);
        k++;
    }
    if (prefix_len >= prefix_out_size) return NULL;
    memcpy(prefix_out, spec, prefix_len);
    prefix_out[prefix_len] = '\0';
    return (k < n) ? toks[k].start : spec + strlen(spec);
}
