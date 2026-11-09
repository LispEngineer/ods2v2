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

/* ods5_names.h - ODS-5 file names in the program: stored ISO Latin-1
 * and UCS-2 names are held as UTF-8 ("literal names"); users write and
 * read them with VMS's circumflex escapes ("spellings": ^_ space, ^xx
 * an ISO Latin-1 byte in hex, ^Uxxxx a 16-bit Unicode character, ^.
 * ^, ^; ^[ ... a literal special character). The rules are the
 * "canonical form" and escape tables of the VSI Guide to Extended File
 * Specifications (B.2.2.3 - B.2.2.6; 3.1.2.2 - 3.1.2.3); see
 * ODS5_FORMAT_NOTES.md in the ods5-cli project.
 */
#ifndef ODS5_NAMES_H
#define ODS5_NAMES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Longest literal (UTF-8) name an entry can hold: DIR$B_NAMECOUNT is one byte, so a
   damaged record can say 255 ISO Latin-1 bytes of 2 UTF-8 bytes each (a legal name is
   at most 236 bytes, or 118 UCS-2 units of up to 3). */
#define ODS5_MAX_LITERAL_NAME 510

/* UTF-8 --- */

/* Decodes one character of the UTF-8 string s (n bytes available, n >= 1).
   Returns the number of bytes used (>= 1) and stores the code point in
   *cp. A byte that is not valid UTF-8 is taken as the ISO Latin-1
   character of the same value, one byte. */
size_t ods5_utf8_decode(const char *s, size_t n, uint32_t *cp);

/* Encodes cp (0 and anything above 0x10FFFF become U+FFFD; a UCS-2
   surrogate unit is kept as a 3-byte sequence). Writes 1-4 bytes to out
   and returns how many. */
size_t ods5_utf8_encode(uint32_t cp, char *out);

/* Number of characters in a UTF-8 string. */
size_t ods5_utf8_length(const char *s);

/* Stored name -> UTF-8 literal name. Return the length, or (size_t) -1
   if it will not fit in cap (a nul is added, so cap >= length + 1), or if a
   UCS-2 name has an odd byte count. */
size_t ods5_isl1_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap);
size_t ods5_ucs2_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap);

/* UTF-8 literal name -> stored form: true and *is_ucs2 = false with
   ISO Latin-1 bytes in out (nothing above U+00FF); true and *is_ucs2 = true
   with little-endian 16-bit units; false if it does not fit or has a
   character above U+FFFF. *out_len is in bytes. */
bool ods5_utf8_to_stored(const char *in, uint8_t *out, size_t cap,
                         size_t *out_len, bool *is_ucs2);

/* Spellings --- */

#define ODS5_ESC_KEEP_LAST_DOT 1u  /* a file name: the last '.' is the type delimiter, not escaped */

/* Literal name -> spelling, the way RMS and DCL write it (B.2.2.6). Returns the length
   or (size_t) -1 if it does not fit. Without ODS5_ESC_KEEP_LAST_DOT every '.'
   is escaped (a directory name). */
size_t ods5_escape_name(const char *utf8, unsigned flags, char *out, size_t cap);

/* Spelling (n bytes) -> literal name. Returns the length, or (size_t) -1
   on a malformed escape (^ at the end, ^x not followed by two hex digits,
   ^U without four, ^00, an escape that RMS reserves) or no room. Wildcard
   characters are not special here. */
size_t ods5_unescape(const char *spelling, size_t n, char *out, size_t cap);

/* Position of the first character of s that is the unescaped byte ch
   (skipping each ^ and the character after it), or NULL. */
const char *ods5_find_unescaped(const char *s, char ch);

/* Position of the last unescaped ch, or NULL. */
const char *ods5_rfind_unescaped(const char *s, char ch);

/* Comparison --- */

/* Case folding for lookups: the upper-case form of a character of the Basic
   Multilingual Plane (simple Unicode mapping). VMS treats the ISO Latin-1 and
   UCS-2 letters of a name case-blind in the same way, and orders directories
   by the folded code points (measured, ODS5_FORMAT_NOTES.md 4.1). Which
   exotic characters (dotless i, long s, micro sign) VMS folds is not measured. */
uint32_t ods5_fold(uint32_t cp);

/* Wildcard match of a pattern written as a spelling against a literal
   name: '*' any run of characters, '%' and '?' exactly one, ^-escapes
   literal; case-blind. Both are NUL-terminated. */
bool ods5_wildcard_match(const char *pattern, const char *name);

/* Names for writing --- */

/* Checks a literal name ("NAME.TYPE", or without the dot) as the name of a new
   file or directory on an ODS-5 volume. Returns NULL if acceptable, else why
   not: not empty, no control characters, none of the characters VMS itself
   refuses at creation (# { } ` ? * and the double quote), none above U+FFFF,
   and at most 236 bytes once stored (236 ISO Latin-1 characters, or 118 UCS-2
   ones; ODS5_FORMAT_NOTES.md 5, 11). */
const char *ods5_check_new_name(const char *utf8);

/* The literal name with its dot completed ("README" -> "README."), as it is
   stored. False if it does not fit in cap. */
bool ods5_complete_name(const char *utf8, char *out, size_t cap);

/* The name as stored in a directory record and in the file header: the
   literal name with its dot completed, case kept. *type is the name type VMS
   stores for it (measured, ODS5_FORMAT_NOTES.md 10-11): ODS5_NAME_ODS2 for a
   name made only of letters, digits, '$', '_' and '-' with one '.', name and
   type each at most 39 characters, in any case; ODS5_NAME_ISL1 for any other
   name with nothing above U+00FF; ODS5_NAME_UCS2 otherwise. out gets the
   stored bytes (ISO Latin-1 for ODS2/ISL1, little-endian 16-bit units for
   UCS2), *len their count in bytes. False if it does not fit in cap or has
   a character above U+FFFF. */
bool ods5_stored_name(const char *utf8, uint8_t *out, size_t cap, size_t *len, unsigned *type);

/* Names on Linux and on the terminal --- */

/* The name made safe for a Linux file name and for the terminal: the characters that
   would be a path separator or could do harm there -- '/', the C0 controls U+0000-U+001F,
   DEL U+007F and the C1 controls U+0080-U+009F -- are written %XX (two upper-case hex
   digits of the code point); a '%' is written %25 only where it is followed by two hex digits
   that would read back as one of those escapes, so every other name, '%' included, is left
   as it is and the mapping is reversible with ods5_unescape_from_linux(). Returns the
   length, or (size_t) -1 if it does not fit in cap (a nul is added). */
size_t ods5_escape_for_linux(const char *utf8, char *out, size_t cap);

/* The inverse: %XX with XX one of the escaped code points (or 25) becomes that character. */
size_t ods5_unescape_from_linux(const char *in, char *out, size_t cap);

#endif /* ODS5_NAMES_H */
