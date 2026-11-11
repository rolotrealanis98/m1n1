/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_PMU_4K_H
#define HV_WIN_PMU_4K_H

#include "types.h"

#define PMU_FRAME_SIZE 0x1000UL
#define PMU_FRAME_WORDS (PMU_FRAME_SIZE / 4)
/* hv_win_pmu_host_frame()'s per-frame view of the block state byte (host tests) */
#define PMU_FRAME_EXECUTED 1U
#define PMU_FRAME_SCANNED 2U
#define PMU_FRAME_LISTED 0x80U /* first frame of a block only: block is on the open list */

/* Small, conservative stage-1 execute-permission walk. Unsupported formats deny scanning.
 * read64 must accept only normal guest RAM and must not perform device reads.
 * Caller must exclude concurrent guest/device writers for the walk AND the rewrite. */
struct pmu_s1 {
    u64 sctlr, tcr, ttbr[2];
};
typedef bool pmu_s1_read_fn(void *arg, u64 ipa, u64 *value);

static inline bool pmu_s1_exec(const struct pmu_s1 *s, u64 va, u64 pstate,
                               pmu_s1_read_fn *read64, void *arg, u64 *ipa)
{
    u32 mode = pstate & 0x1f;
    if ((mode != 0 && mode != 4 && mode != 5) || (va & 3))
        return false;
    bool user = mode == 0;
    if (s->sctlr & (BIT(25) | BIT(24))) /* only little-endian guest data/table walks */
        return false;
    if (!(s->sctlr & BIT(0))) {
        *ipa = va;
        return true;
    }
    /* No LPA2 or E0PD. HPD0/HPD1 are honoured below (Windows sets both: TCR_EL1
     * 0x615b5513511 on hardware); HWU bits only re-purpose descriptor bits 59-62,
     * which the leaf check below never reads. */
    if ((s->tcr & (BIT(59) | BIT(55) | BIT(56))) || ((s->tcr >> 32) & 7) > 5)
        return false;
    bool upper = !!(va & BIT(55));
    u32 sz = upper ? (s->tcr >> 16) & 63 : s->tcr & 63;
    u32 tg = upper ? (s->tcr >> 30) & 3 : (s->tcr >> 14) & 3;
    if (sz < 16 || sz > 39 || tg != (upper ? 2U : 0U) ||
        (s->tcr & BIT(upper ? 23 : 7)))
        return false;
    if ((s->tcr & BIT(upper ? 38 : 37)) && !(s->tcr & BIT(upper ? 52 : 51)))
        va = upper ? va | (0xffUL << 56) : va & ~(0xffUL << 56);
    u32 bits = 64 - sz;
    u64 mask = (1UL << bits) - 1;
    if ((va & ~mask) != (upper ? ~mask : 0))
        return false;
    u32 start = 4 - (bits - 12 + 8) / 9;
    /* The root table holds 2^(bits - shift) entries, so it can be smaller than 4 KiB
     * (Windows: T1SZ 17, a 2 KiB level-0 table, TTBR1_EL1 0x...e800). BADDR is
     * aligned to the table size (at least 64 bytes); the bits below it are RES0.
     * ASID and CnP are ignored. */
    u32 root_align = bits - (12 + 9 * (3 - start)) + 3;
    if (root_align < 6)
        root_align = 6;
    u64 table = s->ttbr[upper] & 0x0000fffffffffffeUL & ~((1UL << root_align) - 1);
    if (s->ttbr[upper] & ((1UL << root_align) - 1) & ~1UL)
        return false;
    /* HPD: APTable, PXNTable and UXNTable are ignored for this half. */
    bool hpd = !!(s->tcr & BIT(upper ? 42 : 41));
    bool pxn = false, uxn = false, no_user = false, ro = false;
    for (u32 level = start; level <= 3; level++) {
        u32 shift = 12 + 9 * (3 - level);
        u32 index_bits = level == start ? bits - shift : 9;
        u64 d, index = (va >> shift) & ((1UL << index_bits) - 1);
        if (!read64(arg, table + index * 8, &d) || !(d & 1) ||
            (d & (3UL << 48)))
            return false;
        if (level < 3 && (d & 2)) {
            if (!hpd) {
                pxn |= !!(d & BIT(59));
                uxn |= !!(d & BIT(60));
                no_user |= !!(d & BIT(61));
                ro |= !!(d & BIT(62));
            }
            table = d & 0x0000fffffffff000UL;
            continue;
        }
        if (level == 0 || (level == 3 && !(d & 2)) || !(d & BIT(10)))
            return false;
        u64 offset = (1UL << shift) - 1;
        u64 output = d & 0x0000fffffffff000UL;
        if (output & offset)
            return false;
        /* Do not infer RO from AP when hardware dirty management may clear it. */
        if ((s->tcr & BIT(40)) && (d & BIT(51)))
            return false;
        ro |= !!(d & BIT(7));
        bool el0 = !!(d & BIT(6)) && !no_user;
        /* Conservative extra restriction: never patch an EL0-writable mapping at EL1. */
        if ((user && (!el0 || uxn || (d & BIT(54)))) ||
            (!user && (pxn || (d & BIT(53)))) ||
            ((s->sctlr & BIT(19)) && !ro) ||
            (!user && el0 && !ro))
            return false;
        *ipa = output | (va & offset);
        return true;
    }
    return false;
}

#endif
