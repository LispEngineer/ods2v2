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

/* ods5_names_selftest.c - ODS-5 name handling: UTF-8 / ISO Latin-1 /
 * UCS-2 conversion, the ^-escape spelling (RMS canonical form, VSI Guide
 * to Extended File Specifications B.2.2.3 - B.2.2.6) and wildcard
 * matching of spellings against names. */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods5_names.h"
#include "ods2_wildcard.h"

static void esc(const char *in, unsigned flags, const char *want)
{
    char out[256];
    size_t n = ods5_escape_name(in, flags, out, sizeof(out));
    if (n == (size_t) -1 || strcmp(out, want) != 0) {
        printf("FAIL: escape(\"%s\") = \"%s\", wanted \"%s\"\n", in,
               n == (size_t) -1 ? "(overflow)" : out, want);
        assert(0);
    }
}

static void unesc(const char *in, const char *want)
{
    char out[256];
    size_t n = ods5_unescape(in, strlen(in), out, sizeof(out));
    if (want == NULL) {
        assert(n == (size_t) -1);
    } else {
        if (n == (size_t) -1 || strcmp(out, want) != 0) {
            printf("FAIL: unescape(\"%s\") = \"%s\", wanted \"%s\"\n", in,
                   n == (size_t) -1 ? "(error)" : out, want);
            assert(0);
        }
    }
}

int main(void)
{
    char buf[ODS5_MAX_LITERAL_NAME + 1];
    uint8_t st[64];
    size_t n, len;
    bool wide;
    uint32_t cp;

    /* --- UTF-8 --- */
    assert(ods5_utf8_decode("A", 1, &cp) == 1 && cp == 'A');
    assert(ods5_utf8_decode("\xC3\xA9", 2, &cp) == 2 && cp == 0xE9);
    assert(ods5_utf8_decode("\xE2\x82\xAC", 3, &cp) == 3 && cp == 0x20AC);
    assert(ods5_utf8_decode("\xE9", 1, &cp) == 1 && cp == 0xE9);        /* Latin-1 byte */
    assert(ods5_utf8_decode("\xC3", 1, &cp) == 1 && cp == 0xC3);        /* truncated */
    assert(ods5_utf8_decode("\xC0\x80", 2, &cp) == 1 && cp == 0xC0);    /* overlong */
    assert(ods5_utf8_encode(0x41, buf) == 1 && buf[0] == 'A');
    assert(ods5_utf8_encode(0xFF, buf) == 2 && (uint8_t) buf[0] == 0xC3 && (uint8_t) buf[1] == 0xBF);
    assert(ods5_utf8_encode(0x20AC, buf) == 3 && memcmp(buf, "\xE2\x82\xAC", 3) == 0);
    assert(ods5_utf8_encode(0, buf) == 3 && memcmp(buf, "\xEF\xBF\xBD", 3) == 0); /* never NUL */
    assert(ods5_utf8_length("caf\xC3\xA9") == 4);
    printf("PASS: UTF-8 decode and encode\n");

    /* --- stored names --- */
    {
        const uint8_t isl1[] = { 'c', 'a', 'f', 0xE9, '.', 'T', 'X', 'T' };
        const uint8_t ucs2[] = { 'e', 0, 'u', 0, 0xAC, 0x20, '.', 0 };
        n = ods5_isl1_to_utf8(isl1, sizeof(isl1), buf, sizeof(buf));
        assert(n == 9 && strcmp(buf, "caf\xC3\xA9.TXT") == 0);
        n = ods5_ucs2_to_utf8(ucs2, sizeof(ucs2), buf, sizeof(buf));
        assert(n == 6 && strcmp(buf, "eu\xE2\x82\xAC.") == 0);
        assert(ods5_ucs2_to_utf8(ucs2, 7, buf, sizeof(buf)) == (size_t) -1); /* odd length */
        assert(ods5_isl1_to_utf8(isl1, sizeof(isl1), buf, 8) == (size_t) -1); /* no room */
        assert(ods5_isl1_to_utf8((const uint8_t *) "a\0b", 3, buf, sizeof(buf)) == 5); /* NUL -> U+FFFD */
        assert(strchr(buf, 0) == buf + 5);
        printf("PASS: ISO Latin-1 and UCS-2 stored names become UTF-8\n");
    }
    assert(ods5_utf8_to_stored("caf\xC3\xA9.TXT", st, sizeof(st), &len, &wide));
    assert(!wide && len == 8 && st[3] == 0xE9);
    assert(ods5_utf8_to_stored("eu\xE2\x82\xAC.", st, sizeof(st), &len, &wide));
    assert(wide && len == 8 && st[0] == 'e' && st[1] == 0 && st[4] == 0xAC && st[5] == 0x20);
    assert(!ods5_utf8_to_stored("x\xF0\x9F\x98\x80", st, sizeof(st), &len, &wide)); /* beyond UCS-2 */
    assert(!ods5_utf8_to_stored("abcdef", st, 3, &len, &wide));
    printf("PASS: UTF-8 names go back to ISO Latin-1 when they can, UCS-2 when they must\n");

    /* --- canonical spelling (B.2.2.6) --- */
    esc("PLAIN.TXT", ODS5_ESC_KEEP_LAST_DOT, "PLAIN.TXT");
    esc("MixedCase.Txt", ODS5_ESC_KEEP_LAST_DOT, "MixedCase.Txt");
    esc("two words.txt", ODS5_ESC_KEEP_LAST_DOT, "two^_words.txt");
    esc("A.b.C.d.TXT", ODS5_ESC_KEEP_LAST_DOT, "A^.b^.C^.d.TXT");
    esc("noext.", ODS5_ESC_KEEP_LAST_DOT, "noext.");
    esc("a.b.", ODS5_ESC_KEEP_LAST_DOT, "a^.b.");
    esc("Lots.Of.Periods", 0, "Lots^.Of^.Periods");
    esc("a!b#c&d'e`f(g)h+i@j{k}l,m;n[o]p%q^r=s.x", ODS5_ESC_KEEP_LAST_DOT,
        "a^!b^#c^&d^'e^`f^(g^)h^+i^@j^{k^}l^,m^;n^[o^]p^%q^^r^=s.x");
    esc("it$-_~.x", ODS5_ESC_KEEP_LAST_DOT, "it$-_~.x");          /* written plain */
    esc("caf\xC3\xA9.txt", ODS5_ESC_KEEP_LAST_DOT, "caf\xC3\xA9.txt"); /* e acute: itself */
    esc("\xC3\xBF.x", ODS5_ESC_KEEP_LAST_DOT, "^FF.x");                /* y diaeresis */
    esc("a\xC2\xA0.x", ODS5_ESC_KEEP_LAST_DOT, "a^A0.x");              /* no-break space */
    esc("a\xC2\x85.x", ODS5_ESC_KEEP_LAST_DOT, "a^85.x");              /* C1 control */
    esc("a\x7F.x", ODS5_ESC_KEEP_LAST_DOT, "a^7F.x");
    esc("euro\xE2\x82\xAC.txt", ODS5_ESC_KEEP_LAST_DOT, "euro^U20AC.txt");
    {
        char small[4];
        assert(ods5_escape_name("abcdef", 0, small, sizeof(small)) == (size_t) -1);
    }
    printf("PASS: names are spelled the way RMS writes them (^_ ^. ^! ... ^xx ^Uxxxx)\n");

    /* --- reading a spelling --- */
    unesc("two^_words", "two words");
    unesc("two^ words", "two words");
    unesc("a^.b^.c", "a.b.c");
    unesc("a.b", "a.b");
    unesc("cafe^E9", "cafe\xC3\xA9");
    unesc("cafe^e9", "cafe\xC3\xA9");
    unesc("^U20AC", "\xE2\x82\xAC");
    unesc("^u20AC", NULL);                 /* only a capital U */
    unesc("^20", " ");
    unesc("^^", "^");
    unesc("a^;b^[c^]d^%e", "a;b[c]d%e");
    unesc("a^", NULL);                     /* ^ at the end */
    unesc("a^E", NULL);                    /* one hex digit */
    unesc("a^U12", NULL);
    unesc("a^U12G4", NULL);
    unesc("a^00", NULL);                   /* NUL */
    unesc("a^Q", NULL);                    /* reserved */
    unesc("caf\xC3\xA9", "caf\xC3\xA9");   /* a typed character is itself */
    unesc("caf\xE9", "caf\xC3\xA9");       /* a Latin-1 byte likewise */
    printf("PASS: ^-escapes are read back, malformed ones refused\n");

    /* round trip: every character an ODS-5 name can hold survives spelling and back */
    {
        char lit[600], sp[3000], back[600];
        size_t i, k = 0;
        for (i = 0x20; i <= 0xFF; i++) {
            if (strchr("\"*\\:<>/?|", (int) i) != NULL) continue;
            k += ods5_utf8_encode((uint32_t) i, lit + k);
        }
        k += ods5_utf8_encode(0x20AC, lit + k);
        k += ods5_utf8_encode(0x0141, lit + k);
        lit[k] = '\0';
        assert(ods5_escape_name(lit, 0, sp, sizeof(sp)) != (size_t) -1);
        assert(ods5_unescape(sp, strlen(sp), back, sizeof(back)) == k);
        assert(strcmp(back, lit) == 0);
        printf("PASS: every legal ISO Latin-1 character, and some UCS-2 ones, survive spelling and back\n");
    }

    /* --- unescaped search --- */
    assert(ods5_find_unescaped("a^.b.c", '.') == strstr("a^.b.c", ".c"));
    assert(ods5_find_unescaped("a^[b", '[') == NULL);
    assert(ods5_rfind_unescaped("a.b^.c.d", '.') != NULL);
    assert(strcmp(ods5_rfind_unescaped("a.b^.c.d", '.'), ".d") == 0);
    assert(ods5_find_unescaped("a^^b", 'b') != NULL && ods5_find_unescaped("a^^", '^') == NULL);
    printf("PASS: unescaped delimiters are found, escaped ones skipped\n");

    /* --- wildcards --- */
    assert(ods2_wildcard_match("*.TXT", "readme.txt"));
    assert(ods2_wildcard_match("READ%E.TXT", "readme.txt"));
    assert(ods2_wildcard_match("READ?E.TXT", "readme.txt"));
    assert(!ods2_wildcard_match("READ??E.TXT", "readme.txt"));
    assert(ods2_wildcard_match("*", ""));
    assert(ods2_wildcard_match("a*b*c", "aXXbYYc"));
    assert(!ods2_wildcard_match("a*b*c", "aXXbYY"));
    assert(ods2_wildcard_match("two^_words.*", "two words.txt"));
    assert(ods2_wildcard_match("two*^_*", "two words.txt"));
    assert(ods2_wildcard_match("a^.b.txt", "a.b.txt"));
    assert(!ods2_wildcard_match("a^.b.txt", "aXb.txt"));        /* ^. is a dot, not "any" */
    assert(ods2_wildcard_match("star^*name", "star*name"));     /* escaped wildcards are literal */
    assert(!ods2_wildcard_match("star^*name", "starXname"));
    assert(ods2_wildcard_match("pct^%x", "pct%x"));
    assert(!ods2_wildcard_match("pct^%x", "pctyx"));
    assert(ods2_wildcard_match("caf^E9.txt", "caf\xC3\xA9.txt"));
    assert(ods2_wildcard_match("caf%.txt", "caf\xC3\xA9.txt"));  /* % is one character, not one byte */
    assert(!ods2_wildcard_match("caf%%.txt", "caf\xC3\xA9.txt"));
    assert(ods2_wildcard_match("euro^U20AC.*", "euro\xE2\x82\xAC.txt"));
    assert(ods2_wildcard_match("euro?.txt", "euro\xE2\x82\xAC.txt"));
    assert(ods2_wildcard_match("MIXEDcase.TXT", "MixedCase.Txt")); /* case blind */
    assert(!ods2_wildcard_match("bad^", "bad"));                   /* malformed pattern: no match */
    assert(!ods2_wildcard_match("bad^Q", "badQ"));
    printf("PASS: wildcard patterns with escapes match names character by character, case-blind\n");

    /* --- case folding beyond ASCII: what VMS does (measured on a VMS-made volume: the
       records for ae/AE, Alpha/alpha, Zhe/zhe are one name with two versions, and the
       directory is ordered by the folded code points: a-diaeresis, e-acute, n-tilde,
       u-diaeresis, sharp s, A-macron, y-diaeresis (folds to U+0178), Alpha, Zhe, euro, CJK) --- */
    assert(ods5_fold('a') == 'A' && ods5_fold('Z') == 'Z' && ods5_fold('0') == '0');
    assert(ods5_fold(0xE4) == 0xC4 && ods5_fold(0xE9) == 0xC9 && ods5_fold(0xF1) == 0xD1 && ods5_fold(0xFC) == 0xDC);
    assert(ods5_fold(0xF7) == 0xF7 && ods5_fold(0xDF) == 0xDF);          /* division sign, sharp s: no upper case */
    assert(ods5_fold(0xFF) == 0x178 && ods5_fold(0x101) == 0x100);
    assert(ods5_fold(0x3B1) == 0x391 && ods5_fold(0x436) == 0x416 && ods5_fold(0x20AC) == 0x20AC);
    assert(ods5_fold(0x65E5) == 0x65E5 && ods5_fold(0x100) == 0x100);
    {
        static const uint32_t order[] = { 0x7E, 0xA0, 0xE4, 0xE9, 0xF1, 0xFC, 0xDF, 0x100, 0xFF, 0x391, 0x416, 0x20AC, 0x65E5 };
        size_t i;
        for (i = 1; i < sizeof(order) / sizeof(order[0]); i++) assert(ods5_fold(order[i - 1]) < ods5_fold(order[i]));
    }
    assert(ods2_wildcard_match("CAF\xC3\x89.TXT", "caf\xC3\xA9.txt"));          /* E-acute finds e-acute */
    assert(ods2_wildcard_match("caf^E9.txt", "CAF\xC3\x89.TXT"));
    assert(ods2_wildcard_match("^U03B1*", "\xCE\x91.TXT"));                    /* small alpha finds capital Alpha */
    assert(ods2_wildcard_match("\xD0\x96.TXT", "\xD0\xB6.txt"));             /* Zhe, both cases */
    assert(!ods2_wildcard_match("\xC3\x9F.TXT", "SS.TXT"));                    /* sharp s is not SS */
    printf("PASS: case folding covers ISO Latin-1 and UCS-2 letters, in the order VMS keeps directories\n");

    esc("sp:c.txt", ODS5_ESC_KEEP_LAST_DOT, "sp^:c.txt");
    esc("a<b>c\\d|e/f.x", ODS5_ESC_KEEP_LAST_DOT, "a^<b^>c^\\d^|e^/f.x");
    printf("PASS: the characters DCL escapes although a name may hold them (: < > \\ | /)\n");

    printf("\nods5_names_selftest: all checks passed\n");
    return 0;
}
