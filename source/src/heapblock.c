/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "heapblock.h"
#include "assert.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * This is a non-freeing allocator, used as a backend for malloc and for uncompressing data.
 *
 * Allocating 0 bytes is allowed, and guarantees "infinite" (until the end of RAM) space is
 * available at the returned pointer as long as no other malloc/heapblock calls occur, which is
 * useful as a buffer for unknown-length uncompressed data. A subsequent call with a size will then
 * actually reserve the block.
 */

static void *heap_base;
static void *heap_limit;
static uintptr_t pool_base, pool_cursor, pool_end;

void heapblock_init(void)
{
    void *top_of_kernel_data = (void *)cur_boot_args.top_of_kernel_data;

    heap_base = top_of_kernel_data;
    heapblock_alloc(0); // align base

    printf("Heap base: %p\n", heap_base);
}

void heapblock_set_limit(void *limit)
{
    heap_limit = limit;
    printf("Heap limit: %p (%ld MiB)\n", limit, ((u8 *)limit - (u8 *)heap_base) >> 20);
}

size_t heapblock_remaining(void)
{
    if (!heap_limit)
        return SIZE_MAX;
    return (uintptr_t)heap_limit > (uintptr_t)heap_base
               ? (uintptr_t)heap_limit - (uintptr_t)heap_base
               : 0;
}

void *heapblock_alloc(size_t size)
{
    return heapblock_alloc_aligned(size, 64);
}

void *heapblock_alloc_aligned(size_t size, size_t align)
{
    assert((align & (align - 1)) == 0);
    assert(heap_base);

    uintptr_t block = (((uintptr_t)heap_base) + align - 1) & ~(align - 1);

    if (heap_limit && block + size > (uintptr_t)heap_limit) {
        /* n5c: the extension pool (size 0 = "infinite" peeks stay on the primary heap) */
        uintptr_t pb = (pool_cursor + align - 1) & ~(align - 1);
        if (size && pool_end && pb >= pool_cursor && pb <= pool_end && size <= pool_end - pb) {
            pool_cursor = pb + size;
            return (void *)pb;
        }
        panic("heapblock: out of memory (%zu bytes at 0x%lx, limit %p; pool 0x%lx..0x%lx used to 0x%lx)\n",
              size, block, heap_limit, pool_base, pool_end, pool_cursor);
    }

    heap_base = (void *)(block + size);

    return (void *)block;
}

int heapblock_add_pool(void *base, size_t size)
{
    uintptr_t b = (uintptr_t)base, e = b + size;
    uintptr_t lo = (uintptr_t)cur_boot_args.phys_base, hi = (uintptr_t)heap_limit;
    /* once, after a limit, 16 KiB aligned, no wrap, clear of [phys_base, limit): iBoot's kernel-area data,
     * m1n1 and the whole primary heap */
    if (pool_end || !heap_limit || !size || ((b | size) & 0x3fff) || e < b || (b < hi && lo < e)) {
        printf("heapblock: pool 0x%lx+0x%zx refused\n", b, size);
        return -1;
    }
    pool_base = pool_cursor = b;
    pool_end = e;
    printf("heapblock: pool 0x%lx..0x%lx (%zu MiB) added; primary %zu MiB left\n", b, e, size >> 20,
           heapblock_remaining() >> 20);
    return 0;
}

size_t heapblock_total_remaining(void)
{
    size_t r = heapblock_remaining();
    if (r == SIZE_MAX)
        return r;
    return r + (pool_end - pool_cursor);
}

bool heapblock_pool_owned(u64 pa, u64 len)
{
    return pool_end && len && pa >= pool_base && pa <= pool_cursor && len <= pool_cursor - pa;
}

void heapblock_pool_info(u64 *base, u64 *size, u64 *used)
{
    *base = pool_base;
    *size = pool_end - pool_base;
    *used = pool_cursor - pool_base;
}

void *heapblock_peek(void)
{
    return (void *)(((uintptr_t)heap_base + 63) & ~(uintptr_t)63);
}
