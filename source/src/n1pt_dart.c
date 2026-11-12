/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * n1pt: offset translation tables for the N1 DMA pool on dart-apcie0 (see n1pt_dart.h). Adapted from
 * dart_identity.c (same entry format, same four-level shape) for one IOVA != PA range; the T8110 register
 * offsets are those of dart.c. Host-tested in tests/win-n1pt against a register model of the DART.
 */

#include "types.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"
#include "n1pt_dart.h"

#define N1D_PTE_OFFSET   GENMASK(39, 10)
#define N1D_PTE_SHIFT    14
#define N1D_PTE_SP_START GENMASK(63, 52)
#define N1D_PTE_SP_END   GENMASK(51, 40)
#define N1D_PTE_VALID    BIT(0)
#define N1D_ENTRIES      (SZ_16K / 8)
#define N1D_SPAN_LEAF    (1ULL << 25)
#define N1D_SPAN_MID     (1ULL << 36)
#define N1D_MAX_PA       (1ULL << 42)
#define N1D_DOORBELL     0xfffff000ULL

/* T8110 registers (dart.c) */
#define N1D_PARAMS3            0x08
#define N1D_PARAMS3_VA_WIDTH   GENMASK(21, 16)
#define N1D_PARAMS4            0x0c
#define N1D_PARAMS4_NUM_SIDS   GENMASK(8, 0)
#define N1D_TLB_CMD            0x80
#define N1D_TLB_CMD_BUSY       BIT(31)
#define N1D_TLB_CMD_OP         GENMASK(10, 8)
#define N1D_ERROR              0x100
#define N1D_PROTECT            0x200
#define N1D_PROTECT_TTBR_TCR   BIT(0)
#define N1D_ENABLE_STREAMS     0xc00
#define N1D_DISABLE_STREAMS    0xc20
#define N1D_TCR(sid)           (0x1000 + 4 * (sid))
#define N1D_TCR_TRANSLATE      BIT(0)
#define N1D_TCR_FOUR_LEVEL     BIT(3)
#define N1D_TTBR(sid)          (0x1400 + 4 * (sid))
#define N1D_TTBR_VALID         BIT(0)
#define N1D_TTBR_ADDR          GENMASK(29, 2)

static u64 n1d_table_pte(const u64 *table)
{
    return FIELD_PREP(N1D_PTE_OFFSET, ((u64)table) >> N1D_PTE_SHIFT) | N1D_PTE_VALID;
}

static u64 n1d_leaf_pte(u64 pa)
{
    return FIELD_PREP(N1D_PTE_OFFSET, pa >> N1D_PTE_SHIFT) | FIELD_PREP(N1D_PTE_SP_END, 0xfff) |
           FIELD_PREP(N1D_PTE_SP_START, 0) | N1D_PTE_VALID;
}

static u64 *n1d_next(u64 pte)
{
    return (u64 *)(FIELD_GET(N1D_PTE_OFFSET, pte) << N1D_PTE_SHIFT);
}

int n1pt_dart_build(struct n1pt_dart_tables *t, u64 iova, u64 pa, u64 size)
{
    memset(t, 0, sizeof(*t));
    if (!size || ((iova | pa | size) & (SZ_16K - 1)) || iova + size < iova || iova + size > N1D_SPAN_MID ||
        pa + size < pa || pa + size > N1D_MAX_PA || (iova <= N1D_DOORBELL && N1D_DOORBELL < iova + size)) {
        printf("n1pt dart: range 0x%lx+0x%lx -> 0x%lx refused\n", iova, size, pa);
        return -1;
    }
    u64 first = iova / N1D_SPAN_LEAF, last = (iova + size - 1) / N1D_SPAN_LEAF;
    u32 tables = 2 + (u32)(last - first + 1);
    if (tables > N1PT_DART_MAX_TABLES) {
        printf("n1pt dart: %u tables needed\n", tables);
        return -1;
    }
    u64 *pool = memalign(SZ_16K, tables * SZ_16K);
    if (!pool)
        return -1;
    memset(pool, 0, tables * SZ_16K);
    if ((u64)pool + tables * SZ_16K > N1D_MAX_PA ||
        ((u64)pool < pa + size && pa < (u64)pool + tables * SZ_16K)) {
        printf("n1pt dart: tables at 0x%lx unusable\n", (u64)pool);
        free(pool);
        return -1;
    }
    u64 *top = pool, *mid = pool + N1D_ENTRIES;
    top[0] = n1d_table_pte(mid); /* DVA[46:36] == 0 for every IOVA below 2^36 */
    u32 used = 2;
    for (u64 dva = iova, p = pa; dva < iova + size; dva += SZ_16K, p += SZ_16K) {
        u32 i1 = (dva >> 25) & 0x7ff, i2 = (dva >> 14) & 0x7ff;
        if (!(mid[i1] & N1D_PTE_VALID)) {
            if (used >= tables) {
                free(pool);
                return -1;
            }
            mid[i1] = n1d_table_pte(pool + used++ * N1D_ENTRIES);
        }
        n1d_next(mid[i1])[i2] = n1d_leaf_pte(p);
    }
    dma_wmb();
    t->pool = pool;
    t->ntables = used;
    t->iova = iova;
    t->pa = pa;
    t->size = size;
    t->root4 = (u64)top;
    t->root3 = (u64)mid;
    return 0;
}

void n1pt_dart_free(struct n1pt_dart_tables *t)
{
    if (t->pool)
        free(t->pool);
    memset(t, 0, sizeof(*t));
}

u64 n1pt_dart_lookup(const struct n1pt_dart_tables *t, u64 dva, int four_level)
{
    u64 pte;
    const u64 *mid;

    if (!t->root4)
        return N1PT_DART_UNMAPPED;
    if (four_level) {
        if (dva >= N1D_MAX_PA << 4)
            return N1PT_DART_UNMAPPED;
        pte = ((const u64 *)t->root4)[(dva >> 36) & 0x7ff];
        if (!(pte & N1D_PTE_VALID))
            return N1PT_DART_UNMAPPED;
        mid = n1d_next(pte);
    } else {
        if (dva >= N1D_SPAN_MID)
            return N1PT_DART_UNMAPPED;
        mid = (const u64 *)t->root3;
    }
    pte = mid[(dva >> 25) & 0x7ff];
    if (!(pte & N1D_PTE_VALID))
        return N1PT_DART_UNMAPPED;
    pte = n1d_next(pte)[(dva >> 14) & 0x7ff];
    if (!(pte & N1D_PTE_VALID))
        return N1PT_DART_UNMAPPED;
    return (FIELD_GET(N1D_PTE_OFFSET, pte) << N1D_PTE_SHIFT) | (dva & (SZ_16K - 1));
}

static int n1d_flush(uintptr_t base)
{
    u32 i;
    for (i = 0; i < 1000 && (read32(base + N1D_TLB_CMD) & N1D_TLB_CMD_BUSY); i++)
        udelay(1);
    if (i == 1000)
        return -1;
    dma_wmb();
    write32(base + N1D_TLB_CMD, FIELD_PREP(N1D_TLB_CMD_OP, 0)); /* flush all */
    for (i = 0; i < 1000 && (read32(base + N1D_TLB_CMD) & N1D_TLB_CMD_BUSY); i++)
        udelay(1);
    return i == 1000 ? -1 : 0;
}

int n1pt_dart_block(uintptr_t base, u32 sid_mask)
{
    for (u32 sid = 0; sid < 32; sid++) {
        if (!(sid_mask & BIT(sid)))
            continue;
        write32(base + N1D_TCR(sid), 0);
        write32(base + N1D_TTBR(sid), 0);
    }
    write32(base + N1D_DISABLE_STREAMS, sid_mask);
    return n1d_flush(base);
}

int n1pt_dart_attach(uintptr_t base, const struct n1pt_dart_tables *t, u32 sid_mask, int *four_level)
{
    u32 nsid = FIELD_GET(N1D_PARAMS4_NUM_SIDS, read32(base + N1D_PARAMS4));
    u32 vaw = FIELD_GET(N1D_PARAMS3_VA_WIDTH, read32(base + N1D_PARAMS3));
    u32 need = 64 - __builtin_clzll(t->iova + t->size - 1);

    if (!t->root4 || !sid_mask || (sid_mask & 1) || nsid > 32 || (sid_mask >> nsid) || vaw < need) {
        printf("n1pt dart: refused (streams %u, VA width %u, need %u)\n", nsid, vaw, need);
        return -1;
    }
    if (read32(base + N1D_PROTECT) & N1D_PROTECT_TTBR_TCR) {
        printf("n1pt dart: instance locked\n");
        return -1;
    }
    if (read32(base + N1D_ERROR)) {
        printf("n1pt dart: error 0x%x pending\n", read32(base + N1D_ERROR));
        return -1;
    }
    int four = 1;
    for (int pass = 0; pass < 2; pass++) {
        u64 root = four ? t->root4 : t->root3;
        u32 ttbr = N1D_TTBR_VALID | FIELD_PREP(N1D_TTBR_ADDR, root >> N1D_PTE_SHIFT);
        u32 tcr = N1D_TCR_TRANSLATE | (four ? N1D_TCR_FOUR_LEVEL : 0);
        if ((FIELD_GET(N1D_TTBR_ADDR, ttbr) << N1D_PTE_SHIFT) != root)
            return -1;
        for (u32 sid = 1; sid < 32; sid++) {
            if (!(sid_mask & BIT(sid)))
                continue;
            write32(base + N1D_TCR(sid), 0);
            write32(base + N1D_TTBR(sid), ttbr);
        }
        write32(base + N1D_ENABLE_STREAMS, sid_mask);
        if (n1d_flush(base))
            break;
        bool ok = true;
        for (u32 sid = 1; sid < 32 && ok; sid++) {
            if (!(sid_mask & BIT(sid)))
                continue;
            write32(base + N1D_TCR(sid), tcr);
            ok = read32(base + N1D_TCR(sid)) == tcr && read32(base + N1D_TTBR(sid)) == ttbr;
        }
        if (ok && !n1d_flush(base) && !read32(base + N1D_ERROR)) {
            *four_level = four;
            return 0;
        }
        if (!four)
            break;
        four = 0; /* FOUR_LEVEL did not stick: the middle table as a 3-level root */
    }
    n1pt_dart_block(base, sid_mask);
    printf("n1pt dart: streams 0x%x refuse the tables\n", sid_mask);
    return -1;
}
