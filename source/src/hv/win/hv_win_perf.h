/* SPDX-License-Identifier: MIT */
/*
 * perf-exits (2026-11-11): hypervisor hot-path options for the Windows guest, all off by default.
 * Pure helpers (no system-register access) so the host test (tests/win-perf) checks the exact code the
 * firmware runs. Selected by P_HV_WIN_PERF (launcher --perf); hv_win_perf_configure() in hv_win.c.
 *
 *   HV_PERF_STATS      PF status line: exits per class per second, EL2 time per exit kind, the busiest
 *                      trapped system registers, fast-path hits, display-sweep busy time. Counters only.
 *   HV_PERF_FAST_MDSCR guest MRS/MSR MDSCR_EL1 (Windows reads it on every EL0<->EL1 transition to check
 *                      the single-step bit) handled at the top of the EL2 sync vector against the same
 *                      per-CPU shadow the slow path uses, before the full exit path (no dsb/isb, no context
 *                      read, no diag ring, no vGIC/timer resync): the trap changes no interrupt state.
 *   HV_PERF_MDSCR_PASS stop trapping MDSCR_EL1 at all: MDCR_EL2.TDE/TDA cleared (TDOSA/TDRA kept), the
 *                      breakpoint/watchpoint registers kept trapped by FEAT_FGT (HDFGRTR/HDFGWTR bits
 *                      0..3) so the EL2 shadow still owns them, real DBGBCRn/DBGWCRn zeroed, real
 *                      MDSCR_EL1 loaded with the shadow. Needs FEAT_FGT; refused otherwise.
 */
#ifndef HV_WIN_PERF_H
#define HV_WIN_PERF_H

#include "types.h"

#define HV_PERF_STATS      BIT(0)
#define HV_PERF_FAST_MDSCR BIT(1)
#define HV_PERF_MDSCR_PASS BIT(2)
#define HV_PERF_ALL        (HV_PERF_STATS | HV_PERF_FAST_MDSCR | HV_PERF_MDSCR_PASS)

/* ESR_EL2 for a trapped AArch64 MSR/MRS (EC 0x18): EC[31:26], Op0[21:20], Op2[19:17], Op1[16:14],
 * CRn[13:10], Rt[9:5], CRm[4:1], Dir[0] (1 = read). */
#define HV_PERF_ESR_EC_SHIFT 26
#define HV_PERF_EC_MSR       0x18
#define HV_PERF_ISS_REGMASK  0x3ffc1eUL /* Op0 | Op2 | Op1 | CRn | CRm */
#define HV_PERF_ISS_SR(op0, op1, crn, crm, op2)                                                       \
    (((u64)(op0) << 20) | ((u64)(op2) << 17) | ((u64)(op1) << 14) | ((u64)(crn) << 10) |            \
     ((u64)(crm) << 1))
#define HV_PERF_ISS_MDSCR HV_PERF_ISS_SR(2, 0, 0, 2, 2) /* MDSCR_EL1 = S2_0_C0_C2_2 */

/* MDCR_EL2 / HDFGxTR_EL2 bits used by HV_PERF_MDSCR_PASS (Arm ARM register descriptions) */
#define HV_PERF_MDCR_TDE   BIT(8)
#define HV_PERF_MDCR_TDA   BIT(9)
#define HV_PERF_MDCR_TDOSA BIT(10)
#define HV_PERF_MDCR_TDRA  BIT(11)
/* bit 0 DBGBCRn_EL1, 1 DBGBVRn_EL1, 2 DBGWCRn_EL1, 3 DBGWVRn_EL1 (same positions read and write) */
#define HV_PERF_HDFG_DBGBW (BIT(0) | BIT(1) | BIT(2) | BIT(3))

/* Is this ESR a trapped MRS/MSR of MDSCR_EL1? Sets *rt (0..31, 31 = XZR) and *is_read. */
static inline bool hv_perf_is_mdscr(u64 esr, u32 *rt, bool *is_read)
{
    if (((esr >> HV_PERF_ESR_EC_SHIFT) & 0x3f) != HV_PERF_EC_MSR)
        return false;
    if (!(esr & BIT(25))) /* IL: a 32-bit instruction; always set for AArch64 MSR/MRS */
        return false;
    if ((esr & HV_PERF_ISS_REGMASK) != HV_PERF_ISS_MDSCR)
        return false;
    *rt = (esr >> 5) & 0x1f;
    *is_read = esr & 1;
    return true;
}

/*
 * The fast-path emulation itself, on the saved register frame: the same semantics as the slow path's
 * SYS_MDSCR_EL1 case in hv_handle_msr_unlocked() (read: Rt = shadow, XZR discarded; write: shadow = Rt,
 * XZR = 0). Returns false (nothing changed) when the ESR is not an MDSCR access.
 */
static inline bool hv_perf_mdscr_emulate(u64 esr, u64 *regs, u64 *shadow)
{
    u32 rt;
    bool is_read;
    if (!hv_perf_is_mdscr(esr, &rt, &is_read))
        return false;
    if (is_read) {
        if (rt != 31)
            regs[rt] = *shadow;
    } else {
        *shadow = rt == 31 ? 0 : regs[rt];
    }
    return true;
}

/* MDCR_EL2 value for HV_PERF_MDSCR_PASS: debug-register traps off except the OS lock and ROM regs */
static inline u64 hv_perf_mdcr_pass(u64 mdcr)
{
    return (mdcr & ~(HV_PERF_MDCR_TDE | HV_PERF_MDCR_TDA)) | HV_PERF_MDCR_TDOSA | HV_PERF_MDCR_TDRA;
}

/*
 * Busiest trapped system registers (PF line): a small open-addressed table keyed by the ESR register
 * fields and direction. Racy increments are acceptable (statistics); a full table counts the rest in
 * *other.
 */
#define HV_PERF_SR_N 32
struct hv_perf_sr {
    u32 key; /* (ISS & HV_PERF_ISS_REGMASK) | dir, +1 so that 0 means empty */
    u32 pad;
    u64 count, reported;
};

static inline u32 hv_perf_sr_key(u64 esr)
{
    return (u32)((esr & HV_PERF_ISS_REGMASK) | (esr & 1)) + 1;
}

static inline void hv_perf_sr_note(struct hv_perf_sr *t, u64 *other, u64 esr)
{
    u32 key = hv_perf_sr_key(esr);
    u32 h = (key * 2654435761U) >> 27; /* 5 bits */
    for (u32 i = 0; i < HV_PERF_SR_N; i++) {
        struct hv_perf_sr *e = &t[(h + i) & (HV_PERF_SR_N - 1)];
        if (e->key == key) {
            e->count++;
            return;
        }
        if (!e->key) {
            e->key = key;
            e->count = 1;
            return;
        }
    }
    (*other)++;
}

/* Index of the entry with the largest count - reported, not in taken[]; -1 if none moved. */
static inline int hv_perf_sr_top(const struct hv_perf_sr *t, const bool *taken)
{
    int best = -1;
    u64 bd = 0;
    for (int i = 0; i < HV_PERF_SR_N; i++) {
        u64 d = t[i].count - t[i].reported;
        if (t[i].key && !taken[i] && d > bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* Decode a table key back to op0/op1/CRn/CRm/op2 and direction (for printing). */
static inline void hv_perf_sr_decode(u32 key, u32 *op0, u32 *op1, u32 *crn, u32 *crm, u32 *op2,
                                     bool *is_read)
{
    u64 v = key - 1;
    *op0 = (v >> 20) & 3;
    *op2 = (v >> 17) & 7;
    *op1 = (v >> 14) & 7;
    *crn = (v >> 10) & 15;
    *crm = (v >> 1) & 15;
    *is_read = v & 1;
}

/* Per-second rate from a delta over an interval of dt counter ticks at freq Hz (0 when dt is 0). */
static inline u64 hv_perf_rate(u64 delta, u64 dt, u64 freq)
{
    if (!dt)
        return 0;
    /* delta * freq can overflow for huge deltas; split it */
    return (delta / dt) * freq + ((delta % dt) * freq) / dt;
}

/* Parts per thousand of busy ticks in an interval (capped at 1000). */
static inline u64 hv_perf_permille(u64 busy, u64 dt)
{
    if (!dt)
        return 0;
    u64 p = busy >= dt ? 1000 : (busy * 1000) / dt;
    return p;
}

#endif
