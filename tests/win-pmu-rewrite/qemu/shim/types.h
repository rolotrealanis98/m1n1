/* SPDX-License-Identifier: MIT */
/* Bare-metal (QEMU harness) stand-in for m1n1's src/types.h (tests/win-vgic3 and tests/win-pmu-rewrite) */

#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LP64 like the firmware, so "%lx" matches u64 on both */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long s64;

#define UNUSED(x)  (void)(x)
#define ALIGNED(x) __attribute__((aligned(x)))

#define BIT(x)                 (1UL << (x))
#define MASK(x)                (BIT(x) - 1)
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))

/* System registers become plain encodings; vgic3_host.h maps mrs()/msr() onto the mock */
#define sys_reg(op0, op1, CRn, CRm, op2)                                                           \
    (((op0) << 16) | ((op1) << 12) | ((CRn) << 8) | ((CRm) << 4) | (op2))

#endif
