/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Identity translation tables (DVA == PA) for T8110-type DARTs, for masters handed to a guest
 * that knows nothing about DARTs (m5pro-windows docs/USB-HOST.md). No MMIO here: dart.c attaches
 * the tables to an instance. Host-tested in tests/usb-host.
 *
 * Table format (DART2, as dart_t8110 in dart.c and Linux io-pgtable-dart): 16 KiB tables of 2048
 * 64-bit entries; an entry holds PA[43:14] in bits 39:10 and VALID in bit 0; a leaf entry also
 * carries the subpage range 0..0xfff (bits 63:52 start, 51:40 end), i.e. the whole page. With
 * TCR.FOUR_LEVEL the TTBR points at a top table indexed by DVA[46:36], then DVA[35:25], then the
 * leaf by DVA[24:14] (three table levels plus the TTBR, Apple's "four level"). Three levels reach
 * only 64 GiB, and DRAM on T6050/T8142 starts at 0x100_0000_0000. Aurora's Mu builds the same
 * shape for J813's usb DART (aurora-silicon/mu 255a340, "AppleDart: identity-map the usb DART,
 * which cannot bypass on T8142").
 */

#ifdef DART_IDENTITY_HOST_TEST
#include "dart_identity_host.h" /* tests/usb-host */
#else
#include "dart.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#endif

#define DART_IDENT_PTE_OFFSET   GENMASK(39, 10)
#define DART_IDENT_PTE_SHIFT    14
#define DART_IDENT_PTE_SP_START GENMASK(63, 52)
#define DART_IDENT_PTE_SP_END   GENMASK(51, 40)
#define DART_IDENT_PTE_VALID    BIT(0)

#define DART_IDENT_SPAN_LEAF  (1ULL << 25) /* one leaf table maps 32 MiB */
#define DART_IDENT_SPAN_MID   (1ULL << 36) /* one middle table maps 64 GiB */
#define DART_IDENT_MAX_PA     (1ULL << 42)
#define DART_IDENT_MAX_TABLES 4096 /* 64 MiB of tables, about 128 GiB of RAM */
#define DART_IDENT_ENTRIES    (SZ_16K / 8)

static u64 dart_ident_table_pte(void *table)
{
    return FIELD_PREP(DART_IDENT_PTE_OFFSET, ((u64)table) >> DART_IDENT_PTE_SHIFT) |
           DART_IDENT_PTE_VALID;
}

static u64 dart_ident_leaf_pte(u64 pa)
{
    return FIELD_PREP(DART_IDENT_PTE_OFFSET, pa >> DART_IDENT_PTE_SHIFT) |
           FIELD_PREP(DART_IDENT_PTE_SP_END, 0xfff) | FIELD_PREP(DART_IDENT_PTE_SP_START, 0) |
           DART_IDENT_PTE_VALID;
}

static u64 *dart_ident_next(u64 pte)
{
    return (u64 *)(FIELD_GET(DART_IDENT_PTE_OFFSET, pte) << DART_IDENT_PTE_SHIFT);
}

static int dart_ident_check_ranges(const struct dart_range *ranges, u32 count)
{
    if (!ranges || !count || count > 64)
        return -1;

    for (u32 i = 0; i < count; i++) {
        u64 base = ranges[i].base, size = ranges[i].size;

        if (!size || (base | size) & (SZ_16K - 1) || base + size < base ||
            base + size > DART_IDENT_MAX_PA)
            return -1;
        if (i && base < ranges[i - 1].base + ranges[i - 1].size)
            return -1; /* unsorted or overlapping */
    }
    return 0;
}

/* Tables the ranges need: one top, one middle table per 64 GiB slot, one leaf per 32 MiB slot. */
static u64 dart_ident_count(const struct dart_range *ranges, u32 count, u32 *leaves)
{
    u64 last_mid = ~0ULL, last_leaf = ~0ULL, mids = 0, leafs = 0;

    for (u32 i = 0; i < count; i++) {
        u64 end = ranges[i].base + ranges[i].size;

        for (u64 dva = ranges[i].base; dva < end; dva = (dva | (DART_IDENT_SPAN_LEAF - 1)) + 1) {
            if (dva / DART_IDENT_SPAN_MID != last_mid) {
                last_mid = dva / DART_IDENT_SPAN_MID;
                mids++;
            }
            if (dva / DART_IDENT_SPAN_LEAF != last_leaf) {
                last_leaf = dva / DART_IDENT_SPAN_LEAF;
                leafs++;
            }
        }
    }
    *leaves = leafs;
    return 1 + mids + leafs;
}

int dart_identity_build(struct dart_identity *id, const struct dart_range *ranges, u32 count)
{
    u32 leaves;

    memset(id, 0, sizeof(*id));
    if (dart_ident_check_ranges(ranges, count)) {
        printf("dart: identity map: ranges must be 1..64, sorted, disjoint, 16 KiB aligned, below "
               "2^42\n");
        return -1;
    }

    u64 tables = dart_ident_count(ranges, count, &leaves);
    if (tables > DART_IDENT_MAX_TABLES) {
        printf("dart: identity map: %lu tables is more than the %u allowed\n", tables,
               DART_IDENT_MAX_TABLES);
        return -1;
    }

    /* One contiguous pool: 1350 separate 16 KiB-aligned allocations would fragment the heap. */
    u64 *pool = memalign(SZ_16K, tables * SZ_16K);
    if (!pool) {
        printf("dart: identity map: no memory for %lu tables\n", tables);
        return -1;
    }
    memset(pool, 0, tables * SZ_16K);

    u64 *top = pool;
    u32 used = 1;

    for (u32 i = 0; i < count; i++) {
        u64 dva = ranges[i].base, end = ranges[i].base + ranges[i].size;

        while (dva < end) {
            u32 i0 = (dva >> 36) & 0x7ff, i1 = (dva >> 25) & 0x7ff, i2 = (dva >> 14) & 0x7ff;

            if (!(top[i0] & DART_IDENT_PTE_VALID)) {
                if (used >= tables)
                    goto overflow;
                top[i0] = dart_ident_table_pte(pool + used++ * DART_IDENT_ENTRIES);
            }
            u64 *mid = dart_ident_next(top[i0]);
            if (!(mid[i1] & DART_IDENT_PTE_VALID)) {
                if (used >= tables)
                    goto overflow;
                mid[i1] = dart_ident_table_pte(pool + used++ * DART_IDENT_ENTRIES);
            }
            u64 *leaf = dart_ident_next(mid[i1]);
            for (; i2 < 2048 && dva < end; i2++, dva += SZ_16K)
                leaf[i2] = dart_ident_leaf_pte(dva);
        }
    }

    /* Table entries hold PA[43:14]; the DART's output address is 42 bits wide. */
    if ((u64)pool + tables * SZ_16K > DART_IDENT_MAX_PA) {
        printf("dart: identity map: tables at 0x%lx are beyond the DART's 42-bit reach\n",
               (u64)pool);
        free(pool);
        return -1;
    }

    /* The master must not be able to rewrite its own translation. */
    for (u32 i = 0; i < count; i++) {
        u64 b = ranges[i].base, e = b + ranges[i].size;
        if ((u64)pool < e && b < (u64)pool + tables * SZ_16K) {
            printf("dart: identity map: the tables at 0x%lx overlap mapped RAM 0x%lx..0x%lx\n",
                   (u64)pool, b, e);
            free(pool);
            return -1;
        }
    }

    dma_wmb();
    id->pool = pool;
    id->ntables = used;
    id->leaves = leaves;
    id->root = (u64)pool;
    return 0;

overflow:
    printf("dart: identity map: table count mismatch (bug)\n");
    free(pool);
    return -1;
}

void dart_identity_free(struct dart_identity *id)
{
    if (id->pool)
        free(id->pool);
    memset(id, 0, sizeof(*id));
}

u64 dart_identity_lookup(const struct dart_identity *id, u64 dva)
{
    if (!id->root || dva >= DART_IDENT_MAX_PA)
        return DART_IDENTITY_UNMAPPED;

    const u64 *top = (const u64 *)id->root;
    u64 pte = top[(dva >> 36) & 0x7ff];
    if (!(pte & DART_IDENT_PTE_VALID))
        return DART_IDENTITY_UNMAPPED;
    pte = dart_ident_next(pte)[(dva >> 25) & 0x7ff];
    if (!(pte & DART_IDENT_PTE_VALID))
        return DART_IDENTITY_UNMAPPED;
    pte = dart_ident_next(pte)[(dva >> 14) & 0x7ff];
    if (!(pte & DART_IDENT_PTE_VALID))
        return DART_IDENTITY_UNMAPPED;
    return (FIELD_GET(DART_IDENT_PTE_OFFSET, pte) << DART_IDENT_PTE_SHIFT) | (dva & (SZ_16K - 1));
}
