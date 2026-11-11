/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TYPES_H
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include "../../src/hv/hv_sysreg_logic.h"

static int fails, checks;
#define CHECK(name, cond)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            fails++;                                                                               \
            printf("FAIL: %s\n", name);                                                            \
        }                                                                                          \
    } while (0)

#define DBG_SHADOW_N 64
static u64 dbg_shadow_mem[2][DBG_SHADOW_N];

/* the firmware's hv_dbg_shadow() body with PERCPU replaced by an explicit CPU (as smp2g test_wfi.c) */
static bool dbg_shadow(int cpu, u32 op0, u32 op1, u32 crn, u32 crm, u32 op2, u64 rt, bool is_read,
                       u64 *regs)
{
    if (!wfi_is_bp_reg(op0, op1, crn, op2))
        return false;
    u32 idx = wfi_bp_index(op2, crm);
    if (is_read) {
        if (rt != 31)
            regs[rt] = dbg_shadow_mem[cpu][idx];
    } else {
        dbg_shadow_mem[cpu][idx] = (rt == 31) ? 0 : regs[rt];
    }
    return true;
}

static u64 esr_msr(u32 op0, u32 op1, u32 crn, u32 crm, u32 op2, u32 rt, bool rd)
{
    u64 iss = ((u64)op0 << 20) | ((u64)op2 << 17) | ((u64)op1 << 14) | ((u64)crn << 10) |
              ((u64)rt << 5) | ((u64)crm << 1) | (rd ? 1 : 0);
    return (0x18ull << 26) | (1ull << 25) | iss;
}

int main(void)
{
    u64 regs[32] = {0};

    /* ---- smp2g debug-shadow checks (tests/win-wfi/test_wfi.c:870-907), unchanged semantics ---- */
    regs[3] = 0xfffff80012345678ull;
    CHECK("DBGBVR0 write handled", dbg_shadow(0, 2, 0, 0, 0, 4, 3, false, regs));
    regs[4] = 0;
    dbg_shadow(0, 2, 0, 0, 0, 4, 4, true, regs);
    CHECK("DBGBVR0 reads back", regs[4] == 0xfffff80012345678ull);
    regs[3] = 0x1234;
    dbg_shadow(1, 2, 0, 0, 0, 4, 3, true, regs);
    CHECK("DBGBVR0 is per-CPU (CPU1 unset)", regs[3] == 0);
    CHECK("DBGBCR5 shadowed, slot 21", wfi_is_bp_reg(2, 0, 0, 5) && wfi_bp_index(5, 5) == 21);
    CHECK("DBGWVR3 shadowed, slot 35", wfi_is_bp_reg(2, 0, 0, 6) && wfi_bp_index(6, 3) == 35);
    CHECK("DBGWCR0 shadowed, slot 48", wfi_is_bp_reg(2, 0, 0, 7) && wfi_bp_index(7, 0) == 48);
    CHECK("DBGBVR15 shadowed (proxy passed n>=5 through)", wfi_is_bp_reg(2, 0, 0, 4) && wfi_bp_index(4, 15) == 15);
    CHECK("MDSCR_EL1 not shadowed here", !wfi_is_bp_reg(2, 0, 0, 2));
    CHECK("MDCCINT_EL1 not shadowed here", !wfi_is_bp_reg(2, 0, 0, 0));
    CHECK("OSLSR_EL1 (CRn=1) not shadowed here", !wfi_is_bp_reg(2, 0, 1, 4));
    CHECK("a normal sysreg (op0=3) not shadowed", !wfi_is_bp_reg(3, 0, 0, 4));
    dbg_shadow_mem[0][wfi_bp_index(4, 1)] = 0x55;
    dbg_shadow(0, 2, 0, 0, 1, 4, 31, false, regs);
    CHECK("write from xzr stores 0", dbg_shadow_mem[0][wfi_bp_index(4, 1)] == 0);
    regs[31] = 0;
    dbg_shadow_mem[0][0] = 0x77;
    dbg_shadow(0, 2, 0, 0, 0, 4, 31, true, regs);
    CHECK("read into xzr leaves regs[31] untouched", regs[31] == 0);
    bool seen[DBG_SHADOW_N] = {false}, unique = true, inrange = true;
    for (u32 op2 = 4; op2 <= 7; op2++)
        for (u32 crm = 0; crm < 16; crm++) {
            u32 i = wfi_bp_index(op2, crm);
            if (i >= DBG_SHADOW_N)
                inrange = false;
            else {
                if (seen[i])
                    unique = false;
                seen[i] = true;
            }
        }
    CHECK("all 64 DBGB/W indices unique and in range", unique && inrange);

    /* ---- r4.9 extra table ---- */
    bool tab_ok = true;
    for (u32 i = 0; i < HV_SR_EXTRA_N; i++) {
        u32 e = hv_sr_extra[i].enc;
        u32 op0 = e >> 14, op1 = (e >> 11) & 7, crn = (e >> 7) & 15, op2 = e & 7;
        if (wfi_is_bp_reg(op0, op1, crn, op2))
            tab_ok = false; /* never overlaps the shadow */
        if (e == HV_SR_ENC(2, 0, 0, 2, 2) || e == HV_SR_ENC(2, 0, 1, 0, 4))
            tab_ok = false; /* MDSCR / OSLAR have their own cases */
        if (op0 == 3 && op1 == 0 && crn == 0)
            tab_ok = false; /* TID3 ID space is hv_win's */
        if (!(op0 == 2 || (op0 == 3 && op1 == 5 && crn == 15)))
            tab_ok = false; /* debug class or Apple ACC only */
        if (hv_sr_extra[i].kind != HV_SR_PASS && hv_sr_extra[i].kind != HV_SR_PASS_RO)
            tab_ok = false;
        for (u32 j = 0; j < i; j++)
            if (hv_sr_extra[j].enc == e)
                tab_ok = false;
    }
    CHECK("extra table: unique, debug/ACC class only, no overlap with shadow/MDSCR/OSLAR/ID", tab_ok);
    CHECK("OSLSR_EL1 in table (smp2g port)", hv_sr_extra_find(2, 0, 1, 1, 4) &&
                                                  hv_sr_extra_find(2, 0, 1, 1, 4)->kind == HV_SR_PASS);
    CHECK("MDCCINT_EL1 pass", hv_sr_extra_find(2, 0, 0, 2, 0) && hv_sr_extra_find(2, 0, 0, 2, 0)->kind == HV_SR_PASS);
    CHECK("OSDLR_EL1 pass", hv_sr_extra_find(2, 0, 1, 3, 4) != NULL);
    CHECK("MDCCSR_EL0 read-only", hv_sr_extra_find(2, 3, 0, 1, 0)->kind == HV_SR_PASS_RO);
    CHECK("DBGAUTHSTATUS read-only", hv_sr_extra_find(2, 0, 7, 14, 6)->kind == HV_SR_PASS_RO);
    CHECK("ACC_CFG write-ignored", hv_sr_extra_find(3, 5, 15, 4, 0)->kind == HV_SR_PASS_RO);
    CHECK("DBGBVR0 not in table", !hv_sr_extra_find(2, 0, 0, 0, 4));
    CHECK("SCTLR_EL1 not in table", !hv_sr_extra_find(3, 0, 1, 0, 0));

    /* ---- pre-escalation decode ---- */
    struct hv_esc_info e;
    u64 esr = esr_msr(2, 0, 0, 5, 5, 3, true); /* mrs x3, DBGBCR5_EL1 */
    hv_esc_decode(esr, 0, &e);
    CHECK("decode EC 0x18", e.ec == 0x18 && e.is_sysreg && !e.is_abort);
    CHECK("decode fields DBGBCR5 read x3", e.op0 == 2 && e.op1 == 0 && e.crn == 0 && e.crm == 5 &&
                                               e.op2 == 5 && e.rt == 3 && e.is_read);
    esr = esr_msr(3, 4, 15, 7, 2, 30, false);
    hv_esc_decode(esr, 0, &e);
    CHECK("decode write x30 s3_4_c15_c7_2", !e.is_read && e.rt == 30 && e.op0 == 3 && e.op1 == 4 &&
                                                e.crn == 15 && e.crm == 7 && e.op2 == 2);
    u64 afs = esr_msr(3, 5, 15, 4, 0, 7, true) & 0x1ffffff;
    hv_esc_decode((0x3full << 26) | 0x20, afs, &e);
    CHECK("IMPDEF-MSR decodes from AFSR1", e.is_sysreg && e.op1 == 5 && e.crn == 15 && e.crm == 4 &&
                                               e.rt == 7 && e.is_read);
    hv_esc_decode((0x3full << 26) | 0x21, afs, &e);
    CHECK("IMPDEF GEXIT/other is not sysreg", !e.is_sysreg);
    hv_esc_decode((0x24ull << 26) | (1ull << 25) | 0x45, 0, &e);
    CHECK("data abort lower: abort, not sysreg", e.is_abort && !e.is_sysreg);
    hv_esc_decode((0x20ull << 26), 0, &e);
    CHECK("insn abort lower: abort", e.is_abort);

    /* ---- fail-soft class gate ---- */
    struct hv_esc_info m;
    hv_esc_decode(esr_msr(2, 0, 0, 1, 0, 1, true), 0, &m);
    CHECK("failsoft: sysreg, locked, option on -> absorb", hv_sr_failsoft_class(true, true, true, &m));
    CHECK("failsoft: option off -> escalate", !hv_sr_failsoft_class(false, true, true, &m));
    CHECK("failsoft: link open -> escalate", !hv_sr_failsoft_class(true, false, true, &m));
    CHECK("failsoft: not a lower sync -> escalate", !hv_sr_failsoft_class(true, true, false, &m));
    hv_esc_decode((0x3full << 26) | 0x20, afs, &m);
    CHECK("failsoft: IMPDEF-MSR absorbed", hv_sr_failsoft_class(true, true, true, &m));
    static const u32 never[] = {0x01, 0x07, 0x0e, 0x15, 0x16, 0x17, 0x20, 0x21, 0x24, 0x25, 0x2f,
                                0x30, 0x32, 0x34, 0x3c, 0x00};
    bool none = true;
    for (u32 i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
        hv_esc_decode(((u64)never[i] << 26) | (1ull << 25) | 0x1, 0, &m);
        if (hv_sr_failsoft_class(true, true, true, &m))
            none = false;
    }
    CHECK("failsoft: aborts/HVC/SMC/SVC/BRK/debug/SError/WFx/unknown never absorbed", none);
    hv_esc_decode((0x3full << 26) | 0x0, 0, &m);
    CHECK("failsoft: IMPDEF non-MSR never absorbed", !hv_sr_failsoft_class(true, true, true, &m));

    /* ---- r4.10: unhandled-exception action table ---- */
    struct hv_esc_info a;
#define ACT(esrv, afs, opt, lk) (hv_esc_decode((esrv), (afs), &a), hv_unhandled_action((opt), (lk), &a))
    u64 brk = (0x3cull << 26) | (1ull << 25) | 0xf002;
    CHECK("BRK #0xf002 reflected even unlocked/option off", ACT(brk, 0, false, false) == HV_ACT_REFLECT);
    CHECK("BRK #0x4242 (m1n1 HV call) escalates", ACT((0x3cull << 26) | 0x4242, 0, true, true) == HV_ACT_ESCALATE);
    CHECK("sysreg locked -> RAZ/WI", ACT(esr_msr(2, 0, 0, 1, 0, 1, true), 0, true, true) == HV_ACT_SYSREG_RAZWI);
    CHECK("sysreg unlocked -> escalate", ACT(esr_msr(2, 0, 0, 1, 0, 1, true), 0, true, false) == HV_ACT_ESCALATE);
    CHECK("hw breakpoint 0x30 locked -> reflect", ACT(0x30ull << 26, 0, true, true) == HV_ACT_REFLECT);
    CHECK("step 0x32 locked -> reflect", ACT(0x32ull << 26, 0, true, true) == HV_ACT_REFLECT);
    CHECK("watch 0x34 locked -> reflect", ACT(0x34ull << 26, 0, true, true) == HV_ACT_REFLECT);
    CHECK("step 0x32 unlocked -> escalate (host debugger)", ACT(0x32ull << 26, 0, true, false) == HV_ACT_ESCALATE);
    CHECK("HVC locked -> SMCCC error", ACT((0x16ull << 26) | 5, 0, true, true) == HV_ACT_SMCCC_ERR);
    CHECK("SMC locked -> SMCCC error", ACT(0x17ull << 26, 0, true, true) == HV_ACT_SMCCC_ERR);
    CHECK("option off: debug escalates", ACT(0x30ull << 26, 0, false, true) == HV_ACT_ESCALATE);
    static const u32 esc[] = {0x00, 0x01, 0x07, 0x0e, 0x20, 0x21, 0x24, 0x25, 0x2f, 0x31, 0x33, 0x35};
    bool allesc = true;
    for (u32 i = 0; i < sizeof(esc) / sizeof(esc[0]); i++)
        if (ACT(((u64)esc[i] << 26) | 1, 0, true, true) != HV_ACT_ESCALATE)
            allesc = false;
    CHECK("aborts/unknown/WFx/FP/SError/current-EL debug always escalate", allesc);
    CHECK("IMPDEF non-MSR escalates", ACT(0x3full << 26, 0, true, true) == HV_ACT_ESCALATE);

    /* ---- r4.11: site table (no blind spot after the first 20) and report cadence ---- */
    struct hv_site st[HV_SITE_N];
    memset(st, 0, sizeof(st));
    u64 ovf = 0;
    CHECK("site: first BRK f002@pc is new", hv_site_note(st, HV_SITE_N, &ovf, 0x3cf002, 0x13c4));
    bool again = false;
    for (int i = 0; i < 103; i++)
        again |= hv_site_note(st, HV_SITE_N, &ovf, 0x3cf002, 0x13c4);
    CHECK("site: repeats are not new, counted (104)", !again && st[0].count == 104);
    CHECK("site: same imm, other PC is new", hv_site_note(st, HV_SITE_N, &ovf, 0x3cf002, 0x2000));
    CHECK("site: other class (step 0x32) is new", hv_site_note(st, HV_SITE_N, &ovf, 0x320000, 0x13c8));
    for (u32 i = 3; i < HV_SITE_N; i++)
        hv_site_note(st, HV_SITE_N, &ovf, 0x3c0000 | i, i);
    CHECK("site: full table: unseen site still reported new, overflow counted",
          hv_site_note(st, HV_SITE_N, &ovf, 0x3cffff, 0x9999) && ovf == 1);
    bool tk[HV_SITE_N] = {false};
    CHECK("site: top = busiest since last report", hv_site_top(st, HV_SITE_N, tk) == 0);
    for (u32 i = 0; i < HV_SITE_N; i++) st[i].reported = st[i].count;
    hv_site_note(st, HV_SITE_N, &ovf, 0x320000, 0x13c8);
    CHECK("site: after report only moved sites rank", hv_site_top(st, HV_SITE_N, tk) == 2);
    tk[2] = true;
    CHECK("site: nothing else moved -> -1", hv_site_top(st, HV_SITE_N, tk) == -1);
    CHECK("cadence: 5 s at 24 MHz due", hv_status_due(1000 + 5 * 24000000ull, 1000, 24000000, 5));
    CHECK("cadence: 4.9 s not due", !hv_status_due(1000 + 49 * 2400000ull, 1000, 24000000, 5));

    printf("sysreg-r49: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
