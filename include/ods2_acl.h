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
 * ods2_acl.h - the access control list of a file header, shown as VMS shows it
 * (DIRECTORY/FULL "Access Cntrl List:").
 *
 * The ACL area of a header lies between FH2$B_ACOFFSET*2 and FH2$B_RSOFFSET*2 (an
 * offset of 255 means "none"). Each ACE: size (1 byte), type (1), flags (2), then data.
 * Layouts decoded here are those measured on VMS-made volumes (ODS5_FORMAT_NOTES.md 15):
 *   type 1 (identifier ACE), size 12 + 4 per extra identifier: access (4), identifier(s) (4 each).
 * Every other ACE is shown as its raw bytes, so that nothing is hidden.
 */
#ifndef ODS2_ACL_H
#define ODS2_ACL_H

#include <stddef.h>
#include <stdint.h>

/* Formats the ACEs of `header` (a 512-byte file header), one string per ACE, each ACE in `out`
 * as one line without a newline. Returns the number of ACEs, 0 if none. `visit` is called per ACE
 * with the text. */
size_t ods2_acl_walk(const uint8_t *header, void (*visit)(void *ctx, const char *ace_text), void *ctx);

/* One identifier value as VMS writes it: [group,member] in octal ('*' for a wildcard), a
 * system identifier by name, otherwise %Xnnnnnnnn. */
void ods2_acl_format_id(uint32_t id, char *out, size_t cap);

#endif
