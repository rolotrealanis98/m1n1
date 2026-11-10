/* SPDX-License-Identifier: MIT */
/*
 * PMUv3 instruction rewriter: classification and in-place rewrite of instruction words.
 * See hv_win_pmu_rewrite.h; the stage-2 side is hv_win_pmu.c.
 */

#include "hv_win_pmu_rewrite.h"

#define RT(insn)  ((insn) & 0x1fU)
#define OP1(insn) (((insn) >> 16) & 7U)
#define CRN(insn) (((insn) >> 12) & 0xfU)
#define CRM(insn) (((insn) >> 8) & 0xfU)
#define OP2(insn) (((insn) >> 5) & 7U)

enum pmu_rw_kind pmu_rw_classify(u32 insn)
{
    if ((insn & PMU_RW_SYSREG_MASK) != PMU_RW_SYSREG_OP03)
        return PMU_RW_NONE;
    if (!pmu_rw_is_pmu_reg(OP1(insn), CRN(insn), CRM(insn)))
        return PMU_RW_NONE;
    if (!(insn & PMU_RW_SYSREG_L))
        return PMU_RW_WRITE;
    /* PMCCNTR_EL0 = (3, 3, 9, 13, 0) */
    if (OP1(insn) == 3 && CRN(insn) == 9 && CRM(insn) == 13 && OP2(insn) == 0)
        return PMU_RW_READ_PMCCNTR;
    return PMU_RW_READ_OTHER;
}

u32 pmu_rw_replacement(u32 insn)
{
    switch (pmu_rw_classify(insn)) {
        case PMU_RW_READ_PMCCNTR:
            return PMU_RW_INSN_MRS_CNTVCT | RT(insn);
        case PMU_RW_READ_OTHER:
            /* Rt = 31 is XZR for both MRS and MOVZ: a no-op either way */
            return PMU_RW_INSN_MOVZ_ZERO | RT(insn);
        case PMU_RW_WRITE:
            return PMU_RW_INSN_NOP;
        default:
            return insn;
    }
}

u32 pmu_rw_rewrite_words(u32 *words, u32 n, pmu_rw_record_fn *rec, void *arg, u64 *counts)
{
    u32 done = 0;

    for (u32 i = 0; i < n; i++) {
        u32 old = words[i];

        /* cheap filter first: almost every word fails it */
        if ((old & PMU_RW_SYSREG_MASK) != PMU_RW_SYSREG_OP03)
            continue;

        enum pmu_rw_kind kind = pmu_rw_classify(old);
        if (kind == PMU_RW_NONE)
            continue;

        u32 new = pmu_rw_replacement(old);
        *(volatile u32 *)&words[i] = new; /* one aligned, single-copy atomic 32-bit store */
        done++;
        if (counts)
            counts[kind]++;
        if (rec)
            rec(arg, i, old, new);
    }

    return done;
}
