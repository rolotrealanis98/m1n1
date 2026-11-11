/* SPDX-License-Identifier: MIT */
/*
 * r4.9: pure, host-testable pieces of the EL2 system-register fast path (no sysreg access here).
 *
 *  - wfi_is_bp_reg / wfi_bp_index: ported unchanged from s1-resident-smp2g (src/hv/hv_wfi_logic.h),
 *    the debug breakpoint/watchpoint register shadow index (smp2d). Names kept for diffability.
 *  - hv_sr_extra_*: the further debug registers r4.9 handles in EL2 C instead of escalating to the host
 *    Python handler (whose semantics for them is plain pass-through: mrs/msr of the same encoding at EL2).
 *    hv_exc.c has one SYSREG_PASS / SYSREG_PASS_RO case per row; tests/sysreg-r49 checks they agree.
 *  - hv_esc_decode: the fields of the pre-escalation console line.
 *  - hv_sr_failsoft_class: which escalations the fail-soft option may absorb (sysreg traps only).
 */
#ifndef HV_SYSREG_LOGIC_H
#define HV_SYSREG_LOGIC_H

#include "types.h"

/* ---- ported from smp2g hv_wfi_logic.h:341-350 -------------------------------------------------- */
/* u1: identical copy in hv_wfi_logic.h (smp line); whichever header comes first defines them */
#ifndef HV_BP_REG_LOGIC_DEFINED
#define HV_BP_REG_LOGIC_DEFINED
/* DBGB{V,C}Rn_EL1 / DBGW{V,C}Rn_EL1: op0=2 op1=0 CRn=0 op2 in 4..7 (n = CRm). */
static inline bool wfi_is_bp_reg(u32 op0, u32 op1, u32 crn, u32 op2)
{
    return op0 == 2 && op1 == 0 && crn == 0 && op2 >= 4 && op2 <= 7;
}

/* Shadow slot for a breakpoint/watchpoint register: (op2-4)*16 + CRm, range 0..63. */
static inline u32 wfi_bp_index(u32 op2, u32 crm)
{
    return (op2 - 4) * 16 + crm;
}
#endif

/* ---- r4.9 additions ---------------------------------------------------------------------------- */
#define HV_SR_ENC(op0, op1, crn, crm, op2)                                                         \
    (((u32)(op0) << 14) | ((u32)(op1) << 11) | ((u32)(crn) << 7) | ((u32)(crm) << 3) | (u32)(op2))

enum hv_sr_kind {
    HV_SR_NONE = 0, /* not in the r4.9 table */
    HV_SR_PASS,     /* read and write passed through at EL2 (Python pass-through semantics) */
    HV_SR_PASS_RO,  /* read passed through; write ignored (read-only register / Python "ro" set) */
};

struct hv_sr_row {
    u32 enc;
    u8 kind;
    const char *name;
};

/* Every register here is handled in hv_handle_msr_unlocked() (hv_exc.c). OSLSR_EL1 is the smp2g port. */
static const struct hv_sr_row hv_sr_extra[] = {
    {HV_SR_ENC(2, 0, 1, 1, 4), HV_SR_PASS, "OSLSR_EL1"}, /* smp2g, SYSREG_PASS as ported */
    {HV_SR_ENC(2, 0, 0, 0, 2), HV_SR_PASS, "OSDTRRX_EL1"},
    {HV_SR_ENC(2, 0, 0, 2, 0), HV_SR_PASS, "MDCCINT_EL1"},
    {HV_SR_ENC(2, 0, 0, 3, 2), HV_SR_PASS, "OSDTRTX_EL1"},
    {HV_SR_ENC(2, 0, 0, 6, 2), HV_SR_PASS, "OSECCR_EL1"},
    {HV_SR_ENC(2, 0, 1, 0, 0), HV_SR_PASS_RO, "MDRAR_EL1"},
    {HV_SR_ENC(2, 0, 1, 3, 4), HV_SR_PASS, "OSDLR_EL1"},
    {HV_SR_ENC(2, 0, 1, 4, 4), HV_SR_PASS, "DBGPRCR_EL1"},
    {HV_SR_ENC(2, 0, 7, 8, 6), HV_SR_PASS, "DBGCLAIMSET_EL1"},
    {HV_SR_ENC(2, 0, 7, 9, 6), HV_SR_PASS, "DBGCLAIMCLR_EL1"},
    {HV_SR_ENC(2, 0, 7, 14, 6), HV_SR_PASS_RO, "DBGAUTHSTATUS_EL1"},
    {HV_SR_ENC(2, 3, 0, 1, 0), HV_SR_PASS_RO, "MDCCSR_EL0"},
    {HV_SR_ENC(2, 3, 0, 4, 0), HV_SR_PASS, "DBGDTR_EL0"},
    {HV_SR_ENC(2, 3, 0, 5, 0), HV_SR_PASS, "DBGDTRRX/TX_EL0"},
    /* Apple ACC_CFG / ACC_OVRD (HACR_EL2.TRAP_ACC): Python "ro" set = read pass-through, write skipped */
    {HV_SR_ENC(3, 5, 15, 4, 0), HV_SR_PASS_RO, "ACC_CFG_EL1"},
    {HV_SR_ENC(3, 5, 15, 6, 0), HV_SR_PASS_RO, "ACC_OVRD_EL1"},
};
#define HV_SR_EXTRA_N (sizeof(hv_sr_extra) / sizeof(hv_sr_extra[0]))

static inline const struct hv_sr_row *hv_sr_extra_find(u32 op0, u32 op1, u32 crn, u32 crm, u32 op2)
{
    u32 enc = HV_SR_ENC(op0, op1, crn, crm, op2);
    for (u32 i = 0; i < HV_SR_EXTRA_N; i++)
        if (hv_sr_extra[i].enc == enc)
            return &hv_sr_extra[i];
    return NULL;
}

/* ESR classes (ARM ARM D17.2.37 / Apple IMPDEF) */
#define HV_SR_EC_MSR    0x18u
#define HV_SR_EC_IMPDEF 0x3fu
#define HV_SR_ISS_IMPDEF_MSR 0x20u

struct hv_esc_info {
    u32 ec;
    u32 iss;
    bool is_sysreg; /* EC 0x18, or Apple IMPDEF with ISS 0x20 (sysreg ISS in AFSR1) */
    bool is_abort;  /* instruction/data abort (lower or same EL): FAR is meaningful */
    u32 op0, op1, crn, crm, op2, rt;
    bool is_read;
};

static inline void hv_esc_decode(u64 esr, u64 afsr1, struct hv_esc_info *o)
{
    o->ec = (u32)((esr >> 26) & 0x3f);
    o->iss = (u32)(esr & 0x1ffffff);
    o->is_abort = o->ec == 0x20 || o->ec == 0x21 || o->ec == 0x24 || o->ec == 0x25;
    o->is_sysreg = o->ec == HV_SR_EC_MSR || (o->ec == HV_SR_EC_IMPDEF && o->iss == HV_SR_ISS_IMPDEF_MSR);
    u64 s = o->ec == HV_SR_EC_IMPDEF ? afsr1 : o->iss;
    o->op0 = (u32)((s >> 20) & 3);
    o->op2 = (u32)((s >> 17) & 7);
    o->op1 = (u32)((s >> 14) & 7);
    o->crn = (u32)((s >> 10) & 15);
    o->rt = (u32)((s >> 5) & 31);
    o->crm = (u32)((s >> 1) & 15);
    o->is_read = s & 1;
}

/*
 * Fail-soft (build option HV_SYSREG_FAILSOFT): an escalation may be absorbed only when the option is
 * on, the host link is locked (storage session armed), the reason is a lower-EL synchronous exception,
 * and the class is a system-register trap. Data/instruction aborts, HVC/SMC, BRK, debug exceptions,
 * SError, IRQ/FIQ and HV events always escalate.
 */
static inline bool hv_sr_failsoft_class(bool option, bool link_locked, bool lower_sync,
                                        const struct hv_esc_info *e)
{
    return option && link_locked && lower_sync && e->is_sysreg;
}

/* ---- r4.10: reflection of a lower-EL synchronous exception into the guest's EL1 vector ----------
 * Mirrors the host Python HV._lower() (proxyclient/m1n1/hv/__init__.py:1151-1180):
 *   ELR_EL12 = trapped PC, SPSR_EL12 = trapped PSTATE, ESR_EL12 = syndrome, FAR_EL12 = FAR_EL2;
 *   offset = 0x80 * exc_code (0 = sync) + 0x400 from EL0t, +0 from EL1t, +0x200 from EL1h, anything
 *   else (EL2, AArch32) refused; return PSTATE = old with M = EL1h and D,A,I,F set; PC = VBAR_EL12 + offset.
 * Architectural corrections over _lower (Arm ARM D1.10 "exception entry"), applied only where _lower
 * differs from what hardware would deliver:
 *   - ESR EC: an exception the guest takes at EL1 *from EL1* reports the "current EL" class, so the
 *     lower-EL classes 0x20/0x24/0x30/0x32/0x34 become 0x21/0x25/0x31/0x33/0x35 for an EL1 origin
 *     (_lower copies ESR_EL2 verbatim). BRK (0x3c) is the same for both: no change.
 *   - return PSTATE: SS = 0, IL = 0, BTYPE = 0 (cleared on every exception entry), PAN = 1 when
 *     SCTLR_EL1.SPAN == 0 (_lower keeps these from the trapped PSTATE).
 */
#define HV_SPSR_M_MASK 0x1fu
#define HV_SPSR_M_EL0t 0x0u
#define HV_SPSR_M_EL1t 0x4u
#define HV_SPSR_M_EL1h 0x5u
#define HV_SPSR_DAIF   (0xfull << 6)
#define HV_SPSR_IL     (1ull << 20)
#define HV_SPSR_SS     (1ull << 21)
#define HV_SPSR_PAN    (1ull << 22)
#define HV_SPSR_BTYPE  (3ull << 10)
#define HV_SCTLR_SPAN  (1ull << 23)

struct hv_reflect {
    u64 elr_el1, spsr_el1, esr_el1, far_el1; /* written to the guest's EL1 registers (_EL12) */
    u64 new_elr, new_spsr;                   /* guest return state (ELR_EL2/SPSR_EL2) */
    u32 offset;
};

static inline bool hv_reflect_compute(u64 elr, u64 spsr, u64 esr, u64 far, u64 vbar, u64 sctlr,
                                      u32 exc_code, struct hv_reflect *o)
{
    u32 off = 0x80 * exc_code;
    u32 m = (u32)(spsr & HV_SPSR_M_MASK);
    bool from_el1;
    if (m == HV_SPSR_M_EL0t) {
        off += 0x400;
        from_el1 = false;
    } else if (m == HV_SPSR_M_EL1t) {
        from_el1 = true;
    } else if (m == HV_SPSR_M_EL1h) {
        off += 0x200;
        from_el1 = true;
    } else {
        return false;
    }
    u32 ec = (u32)((esr >> 26) & 0x3f);
    u64 esr1 = esr;
    if (from_el1 && (ec == 0x20 || ec == 0x24 || ec == 0x30 || ec == 0x32 || ec == 0x34))
        esr1 = (esr & ~(0x3full << 26)) | ((u64)(ec + 1) << 26);
    u64 ns = (spsr & ~(u64)HV_SPSR_M_MASK) | HV_SPSR_M_EL1h | HV_SPSR_DAIF;
    ns &= ~(HV_SPSR_SS | HV_SPSR_IL | HV_SPSR_BTYPE);
    if (!(sctlr & HV_SCTLR_SPAN))
        ns |= HV_SPSR_PAN;
    o->elr_el1 = elr;
    o->spsr_el1 = spsr;
    o->esr_el1 = esr1;
    o->far_el1 = far;
    o->offset = off;
    o->new_elr = vbar + off;
    o->new_spsr = ns;
    return true;
}

/* ---- r4.10: what to do with a lower-EL synchronous exception no EL2 handler took -------------- */
enum hv_unhandled_action {
    HV_ACT_ESCALATE = 0, /* to the host Python handler (logged first) */
    HV_ACT_SYSREG_RAZWI, /* r4.9 fail-soft: read 0 / ignore write */
    HV_ACT_REFLECT,      /* deliver to the guest's EL1 sync vector (hv_reflect_compute) */
    HV_ACT_SMCCC_ERR,    /* x0 = SMCCC_NOT_SUPPORTED (-1), complete the HVC/SMC */
};
#define HV_SMCCC_NOT_SUPPORTED ((u64)-1)

/*
 * BRK other than the m1n1 HV-call immediate 0x4242 is always reflected (the host Python does exactly
 * that: handle_brk -> _lower). The rest only while the link is locked and the option is on:
 * sysreg -> RAZ/WI (r4.9); hardware breakpoint / software step / watchpoint (0x30/0x32/0x34) -> reflect
 * (the guest's debug registers are shadowed in EL2 and never programmed, so these are not the host's);
 * HVC/SMC with no EL2 handler -> SMCCC error in x0. Everything else (aborts, unknown/undefined,
 * IMPDEF non-MSR, SError) escalates: the host Python would not reflect them either.
 */
static inline enum hv_unhandled_action hv_unhandled_action(bool option, bool link_locked,
                                                           const struct hv_esc_info *e)
{
    if (e->ec == 0x3c && (e->iss & 0xffff) != 0x4242)
        return HV_ACT_REFLECT;
    if (!option || !link_locked)
        return HV_ACT_ESCALATE;
    if (e->is_sysreg)
        return HV_ACT_SYSREG_RAZWI;
    if (e->ec == 0x30 || e->ec == 0x32 || e->ec == 0x34)
        return HV_ACT_REFLECT;
    if (e->ec == 0x16 || e->ec == 0x17)
        return HV_ACT_SMCCC_ERR;
    return HV_ACT_ESCALATE;
}

/* ---- r4.11 diagnostics: (key, PC) site table for reflected exceptions and fail-soft registers ------
 * Fixed-size, no allocation. hv_site_note() counts one event and returns true the first time a
 * (key, pc) combination is seen (so the caller prints it at once); when the table is full, events of
 * unseen combinations are counted in *overflow and reported as new each time (never silently lost).
 */
#define HV_SITE_N 16
struct hv_site {
    u32 key;   /* r4.11: EC << 16 | ISS[15:0] (reflections) or sysreg encoding | dir (fail-soft) */
    u32 used;
    u64 pc;
    u64 count, reported;
};

static inline bool hv_site_note(struct hv_site *t, u32 n, u64 *overflow, u32 key, u64 pc)
{
    for (u32 i = 0; i < n; i++) {
        if (t[i].used && t[i].key == key && t[i].pc == pc) {
            t[i].count++;
            return false;
        }
    }
    for (u32 i = 0; i < n; i++) {
        if (!t[i].used) {
            t[i].used = 1;
            t[i].key = key;
            t[i].pc = pc;
            t[i].count = 1;
            t[i].reported = 0;
            return true;
        }
    }
    (*overflow)++;
    return true;
}

/* Index of the entry with the largest count increase since its last report, or -1 if none moved. */
static inline int hv_site_top(const struct hv_site *t, u32 n, const bool *taken)
{
    int best = -1;
    u64 bd = 0;
    for (u32 i = 0; i < n; i++) {
        if (!t[i].used || taken[i])
            continue;
        u64 d = t[i].count - t[i].reported;
        if (d > bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

/* 5 s report cadence on the 1 kHz hypervisor tick, measured on the counter (robust to tick jitter). */
static inline bool hv_status_due(u64 now, u64 last, u64 freq, u32 period_s)
{
    return now - last >= freq * period_s;
}

#endif
