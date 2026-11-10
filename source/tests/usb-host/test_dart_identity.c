/* SPDX-License-Identifier: MIT */
/*
 * Host unit test for src/dart_identity.c: the four-level identity tables (DVA == PA) EL2 gives a
 * guest-owned USB DART (m5pro-windows docs/USB-HOST.md). Checks every 16 KiB page of a synthetic
 * J716s-shaped guest RAM map (DRAM at 0x100_0000_0000, holes for m1n1, carveouts and the MTE tag
 * storage), the entry encoding against an independent formula (Aurora Mu 255a340: PA >> 4 in bits
 * 37:10, subpage 0..0xfff, valid), refusals, and that the pool never overlaps mapped RAM.
 *   make -C tests/usb-host && tests/usb-host/test_dart_identity
 */
#include "shim/dart_identity_host.h"
#include <sys/mman.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                \
            exit(1);                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)

static unsigned long checks;
static int frees;

#undef free
void free_hook(void *p)
{
    (void)p; /* pool memory comes from mmap below and is left mapped */
    frees++;
}

/*
 * The tables hold 42-bit physical addresses: keep the pool low, as m1n1's heap is (a sanitizer
 * heap lives near 2^46). mmap at a hint, never freed back (free_hook only counts).
 */
void *memalign(size_t align, size_t size)
{
    static uintptr_t hint = 0x300000000UL;
    size_t len = (size + align + 0xffff) & ~(size_t)0xffff;
    void *p = mmap((void *)hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    if (p == MAP_FAILED)
        return NULL;
    hint = (uintptr_t)p + len + 0x10000;
    uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    if (a + size > (1UL << 42)) {
        fprintf(stderr, "test: mmap gave 0x%lx, beyond 42 bits\n", (unsigned long)a);
        exit(1);
    }
    return (void *)a;
}

#define DRAM 0x10000000000UL
#define GiB  (1UL << 30)
#define MiB  (1UL << 20)

/* expected leaf entry, written independently of the firmware's macros */
static u64 leaf_pte(u64 pa)
{
    return ((pa >> 4) & 0x0000003FFFFFFC00UL) | (0xfffUL << 40) | 1;
}

static u64 table_pte(u64 pa)
{
    return ((pa >> 4) & 0x0000003FFFFFFC00UL) | 1;
}

static int in_ranges(const struct dart_range *r, unsigned n, u64 a)
{
    for (unsigned i = 0; i < n; i++)
        if (a >= r[i].base && a < r[i].base + r[i].size)
            return 1;
    return 0;
}

static void walk_all(const struct dart_identity *id, const struct dart_range *r, unsigned n,
                     u64 lo, u64 hi)
{
    for (u64 a = lo; a < hi; a += SZ_16K) {
        u64 pa = dart_identity_lookup(id, a + 0x123);
        if (in_ranges(r, n, a))
            CHECK(pa == a + 0x123);
        else
            CHECK(pa == DART_IDENTITY_UNMAPPED);
    }
}

static void test_j716s_shape(void)
{
    /* boot_args RAM split by m1n1 + heaps, two carveouts and the tag storage (synthetic values) */
    struct dart_range r[] = {
        {DRAM + 0x10e274000UL, 0x2c000UL},                 /* odd small piece */
        {DRAM + 0x10e400000UL, 9 * GiB + 0x1234UL * SZ_16K}, /* main bank */
        {DRAM + 16 * GiB, 20 * GiB},                         /* past a carveout */
        {DRAM + 63 * GiB, 2 * GiB},                          /* straddles a 64 GiB slot */
    };
    unsigned n = sizeof(r) / sizeof(r[0]);
    struct dart_identity id;

    CHECK(dart_identity_build(&id, r, n) == 0);
    CHECK(id.root == (u64)id.pool);
    CHECK(id.ntables >= 3);

    u64 pages = 0, leaves = 0;
    for (unsigned i = 0; i < n; i++)
        pages += r[i].size / SZ_16K;
    /* every 32 MiB slot touched costs one leaf, every 64 GiB slot one middle table */
    {
        u64 last = ~0UL;
        for (unsigned i = 0; i < n; i++)
            for (u64 a = r[i].base; a < r[i].base + r[i].size; a += SZ_16K)
                if (a >> 25 != last) {
                    last = a >> 25;
                    leaves++;
                }
    }
    CHECK(id.leaves == leaves);
    CHECK(id.ntables == 1 + 2 + leaves); /* two middle tables: slot 16 and slot 17 */

    /* entry encoding: walk by hand with the independent formula */
    const u64 *top = (const u64 *)id.root;
    for (unsigned i = 0; i < 2048; i++) {
        if (i == 16 || i == 17)
            CHECK(top[i] & 1);
        else
            CHECK(top[i] == 0);
    }
    u64 a = r[1].base + 777 * SZ_16K;
    u64 mid_pte = top[(a >> 36) & 0x7ff];
    const u64 *mid = (const u64 *)((mid_pte & 0x3FFFFFFC00UL) << 4);
    CHECK(mid_pte == table_pte((u64)mid));
    u64 leaf_tbl_pte = mid[(a >> 25) & 0x7ff];
    const u64 *leaf = (const u64 *)((leaf_tbl_pte & 0x3FFFFFFC00UL) << 4);
    CHECK(leaf_tbl_pte == table_pte((u64)leaf));
    CHECK(leaf[(a >> 14) & 0x7ff] == leaf_pte(a));

    /* every page of the map and its surroundings (holes, below DRAM, above the top) */
    walk_all(&id, r, n, DRAM, DRAM + 66 * GiB);
    CHECK(dart_identity_lookup(&id, 0x80000000UL) == DART_IDENTITY_UNMAPPED);
    CHECK(dart_identity_lookup(&id, 1UL << 42) == DART_IDENTITY_UNMAPPED);
    CHECK(dart_identity_lookup(&id, ~0UL) == DART_IDENTITY_UNMAPPED);

    /* the pool lies outside every mapped range (host heap: trivially; the firmware checks too) */
    for (unsigned i = 0; i < n; i++)
        CHECK(!((u64)id.pool < r[i].base + r[i].size &&
                r[i].base < (u64)id.pool + (u64)id.ntables * SZ_16K));
    printf("j716s shape: %lu pages, %u tables (%u leaves), %lu KiB of tables\n", pages, id.ntables,
           id.leaves, (u64)id.ntables * 16);

    int f = frees;
    dart_identity_free(&id);
    CHECK(frees == f + 1 && id.pool == NULL && id.root == 0);
}

static void test_refusals(void)
{
    struct dart_identity id;
    struct dart_range bad_align[] = {{DRAM + 0x1000, 16 * MiB}};
    struct dart_range bad_size[] = {{DRAM, 16 * MiB + 0x2000}};
    struct dart_range empty[] = {{DRAM, 0}};
    struct dart_range unsorted[] = {{DRAM + GiB, MiB}, {DRAM, MiB}};
    struct dart_range overlap[] = {{DRAM, 2 * MiB}, {DRAM + MiB, MiB}};
    struct dart_range too_high[] = {{(1UL << 42) - MiB, 2 * MiB}};
    struct dart_range wraps[] = {{~0UL - SZ_16K + 1, 2 * SZ_16K}};
    struct dart_range too_big[] = {{DRAM, 200 * GiB}};
    struct dart_range many[65];

    for (int i = 0; i < 65; i++)
        many[i] = (struct dart_range){DRAM + (u64)i * 4 * MiB, MiB};

    CHECK(dart_identity_build(&id, bad_align, 1) < 0 && id.root == 0);
    CHECK(dart_identity_build(&id, bad_size, 1) < 0);
    CHECK(dart_identity_build(&id, empty, 1) < 0);
    CHECK(dart_identity_build(&id, unsorted, 2) < 0);
    CHECK(dart_identity_build(&id, overlap, 2) < 0);
    CHECK(dart_identity_build(&id, too_high, 1) < 0);
    CHECK(dart_identity_build(&id, wraps, 1) < 0);
    CHECK(dart_identity_build(&id, too_big, 1) < 0);
    CHECK(dart_identity_build(&id, many, 65) < 0);
    CHECK(dart_identity_build(&id, NULL, 1) < 0);
    CHECK(dart_identity_build(&id, many, 0) < 0);
    CHECK(dart_identity_build(&id, many, 64) == 0);
    CHECK(dart_identity_lookup(&id, many[63].base) == many[63].base);
    CHECK(dart_identity_lookup(&id, many[63].base + MiB) == DART_IDENTITY_UNMAPPED);
    dart_identity_free(&id);
    CHECK(dart_identity_lookup(&id, DRAM) == DART_IDENTITY_UNMAPPED);
}

/* adjacent ranges (as m5w,memory may split around a 16 KiB hole) and a single page */
static void test_edges(void)
{
    struct dart_range r[] = {
        {DRAM + 32 * MiB - SZ_16K, SZ_16K},     /* last page of a leaf */
        {DRAM + 32 * MiB, SZ_16K},              /* first page of the next, adjacent */
        {DRAM + 64 * MiB + SZ_16K, 3 * SZ_16K}, /* hole of one page before it */
    };
    struct dart_identity id;

    CHECK(dart_identity_build(&id, r, 3) == 0);
    CHECK(id.leaves == 3 && id.ntables == 5);
    walk_all(&id, r, 3, DRAM, DRAM + 128 * MiB);
    dart_identity_free(&id);
}

int main(void)
{
    test_j716s_shape();
    test_refusals();
    test_edges();
    printf("PASS: %lu checks\n", checks);
    return 0;
}
