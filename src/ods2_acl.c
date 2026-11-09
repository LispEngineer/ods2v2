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
#include <stdio.h>
#include <string.h>

#define ACE_TYPE_ID 1u        /* identifier ACE */
#define ACE_FLAG_DEFAULT 0x0100u       /* measured: the directory default-protection ACE has flags word 0x0100 */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

void ods2_acl_format_id(uint32_t id, char *out, size_t cap)
{
    if (id == 0x80000005u) {            /* measured on V8.4: IDENTIFIER=NETWORK */
        snprintf(out, cap, "NETWORK");
    } else if ((id & 0x80000000u) == 0) {
        unsigned group = id >> 16, member = id & 0xFFFFu;
        char g[16], m[16];
        if (group == 0xFFFFu) snprintf(g, sizeof(g), "*"); else snprintf(g, sizeof(g), "%o", group);
        if (member == 0xFFFFu) snprintf(m, sizeof(m), "*"); else snprintf(m, sizeof(m), "%o", member);
        snprintf(out, cap, "[%s,%s]", g, m);
    } else {
        snprintf(out, cap, "%%X%08X", (unsigned) id);
    }
}

static void format_access(uint32_t a, char *out, size_t cap)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { 1, "READ" }, { 2, "WRITE" }, { 4, "EXECUTE" }, { 8, "DELETE" }, { 16, "CONTROL" } };
    size_t n = 0, i;
    out[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (a & names[i].bit) n += (size_t) snprintf(out + n, cap - n, "%s%s", n ? "+" : "", names[i].name);
    }
    if (a & ~31u) n += (size_t) snprintf(out + n, cap - n, "%s%%X%X", n ? "+" : "", (unsigned) (a & ~31u));
    if (n == 0) snprintf(out, cap, "NONE");
}

size_t ods2_acl_walk(const uint8_t *header, void (*visit)(void *, const char *), void *ctx)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    size_t o = (size_t) core->acoffset * 2, end = (size_t) core->rsoffset * 2, count = 0;

    if (core->acoffset == 0 || core->acoffset == 255 || end > 512 || o >= end) return 0;
    while (o + 4 <= end && header[o] != 0) {
        unsigned size = header[o], type = header[o + 1];
        unsigned flags = (unsigned) header[o + 2] | (unsigned) header[o + 3] << 8;
        char text[512];
        if (o + size > end || size < 4) {
            snprintf(text, sizeof(text), "(damaged ACE at offset %zu)", o);
            visit(ctx, text);
            return count + 1;
        }
        if (type == ACE_TYPE_ID && size >= 12 && (size - 8) % 4 == 0 && (flags & ~ACE_FLAG_DEFAULT) == 0) {
            char ids[256] = "", acc[128], one[32];
            size_t k, n = 0;
            for (k = 8; k < size; k += 4) {
                ods2_acl_format_id(rd32(header + o + k), one, sizeof(one));
                n += (size_t) snprintf(ids + n, sizeof(ids) - n, "%s%s", k > 8 ? "+" : "", one);
            }
            format_access(rd32(header + o + 4), acc, sizeof(acc));
            snprintf(text, sizeof(text), "(IDENTIFIER=%s%s,ACCESS=%s)", ids,
                     (flags & ACE_FLAG_DEFAULT) ? ",OPTIONS=DEFAULT" : "", acc);
        } else {
            size_t k, n = (size_t) snprintf(text, sizeof(text), "(ACE type %u, flags %%X%04X, data", type, flags);
            for (k = 4; k < size && n + 4 < sizeof(text); k++) n += (size_t) snprintf(text + n, sizeof(text) - n, " %02X", header[o + k]);
            snprintf(text + n, sizeof(text) - n, ")");
        }
        visit(ctx, text);
        count++;
        o += size;
    }
    return count;
}
