/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "hv.h"
#include "assert.h"
#include "cpu_regs.h"
#include "exception.h"
#include "gxf.h"
#include "hv_sprr.h"
#include "iodev.h"
#include "smp.h"
#include "string.h"
#include "uart.h"
#include "uartproxy.h"

#include "win/hv_win.h"
#include "win/s1/native_link.h"
#include "win/hv_win_storage.h"
#include "hv_sysreg_logic.h"

#define TIME_ACCOUNTING

/*
 * r4.9 fail-soft (named build option). While the host link is locked (storage session armed), a
 * system-register trap (EC 0x18, or Apple IMPDEF-MSR) that no EL2 handler took is absorbed in EL2
 * instead of escalated: a READ returns 0, a WRITE is ignored, and a loud console line names it. Only that
 * class: aborts, HVC/SMC, BRK, debug exceptions, SError, IRQ/FIQ and HV events always escalate.
 * Risk: the guest gets 0 for a register we do not know. Alternative (option off): the escalation is
 * refused by the locked link and the session dies. Default ON for the stage-3 (guest-write) build.
 * r4.10: the same option also gates, while locked, reflecting debug exceptions (EC 0x30/0x32/0x34) to
 * the guest and answering unhandled HVC/SMC with SMCCC_NOT_SUPPORTED; BRK reflection is unconditional.
 */
#ifndef HV_SYSREG_FAILSOFT
#if S1N_STAGE == 3
#define HV_SYSREG_FAILSOFT 1
#else
#define HV_SYSREG_FAILSOFT 0
#endif
#endif

extern spinlock_t bhl;

#define PERCPU(x) pcpu[mrs(TPIDR_EL2)].x

/*
 * smp2d (ported to r4.9 from s1-resident-smp2g): per-CPU shadow of the guest debug
 * breakpoint/watchpoint registers (DBGB{V,C}Rn_EL1, DBGW{V,C}Rn_EL1), indexed (op2-4)*16 + CRm, op2 in
 * 4..7, CRm 0..15. The host Python proxy used to trap these to EL1->EL2->USB (MDCR_EL2.TDA/TDE) and
 * rewrite the whole ExcInfo for every access; handling them in EL2 keeps them off the host link (which
 * r4.5+ locks while a storage session is armed).
 */
#define HV_DBG_SHADOW_N 64

struct hv_pcpu_data {
    u32 ipi_queued;
    u32 ipi_pending;
    u32 pmc_pending;
    u64 pmc_irq_mode;
    u64 exc_entry_pmcr0_cnt;
    u64 mdscr;
    u32 tmr_hv_masked;   /* BIT(HV_TMR_*): we hold this guest timer's IMASK */
    u32 tmr_guest_imask; /* BIT(HV_TMR_*): the guest's own IMASK while we do */
    u64 dbg_shadow[HV_DBG_SHADOW_N];
    u64 dbg_traps; /* debug-register accesses handled in EL2 (were host round trips) */
} ALIGNED(64);

/* r4.9: escalations to the host (all reasons) and fail-soft absorptions */
static u64 hv_esc_count, hv_failsoft_count;
/* r4.11 diagnostics: fail-soft sites, exits by kind, synchronous traps by EC with their last PC/FAR */
static struct hv_site hv_fs_sites[HV_SITE_N];
static u64 hv_fs_overflow;
static u64 hv_exits_irq, hv_exits_fiq, hv_exits_sync;
static u64 hv_trap_ec[64], hv_trap_ec_pc[64], hv_trap_ec_far[64];

struct hv_pcpu_data pcpu[MAX_CPUS];

/*
 * Some SoCs (T6050) lock SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2: it reads back with both
 * guest timer FIQs enabled and every write is UNDEFINED. Without that gate a
 * firing guest timer re-raises its FIQ at EL2 before the guest can take the
 * vFIQ. There, a firing guest timer is masked through its own IMASK instead,
 * and the guest-visible CTL hides that (ECV traps every guest timer access).
 */
bool hv_vm_tmr_fiq_locked;

#define HV_TMR_P 0
#define HV_TMR_V 1

static u64 hv_tmr_read_ctl(int t)
{
    return t == HV_TMR_P ? mrs(CNTP_CTL_EL02) : mrs(CNTV_CTL_EL02);
}

static void hv_tmr_write_ctl(int t, u64 val)
{
    if (t == HV_TMR_P)
        msr(CNTP_CTL_EL02, val);
    else
        msr(CNTV_CTL_EL02, val);
}

/* Returns whether the guest timer is firing from the guest's point of view. */
static bool hv_tmr_gate(int t)
{
    u64 ctl = hv_tmr_read_ctl(t);
    bool masked = PERCPU(tmr_hv_masked) & BIT(t);
    bool guest_imask = masked ? PERCPU(tmr_guest_imask) & BIT(t) : ctl & CNTx_CTL_IMASK;
    bool firing = (ctl & CNTx_CTL_ENABLE) && (ctl & CNTx_CTL_ISTATUS) && !guest_imask;

    if (firing && !masked) {
        PERCPU(tmr_guest_imask) &= ~BIT(t);
        PERCPU(tmr_hv_masked) |= BIT(t);
        hv_tmr_write_ctl(t, ctl | CNTx_CTL_IMASK);
    } else if (!firing && masked) {
        PERCPU(tmr_hv_masked) &= ~BIT(t);
        hv_tmr_write_ctl(t, (ctl & ~CNTx_CTL_IMASK) | (guest_imask ? CNTx_CTL_IMASK : 0));
    }

    return firing;
}

/* Guest access to a timer CTL: hide an IMASK we are holding. */
static void hv_guest_tmr_ctl(int t, bool is_read, u64 *val)
{
    if (!(PERCPU(tmr_hv_masked) & BIT(t))) {
        if (is_read)
            *val = hv_tmr_read_ctl(t);
        else
            hv_tmr_write_ctl(t, *val);
        return;
    }

    if (is_read) {
        u64 ctl = hv_tmr_read_ctl(t) & ~CNTx_CTL_IMASK;
        *val = ctl | ((PERCPU(tmr_guest_imask) & BIT(t)) ? CNTx_CTL_IMASK : 0);
    } else {
        if (*val & CNTx_CTL_IMASK)
            PERCPU(tmr_guest_imask) |= BIT(t);
        else
            PERCPU(tmr_guest_imask) &= ~BIT(t);
        hv_tmr_write_ctl(t, *val | CNTx_CTL_IMASK);
    }
}

void hv_exit_guest(void) __attribute__((noreturn));
void hv_gexit(void);

static u64 stolen_time = 0;
static u64 exc_entry_time;

extern u64 hv_cpus_in_guest;
extern int hv_pinned_cpu;
extern int hv_want_cpu;

static bool time_stealing = true;

static void _hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type,
                          void *extra)
{
    int from_el = FIELD_GET(SPSR_M, ctx->spsr) >> 2;

    hv_wdt_breadcrumb('P');

    /*
     * Get all the CPUs into the HV before running the proxy, to make sure they all exit to
     * the guest with a consistent time offset.
     */
    if (time_stealing)
        hv_rendezvous();

    u64 entry_time = mrs(CNTPCT_EL0);

    ctx->elr_phys = hv_translate(ctx->elr, false, false, NULL);
    ctx->far_phys = hv_translate(ctx->far, false, false, NULL);
    ctx->sp_phys = hv_translate(from_el == 0 ? ctx->sp[0] : ctx->sp[1], false, false, NULL);
    ctx->extra = extra;

    struct uartproxy_msg_start start = {
        .reason = reason,
        .code = type,
        .info = ctx,
    };

    hv_wdt_suspend();
    int ret = uartproxy_run(&start);
    hv_wdt_resume();

    switch (ret) {
        case EXC_RET_HANDLED:
            hv_wdt_breadcrumb('p');
            if (time_stealing) {
                u64 lost = mrs(CNTPCT_EL0) - entry_time;
                stolen_time += lost;
            }
            break;
        case EXC_EXIT_GUEST:
            hv_rendezvous();
            spin_unlock(&bhl);
            hv_exit_guest(); // does not return
        default:
            printf("Guest exception not handled, rebooting.\n");
            print_regs(ctx->regs, 0);
            flush_and_reboot(); // does not return
    }
}

static void hv_maybe_switch_cpu(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type,
                                void *extra)
{
    while (hv_want_cpu != -1) {
        if (hv_want_cpu == smp_id()) {
            hv_want_cpu = -1;
            _hv_exc_proxy(ctx, reason, type, extra);
        } else {
            // Unlock the HV so the target CPU can get into the proxy
            spin_unlock(&bhl);
            while (hv_want_cpu != -1)
                sysop("dmb sy");
            spin_lock(&bhl);
        }
    }
}

/*
 * r4.9: one console line per escalation, printed and flushed BEFORE the host round-trip, through the
 * resident's console (iodev; the host shows it as "TTY>"), which is output-only and not gated by the
 * locked host link. So the next run names any register or fault that still leaves EL2, even when the
 * round-trip itself is refused. Format (one line):
 *   HV/win r4.9 ESC #<n> cpu<c> reason <r> type <t> EC 0x<ec> ESR 0x<esr> PC 0x<elr> link <locked|open>
 *     + sysreg: " SYSREG <mrs|msr> s<op0>_<op1>_c<CRn>_c<CRm>_<op2> x<rt> (op0 o op1 o CRn n CRm m op2 o)"
 *     + abort:  " FAR 0x<far>"
 * The first 4096 are all printed, then every 4096th (with the running count).
 */
#define HV_ESC_LOG_ALL 4096

static void hv_esc_log(struct exc_info *ctx, const char *what, u64 n, u32 reason, u32 type)
{
    struct hv_esc_info e;
    hv_esc_decode(ctx->esr, ctx->afsr1, &e);

    printf("HV/win r4.14 %s #%ld cpu%ld reason %u type %u EC 0x%02x ESR 0x%lx PC 0x%lx link %s", what,
           (long)n, (long)ctx->cpu_id, reason, type, e.ec, ctx->esr, ctx->elr,
           s1n_link_locked() ? "locked" : "open");
    if (e.is_sysreg)
        printf(" SYSREG %s s%u_%u_c%u_c%u_%u x%u (op0 %u op1 %u CRn %u CRm %u op2 %u)",
               e.is_read ? "mrs" : "msr", e.op0, e.op1, e.crn, e.crm, e.op2, e.rt, e.op0, e.op1, e.crn,
               e.crm, e.op2);
    if (e.is_abort)
        printf(" FAR 0x%lx", ctx->far);
    printf("\n");
    iodev_console_flush();
}

void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra)
{
    hv_esc_count++;
    if (hv_esc_count <= HV_ESC_LOG_ALL || !(hv_esc_count % HV_ESC_LOG_ALL))
        hv_esc_log(ctx, "ESC", hv_esc_count, reason, type);

    /*
     * Wait while another CPU is pinned or being switched to.
     * If a CPU switch is requested, handle it before actually handling the
     * exception. We still tell the host the real reason code, though.
     */
    while ((hv_pinned_cpu != -1 && hv_pinned_cpu != smp_id()) || hv_want_cpu != -1) {
        if (hv_want_cpu == smp_id()) {
            hv_want_cpu = -1;
            _hv_exc_proxy(ctx, reason, type, extra);
        } else {
            // Unlock the HV so the target CPU can get into the proxy
            spin_unlock(&bhl);
            while ((hv_pinned_cpu != -1 && hv_pinned_cpu != smp_id()) || hv_want_cpu != -1)
                sysop("dmb sy");
            spin_lock(&bhl);
        }
    }

    /* Handle the actual exception */
    _hv_exc_proxy(ctx, reason, type, extra);

    /*
     * If as part of handling this exception we want to switch CPUs, handle it without returning
     * to the guest.
     */
    hv_maybe_switch_cpu(ctx, reason, type, extra);
}

void hv_set_time_stealing(bool enabled, bool reset)
{
    time_stealing = enabled;
    if (reset)
        stolen_time = 0;
}

void hv_add_time(s64 time)
{
    stolen_time -= (u64)time;
}

void hv_update_fiq(void)
{
    u64 hcr = mrs(HCR_EL2);
    bool fiq_pending = false;
    bool ptimer = false, vtimer = false;

    if (hv_vm_tmr_fiq_locked) {
        ptimer = hv_tmr_gate(HV_TMR_P);
        vtimer = hv_tmr_gate(HV_TMR_V);
    } else {
        if (mrs(CNTP_CTL_EL02) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) {
            ptimer = true;
            reg_clr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, VM_TMR_FIQ_ENA_ENA_P);
        } else {
            reg_set(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, VM_TMR_FIQ_ENA_ENA_P);
        }

        if (mrs(CNTV_CTL_EL02) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) {
            vtimer = true;
            reg_clr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, VM_TMR_FIQ_ENA_ENA_V);
        } else {
            reg_set(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, VM_TMR_FIQ_ENA_ENA_V);
        }
    }

    /* Windows GIC model: guest timers are level PPIs 30/27 in the list registers, not vFIQs */
    if (hv_win_vgic_enabled())
        hv_win_update_virq(ptimer, vtimer);
    else
        fiq_pending = ptimer || vtimer;

    fiq_pending |= PERCPU(ipi_pending) || PERCPU(pmc_pending);
    fiq_pending |= hv_sprr_timer_pending();

    sysop("isb");

    if ((hcr & HCR_VF) && !fiq_pending) {
        hv_write_hcr(hcr & ~HCR_VF);
    } else if (!(hcr & HCR_VF) && fiq_pending) {
        hv_write_hcr(hcr | HCR_VF);
    }
}

u64 hv_pmcr0_read(void)
{
    u64 val = (mrs(SYS_IMP_APL_PMCR0) & ~PMCR0_IMODE_MASK) | PERCPU(pmc_irq_mode);
    return val | (PERCPU(pmc_pending) ? PMCR0_IACT : 0);
}

void hv_pmcr0_write(u64 val)
{
    PERCPU(pmc_pending) = !!(val & PMCR0_IACT);
    PERCPU(pmc_irq_mode) = val & PMCR0_IMODE_MASK;
    msr(SYS_IMP_APL_PMCR0, val);
}

/*
 * smp2d (ported unchanged from smp2g hv_exc.c:374-391): handle a guest debug breakpoint/watchpoint
 * register access per CPU in EL2, lock-free, so it never reaches the host Python proxy. Mirrors the
 * proxy's "Shadow" behaviour (store on write, read back what was written, default 0) for
 * DBGB{V,C}Rn_EL1 / DBGW{V,C}Rn_EL1 (op0=2 op1=0 CRn=0 op2 in 4..7), all 16 of each (the proxy shadowed
 * only 5 BRPs / 4 WRPs and passed higher indices through). These are never programmed into real
 * hardware, so the guest's breakpoints do not actually fire at EL1. Returns false for every other
 * register (MDSCR_EL1 and the OS-lock/status regs have their own cases) so the caller continues.
 */
static bool hv_dbg_shadow(u64 reg, u64 rt, bool is_read, u64 *regs)
{
    u32 op2 = FIELD_GET(ESR_ISS_MSR_OP2, reg);
    if (!wfi_is_bp_reg(FIELD_GET(ESR_ISS_MSR_OP0, reg), FIELD_GET(ESR_ISS_MSR_OP1, reg),
                       FIELD_GET(ESR_ISS_MSR_CRn, reg), op2))
        return false;

    u32 idx = wfi_bp_index(op2, FIELD_GET(ESR_ISS_MSR_CRm, reg)); /* 0..63 */
    u64 *sh = PERCPU(dbg_shadow);

    if (is_read) {
        if (rt != 31)
            regs[rt] = sh[idx];
    } else {
        sh[idx] = (rt == 31) ? 0 : regs[rt];
    }
    PERCPU(dbg_traps)++;
    return true;
}

/* r4.9: read passed through at EL2, write ignored (read-only registers; the proxy's "ro" set) */
#define SYSREG_PASS_RO(sr)                                                                         \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr));                                                           \
        return true;

#define SYSREG_MAP(sr, to)                                                                       \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(to));                                                           \
        else                                                                                       \
            _msr(sr_tkn(to), regs[rt]);                                                            \
        return true;

#define SYSREG_PASS(sr)                                                                            \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr));                                                           \
        else                                                                                       \
            _msr(sr_tkn(sr), regs[rt]);                                                            \
        return true;

#define SYSREG_TMR_CTL(sr, t)                                                                      \
    case SYSREG_ISS(sr):                                                                           \
        hv_guest_tmr_ctl(t, is_read, &regs[rt]);                                                   \
        return true;

static bool hv_handle_msr_unlocked(struct exc_info *ctx, u64 iss)
{
    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;

    u64 *regs = ctx->regs;

    regs[31] = 0;

    /* Don't handle any TLB maintenance here if we're running in emulated SPRR mode */
    u64 crn = FIELD_GET(ESR_ISS_MSR_CRn, reg);
    if (!is_read && FIELD_GET(ESR_ISS_MSR_OP0, reg) == 1 &&
        (crn == 8 || crn == 9) && /* 8 = standard , 9 = nXS */
        hv_sprr_traps_tlbi())
        return false;

    switch (reg) {
        SYSREG_PASS(SYS_TPIDR2_EL0);
        SYSREG_PASS(SYS_IMP_APL_TPIDR_GL2);
        SYSREG_PASS(SYS_IMP_APL_CORE_NRG_ACC_DAT);
        SYSREG_PASS(SYS_IMP_APL_CORE_SRM_NRG_ACC_DAT);
        /* Architectural timer, for ECV */
        SYSREG_TMR_CTL(SYS_CNTV_CTL_EL0, HV_TMR_V)
        SYSREG_MAP(SYS_CNTV_CVAL_EL0, SYS_CNTV_CVAL_EL02)
        SYSREG_MAP(SYS_CNTV_TVAL_EL0, SYS_CNTV_TVAL_EL02)
        SYSREG_TMR_CTL(SYS_CNTP_CTL_EL0, HV_TMR_P)
        SYSREG_MAP(SYS_CNTP_CVAL_EL0, SYS_CNTP_CVAL_EL02)
        SYSREG_MAP(SYS_CNTP_TVAL_EL0, SYS_CNTP_TVAL_EL02)
        /* Spammy stuff seen on t600x p-cores */
        /* These are PMU/PMC registers */
        SYSREG_PASS(sys_reg(3, 2, 15, 12, 0));
        SYSREG_PASS(sys_reg(3, 2, 15, 13, 0));
        SYSREG_PASS(sys_reg(3, 2, 15, 14, 0));
        SYSREG_PASS(sys_reg(3, 2, 15, 15, 0));
        SYSREG_PASS(sys_reg(3, 1, 15, 7, 0));
        SYSREG_PASS(sys_reg(3, 1, 15, 8, 0));
        SYSREG_PASS(sys_reg(3, 1, 15, 9, 0));
        SYSREG_PASS(sys_reg(3, 1, 15, 10, 0));
        /* Noisy traps */
        SYSREG_PASS(SYS_IMP_APL_HID4)
        SYSREG_PASS(SYS_IMP_APL_EHID4)
        SYSREG_PASS(SYS_OSLAR_EL1);
        /* smp2d: the guest's OS-lock status read reaches EL2 directly, not the host proxy */
        SYSREG_PASS(SYS_OSLSR_EL1);
        /*
         * r4.9: the rest of the MDCR_EL2 TDE/TDA/TDOSA/TDRA debug-register class, with the host proxy's
         * semantics for them (pass-through at EL2; read-only ones: write ignored). One row each in
         * hv_sysreg_logic.h hv_sr_extra[] (tests/sysreg-r49 checks both lists agree).
         */
        SYSREG_PASS(SYS_OSDTRRX_EL1)
        SYSREG_PASS(SYS_MDCCINT_EL1)
        SYSREG_PASS(SYS_OSDTRTX_EL1)
        SYSREG_PASS(SYS_OSECCR_EL1)
        SYSREG_PASS_RO(SYS_MDRAR_EL1)
        SYSREG_PASS(SYS_OSDLR_EL1)
        SYSREG_PASS(SYS_DBGPRCR_EL1)
        SYSREG_PASS(SYS_DBGCLAIMSET_EL1)
        SYSREG_PASS(SYS_DBGCLAIMCLR_EL1)
        SYSREG_PASS_RO(SYS_DBGAUTHSTATUS_EL1)
        SYSREG_PASS_RO(SYS_MDCCSR_EL0)
        SYSREG_PASS(SYS_DBGDTR_EL0)
        SYSREG_PASS(SYS_DBGDTRRXTX_EL0)
        /* r4.9: HACR_EL2.TRAP_ACC; the proxy reads these through and skips writes */
        SYSREG_PASS_RO(SYS_IMP_APL_ACC_CFG)
        SYSREG_PASS_RO(SYS_IMP_APL_ACC_OVRD)
        SYSREG_PASS(SYS_SMPRI_EL1);
        /* We don't normally trap these, but if we do, they're noisy */
        SYSREG_PASS(SYS_IMP_APL_GXF_STATUS_EL1)
        SYSREG_PASS(SYS_IMP_APL_CNTVCT_ALIAS_EL0)
        SYSREG_PASS(SYS_IMP_APL_TPIDR_GL1)
        SYSREG_MAP(SYS_IMP_APL_SPSR_GL1, SYS_IMP_APL_SPSR_GL12)
        SYSREG_MAP(SYS_IMP_APL_ASPSR_GL1, SYS_IMP_APL_ASPSR_GL12)
        SYSREG_MAP(SYS_IMP_APL_ELR_GL1, SYS_IMP_APL_ELR_GL12)
        SYSREG_MAP(SYS_IMP_APL_ESR_GL1, SYS_IMP_APL_ESR_GL12)
        SYSREG_MAP(SYS_IMP_APL_SPRR_PERM_EL1, SYS_IMP_APL_SPRR_PERM_EL12)
        SYSREG_MAP(SYS_IMP_APL_APCTL_EL1, SYS_IMP_APL_APCTL_EL12)
        SYSREG_MAP(SYS_IMP_APL_AMX_CTL_EL1, SYS_IMP_APL_AMX_CTL_EL12)
        /* FIXME:Might be wrong */
        SYSREG_PASS(SYS_IMP_APL_AMX_STATE_T)
        /* pass through PMU handling */
        SYSREG_PASS(SYS_IMP_APL_PMCR1)
        SYSREG_PASS(SYS_IMP_APL_PMCR2)
        SYSREG_PASS(SYS_IMP_APL_PMCR3)
        SYSREG_PASS(SYS_IMP_APL_PMCR4)
        SYSREG_PASS(SYS_IMP_APL_PMESR0)
        SYSREG_PASS(SYS_IMP_APL_PMESR1)
        SYSREG_PASS(SYS_IMP_APL_PMSR)
#ifndef DEBUG_PMU_IRQ
        SYSREG_PASS(SYS_IMP_APL_PMC0)
#endif
        SYSREG_PASS(SYS_IMP_APL_PMC1)
        SYSREG_PASS(SYS_IMP_APL_PMC2)
        SYSREG_PASS(SYS_IMP_APL_PMC3)
        SYSREG_PASS(SYS_IMP_APL_PMC4)
        SYSREG_PASS(SYS_IMP_APL_PMC5)
        SYSREG_PASS(SYS_IMP_APL_PMC6)
        SYSREG_PASS(SYS_IMP_APL_PMC7)
        SYSREG_PASS(SYS_IMP_APL_PMC8)
        SYSREG_PASS(SYS_IMP_APL_PMC9)

        /* Outer Sharable TLB maintenance instructions */
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 0)) // TLBI VMALLE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 1)) // TLBI VAE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 2)) // TLBI ASIDE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 3)) // TLBI VAAE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 5)) // TLBI VALE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 1, 7)) // TLBI VAALE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 5, 1)) // TLBI RVAE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 5, 3)) // TLBI RVAAE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 5, 5)) // TLBI RVALE1OS
        SYSREG_PASS(sys_reg(1, 0, 8, 5, 7)) // TLBI RVAALE1OS

        /* Outer Shareable nXS TLB maintenance instructions */
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 0)) // TLBI VMALLE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 1)) // TLBI VAE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 2)) // TLBI ASIDE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 3)) // TLBI VAAE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 5)) // TLBI VALE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 1, 7)) // TLBI VAALE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 5, 1)) // TLBI RVAE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 5, 3)) // TLBI RVAAE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 5, 5)) // TLBI RVALE1OSNXS
        SYSREG_PASS(sys_reg(1, 0, 9, 5, 7)) // TLBI RVAALE1OSNX

        case SYSREG_ISS(SYS_ACTLR_EL1):
            if (is_read) {
                if (cpu_features->actlr_el2)
                    regs[rt] = mrs(SYS_ACTLR_EL12);
                else
                    regs[rt] = mrs(SYS_IMP_APL_ACTLR_EL12);
            } else {
                if (cpu_features->actlr_el2)
                    msr(SYS_ACTLR_EL12, regs[rt]);
                else
                    msr(SYS_IMP_APL_ACTLR_EL12, regs[rt]);
            }
            return true;

        case SYSREG_ISS(SYS_IMP_APL_IPI_SR_EL1):
            if (is_read)
                regs[rt] = PERCPU(ipi_pending) ? IPI_SR_PENDING : 0;
            else if (regs[rt] & IPI_SR_PENDING)
                PERCPU(ipi_pending) = false;
            return true;

        /* the hypervisor needs this one for breakpoints and single-stepping */
        case SYSREG_ISS(SYS_MDSCR_EL1): {
            u64 *mdscr = hv_sptm_mode == SPTM_EMULATION ? hv_sprr_mdscr() : &PERCPU(mdscr);
            if (is_read)
                regs[rt] = *mdscr;
            else
                *mdscr = regs[rt];
            return true;
        }

        /* shadow the interrupt mode and state flag */
        case SYSREG_ISS(SYS_IMP_APL_PMCR0):
            if (is_read)
                regs[rt] = hv_pmcr0_read();
            else
                hv_pmcr0_write(regs[rt]);
            return true;

        /*
         * Handle this one here because m1n1/Linux (will) use it for explicit cpuidle.
         * We can pass it through; going into deep sleep doesn't break the HV since we
         * don't do any wfis that assume otherwise in m1n1. However, don't het macOS
         * disable WFI ret (when going into systemwide sleep), since that breaks things.
         */
        case SYSREG_ISS(SYS_IMP_APL_CYC_OVRD):
            if (is_read) {
                regs[rt] = mrs(SYS_IMP_APL_CYC_OVRD);
            } else {
                if (regs[rt] & (CYC_OVRD_DISABLE_WFI_RET | CYC_OVRD_FIQ_MODE_MASK))
                    return false;
                msr(SYS_IMP_APL_CYC_OVRD, regs[rt]);
            }
            return true;
            /* clang-format off */
        /* IPI handling */
        SYSREG_PASS(SYS_IMP_APL_IPI_CR_EL1)
        /* M1RACLES reg, handle here due to silly 12.0 "mitigation" */
        case SYSREG_ISS(sys_reg(3, 5, 15, 10, 1)):
            if (is_read)
                regs[rt] = 0;
            return true;
    }

    /* smp2d: shadow guest breakpoint/watchpoint registers in EL2 (was a host proxy round trip) */
    if (hv_dbg_shadow(reg, rt, is_read, regs))
        return true;

    return false;
}

static bool hv_handle_nested_eret(struct exc_info *ctx, bool is_gexit)
{
    if (FIELD_GET(ESR_EC, ctx->esr) != ESR_EC_ERET && !is_gexit) return false;

    const char* operation = is_gexit ? "GEXIT" : "ERET";

    if (!hv_is_nested()) hv_panic("Tried to handle nested %s while not in nested mode\n", operation);

    // Restore SPSR
    u64 spsr = in_gl12() ? mrs(SYS_IMP_APL_SPSR_GL12) : mrs(SPSR_EL12);
    if ((spsr & SPSR_M_EL2x) == SPSR_M_EL2x)
    {
        spsr &= ~SPSR_M_EL2x;
        spsr |= SPSR_M_EL1x;
    }
    ctx->spsr = spsr;

    if (is_gexit)
    {
        msr(SYS_IMP_APL_ASPSR_GL1, mrs(SYS_IMP_APL_ASPSR_GL12));
        hv_enable_genter_trap();
    }

    // Return to where the guest wanted to return to
    ctx->elr = in_gl12() ? mrs(SYS_IMP_APL_ELR_GL12) : mrs(ELR_EL12);

    return true;
}

static bool hv_handle_msr(struct exc_info *ctx, u64 iss)
{
    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;

    u64 *regs = ctx->regs;

    regs[31] = 0;

    switch (reg) {
        /* clang-format on */
        case SYSREG_ISS(SYS_IMP_APL_IPI_RR_LOCAL_EL1): {
            assert(!is_read);
            u64 mpidr = (regs[rt] & 0xff) | (mrs(MPIDR_EL1) & 0xffff00);
            for (int i = 0; i < MAX_CPUS; i++)
                if (mpidr == smp_get_mpidr(i)) {
                    pcpu[i].ipi_queued = true;
                    msr(SYS_IMP_APL_IPI_RR_LOCAL_EL1, regs[rt]);
                    return true;
                }
            return false;
        }
        case SYSREG_ISS(SYS_IMP_APL_IPI_RR_GLOBAL_EL1):
            assert(!is_read);
            u64 mpidr = (regs[rt] & 0xff) | ((regs[rt] & 0xff0000) >> 8);
            for (int i = 0; i < MAX_CPUS; i++) {
                if (mpidr == (smp_get_mpidr(i) & 0xffff)) {
                    pcpu[i].ipi_queued = true;
                    msr(SYS_IMP_APL_IPI_RR_GLOBAL_EL1, regs[rt]);
                    return true;
                }
            }
            return false;
#ifdef DEBUG_PMU_IRQ
        case SYSREG_ISS(SYS_IMP_APL_PMC0):
            if (is_read) {
                regs[rt] = mrs(SYS_IMP_APL_PMC0);
            } else {
                msr(SYS_IMP_APL_PMC0, regs[rt]);
                printf("msr(SYS_IMP_APL_PMC0, 0x%04lx_%08lx)\n", regs[rt] >> 32,
                       regs[rt] & 0xFFFFFFFF);
            }
            return true;
#endif
    }

    return hv_sprr_handle_msr(ctx, reg, rt, is_read);
}

static void hv_get_context(struct exc_info *ctx)
{
    ctx->spsr = hv_get_spsr();
    ctx->elr = hv_get_elr();
    ctx->esr = hv_get_esr();
    ctx->far = hv_get_far();
    ctx->afsr1 = hv_get_afsr1();
    ctx->sp[0] = mrs(SP_EL0);
    ctx->sp[1] = mrs(SP_EL1);
    ctx->sp[2] = (u64)ctx;
    ctx->cpu_id = smp_id();
    ctx->mpidr = mrs(MPIDR_EL1);

    sysop("isb");
}

static void hv_exc_entry(void)
{
    // Enable SErrors in the HV, but only if not already pending
    if (!(mrs(ISR_EL1) & 0x100))
        sysop("msr daifclr, 4");

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);
    hv_wdt_breadcrumb('X');
    exc_entry_time = mrs(CNTPCT_EL0);
    /* disable PMU counters in the hypervisor */
    u64 pmcr0 = mrs(SYS_IMP_APL_PMCR0);
    PERCPU(exc_entry_pmcr0_cnt) = pmcr0 & PMCR0_CNT_MASK;
    msr(SYS_IMP_APL_PMCR0, pmcr0 & ~PMCR0_CNT_MASK);
}

static void hv_exc_exit(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('x');
    hv_update_fiq();
    hv_sprr_timer_arm();
    /* reenable PMU counters */
    reg_set(SYS_IMP_APL_PMCR0, PERCPU(exc_entry_pmcr0_cnt));
    msr(CNTVOFF_EL2, stolen_time);
    spin_unlock(&bhl);
    hv_maybe_exit();
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(smp_id()), __ATOMIC_ACQUIRE);

    hv_set_spsr(ctx->spsr);
    hv_set_elr(ctx->elr);
    msr(SP_EL0, ctx->sp[0]);
    msr(SP_EL1, ctx->sp[1]);
}

static void hv_dump_serror(struct exc_info *ctx)
{
    /*
     * This doesn't do any sysreg access or similar to prevent any double faults which
     * may corrupt the output and make debugging these SErrors very hard.
     */
    printf("HV: Guest SError\n");
    printf("CPU: %lu\n", ctx->cpu_id);
    printf("MPIDR: 0x%lx\n", ctx->mpidr);
    printf("Registers: (@%p)\n", ctx->regs);
    printf("  x0-x3: %016lx %016lx %016lx %016lx\n", ctx->regs[0], ctx->regs[1], ctx->regs[2],
           ctx->regs[3]);
    printf("  x4-x7: %016lx %016lx %016lx %016lx\n", ctx->regs[4], ctx->regs[5], ctx->regs[6],
           ctx->regs[7]);
    printf(" x8-x11: %016lx %016lx %016lx %016lx\n", ctx->regs[8], ctx->regs[9], ctx->regs[10],
           ctx->regs[11]);
    printf("x12-x15: %016lx %016lx %016lx %016lx\n", ctx->regs[12], ctx->regs[13], ctx->regs[14],
           ctx->regs[15]);
    printf("x16-x19: %016lx %016lx %016lx %016lx\n", ctx->regs[16], ctx->regs[17], ctx->regs[18],
           ctx->regs[19]);
    printf("x20-x23: %016lx %016lx %016lx %016lx\n", ctx->regs[20], ctx->regs[21], ctx->regs[22],
           ctx->regs[23]);
    printf("x24-x27: %016lx %016lx %016lx %016lx\n", ctx->regs[24], ctx->regs[25], ctx->regs[26],
           ctx->regs[27]);
    printf("x28-x30: %016lx %016lx %016lx\n", ctx->regs[28], ctx->regs[29], ctx->regs[30]);
    printf("PC:       0x%lx\n", ctx->elr);
    printf("SP_EL0:   0x%lx\n", ctx->sp[0]);
    printf("SP_EL1:   0x%lx\n", ctx->sp[1]);
    printf("SPSR:     0x%lx\n", ctx->spsr);
    printf("FAR:      0x%lx\n", ctx->far);
    printf("ESR:      0x%lx\n", ctx->esr);

    iodev_console_flush();
}

/*
 * r4.10/r4.11: one line per reflected/answered exception. r4.11: the first 20 of each class, PLUS the
 * first occurrence of every new (class, ISS[15:0], PC) site, always; every event is counted per site
 * and the periodic status report prints the busiest sites (no blind spot after the first 20).
 */
#define HV_CLASS_LOG_FIRST 20
static u64 hv_class_count[64];
static struct hv_site hv_refl_sites[HV_SITE_N];
static u64 hv_refl_overflow;

static void hv_exc_class_log(struct exc_info *ctx, u32 ec, const char *what, u64 vbar, u32 off)
{
    u64 n = ++hv_class_count[ec & 63];
    bool fresh = hv_site_note(hv_refl_sites, HV_SITE_N, &hv_refl_overflow,
                              (ec << 16) | (u32)(ctx->esr & 0xffff), ctx->elr);
    if (n > HV_CLASS_LOG_FIRST && !fresh)
        return;
    static const char *const mname[16] = {"EL0t", "?", "?", "?", "EL1t", "EL1h"};
    u32 m = ctx->spsr & 0xf;
    printf("HV/win r4.14 EC 0x%02x #%ld%s %s: cpu%ld ESR 0x%lx PC 0x%lx FAR 0x%lx from %s", ec, (long)n,
           fresh ? " NEW-SITE" : "", what, (long)ctx->cpu_id, ctx->esr, ctx->elr, ctx->far,
           (ctx->spsr & 0x10) ? "AArch32" : (mname[m] ? mname[m] : "?"));
    if (vbar)
        printf(" -> VBAR_EL1 0x%lx +0x%x", vbar, off);
    printf("\n");
    iodev_console_flush();
}

/*
 * r4.10: deliver the trapped lower-EL synchronous exception to the guest's own EL1 vector, as the host
 * Python HV._lower() does (hv_sysreg_logic.h hv_reflect_compute has the exact semantics). The guest's
 * EL1 registers are written through the _EL12 accessors (HCR_EL2.E2H = 1 here, as hv_handle_nested_eret
 * reads SPSR_EL12/ELR_EL12); the new PC/PSTATE go back through ctx (hv_exc_exit sets ELR/SPSR_EL2).
 */
static bool hv_reflect_sync(struct exc_info *ctx, u32 ec)
{
    struct hv_reflect r;
    u64 vbar = mrs(SYS_VBAR_EL12);
    if (!hv_reflect_compute(ctx->elr, ctx->spsr, ctx->esr, ctx->far, vbar, mrs(SYS_SCTLR_EL12),
                            0 /* EXC_SYNC */, &r))
        return false; /* EL2/AArch32 origin: escalate, as _lower refuses it */
    hv_exc_class_log(ctx, ec, "reflected to guest EL1 (NOT escalated)", vbar, r.offset);
    msr(SYS_ELR_EL12, r.elr_el1);
    msr(SYS_SPSR_EL12, r.spsr_el1);
    msr(SYS_ESR_EL12, r.esr_el1);
    msr(SYS_FAR_EL12, r.far_el1);
    ctx->elr = r.new_elr;
    ctx->spsr = r.new_spsr;
    return true;
}

bool hv_exc_sync(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('S');
    hv_get_context(ctx);
    hv_win_diag_note(HV_WIN_DIAG_TRAP, ctx, 0);
    bool handled = false;
    u32 ec = FIELD_GET(ESR_EC, ctx->esr);
    /* r4.11 diagnostics (lock-free counters; 1 vCPU) */
    hv_exits_sync++;
    hv_trap_ec[ec]++;
    hv_trap_ec_pc[ec] = ctx->elr;
    hv_trap_ec_far[ec] = ctx->far;

    bool do_gexit = false;

    switch (ec) {
        case ESR_EC_ERET:
            handled = hv_sprr_handle_eret(ctx, false);
            break;
        case ESR_EC_MSR:
            hv_wdt_breadcrumb('m');
            /* ICC_SGI1R_EL1 (Windows GIC model) takes the vGIC's own lock, not the big HV lock */
            handled = hv_win_handle_msr(ctx, FIELD_GET(ESR_ISS, ctx->esr)) ||
                      hv_handle_msr_unlocked(ctx, FIELD_GET(ESR_ISS, ctx->esr));
            break;
        case ESR_EC_IMPDEF:
            hv_wdt_breadcrumb('a');
            switch (FIELD_GET(ESR_ISS, ctx->esr)) {
                case ESR_ISS_IMPDEF_MSR:
                    handled = hv_handle_msr_unlocked(ctx, ctx->afsr1);
                    break;
            }
            break;
        case ESR_EC_HVC:
            hv_wdt_breadcrumb('h');
            if (hv_hvc_dispatch_unlocked(ctx, FIELD_GET(ESR_ISS, ctx->esr))) {
                hv_wdt_breadcrumb('s');
                return do_gexit;
            }
            break;
    }

    if (handled) {
        hv_wdt_breadcrumb('#');

        /* A trapped ERET already contains the right target address in ELR */
        if (ec != ESR_EC_ERET) {
            ctx->elr += 4;
            hv_set_elr(ctx->elr);
        }

        hv_update_fiq();
        hv_wdt_breadcrumb('s');
        return do_gexit;
    }

    hv_exc_entry();

    bool protect_elr = false;

    switch (ec) {
        case ESR_EC_DABORT_LOWER:
            hv_wdt_breadcrumb('D');
            handled = hv_handle_dabort(ctx);
            break;
        case ESR_EC_IABORT_LOWER:
            /* Windows PMU rewrite: first instruction fetch from a guest RAM page (stage-2 XN) */
            hv_wdt_breadcrumb('I');
            handled = protect_elr = hv_win_handle_iabort(ctx);
            break;
        case ESR_EC_MSR:
            hv_wdt_breadcrumb('M');
            handled = hv_handle_msr(ctx, FIELD_GET(ESR_ISS, ctx->esr));
            break;
        case ESR_EC_SMC:
            /* SMCCC/PSCI over SMC (only trapped when HV_WIN_F_PSCI_SMC set HCR_EL2.TSC) */
            hv_wdt_breadcrumb('C');
            handled = hv_win_handle_smc(ctx);
            break;
        case ESR_EC_ERET:
            hv_wdt_breadcrumb('N');
            if (hv_sptm_mode == SPTM_EMULATION)
                handled = hv_sprr_handle_eret(ctx, true);
            else
                handled = hv_handle_nested_eret(ctx, false);
            protect_elr = handled;
            break;
        case ESR_EC_IMPDEF:
            hv_wdt_breadcrumb('A');
            switch (FIELD_GET(ESR_ISS, ctx->esr)) {
                case ESR_ISS_IMPDEF_MSR:
                    handled = hv_handle_msr(ctx, ctx->afsr1);
                    break;
                case ESR_ISS_IMPDEF_GEXIT:
                    handled = do_gexit = protect_elr = hv_handle_nested_eret(ctx, true);
                    break;
                case ESR_ISS_IMPDEF_GENTER:
                    if (!hv_genter_trap_enabled)
                        handled = protect_elr = true;
            }
            break;
        case ESR_EC_HVC:
            hv_wdt_breadcrumb('H');
            handled = hv_win_handle_hvc(ctx) || hv_hvc_dispatch(ctx, FIELD_GET(ESR_ISS, ctx->esr));
            protect_elr = true; // HVC alread leaves ELR past the instruction
            break;
    }

    if (!handled) {
        /*
         * r4.9/r4.10: what the resident does itself instead of escalating (hv_sysreg_logic.h
         * hv_unhandled_action): BRK always reflected (= host handle_brk -> _lower); while the link is
         * locked, sysreg RAZ/WI, debug exceptions reflected, HVC/SMC answered with an SMCCC error.
         */
        struct hv_esc_info e;
        hv_esc_decode(ctx->esr, ctx->afsr1, &e);
        switch (hv_unhandled_action(HV_SYSREG_FAILSOFT, s1n_link_locked(), &e)) {
            case HV_ACT_SYSREG_RAZWI: {
                hv_failsoft_count++;
                if (e.is_read && e.rt != 31)
                    ctx->regs[e.rt] = 0;
                /* r4.11: every new (register, direction, PC) printed at once; first 20 overall */
                u32 key = (u32)HV_SR_ENC(e.op0, e.op1, e.crn, e.crm, e.op2) << 1 | e.is_read;
                bool fresh = hv_site_note(hv_fs_sites, HV_SITE_N, &hv_fs_overflow, key, ctx->elr);
                if (fresh || hv_failsoft_count <= 20)
                    hv_esc_log(ctx, e.is_read ? "FAILSOFT read-as-zero (NOT escalated)"
                                              : "FAILSOFT write-ignored (NOT escalated)",
                               hv_failsoft_count, START_EXCEPTION_LOWER, EXC_SYNC);
                handled = true;
                protect_elr = false;
                break;
            }
            case HV_ACT_REFLECT:
                if (hv_reflect_sync(ctx, e.ec)) {
                    handled = true;
                    protect_elr = true; /* ELR/SPSR now point at the guest's EL1 vector */
                }
                break;
            case HV_ACT_SMCCC_ERR:
                hv_exc_class_log(ctx, e.ec, "answered x0=SMCCC_NOT_SUPPORTED (NOT escalated)", 0, 0);
                ctx->regs[0] = HV_SMCCC_NOT_SUPPORTED;
                handled = true;
                protect_elr = e.ec == ESR_EC_HVC; /* HVC: ELR already past; SMC trap: ELR at the SMC */
                break;
            case HV_ACT_ESCALATE:
                break;
        }
    }

    if (handled) {
        hv_wdt_breadcrumb('+');
        if (!protect_elr)
            ctx->elr += 4;
    } else {
        hv_wdt_breadcrumb('-');
        // VM code can forward a nested SError exception here
        if (FIELD_GET(ESR_EC, ctx->esr) == ESR_EC_SERROR) {
            hv_dump_serror(ctx);
            hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);
        } else
            hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SYNC, NULL);
    }

    // We handle trapped GENTER instructions by just disabling the trap and executing the
    // instruction again (not advancing elr by one word). This is easier than issuing the GENTER
    // from the HV, since this would switch to GL2 instead of GL1. Due to nested virt, we have to
    // handle every GEXIT anyways, so we can always enable the trap again for the next GENTER.
    if (ec == ESR_EC_IMPDEF && FIELD_GET(ESR_ISS, ctx->esr) == ESR_ISS_IMPDEF_GENTER)
        hv_disable_genter_trap();

    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('s');

    return do_gexit;
}

void hv_exc_irq(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('I');
    hv_exits_irq++;
    hv_get_context(ctx);
    hv_exc_entry();
    /*
     * vGIC model: a physical IRQ at EL2 is an AIC device line to route to the guest's emulated GIC.
     * In the AIC-passthrough (M0) model IRQs stay routed to EL1 and this vector is not taken for
     * device lines, so hv_win_handle_irq() returns false and the stock proxy path runs.
     */
    if (!hv_win_handle_irq(ctx))
        hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_IRQ, NULL);
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('i');
}

void hv_exc_fiq(struct exc_info *ctx)
{
    bool tick = false;

    hv_exits_fiq++;
    hv_win_fiq_entry();
    hv_maybe_exit();

    if (mrs(CNTP_CTL_EL0) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) {
        msr(CNTP_CTL_EL0, CNTx_CTL_ISTATUS | CNTx_CTL_IMASK | CNTx_CTL_ENABLE);
        tick = true;
    }

    int interruptible_cpu = hv_pinned_cpu;
    if (interruptible_cpu == -1)
        interruptible_cpu = boot_cpu_idx;

    if (smp_id() != interruptible_cpu && !(mrs(ISR_EL1) & 0x40) && hv_want_cpu == -1) {
        // Non-interruptible CPU and it was just a timer tick (or spurious), so just update FIQs
        hv_update_fiq();
        hv_arm_tick(true);
        return;
    }

    // Slow (single threaded) path
    hv_wdt_breadcrumb('F');
    hv_get_context(ctx);
    hv_exc_entry();

    // Only poll for HV events in the interruptible CPU
    if (tick) {
        if (smp_id() == interruptible_cpu) {
            hv_tick(ctx);
            hv_arm_tick(false);
        } else {
            hv_arm_tick(true);
        }
    }

    if (mrs(CNTV_CTL_EL0) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) {
        msr(CNTV_CTL_EL0, CNTx_CTL_ISTATUS | CNTx_CTL_IMASK | CNTx_CTL_ENABLE);
        hv_exc_proxy(ctx, START_HV, HV_VTIMER, NULL);
    }

    u64 reg = mrs(SYS_IMP_APL_PMCR0);
    if ((reg & (PMCR0_IMODE_MASK | PMCR0_IACT)) == (PMCR0_IMODE_FIQ | PMCR0_IACT)) {
#ifdef DEBUG_PMU_IRQ
        printf("[FIQ] PMC IRQ, masking and delivering to the guest\n");
#endif
        reg_clr(SYS_IMP_APL_PMCR0, PMCR0_IACT | PMCR0_IMODE_MASK);
        PERCPU(pmc_pending) = true;
    }

    reg = mrs(SYS_IMP_APL_UPMCR0);
    if (FIELD_GET(UPMCR0_IMODE_T8020, reg) == UPMCR0_IMODE_FIQ &&
        (mrs(SYS_IMP_APL_UPMSR) & UPMSR_IACT)) {
        printf("[FIQ] UPMC IRQ, masking");
        reg_clr(SYS_IMP_APL_UPMCR0, UPMCR0_IMODE_T8020);
        hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_FIQ, NULL);
    }

    if (mrs(SYS_IMP_APL_IPI_SR_EL1) & IPI_SR_PENDING) {
        if (PERCPU(ipi_queued)) {
            PERCPU(ipi_pending) = true;
            PERCPU(ipi_queued) = false;
        }
        msr(SYS_IMP_APL_IPI_SR_EL1, IPI_SR_PENDING);
        sysop("isb");
    }

    hv_maybe_switch_cpu(ctx, START_HV, HV_CPU_SWITCH, NULL);

    // Handles guest timers
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('f');
}

void hv_exc_serr(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('E');
    hv_get_context(ctx);
    hv_exc_entry();
    hv_dump_serror(ctx);
    hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('e');
}

/*
 * r4.11 diagnostics: periodic status report, called from hv_tick() (1 kHz hypervisor tick, boot CPU).
 * The tick is a physical FIQ routed to EL2, taken even while the guest runs with DAIF masked, so ctx is
 * a fresh sample of the guest's own state every report. Printed on the resident console (iodev; "TTY>"
 * on the host), which the locked host link does not gate. Every ~5 s:
 *   ST  #n t  PC/LR/SP0/SP1/PSTATE, exits (irq/fiq/sync) and sync traps by EC since the last report with
 *       the last PC/FAR of each, escalations, fail-soft, reflections
 *   RF  busiest reflection sites (class/imm/PC) and fail-soft registers since the last report
 *   TM  guest timers (EL02 views), virtual counter, EL2 IMASK holds
 *   VG  vGIC (hv_vgic3_status_line)
 *   NV  emulated NVMe + native backend (hv_win_storage_status_lines, two lines)
 *   PU  PL011 bytes, PMU rewrite counters
 * Storage events (fatal, refusal, error, ANS state change) are printed on the tick they happen.
 */
#define HV_STATUS_PERIOD_S 5
extern u64 hv_win_pl011_stats(void);
static u64 st_last_t, st_n, st_t0;
static u64 st_prev_ec[64], st_prev_irq, st_prev_fiq, st_prev_sync, st_prev_esc, st_prev_fs;
static u64 st_prev_pmu[HV_WIN_PMU_NR_STATS];

static void hv_status_report(struct exc_info *ctx, u64 now, u64 freq)
{
    static const char *const mname[16] = {"EL0t", "?", "?", "?", "EL1t", "EL1h"};
    const char *P = "HV/win r4.14";
    u32 m = ctx->spsr & 0xf;
    st_n++;
    printf("%s ST #%lu t+%lus PC 0x%lx LR 0x%lx SP0 0x%lx SP1 0x%lx PSTATE 0x%lx (%s DAIF %lx) | exits irq +%lu "
           "fiq +%lu sync +%lu | esc +%lu failsoft +%lu refl %lu ovf %lu | EC",
           P, (unsigned long)st_n, (unsigned long)((now - st_t0) / freq), ctx->elr, ctx->regs[30], ctx->sp[0],
           ctx->sp[1], ctx->spsr, (ctx->spsr & 0x10) ? "AArch32" : (mname[m] ? mname[m] : "?"),
           (ctx->spsr >> 6) & 0xf, (unsigned long)(hv_exits_irq - st_prev_irq),
           (unsigned long)(hv_exits_fiq - st_prev_fiq), (unsigned long)(hv_exits_sync - st_prev_sync),
           (unsigned long)(hv_esc_count - st_prev_esc), (unsigned long)(hv_failsoft_count - st_prev_fs),
           (unsigned long)hv_class_count[0x3c], (unsigned long)(hv_refl_overflow + hv_fs_overflow));
    for (int ec = 0; ec < 64; ec++) {
        u64 d = hv_trap_ec[ec] - st_prev_ec[ec];
        if (d)
            printf(" %02x:+%lu@0x%lx/0x%lx", ec, (unsigned long)d, hv_trap_ec_pc[ec], hv_trap_ec_far[ec]);
        st_prev_ec[ec] = hv_trap_ec[ec];
    }
    printf("\n");
    st_prev_irq = hv_exits_irq; st_prev_fiq = hv_exits_fiq; st_prev_sync = hv_exits_sync;
    st_prev_esc = hv_esc_count; st_prev_fs = hv_failsoft_count;

    /* busiest reflection / fail-soft sites since the last report */
    bool taken[HV_SITE_N] = {false}, any = false;
    for (int k = 0; k < 4; k++) {
        int i = hv_site_top(hv_refl_sites, HV_SITE_N, taken);
        if (i < 0) break;
        if (!any) printf("%s RF refl", P);
        any = true;
        taken[i] = true;
        printf(" ec%02x/imm%04x@0x%lx +%lu (=%lu)", hv_refl_sites[i].key >> 16, hv_refl_sites[i].key & 0xffff,
               hv_refl_sites[i].pc, (unsigned long)(hv_refl_sites[i].count - hv_refl_sites[i].reported),
               (unsigned long)hv_refl_sites[i].count);
    }
    for (int i = 0; i < HV_SITE_N; i++) hv_refl_sites[i].reported = hv_refl_sites[i].count;
    bool taken2[HV_SITE_N] = {false};
    for (int k = 0; k < 4; k++) {
        int i = hv_site_top(hv_fs_sites, HV_SITE_N, taken2);
        if (i < 0) break;
        if (!any) printf("%s RF", P);
        any = true;
        taken2[i] = true;
        u32 enc = hv_fs_sites[i].key >> 1;
        printf(" failsoft %s s%u_%u_c%u_c%u_%u@0x%lx +%lu", (hv_fs_sites[i].key & 1) ? "mrs" : "msr", enc >> 14,
               (enc >> 11) & 7, (enc >> 7) & 15, (enc >> 3) & 15, enc & 7, hv_fs_sites[i].pc,
               (unsigned long)(hv_fs_sites[i].count - hv_fs_sites[i].reported));
    }
    for (int i = 0; i < HV_SITE_N; i++) hv_fs_sites[i].reported = hv_fs_sites[i].count;
    if (any) printf("\n");

    /* guest timers as the guest sees them (EL02 views), virtual counter, our IMASK holds */
    u64 voff = mrs(CNTVOFF_EL2);
    printf("%s TM cntv ctl 0x%lx cval 0x%lx | cntp ctl 0x%lx cval 0x%lx | vcnt 0x%lx pcnt 0x%lx voff 0x%lx | "
           "hv-masked 0x%x guest-imask 0x%x\n", P, mrs(SYS_CNTV_CTL_EL02), mrs(SYS_CNTV_CVAL_EL02),
           mrs(SYS_CNTP_CTL_EL02), mrs(SYS_CNTP_CVAL_EL02), now - voff, now, voff, PERCPU(tmr_hv_masked),
           PERCPU(tmr_guest_imask));
    hv_vgic3_status_line("HV/win r4.14 VG");
    hv_win_storage_status_lines("HV/win r4.14 NV");
    u64 pl = hv_win_pl011_stats();
    printf("%s PU pl011 tx %lu rx %lu drop %lu | pmu", P, (unsigned long)(pl & 0xffffffff),
           (unsigned long)((pl >> 32) & 0xffff), (unsigned long)(pl >> 48));
    static const struct { int idx; const char *name; } pk[] = {
        {HV_WIN_PMU_SCANS, "scan"}, {HV_WIN_PMU_PAGES_REWRITTEN, "rewr"}, {HV_WIN_PMU_WRITE_FLIPS, "wflip"},
        {HV_WIN_PMU_WALK_FLIPS, "walk"}, {HV_WIN_PMU_PINS, "pin"}, {HV_WIN_PMU_SWEEPS, "sweep"},
        {HV_WIN_PMU_SPURIOUS, "spur"}, {HV_WIN_PMU_IPA_FAIL, "ipafail"}, {HV_WIN_PMU_NOT_OURS, "notours"},
        {HV_WIN_PMU_STUCK, "stuck"}, {HV_WIN_PMU_DMA_RESETS, "dma"}};
    for (unsigned i = 0; i < sizeof(pk) / sizeof(pk[0]); i++) {
        u64 v = hv_win_pmu_stat(pk[i].idx);
        printf(" %s %lu(+%lu)", pk[i].name, (unsigned long)v, (unsigned long)(v - st_prev_pmu[pk[i].idx]));
        st_prev_pmu[pk[i].idx] = v;
    }
    printf("\n");
    iodev_console_flush();
}

void hv_status_tick(struct exc_info *ctx)
{
    if (!hv_win_enabled())
        return;
    if (hv_win_storage_watch())
        iodev_console_flush();
    u64 now = mrs(CNTPCT_EL0), freq = mrs(CNTFRQ_EL0);
    if (!st_t0) {
        st_t0 = st_last_t = now;
        return;
    }
    if (!hv_status_due(now, st_last_t, freq, HV_STATUS_PERIOD_S))
        return;
    st_last_t = now;
    hv_status_report(ctx, now, freq);
}
