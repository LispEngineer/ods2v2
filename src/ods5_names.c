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

#include "ods5_names.h"
#include "ods5_ondisk.h"
#include <string.h>
#include <stdio.h>

/* --- UTF-8 --- */

size_t ods5_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *) s;
    uint32_t c = u[0];
    size_t need, i;

    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; c &= 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; c &= 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; c &= 0x07; }
    else { *cp = u[0]; return 1; }          /* not UTF-8: ISO Latin-1 */
    if (n < need + 1) { *cp = u[0]; return 1; }
    for (i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = u[0]; return 1; }
        c = (c << 6) | (u[i] & 0x3F);
    }
    /* overlong forms and out-of-range values are not UTF-8 either
       (surrogate units are kept: ods5_utf8_encode() writes them) */
    if ((need == 2 && c < 0x800) || (need == 3 && (c < 0x10000 || c > 0x10FFFF))) {
        *cp = u[0];
        return 1;
    }
    *cp = c;
    return need + 1;
}

size_t ods5_utf8_encode(uint32_t cp, char *out)
{
    if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

size_t ods5_utf8_length(const char *s)
{
    size_t chars = 0, left = strlen(s);
    while (left > 0) {
        uint32_t cp;
        size_t used = ods5_utf8_decode(s, left, &cp);
        s += used;
        left -= used;
        chars++;
    }
    return chars;
}

size_t ods5_isl1_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t i, len = 0;
    for (i = 0; i < n; i++) {
        if (len + 3 > cap) return (size_t) -1; /* 2 bytes + nul */
        len += ods5_utf8_encode(in[i], out + len);
    }
    if (len + 1 > cap) return (size_t) -1;
    out[len] = '\0';
    return len;
}

size_t ods5_ucs2_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t i, len = 0;
    if (n % 2 != 0) return (size_t) -1;
    for (i = 0; i < n; i += 2) {
        if (len + 4 > cap) return (size_t) -1; /* 3 bytes + nul */
        len += ods5_utf8_encode((uint32_t) in[i] | ((uint32_t) in[i + 1] << 8),
                                out + len);
    }
    if (len + 1 > cap) return (size_t) -1;
    out[len] = '\0';
    return len;
}

bool ods5_utf8_to_stored(const char *in, uint8_t *out, size_t cap,
                         size_t *out_len, bool *is_ucs2)
{
    size_t left = strlen(in), n = 0;
    const char *p = in;
    bool wide = false;

    while (left > 0) {
        uint32_t cp;
        size_t used = ods5_utf8_decode(p, left, &cp);
        if (cp > 0xFFFF) return false;
        if (cp > 0xFF) wide = true;
        p += used;
        left -= used;
    }
    p = in;
    left = strlen(in);
    while (left > 0) {
        uint32_t cp;
        size_t used = ods5_utf8_decode(p, left, &cp);
        if (wide) {
            if (n + 2 > cap) return false;
            out[n++] = (uint8_t) (cp & 0xFF);
            out[n++] = (uint8_t) (cp >> 8);
        } else {
            if (n + 1 > cap) return false;
            out[n++] = (uint8_t) cp;
        }
        p += used;
        left -= used;
    }
    *out_len = n;
    *is_ucs2 = wide;
    return true;
}

/* --- Names for writing --- */

bool ods5_complete_name(const char *utf8, char *out, size_t cap)
{
    size_t n = strlen(utf8);
    bool dot = strchr(utf8, '.') != NULL;
    if (n + (dot ? 0 : 1) + 1 > cap) return false;
    memcpy(out, utf8, n);
    if (!dot) out[n++] = '.';
    out[n] = '\0';
    return true;
}

/* Is the name (dot completed) legal on an ODS-2 volume, case aside? */
static bool name_is_ods2_legal(const char *s)
{
    size_t name_len = 0, type_len = 0;
    bool seen_dot = false;
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '.') {
            if (seen_dot) return false;
            seen_dot = true;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '$' || c == '_' || c == '-') {
            if (seen_dot) type_len++; else name_len++;
        } else {
            return false;
        }
    }
    return name_len <= 39 && type_len <= 39;
}

bool ods5_stored_name(const char *utf8, uint8_t *out, size_t cap, size_t *len, unsigned *type)
{
    char full[ODS5_MAX_LITERAL_NAME + 2];
    bool ucs2;
    if (!ods5_complete_name(utf8, full, sizeof(full))) return false;
    if (!ods5_utf8_to_stored(full, out, cap, len, &ucs2)) return false;
    if (ucs2) *type = ODS5_NAME_UCS2;
    else *type = name_is_ods2_legal(full) ? ODS5_NAME_ODS2 : ODS5_NAME_ISL1;
    return true;
}

const char *ods5_check_new_name(const char *utf8)
{
    uint8_t stored[2 * ODS5_MAX_LITERAL_NAME + 4];
    size_t len, left;
    unsigned type;
    const char *p;

    if (utf8[0] == '\0') return "empty file name";
    if (strlen(utf8) > ODS5_MAX_LITERAL_NAME) return "file name too long";
    p = utf8;
    left = strlen(utf8);
    while (left > 0) {
        uint32_t cp;
        size_t used = ods5_utf8_decode(p, left, &cp);
        if (cp < 0x20) return "a file name cannot contain control characters";
        if (cp == '#' || cp == '{' || cp == '}' || cp == '`' || cp == '?' || cp == '*' || cp == '"') {
            return "VMS refuses the characters # { } ` ? * and \" in file names";
        }
        if (cp > 0xFFFF) return "a file name can only hold characters up to U+FFFF (UCS-2)";
        p += used;
        left -= used;
    }
    if (!ods5_stored_name(utf8, stored, sizeof(stored), &len, &type)) return "file name too long";
    if (len > ODS5_DIR_MAX_NAME) {
        return "a file name and type are limited to 236 bytes (118 characters if any is above U+00FF)";
    }
    return NULL;
}

/* --- Spellings --- */

/* Printable ASCII that is written with a leading ^ (B.2.2.6). '.' is
   handled apart: only the ones that are not the type delimiter. */
static bool needs_caret(uint32_t cp)
{
    return cp != 0 && strchr("!#&'`(){}+@,;[]%^=:<>\\|/\"*?", (int) cp) != NULL;
}

size_t ods5_escape_name(const char *utf8, unsigned flags, char *out, size_t cap)
{
    const char *last_dot = NULL;
    size_t left = strlen(utf8), len = 0;
    const char *p = utf8;

    if (flags & ODS5_ESC_KEEP_LAST_DOT) {
        const char *q;
        for (q = utf8; *q; q++) if (*q == '.') last_dot = q;
    }
    while (left > 0) {
        uint32_t cp;
        char piece[8];
        size_t used = ods5_utf8_decode(p, left, &cp), plen;
        if (cp == '.') {
            if (p == last_dot) { piece[0] = '.'; plen = 1; }
            else { piece[0] = '^'; piece[1] = '.'; plen = 2; }
        } else if (cp > 0xFF) {
            plen = (size_t) snprintf(piece, sizeof(piece), "^U%04X", (unsigned) (cp & 0xFFFF));
        } else if (cp == ' ') {
            piece[0] = '^'; piece[1] = '_'; plen = 2;
        } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0xA0) || cp == 0xFF) {
            plen = (size_t) snprintf(piece, sizeof(piece), "^%02X", (unsigned) cp);
        } else if (needs_caret(cp)) {
            piece[0] = '^'; piece[1] = (char) cp; plen = 2;
        } else {
            plen = ods5_utf8_encode(cp, piece);
        }
        if (len + plen + 1 > cap) return (size_t) -1;
        memcpy(out + len, piece, plen);
        len += plen;
        p += used;
        left -= used;
    }
    if (len + 1 > cap) return (size_t) -1;
    out[len] = '\0';
    return len;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* One escape sequence at s (s[0] == '^', n bytes available): its code
   point in *cp and its length, or 0 if malformed or reserved. */
static size_t decode_escape(const char *s, size_t n, uint32_t *cp)
{
    int h1, h2, h3, h4;

    if (n < 2) return 0;
    if (s[1] == '_' || s[1] == ' ') { *cp = ' '; return 2; }
    if (s[1] == 'U') {
        if (n < 6) return 0;
        h1 = hex_value(s[2]); h2 = hex_value(s[3]);
        h3 = hex_value(s[4]); h4 = hex_value(s[5]);
        if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return 0;
        *cp = (uint32_t) ((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
        return *cp == 0 ? 0 : 6;
    }
    h1 = hex_value(s[1]);
    if (h1 >= 0) {
        if (n < 3) return 0;
        h2 = hex_value(s[2]);
        if (h2 < 0) return 0;
        *cp = (uint32_t) ((h1 << 4) | h2);
        return *cp == 0 ? 0 : 3;
    }
    /* ^ before any other printable ASCII punctuation: that character,
       as a literal. Before a letter or a control: reserved. */
    if ((unsigned char) s[1] > 0x20 && (unsigned char) s[1] < 0x7F &&
        !((s[1] >= 'A' && s[1] <= 'Z') || (s[1] >= 'a' && s[1] <= 'z') ||
          (s[1] >= '0' && s[1] <= '9'))) {
        *cp = (unsigned char) s[1];
        return 2;
    }
    return 0;
}

size_t ods5_unescape(const char *spelling, size_t n, char *out, size_t cap)
{
    size_t i = 0, len = 0;

    while (i < n) {
        uint32_t cp;
        char piece[4];
        size_t used, plen;
        if (spelling[i] == '^') {
            used = decode_escape(spelling + i, n - i, &cp);
            if (used == 0) return (size_t) -1;
            plen = ods5_utf8_encode(cp, piece);
        } else {
            used = ods5_utf8_decode(spelling + i, n - i, &cp);
            plen = ods5_utf8_encode(cp, piece);
            if (cp == 0) return (size_t) -1;
        }
        if (len + plen + 1 > cap) return (size_t) -1;
        memcpy(out + len, piece, plen);
        len += plen;
        i += used;
    }
    if (len + 1 > cap) return (size_t) -1;
    out[len] = '\0';
    return len;
}

const char *ods5_find_unescaped(const char *s, char ch)
{
    while (*s) {
        if (*s == '^' && s[1] != '\0') { s += 2; continue; }
        if (*s == ch) return s;
        s++;
    }
    return NULL;
}

const char *ods5_rfind_unescaped(const char *s, char ch)
{
    const char *last = NULL;
    while (*s) {
        if (*s == '^' && s[1] != '\0') { s += 2; continue; }
        if (*s == ch) last = s;
        s++;
    }
    return last;
}

/* --- Comparison --- */

#include "ods5_fold_table.h"

uint32_t ods5_fold(uint32_t cp)
{
    size_t lo = 0, hi = sizeof(ods5_fold_ranges) / sizeof(ods5_fold_ranges[0]);

    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
    if (cp > 0xFFFF) return cp;
    while (lo < hi) {                      /* binary search of the range table */
        size_t mid = (lo + hi) / 2;
        if (cp < ods5_fold_ranges[mid].lo) hi = mid;
        else if (cp > ods5_fold_ranges[mid].hi) lo = mid + 1;
        else {
            if ((cp - ods5_fold_ranges[mid].lo) % ods5_fold_ranges[mid].stride != 0) return cp;
            return (uint32_t) ((int32_t) cp + ods5_fold_ranges[mid].delta);
        }
    }
    return cp;
}

typedef enum { TOK_END, TOK_STAR, TOK_ONE, TOK_CHAR, TOK_BAD } tok_kind_t;

/* Next pattern element at *p. */
static tok_kind_t pattern_next(const char **p, uint32_t *cp)
{
    const char *s = *p;
    size_t left = strlen(s);

    if (*s == '\0') return TOK_END;
    if (*s == '*') { *p = s + 1; return TOK_STAR; }
    if (*s == '%' || *s == '?') { *p = s + 1; return TOK_ONE; }
    if (*s == '^') {
        size_t used = decode_escape(s, left, cp);
        if (used == 0) return TOK_BAD;
        *p = s + used;
        return TOK_CHAR;
    }
    *p = s + ods5_utf8_decode(s, left, cp);
    return TOK_CHAR;
}

bool ods5_wildcard_match(const char *pattern, const char *name)
{
    const char *p = pattern, *n = name;
    const char *star_p = NULL, *star_n = NULL;

    for (;;) {
        const char *pp = p;
        uint32_t pc = 0, nc;
        tok_kind_t k = pattern_next(&pp, &pc);

        if (k == TOK_BAD) return false;
        if (k == TOK_STAR) {
            star_p = pp;
            star_n = n;
            p = pp;
            continue;
        }
        if (k == TOK_END) {
            if (*n == '\0') return true;
        } else if (*n != '\0') {
            size_t used = ods5_utf8_decode(n, strlen(n), &nc);
            if (k == TOK_ONE || ods5_fold(pc) == ods5_fold(nc)) {
                p = pp;
                n += used;
                continue;
            }
        }
        /* mismatch: let the last '*' take one more character */
        if (star_p == NULL || *star_n == '\0') return false;
        {
            uint32_t skip;
            star_n += ods5_utf8_decode(star_n, strlen(star_n), &skip);
        }
        p = star_p;
        n = star_n;
    }
}
