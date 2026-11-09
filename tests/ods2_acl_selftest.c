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
static char got[16][1400];
static size_t ngot;
static void visit(void *ctx, const char *t) { (void) ctx; if (ngot < 16) snprintf(got[ngot++], sizeof(got[0]), "%s", t); }

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

    /* AXP1SYS (V8.4-2L2) DEFSTYLE.DDIF, an APPLICATION ACE with the HIDDEN flag: VMS with SECURITY printed
       (APPLICATION,SIZE=%D25,FLAGS=%X0E03,ACCESS=%X00010000,DATA=%X00140000,%X00010002,%X00000002,%X03010302,%X00000001) */
    {
        static const uint8_t app[25] = { 25, 7, 0x03, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x14, 0x00, 0x02, 0x00, 0x01, 0x00,
                                         0x02, 0x00, 0x00, 0x00, 0x02, 0x03, 0x01, 0x03, 0x01 };
        ngot = 0; memset(h, 0, 512); h[0] = 40; h[1] = 100; h[2] = 242; h[3] = 255; memcpy(h + 484, app, sizeof(app)); /* ACL area at 484: as in the AXP1SYS header */
        check("an APPLICATION ACE is shown as VMS prints it (SECURITY privilege)", ods2_acl_walk(h, visit, NULL) == 1 &&
              strcmp(got[0], "(APPLICATION,SIZE=%D25,FLAGS=%X0E03,ACCESS=%X00010000,DATA=%X00140000,%X00010002,%X00000002,%X03010302,%X00000001)") == 0);
    }

    ods2_acl_format_id(0xFFFFFFFFu, id, sizeof(id)); check("the all-ones identifier is *, as VMS prints it", strcmp(id, "*") == 0);
    ods2_acl_format_id(0x80010001u, id, sizeof(id)); check("unnamed general identifier in hex", strcmp(id, "%X80010001") == 0);


    /* batch m3 (VMS V8.4-2L2, ACLS.LOG): the bytes of the ACEs SET ACL made and VMS's own text */
    {
        struct { const char *name; uint8_t b[32]; size_t n; const char *vms; } t[] = {
            { "INTERACTIVE", {0x0c,1,0,0, 1,0,0,0, 3,0,0,0x80}, 12, "(IDENTIFIER=INTERACTIVE,ACCESS=READ)" },
            { "BATCH", {0x0c,1,0,0, 1,0,0,0, 1,0,0,0x80}, 12, "(IDENTIFIER=BATCH,ACCESS=READ)" },
            { "DIALUP", {0x0c,1,0,0, 1,0,0,0, 2,0,0,0x80}, 12, "(IDENTIFIER=DIALUP,ACCESS=READ)" },
            { "LOCAL", {0x0c,1,0,0, 1,0,0,0, 4,0,0,0x80}, 12, "(IDENTIFIER=LOCAL,ACCESS=READ)" },
            { "NETWORK", {0x0c,1,0,0, 1,0,0,0, 5,0,0,0x80}, 12, "(IDENTIFIER=NETWORK,ACCESS=READ)" },
            { "REMOTE", {0x0c,1,0,0, 1,0,0,0, 6,0,0,0x80}, 12, "(IDENTIFIER=REMOTE,ACCESS=READ)" },
            { "[200,*]", {0x0c,1,0,0, 1,0,0,0, 0xff,0xff,0x80,0}, 12, "(IDENTIFIER=[200,*],ACCESS=READ)" },
            { "[*,1]", {0x0c,1,0,0, 1,0,0,0, 1,0,0xff,0x3f}, 12, "(IDENTIFIER=[*,1],ACCESS=READ)" },
            { "[*,*]", {0x0c,1,0,0, 1,0,0,0, 0xff,0xff,0xff,0x3f}, 12, "(IDENTIFIER=[*,*],ACCESS=READ)" },
            { "three identifiers", {0x14,1,0,0, 1,0,0,0, 2,0,1,0, 5,0,0,0x80, 1,0,0,0x80}, 20, "(IDENTIFIER=[1,2]+NETWORK+BATCH,ACCESS=READ)" },
            { "all accesses", {0x0c,1,0,0, 0x1f,0,0,0, 3,0,1,0}, 12, "(IDENTIFIER=[1,3],ACCESS=READ+WRITE+EXECUTE+DELETE+CONTROL)" },
            { "PROTECTED", {0x0c,1,0,0x02, 1,0,0,0, 5,0,1,0}, 12, "(IDENTIFIER=[1,5],OPTIONS=PROTECTED,ACCESS=READ)" },
            { "NOPROPAGATE", {0x0c,1,0,0x08, 1,0,0,0, 6,0,1,0}, 12, "(IDENTIFIER=[1,6],OPTIONS=NOPROPAGATE,ACCESS=READ)" },
            { "CREATOR", {0x08,0x0d,0,0, 1,0,0,0}, 8, "(CREATOR,ACCESS=READ)" },
            { "DEFAULT_PROTECTION", {0x18,9,0,0, 0,0,0,0, 0x10,0,0,0, 0x18,0,0,0, 0x1e,0,0,0, 0x1f,0,0,0}, 24,
              "(DEFAULT_PROTECTION,SYSTEM:RWED,OWNER:RWE,GROUP:R,WORLD:)" },
            { "ALARM", {0x10,6,2,0, 3,0,0,0, 'S','E','C','U','R','I','T','Y'}, 16, "(ALARM=SECURITY,ACCESS=READ+WRITE+FAILURE)" },
            { "ALARM, all", {0x10,6,3,0, 0x1f,0,0,0, 'S','E','C','U','R','I','T','Y'}, 16,
              "(ALARM=SECURITY,ACCESS=READ+WRITE+EXECUTE+DELETE+CONTROL+SUCCESS+FAILURE)" },
            { "AUDIT", {0x10,5,3,0, 1,0,0,0, 'S','E','C','U','R','I','T','Y'}, 16, "(AUDIT=SECURITY,ACCESS=READ+SUCCESS+FAILURE)" },
        };
        size_t k;
        for (k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
            ngot = 0;
            header_with(h, t[k].b, t[k].n);
            check(t[k].name, ods2_acl_walk(h, visit, NULL) == 1 && strcmp(got[0], t[k].vms) == 0);
            if (strcmp(got[0], t[k].vms) != 0) printf("  got %s\n", got[0]);
        }
    }

    /* the largest identifier ACE (size 252: 61 identifiers; review 4 found a stack overflow from 18 on) */
    {
        uint8_t big[252];
        char want[1400];
        size_t k, n = 0;
        memset(big, 0, sizeof(big));
        big[0] = 252; big[1] = 1; big[4] = 1;
        for (k = 0; k < 61; k++) {          /* [37776,177776] each: 14 characters */
            big[8 + 4 * k] = 0xFE; big[9 + 4 * k] = 0xFF; big[10 + 4 * k] = 0xFE; big[11 + 4 * k] = 0x3F;
        }
        n += (size_t) snprintf(want + n, sizeof(want) - n, "(IDENTIFIER=");
        for (k = 0; k < 61; k++) n += (size_t) snprintf(want + n, sizeof(want) - n, "%s[37776,177776]", k ? "+" : "");
        snprintf(want + n, sizeof(want) - n, ",ACCESS=READ)");
        ngot = 0;
        header_with(h, big, sizeof(big));
        check("an identifier ACE of 61 identifiers is formatted whole, without overrunning anything",
              ods2_acl_walk(h, visit, NULL) == 1 && strcmp(got[0], want) == 0);
        /* and with a caller buffer far too small for it: the text is cut, never overrun */
        ods2_acl_format_id(0x3FFEFFFEu, id, 8);
        check("an identifier formatted into 8 bytes is cut, not overrun", strlen(id) <= 7);
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all ods2_acl checks passed\n");
    return 0;
}
