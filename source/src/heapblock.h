/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HEAPBLOCK_H
#define HEAPBLOCK_H

#include "types.h"

void heapblock_init(void);

void *heapblock_alloc(size_t size);
void *heapblock_peek(void); /* read-only aligned next allocation address */
void *heapblock_alloc_aligned(size_t size, size_t align);
void heapblock_set_limit(void *limit);
size_t heapblock_remaining(void); /* bytes below the limit; SIZE_MAX without one */

/*
 * n5c: one extension pool (owner decision 2026-11-11: ~512 MiB for the resident). The launcher takes it out of
 * the guest's RAM (absent from /chosen/m5w,memory, stage 2, the storage contract's RAM and the USB identity map)
 * and registers it once, after setting the limit, before the guest starts. An allocation that does not fit
 * below the limit is served from the pool instead (dlmalloc treats it as a new segment). heapblock_peek(),
 * heapblock_alloc(0) and heapblock_remaining() keep describing the primary heap only (the storage heap proofs
 * and the framebuffer shadow use them unchanged).
 */
int heapblock_add_pool(void *base, size_t size); /* 0, or -1: refused (once; aligned; clear of the heap) */
size_t heapblock_total_remaining(void);         /* primary remaining + pool remaining */
bool heapblock_pool_owned(u64 pa, u64 len);     /* inside the pool's allocated part */
void heapblock_pool_info(u64 *base, u64 *size, u64 *used);

#endif
