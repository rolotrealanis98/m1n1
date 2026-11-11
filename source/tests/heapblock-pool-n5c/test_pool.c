/* SPDX-License-Identifier: MIT */
/*
 * n5c: src/heapblock.c's extension pool, built for the host. The primary heap's peek / alloc(0) / remaining
 * (used by the storage heap proofs and the framebuffer shadow) must not change; a request that does not fit
 * below the limit goes to the pool; the pool is registered once, aligned, clear of the primary heap and of
 * m1n1 below it; ownership covers exactly the allocated part.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "heapblock.h"
#include "xnuboot.h"

struct boot_args cur_boot_args;
static jmp_buf panicked;
static unsigned long checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int expect_panic;
void host_panic(const char *fmt, ...)
{
    (void)fmt;
    if (!expect_panic) {
        fprintf(stderr, "FAIL: unexpected heapblock panic\n");
        exit(1);
    }
    expect_panic = 0;
    longjmp(panicked, 1);
}
#define PANICS(stmt) do { checks++; expect_panic = 1; if (!setjmp(panicked)) { stmt; CHECK(0); } CHECK(!expect_panic); } while (0)

#define MiB (1UL << 20)
static u8 *ram;

int main(void)
{
    ram = aligned_alloc(1 << 20, 64 * MiB);
    u64 base = (u64)ram;
    cur_boot_args.phys_base = base;
    cur_boot_args.top_of_kernel_data = base + 1 * MiB + 100; /* m1n1 image below */
    heapblock_init();
    u64 start = (u64)heapblock_peek();
    CHECK(start == ((base + MiB + 100 + 63) & ~63UL));
    u64 limit = start + 4 * MiB;
    u64 pb, ps, pu;
    CHECK(heapblock_add_pool((void *)(base + 32 * MiB), 8 * MiB) == -1); /* no limit yet */
    heapblock_set_limit((void *)limit);
    CHECK(heapblock_remaining() == 4 * MiB && heapblock_total_remaining() == 4 * MiB);
    /* refusals: misaligned, empty, wrapping, overlapping the primary heap, below the limit over m1n1 */
    CHECK(heapblock_add_pool((void *)(base + 32 * MiB + 4096), 8 * MiB) == -1);
    CHECK(heapblock_add_pool((void *)(base + 32 * MiB), 4096) == -1);
    CHECK(heapblock_add_pool((void *)(base + 32 * MiB), 0) == -1);
    CHECK(heapblock_add_pool((void *)(~0UL & ~0x3fffUL), 1UL << 20) == -1);
    CHECK(heapblock_add_pool((void *)(limit & ~0x3fffUL), 8 * MiB) == -1);
    CHECK(heapblock_add_pool((void *)base, 16384) == -1);
    heapblock_pool_info(&pb, &ps, &pu);
    CHECK(ps == 0 && !heapblock_pool_owned(base + 32 * MiB, 64));
    /* without a pool an over-limit request panics as before */
    PANICS(heapblock_alloc(5 * MiB));
    CHECK(heapblock_add_pool((void *)(base + 32 * MiB), 8 * MiB) == 0);
    CHECK(heapblock_add_pool((void *)(base + 48 * MiB), 8 * MiB) == -1); /* once */
    CHECK(heapblock_total_remaining() == 12 * MiB);
    /* primary first */
    u64 a = (u64)heapblock_alloc(3 * MiB);
    CHECK(a == start && heapblock_remaining() == MiB && (u64)heapblock_peek() == start + 3 * MiB);
    /* does not fit below the limit: the pool, and the primary view is unchanged */
    u64 b = (u64)heapblock_alloc_aligned(2 * MiB, 16384);
    CHECK(b == base + 32 * MiB && heapblock_remaining() == MiB && (u64)heapblock_peek() == start + 3 * MiB);
    CHECK((u64)heapblock_alloc(0) == start + 3 * MiB); /* alloc(0) stays primary */
    CHECK(heapblock_total_remaining() == MiB + 6 * MiB);
    CHECK(heapblock_pool_owned(b, 2 * MiB) && !heapblock_pool_owned(b, 2 * MiB + 1) &&
          !heapblock_pool_owned(b + 2 * MiB, 1) && !heapblock_pool_owned(a, 64));
    /* small requests still fit the primary heap */
    u64 c = (u64)heapblock_alloc(MiB / 2);
    CHECK(c == start + 3 * MiB);
    u64 d = (u64)heapblock_alloc_aligned(MiB, 1 << 20); /* primary short: pool, aligned */
    CHECK(d == base + 34 * MiB && !(d & (MiB - 1)));
    heapblock_pool_info(&pb, &ps, &pu);
    CHECK(pb == base + 32 * MiB && ps == 8 * MiB && pu == 3 * MiB);
    /* both exhausted: panic */
    PANICS(heapblock_alloc(6 * MiB));
    CHECK(heapblock_alloc(5 * MiB) == (void *)(base + 35 * MiB)); /* exactly to the pool's end */
    heapblock_pool_info(&pb, &ps, &pu);
    CHECK(pu == 8 * MiB && heapblock_total_remaining() == heapblock_remaining());
    heapblock_alloc(heapblock_remaining() - 8); /* primary: 8 bytes left */
    PANICS(heapblock_alloc(64)); /* neither fits: panic, nothing past the pool */
    /* a size-0 peek past the limit never moves into the pool */
    PANICS(heapblock_alloc_aligned(0, 16384));
    printf("heapblock-pool-n5c: %lu checks, 0 failed\n", checks);
    free(ram);
    return 0;
}
