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
#include <stdlib.h>
#include <string.h>

bool ods2_wildcard_match(const char *pattern, const char *name)
{
    /* One matcher for ODS-2 and ODS-5 names: '*', '%' and '?' as wildcards,
       ^-escapes literal, case-blind, a character being a whole UTF-8
       character (ods5_names.h). */
    return ods5_wildcard_match(pattern, name);
}

/* --- Directory specifications (see ods2_wildcard.h) --- */

/* A name is at most this many characters as written (with its ^ escapes). The number of names in a
   specification or path is not limited: the tokens are allocated, so that a tree as deep as a volume can hold
   (VMS V7.3-2 made 400 levels; a walk must not lose the part below some fixed depth) is matched like any other. */
#define MAX_DIR_NAME   768

typedef struct {
    bool ellipsis;              /* "..." rather than a name */
    const char *start;          /* where it begins in the source string */
    size_t len;                 /* the length of the name as written (not for "...") */
} dir_token_t;

static bool starts_ellipsis(const char *s)
{
    return s[0] == '.' && s[1] == '.' && s[2] == '.';
}

/* Splits a spec (allow_ellipsis) or a concrete path (!allow_ellipsis) into tokens, allocated in *toks (free()
   it). Returns the count, or -1 if malformed (or out of memory); *toks is then NULL. */
static int tokenize_dir(const char *s, dir_token_t **toks_out, bool allow_ellipsis)
{
    int n = 0;
    size_t i = 0;
    dir_token_t *toks = malloc((strlen(s) / 2 + 2) * sizeof(*toks));

    *toks_out = NULL;
    if (toks == NULL) return -1;
    while (s[i] != '\0') {
        if (allow_ellipsis && starts_ellipsis(s + i)) {
            toks[n].ellipsis = true;
            toks[n].start = s + i;
            toks[n].len = 0;
            n++;
            i += 3;
            /* After an ellipsis: the end, a name straight away
               ("A...B"), or one separating dot then a name
               ("A....B" - VMS accepts both). */
            if (s[i] == '.') {
                if (s[i + 1] == '\0' || s[i + 1] == '.') goto bad;
                i++;
            }
            continue;
        }
        if (s[i] == '.') goto bad; /* empty name */
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
            if (len >= MAX_DIR_NAME) goto bad;
            toks[n].len = len;
            n++;
            i += len;
        }
        if (s[i] == '.') {
            if (allow_ellipsis && starts_ellipsis(s + i)) continue;
            i++; /* separator */
            if (s[i] == '\0') goto bad; /* trailing dot */
        }
    }
    *toks_out = toks;
    return n;
bad:
    free(toks);
    return -1;
}

/* The name of a token as a NUL-terminated string in out (MAX_DIR_NAME bytes). A concrete path's names are literal:
   the characters their spellings stand for, to be matched against a spec's pattern. */
static void token_text(const dir_token_t *t, bool literal, char *out)
{
    memcpy(out, t->start, t->len);
    out[t->len] = '\0';
    if (literal) {
        char lit[MAX_DIR_NAME];
        size_t k = ods5_unescape(out, t->len, lit, sizeof(lit));
        if (k != (size_t) -1) memcpy(out, lit, k + 1);
    }
}

static bool token_match(const dir_token_t *p, const dir_token_t *c)
{
    char pat[MAX_DIR_NAME], name[MAX_DIR_NAME];
    token_text(p, false, pat);
    token_text(c, true, name);
    return ods2_wildcard_match(pat, name);
}

static bool match_tokens(const dir_token_t *p, int np, const dir_token_t *c, int nc)
{
    if (np == 0) return nc == 0;
    if (p[0].ellipsis) {
        /* Zero more levels, or swallow one and try again. */
        return match_tokens(p + 1, np - 1, c, nc) ||
               (nc > 0 && match_tokens(p, np, c + 1, nc - 1));
    }
    return nc > 0 && token_match(&p[0], &c[0]) &&
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
    return token_match(&p[0], &c[0]) &&
           descend_tokens(p + 1, np - 1, c + 1, nc - 1);
}

bool ods2_dir_spec_valid(const char *spec)
{
    dir_token_t *toks;
    int n = tokenize_dir(spec, &toks, true);
    free(toks);
    return n >= 0;
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
    dir_token_t *p, *c;
    int np = tokenize_dir(spec, &p, true);
    int nc = tokenize_dir(path, &c, false);
    bool r = np >= 0 && nc >= 0 && match_tokens(p, np, c, nc);
    free(p);
    free(c);
    return r;
}

bool ods2_dir_spec_could_descend(const char *spec, const char *path)
{
    dir_token_t *p, *c;
    int np = tokenize_dir(spec, &p, true);
    int nc = tokenize_dir(path, &c, false);
    bool r = np >= 0 && nc >= 0 && descend_tokens(p, np, c, nc);
    free(p);
    free(c);
    return r;
}

const char *ods2_dir_spec_split_prefix(const char *spec, char *prefix_out,
                                       size_t prefix_out_size)
{
    dir_token_t *toks;
    int n = tokenize_dir(spec, &toks, true);
    int k = 0;
    size_t prefix_len = 0;
    const char *rest;

    if (n < 0 || prefix_out_size == 0) return NULL;
    while (k < n && !toks[k].ellipsis) {
        char name[MAX_DIR_NAME];
        token_text(&toks[k], false, name);
        if (ods2_dir_spec_has_wildcard(name)) break;
        prefix_len = (size_t) (toks[k].start - spec) + toks[k].len;
        k++;
    }
    rest = (k < n) ? toks[k].start : spec + strlen(spec);
    free(toks);
    if (prefix_len >= prefix_out_size) return NULL;
    memcpy(prefix_out, spec, prefix_len);
    prefix_out[prefix_len] = '\0';
    return rest;
}
