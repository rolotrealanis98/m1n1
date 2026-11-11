/* SPDX-License-Identifier: MIT */
/*
 * PMUv3 instruction rewriter for the Windows EL2 runtime (m5pro-windows docs/PMU-EL2-REWRITE.md).
 *
 * The T6050 has no PMUv3 and its EL1 PMU accesses UNDEF at EL1 before any EL2 trap (smoke run
 * 2026-11-08), while Windows reads PMCCNTR_EL0 unconditionally. When the host turns the EL2 rewrite
 * on, guest RAM starts non-executable in stage 2. After proving a faulting 4 KiB frame executable
 * in stage 1, EL2 scans only that frame for PMUv3 accesses (guarded below). Stage-2 permissions
 * still cover 16 KiB, so unobserved sibling execution is possible; see hv_win_pmu.c. Replacements:
 *
 *   mrs Xt, PMCCNTR_EL0      ->  mrs Xt, CNTVCT_EL0  (monotonic, as EL2's trap emulation answers)
 *   mrs Xt, <other PMU reg>  ->  movz Xt, #0         (RAZ: PMCR_EL0.N = 0, nothing implemented)
 *   msr <PMU reg>, Xt        ->  nop                 (WI)
 *
 * Pure functions on instruction words, no m1n1 state: tests/win-pmu-rewrite builds this file's .c
 * unchanged on the host.
 */

#ifndef HV_WIN_PMU_REWRITE_H
#define HV_WIN_PMU_REWRITE_H

#include "types.h"

/* MRS/MSR (register) with op0 = 3: 1101010100 L 11 op1 CRn CRm op2 Rt (L = bit 21: 1 = MRS) */
#define PMU_RW_SYSREG_MASK 0xffd80000U
#define PMU_RW_SYSREG_OP03 0xd5180000U
#define PMU_RW_SYSREG_L    (1U << 21)

#define PMU_RW_INSN_NOP        0xd503201fU /* nop */
#define PMU_RW_INSN_MOVZ_ZERO  0xd2800000U /* movz x<Rt>, #0 */
#define PMU_RW_INSN_MRS_CNTVCT 0xd53be040U /* mrs x<Rt>, CNTVCT_EL0 (3, 3, 14, 0, 2) */

enum pmu_rw_kind {
    PMU_RW_NONE = 0,     /* not a PMUv3 register access: left alone */
    PMU_RW_READ_PMCCNTR, /* mrs Xt, PMCCNTR_EL0 */
    PMU_RW_READ_OTHER,   /* mrs Xt, any other PMUv3 register */
    PMU_RW_WRITE,        /* msr <PMUv3 register>, Xt */
    PMU_RW_SKIP_DATA,    /* PMU-looking word left alone: the next word cannot execute (data) */
    PMU_RW_SKIP_BOUNDARY,
    PMU_RW_SKIP_OTHER,
    PMU_RW_NR_KINDS,
};

/*
 * The PMUv3 system-register space with op0 = 3, the same set EL2 traps with MDCR_EL2.TPM and the
 * FGT bits (hv_win.c uses this predicate for its trap emulation too, so both paths agree):
 *   op1 3, CRn 9,  CRm 12..14: PMCR, PMCNTENSET/CLR, PMOVSCLR, PMSWINC, PMSELR, PMCEID0/1, PMCCNTR,
 *                              PMXEVTYPER, PMXEVCNTR, PMZR, PMUSERENR, PMOVSSET (every op2)
 *   op1 0, CRn 9,  CRm 14:     PMINTENSET/CLR, PMUACR, PMECR, PMMIR, PMIAR (every op2)
 *   op1 3, CRn 14, CRm 8..15:  PMEVCNTR<n>, PMEVTYPER<n>, PMCCFILTR
 * Not included: the SPE registers (PMB*, PMS*: op1 0, CRn 9, CRm 9/10), which Windows touches only
 * when ID_AA64DFR0_EL1.PMSVer says so (it reads 0 on the T6050), and the FEAT_PMUv3_ICNTR registers
 * (CRn 9, CRm 4/6), which no Windows image in the corpus uses.
 */
static inline bool pmu_rw_is_pmu_reg(u32 op1, u32 crn, u32 crm)
{
    if (crn == 9)
        return (op1 == 3 && crm >= 12 && crm <= 14) || (op1 == 0 && crm == 14);
    return crn == 14 && op1 == 3 && crm >= 8;
}

enum pmu_rw_kind pmu_rw_classify(u32 insn);

/*
 * True when <next> cannot be the instruction executed after a PMU access on this CPU, so the
 * PMU-looking word before it is data, not code. Top five bits clear = op0 0000..0011 with bit 31
 * clear: UDF/reserved (0000), unallocated (0001, 0011) and SVE (0010), which the T6050 does not
 * implement outside streaming mode. An mrs/msr does not branch, so real code always executes the
 * next word.
 *
 * Why (bugcheck 0x1A/0x404, 2026-11-10/11): stage 2 tracks 16 KiB, Windows allocates 4 KiB frames,
 * so a first-execute scan also covers up to three sibling frames that may hold data. A page-table
 * page there holds stage-1 descriptors whose low word, for an output address with PA[31:12] =
 * 0xd53be (nG, AF, SH = 3, AttrIndx 0), decodes as mrs X3, PMEVTYPER<n>_EL0. The rewrite turned the
 * user PTE 0x00200100_d53beXc3 into 0x00200100_d2800003 (movz x3, #0). The high word of every
 * descriptor (bits 63:59 clear) is in this class, so the guard keeps every such low word intact.
 */
static inline bool pmu_rw_next_is_data(u32 next)
{
    return (next & 0xf8000000U) == 0;
}

/* The replacement word for a PMUv3 access, or <insn> itself when it is not one */
u32 pmu_rw_replacement(u32 insn);

/*
 * Rewrite every PMUv3 access in words[0..n) in place, each with one aligned 32-bit store. <rec>
 * (may be NULL) is called once per rewritten word with its index, the old and the new word.
 * A PMU access whose next word (inside words[]) satisfies pmu_rw_next_is_data() is not rewritten:
 * counts[PMU_RW_SKIP_DATA] is incremented and <skip> (may be NULL) is called with its index.
 * Returns the number of words rewritten; counts[kind] (may be NULL) is incremented per kind.
 */
typedef void pmu_rw_record_fn(void *arg, u32 index, u32 old_insn, u32 new_insn);
u32 pmu_rw_rewrite_words(u32 *words, u32 n, pmu_rw_record_fn *rec, void *arg, u64 *counts);
u32 pmu_rw_rewrite_words_ex(u32 *words, u32 n, pmu_rw_record_fn *rec, pmu_rw_record_fn *skip,
                            void *arg, u64 *counts);

/* Production frame scanner: no read outside 4 KiB; final PMU word is left alone. */
u32 pmu_rw_rewrite_frame(u32 *words, pmu_rw_record_fn *rec, pmu_rw_record_fn *skip,
                         void *arg, u64 *counts);

#endif
