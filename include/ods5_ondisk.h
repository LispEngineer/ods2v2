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

/* ods5_ondisk.h - what ODS-5 (Files-11 structure level 5) adds to the
 * ODS-2 structures in ods2_ondisk.h. Offsets and values were derived
 * from the OpenVMS definition modules (HM2DEF, FH2DEF, FI5DEF, DIRDEF,
 * FATDEF, FIBDEF, ATRDEF), read as a reference, and checked by
 * measurement on real ODS-5 images; see ODS5_FORMAT_NOTES.md in the
 * ods5-cli project, which cites each fact.
 */

#ifndef ODS5_ONDISK_H
#define ODS5_ONDISK_H

#include <stdint.h>

/* HM2$W_STRUCLEV / FH2$W_STRUCLEV: high byte level, low byte version. */
#define ODS5_LEVEL_2        2u
#define ODS5_LEVEL_5        5u

/* HM2$W_VOLCHAR bits (HM2$M_*). Bits 0-4 are ODS-2's. */
#define ODS5_VC_READCHECK       0x0001u
#define ODS5_VC_WRITCHECK       0x0002u
#define ODS5_VC_ERASE           0x0004u
#define ODS5_VC_NOHIGHWATER     0x0008u
#define ODS5_VC_CLASS_PROT      0x0010u
#define ODS5_VC_ACCESSTIMES     0x0020u  /* level 5 */
#define ODS5_VC_HARDLINKS       0x0040u  /* level 5 */
#define ODS5_VC_NO_SPECIAL_FILES 0x0080u /* level 5 */

/* Name types: FI5$V_NAMETYPE (FI5$B_CONTROL bits 0-1) and
   DIR$V_NAMETYPE (DIR$B_FLAGS bits 3-5) share these values. */
#define ODS5_NAME_ODS2      0u
#define ODS5_NAME_ISL1      1u
#define ODS5_NAME_UCS2      3u

/* DIR$B_FLAGS. */
#define ODS5_DIR_TYPE_MASK      0x07u    /* DIR$V_TYPE, 3 bits */
#define ODS5_DIR_TYPE_FID       0u       /* DIR$C_FID */
#define ODS5_DIR_TYPE_LINKNAME  1u       /* DIR$C_LINKNAME */
#define ODS5_DIR_NAMETYPE_SHIFT 3u       /* DIR$V_NAMETYPE, 3 bits */
#define ODS5_DIR_NAMETYPE_MASK  0x38u
#define ODS5_DIR_NEXTREC        0x40u    /* DIR$V_NEXTREC */
#define ODS5_DIR_PREVREC        0x80u    /* DIR$V_PREVREC */

/* Longest ODS-5 name in a directory record (DIR$S_ODS5_NAME). */
#define ODS5_DIR_MAX_NAME       236

/* FI5: the ODS-5 ident area (at 2 * FH2$B_IDOFFSET in a header whose
   FH2$W_STRUCLEV level is 5). Offsets are relative to the start of
   the ident area. */
#define ODS5_FI5_CONTROL        0        /* FI5$B_CONTROL */
#define ODS5_FI5_NAMETYPE_MASK  0x03u    /* FI5$V_NAMETYPE, 2 bits */
#define ODS5_FI5_FIXED_LENGTH   0x10u    /* FI5$M_FIXED_LENGTH */
#define ODS5_FI5_NAMELEN        1        /* FI5$B_NAMELEN: bytes, ";version" included */
#define ODS5_FI5_REVISION       2        /* FI5$W_REVISION */
#define ODS5_FI5_CREDATE        4        /* FI5$Q_CREDATE */
#define ODS5_FI5_REVDATE        12       /* FI5$Q_REVDATE */
#define ODS5_FI5_EXPDATE        20       /* FI5$Q_EXPDATE */
#define ODS5_FI5_BAKDATE        28       /* FI5$Q_BAKDATE */
#define ODS5_FI5_ACCDATE        36       /* FI5$Q_ACCDATE */
#define ODS5_FI5_ATTDATE        44       /* FI5$Q_ATTDATE */
#define ODS5_FI5_EX_RECATTR     52       /* FI5$Q_EX_RECATTR */
#define ODS5_FI5_LENGTH_HINT    60       /* FI5$R_LENGTH_HINT, 16 bytes */
#define ODS5_FI5_FILENAME       76       /* FI5$T_FILENAME (44) + FI5$T_FILENAMEXT (204) */
#define ODS5_FI5_MIN_LENGTH     120      /* FI5$C_MIN_LENGTH */
#define ODS5_FI5_MAX_LENGTH     324      /* FI5$C_MAX_LENGTH */

/* FH2$W_LINKCOUNT (byte offset 74 in the header). */
#define ODS5_FH2_LINKCOUNT      74

/* FAT: for a special file (FAT$V_FILEORG == FAT$C_SPECIAL) the byte
   after the record-type byte is FAT$B_SPECIAL_TYPE. */
#define ODS5_FAT_ORG_SPECIAL    4u
#define ODS5_FAT_FIFO           1u
#define ODS5_FAT_CHAR_SPECIAL   2u
#define ODS5_FAT_BLOCK_SPECIAL  3u
#define ODS5_FAT_SYMLINK        4u       /* pre-V8.2 */
#define ODS5_FAT_SYMBOLIC_LINK  5u

#endif /* ODS5_ONDISK_H */
