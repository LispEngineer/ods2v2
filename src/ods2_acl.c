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

/*
 * ods2_acl.c - see ods2_acl.h.
 */
#include "ods2_acl.h"
#include "ods2_ondisk.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ACE types and flag bits, as measured on VMS V8.4-2L2 (ODS5_FORMAT_NOTES 15.8) */
#define ACE_TYPE_ID 1u         /* identifier ACE */
#define ACE_TYPE_DEFPROT 9u    /* DEFAULT_PROTECTION */
#define ACE_TYPE_AUDIT 5u      /* AUDIT=class */
#define ACE_TYPE_ALARM 6u      /* ALARM=class */
#define ACE_TYPE_CREATOR 13u   /* CREATOR */
#define ACE_FLAG_DEFAULT 0x0100u
#define ACE_FLAG_PROTECTED 0x0200u
#define ACE_FLAG_NOPROPAGATE 0x0800u
#define ACE_FLAG_SUCCESS 0x0001u   /* audit and alarm ACEs */
#define ACE_FLAG_FAILURE 0x0002u

/* Appends to buf (capacity cap) at offset n and returns the new offset, never beyond cap - 1: the text is
 * cut when it does not fit (review 4: an ACE of 18 or more identifiers overran a fixed buffer). */
static size_t appendf(char *buf, size_t cap, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (cap == 0) return 0;
    if (n >= cap) n = cap - 1;
    va_start(ap, fmt);
    r = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (r < 0) return n;
    n += (size_t) r;
    return n >= cap ? cap - 1 : n;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

void ods2_acl_format_id(uint32_t id, char *out, size_t cap)
{
    static const char *const system_ids[] = { NULL, "BATCH", "DIALUP", "INTERACTIVE", "LOCAL", "NETWORK", "REMOTE" };
    if ((id & 0xFFFFFFF8u) == 0x80000000u && (id & 7u) >= 1 && (id & 7u) <= 6) {
        snprintf(out, cap, "%s", system_ids[id & 7u]);   /* measured: BATCH 1 DIALUP 2 INTERACTIVE 3 LOCAL 4 NETWORK 5 REMOTE 6 */
    } else if ((id & 0xC0000000u) == 0) {
        unsigned group = id >> 16, member = id & 0xFFFFu;
        char g[16], m[16];
        /* wildcards (measured): group 0x3FFF, member 0xFFFF */
        if (group == 0x3FFFu) snprintf(g, sizeof(g), "*"); else snprintf(g, sizeof(g), "%o", group);
        if (member == 0xFFFFu) snprintf(m, sizeof(m), "*"); else snprintf(m, sizeof(m), "%o", member);
        snprintf(out, cap, "[%s,%s]", g, m);
    } else {
        snprintf(out, cap, "%%X%08X", (unsigned) id);
    }
}

static void format_access(uint32_t a, unsigned flags, int with_sf, char *out, size_t cap)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { 1, "READ" }, { 2, "WRITE" }, { 4, "EXECUTE" }, { 8, "DELETE" }, { 16, "CONTROL" } };
    size_t n = 0, i;
    out[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (a & names[i].bit) n = appendf(out, cap, n, "%s%s", n ? "+" : "", names[i].name);
    }
    if (a & ~31u) n = appendf(out, cap, n, "%s%%X%X", n ? "+" : "", (unsigned) (a & ~31u));
    if (with_sf) {
        if (flags & ACE_FLAG_SUCCESS) n = appendf(out, cap, n, "%sSUCCESS", n ? "+" : "");
        if (flags & ACE_FLAG_FAILURE) n = appendf(out, cap, n, "%sFAILURE", n ? "+" : "");
    }
    if (n == 0) snprintf(out, cap, "NONE");
}

/* a protection mask (a set bit denies): R W E D, then C */
static void format_prot(uint32_t denied, char *out, size_t cap)
{
    static const char letters[] = "RWEDC";
    size_t n = 0, i;
    for (i = 0; i < 5 && n + 1 < cap; i++) if (!(denied & (1u << i))) out[n++] = letters[i];
    out[n] = '\0';
}

size_t ods2_acl_walk(const uint8_t *header, void (*visit)(void *, const char *), void *ctx)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    size_t o = (size_t) core->acoffset * 2, end = (size_t) core->rsoffset * 2, count = 0;

    if (core->acoffset == 0 || core->acoffset == 255 || end > 512 || o >= end) return 0;
    while (o + 4 <= end && header[o] != 0) {
        unsigned size = header[o], type = header[o + 1];
        unsigned flags = (unsigned) header[o + 2] | (unsigned) header[o + 3] << 8;
        char text[1280];
        bool done = false;
        if (o + size > end || size < 4) {
            snprintf(text, sizeof(text), "(damaged ACE at offset %zu)", o);
            visit(ctx, text);
            return count + 1;
        }
        if (type == ACE_TYPE_ID && size >= 12 && (size - 8) % 4 == 0 &&
            (flags & ~(ACE_FLAG_DEFAULT | ACE_FLAG_PROTECTED | ACE_FLAG_NOPROPAGATE)) == 0) {
            char ids[1024] = "", acc[128], one[32], opts[64] = "";   /* 61 identifiers of up to 15 characters fit */
            size_t k, n = 0;
            for (k = 8; k < size; k += 4) {
                ods2_acl_format_id(rd32(header + o + k), one, sizeof(one));
                n = appendf(ids, sizeof(ids), n, "%s%s", k > 8 ? "+" : "", one);
            }
            if (flags & (ACE_FLAG_DEFAULT | ACE_FLAG_PROTECTED | ACE_FLAG_NOPROPAGATE)) {
                snprintf(opts, sizeof(opts), ",OPTIONS=%s%s%s%s%s",
                         (flags & ACE_FLAG_DEFAULT) ? "DEFAULT" : "",
                         ((flags & ACE_FLAG_DEFAULT) && (flags & (ACE_FLAG_PROTECTED | ACE_FLAG_NOPROPAGATE))) ? "+" : "",
                         (flags & ACE_FLAG_PROTECTED) ? "PROTECTED" : "",
                         ((flags & ACE_FLAG_PROTECTED) && (flags & ACE_FLAG_NOPROPAGATE)) ? "+" : "",
                         (flags & ACE_FLAG_NOPROPAGATE) ? "NOPROPAGATE" : "");
            }
            format_access(rd32(header + o + 4), 0, 0, acc, sizeof(acc));
            snprintf(text, sizeof(text), "(IDENTIFIER=%s%s,ACCESS=%s)", ids, opts, acc);
            done = true;
        } else if (type == ACE_TYPE_CREATOR && size == 8 && flags == 0) {
            char acc[128];
            format_access(rd32(header + o + 4), 0, 0, acc, sizeof(acc));
            snprintf(text, sizeof(text), "(CREATOR,ACCESS=%s)", acc);
            done = true;
        } else if ((type == ACE_TYPE_AUDIT || type == ACE_TYPE_ALARM) && size >= 8 &&
                   (flags & ~(ACE_FLAG_SUCCESS | ACE_FLAG_FAILURE)) == 0) {
            char acc[128], cls[64];
            size_t cl = size - 8;
            if (cl >= sizeof(cls)) cl = sizeof(cls) - 1;
            memcpy(cls, header + o + 8, cl);
            while (cl > 0 && (cls[cl - 1] == ' ' || cls[cl - 1] == '\0')) cl--;
            cls[cl] = '\0';
            format_access(rd32(header + o + 4), flags, 1, acc, sizeof(acc));
            snprintf(text, sizeof(text), "(%s=%s,ACCESS=%s)", type == ACE_TYPE_ALARM ? "ALARM" : "AUDIT", cls, acc);
            done = true;
        } else if (type == ACE_TYPE_DEFPROT && size == 24 && flags == 0 && rd32(header + o + 4) == 0) {
            char s_[8], o_[8], g_[8], w_[8];
            format_prot(rd32(header + o + 8), s_, sizeof(s_));
            format_prot(rd32(header + o + 12), o_, sizeof(o_));
            format_prot(rd32(header + o + 16), g_, sizeof(g_));
            format_prot(rd32(header + o + 20), w_, sizeof(w_));
            snprintf(text, sizeof(text), "(DEFAULT_PROTECTION,SYSTEM:%s,OWNER:%s,GROUP:%s,WORLD:%s)", s_, o_, g_, w_);
            done = true;
        }
        if (!done) {
            size_t k, n = appendf(text, sizeof(text), 0, "(ACE type %u, flags %%X%04X, data", type, flags);
            for (k = 4; k < size; k++) n = appendf(text, sizeof(text), n, " %02X", header[o + k]);
            appendf(text, sizeof(text), n, ")");
        }
        visit(ctx, text);
        count++;
        o += size;
    }
    return count;
}
