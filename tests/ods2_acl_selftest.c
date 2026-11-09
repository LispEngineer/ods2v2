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

/* ods2_acl_selftest.c - ACEs as VMS DIRECTORY/FULL shows them. The ACE bytes are those of files made by
 * VMS V8.4 on the corpus volumes (ACL1.TXT: NETWORK none, [1,2] read, [200,1] RWED; the directory's
 * default ACE; two of the 30 ACEs of ACL2_MANY.TXT) with VMS's own listing as the expected text. */
#include "ods2_acl.h"
#include <stdio.h>
#include <string.h>

static int failures;
static char got[16][256];
static size_t ngot;
static void visit(void *ctx, const char *t) { (void) ctx; if (ngot < 16) snprintf(got[ngot++], 256, "%s", t); }

static void check(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static void header_with(uint8_t *h, const uint8_t *aces, size_t n)
{
    memset(h, 0, 512);
    h[0] = 40; h[1] = 100;
    h[2] = (uint8_t) ((510 - n) / 2); h[3] = 255;
    memcpy(h + 510 - n, aces, n);
}

int main(void)
{
    uint8_t h[512];
    static const uint8_t acl1[] = { 0x0c,1,0,0, 0,0,0,0, 5,0,0,0x80,   0x0c,1,0,0, 1,0,0,0, 2,0,1,0,
                                    0x0c,1,0,0, 0x0f,0,0,0, 1,0,0x80,0 };
    static const uint8_t dirdef[] = { 0x0c,1,0,1, 3,0,0,0, 1,0,0x80,0 };
    static const uint8_t many[] = { 0x0c,1,0,0, 1,0,0,0, 0x1e,0,0xc0,0,   0x0c,1,0,0, 1,0,0,0, 6,0,0xc0,0 };
    static const uint8_t two[] = { 0x10,1,0,0, 1,0,0,0, 2,0,1,0, 5,0,0,0x80 };
    static const uint8_t other[] = { 0x08,9,0x12,0x03, 0xde,0xad,0xbe,0xef };
    char id[32];

    header_with(h, acl1, sizeof(acl1));
    check("three ACEs", ods2_acl_walk(h, visit, NULL) == 3);
    check("NETWORK none", strcmp(got[0], "(IDENTIFIER=NETWORK,ACCESS=NONE)") == 0);
    check("[1,2] read", strcmp(got[1], "(IDENTIFIER=[1,2],ACCESS=READ)") == 0);
    check("[200,1] RWED, group in octal", strcmp(got[2], "(IDENTIFIER=[200,1],ACCESS=READ+WRITE+EXECUTE+DELETE)") == 0);

    ngot = 0; header_with(h, dirdef, sizeof(dirdef));
    check("default ACE", ods2_acl_walk(h, visit, NULL) == 1 && strcmp(got[0], "(IDENTIFIER=[200,1],OPTIONS=DEFAULT,ACCESS=READ+WRITE)") == 0);

    ngot = 0; header_with(h, many, sizeof(many));
    check("[300,36] and [300,6]", ods2_acl_walk(h, visit, NULL) == 2 && strcmp(got[0], "(IDENTIFIER=[300,36],ACCESS=READ)") == 0 &&
          strcmp(got[1], "(IDENTIFIER=[300,6],ACCESS=READ)") == 0);

    ngot = 0; header_with(h, two, sizeof(two));
    check("two identifiers in one ACE", ods2_acl_walk(h, visit, NULL) == 1 && strcmp(got[0], "(IDENTIFIER=[1,2]+NETWORK,ACCESS=READ)") == 0);

    ngot = 0; header_with(h, other, sizeof(other));
    check("an unknown ACE is shown in hex", ods2_acl_walk(h, visit, NULL) == 1 && strstr(got[0], "ACE type 9") && strstr(got[0], "DE AD BE EF"));

    memset(h, 0, 512); h[2] = 255; h[3] = 255;
    check("no ACL (offset 255)", ods2_acl_walk(h, visit, NULL) == 0);
    memset(h, 0, 512); h[2] = 200; h[3] = 255; h[400] = 0xF0; h[401] = 1;   /* size runs past the area */
    ngot = 0;
    check("an ACE longer than the area is reported damaged", ods2_acl_walk(h, visit, NULL) == 1 && strstr(got[0], "damaged"));

    ods2_acl_format_id(0x80010001u, id, sizeof(id)); check("unnamed general identifier in hex", strcmp(id, "%X80010001") == 0);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all ods2_acl checks passed\n");
    return 0;
}
