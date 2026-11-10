/* SPDX-License-Identifier: MIT */
/* Host stand-ins for src/dart_identity.c (tests/usb-host only) */

#ifndef DART_IDENTITY_HOST_H
#define DART_IDENTITY_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LP64 like the firmware, so "%lx" matches u64 on both */
typedef unsigned int u32;
typedef unsigned long u64;

#define BIT(x)                 (1UL << (x))
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))
#define SZ_16K                 (1 << 14)
#define dma_wmb()              __atomic_thread_fence(__ATOMIC_RELEASE)

/* the test supplies an allocator that can place the pool where it wants */
void *memalign(size_t align, size_t size);
void free_hook(void *p);
#define free(p) free_hook(p)

#include "../../../src/dart_identity.h"

#endif
