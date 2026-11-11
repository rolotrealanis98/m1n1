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
#include "utils.h"

#include "win/hv_win.h"
#include "win/hv_win_regs.h" /* smp2f: ICH_MISR/ELRSR for the snapshot */
#include "hv_wfi_logic.h"

#define SYS_FPCR sys_reg(3, 3, 4, 4, 0)
#define SYS_FPSR sys_reg(3, 3, 4, 4, 1)
#include "win/s1/native_link.h"
#include "win/hv_win_storage.h"
#include "hv_sysreg_logic.h"
#include "win/hv_win_perf.h"

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
    /* smp2d WFI emulation counters (this CPU only) */
    u64 wfi_traps;
    u64 wfi_wake_fiq, wfi_wake_irq, wfi_wake_exit, wfi_wake_cap, wfi_spins;
    u64 wfi_wake_virq;  /* smp2e: woke on a pending virtual interrupt (LR / vFIQ / dirty) */
    u64 wfi_immediate;  /* smp2e: completed with no physical WFI (something already pending) */
    u64 wfx_other;      /* smp2e: trapped WFIT/WFE/WFET (TI != 0), completed at once */
    u64 wfi_longest;    /* CNTPCT ticks of the longest single emulated WFI wait */
    /* smp2f */
    u64 wfi_wake_kvm_only; /* woke on an LR the strict GICv3 rule (smp2e) would have slept on */
    u64 wfi_bg_armed;      /* physical WFIs with the EL2 timer pulled in to a guest timer deadline */
    u64 wfi_bg_fired;      /* ... after which the EL2 timer had fired: the guest timer's own FIQ did
                              not end the WFI within 50 us of its deadline */
    u64 wfi_idle_long;     /* physical WFIs longer than 10 ms */
    u64 wfi_idle_longest;  /* CNTPCT ticks of the longest single physical WFI */
    u64 vctx_idles;        /* physical WFIs with the interface/timer state saved around them */
    u64 vctx_lost;         /* ... after which some of it had changed (restored) */
    u64 vctx_lost_class[WFI_VCTX_CLASSES];
    u64 vctx_first_t, vctx_first_dur; /* CNTPCT and length of the first such WFI */
    u32 vctx_first_mask;
    struct wfi_vctx vctx_first_before, vctx_first_after;
    /* smp2g (review W1/W2) */
    u64 wfi_bg_seq;       /* background-timer arm decisions (every 8th uses the long probe slack) */
    u64 wfi_bg_parked;    /* guest timer enabled, but no usable deadline (far future or wrapped) */
    u64 wfi_bg_passed;    /* deadline already passed with ISTATUS clear at the first read */
    u32 wfi_early[WFI_WSRC_N];      /* physical WFIs that ended before the guest deadline, by source */
    struct wfi_lat wfi_lat[2][WFI_WSRC_N]; /* ... at/after it: lateness by source; [1] = probe waits */
    /* sched1: vGIC kick (another CPU's IPI for this vCPU) to the end of the emulated WFI it ended */
    struct wfi_lat kick_lat;  /* the kick arrived while this CPU was in a physical WFI */
    u64 kick_immediate;       /* ... arrived before the physical WFI (the wait completed at once) */
    /* smp2d async-exit GPR integrity snapshot (suspect A: a callee-saved reg lost across an exit) */
    bool integ_async;
    u8 integ_reason;
    u64 integ_gpr[10]; /* x19..x28 */
    u64 integ_sp0, integ_sp1, integ_tpidr;
    u64 integ_mismatch; /* exits (async or WFI wait) where x19..x28, SP_EL0/1 or TPIDR_EL1 changed */
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
 * perf-exits (hv_win_perf.h): options selected by P_HV_WIN_PERF before the guest starts; all 0 = the
 * old behaviour. hv_perf_fast_sync is read by the EL2 sync vector (hv_asm.S) to choose the fast entry.
 */
u8 hv_perf_fast_sync;
static u32 hv_perf_flags;
static u64 hv_perf_fast_mdscr[MAX_CPUS];               /* MDSCR_EL1 accesses done by the fast path */
static u64 hv_perf_el2_sync, hv_perf_el2_irq, hv_perf_el2_fiq; /* CNTPCT ticks in the C exit paths */
static struct hv_perf_sr hv_perf_sr_tab[HV_PERF_SR_N];  /* busiest trapped system registers */
static u64 hv_perf_sr_other;

void hv_exc_perf_set(u32 flags)
{
    hv_perf_flags = flags;
    hv_perf_fast_sync = !!(flags & HV_PERF_FAST_MDSCR);
    sysop("dsb ish");
}

/* The guest's MDSCR_EL1 as EL2 shadows it on this CPU (HV_PERF_MDSCR_PASS loads it into the real one) */
u64 hv_exc_mdscr_shadow(void)
{
    return PERCPU(mdscr);
}

static inline u64 hv_perf_t0(void)
{
    return (hv_perf_flags & HV_PERF_STATS) ? mrs(CNTPCT_EL0) : 0;
}

static inline void hv_perf_t1(u64 *acc, u64 t0)
{
    if (t0)
        __atomic_fetch_add(acc, mrs(CNTPCT_EL0) - t0, __ATOMIC_RELAXED);
}

/*
 * smp2f: per-CPU published snapshot of the vCPU's interface/timer state, written by the CPU itself
 * (its registers are not readable from another CPU), read and printed by the boot CPU only
 * (hv_win_smp_dump). Slot 0: last emulated-WFI entry/exit; slot 1: last FIQ entry. seq is odd while
 * the owner writes the slot.
 */
#define HV_SNAP_WFI_ENTRY 1
#define HV_SNAP_WFI_EXIT  2
#define HV_SNAP_FIQ       5
struct hv_smp_snap {
    u64 seq;
    u64 t, where;
    u64 elr, spsr, hcr, isr, misr, elrsr, cnthp_ctl, cnthp_cval;
    u64 x30, sp_el1, sp_el0; /* smp2g: the guest's link register and stack pointers */
    u32 wake;                /* smp2g: wfi-exit only: enum wfi_wake */
    u32 tmr_hv_masked, tmr_guest_imask;
    struct wfi_vctx v;
};
static struct hv_smp_snap hv_snap[MAX_CPUS][2];

/* smp2d: guest FP/SIMD save area, preserved across the physical WFI (WFI loses FP on M4+). */
static u8 hv_wfi_fp[MAX_CPUS][32 * 16] ALIGNED(16);

void hv_wfi_idle(void); /* hv_asm.S: save-around-WFI, Apple M4+ register-safe idle */

/*
 * sched1: CNTPCT of the first vGIC kick (vgic_unlock -> smp_send_ipi) for each CPU not yet consumed by
 * that CPU's emulated WFI; 0 = none. Written by the sender (first writer wins), cleared by the owner at
 * WFI entry and consumed at WFI exit, so only kicks sent during a wait are measured.
 */
static u64 hv_kick_t[MAX_CPUS];

void hv_win_kick_note(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    u64 zero = 0, now = mrs(CNTPCT_EL0);
    if (!now)
        now = 1;
    __atomic_compare_exchange_n(&hv_kick_t[cpu], &zero, now, false, __ATOMIC_RELAXED,
                                __ATOMIC_RELAXED);
}

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

/*
 * smp2: program this CPU's CNTVOFF_EL2 with the value every slow-path exit writes (hv_exc_exit()).
 * Called by hv_init_secondary(): a secondary otherwise enters the guest with its reset/UNKNOWN
 * CNTVOFF_EL2 and keeps it until its first slow exit, so CNTVCT_EL0 would differ between vCPUs.
 */
void hv_sync_cntvoff(void)
{
    msr(CNTVOFF_EL2, stolen_time);
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
 * smp2d: handle a guest debug breakpoint/watchpoint register access per CPU in EL2, lock-free, so it
 * never reaches the host Python proxy (which rewrites the whole saved register frame on each access).
 * Mirrors the proxy's "Shadow" behaviour (store on write, read back what was written, default 0) for
 * DBGB{V,C}Rn_EL1 / DBGW{V,C}Rn_EL1 (op0=2 op1=0 CRn=0 op2 in 4..7). These are never programmed into
 * real hardware, so the guest's breakpoints do not actually fire at EL1. Returns false for every
 * other register (including MDSCR_EL1 and the OS-lock/status regs, handled by their own cases) so the
 * caller continues its switch.
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

/*
 * smp2d/smp2e guest-WFI emulation (bugcheck-0A fix). HCR_EL2.TWI is set for the Windows guest with >1
 * vCPU (hv_win_configure), so the guest's WFI traps to EL2 (EC 0x01, lock-free fast path) instead of
 * running natively. The guest's register frame is already on this CPU's EL2 stack (hv_asm.S
 * _hv_entry): the T6050 clears GPRs and FP across a retention WFI, but SP and stack memory survive
 * (owner's Linux idle-apple.S header; m1n1 kboot.c:370 "M4 and later lose architectural state in WFI
 * and WFIT"; m1n1's own spin table parks CPUs at EL2 in _deep_wfi_helper).
 *
 * smp2e (review F1): the wait completes on exactly what completes a native WFI at EL1, which includes a
 * virtual interrupt pending for this vCPU even with PSTATE.I set; a pending list register does not
 * wake a WFI executed at EL2, so each iteration of wfi_wait() (hv_wfi_logic.h) first runs
 * hv_update_fiq() (timer gating, PPI levels, rebuild of a dirty vCPU), then checks, with EL2
 * interrupts masked throughout: exit/switch requests; virtual work (hv_vgic3_wfi_pending(): a list
 * register signalling under the GICv3 priority/group rule, or the vCPU left dirty by another CPU;
 * HCR_EL2.VF/VI); then a pending physical FIQ/IRQ. Only then does it idle the core (hv_wfi_idle,
 * the Linux/m1n1 save-around-wfi method, with the guest's FP/SIMD saved around it). Anything that can
 * become pending after the check arrives physically (IPI kick after vgic_mark, guest timer FIQ while
 * EL2 does not hold its IMASK, device IRQ, hv tick), so the physical WFI falls through: no lost
 * wake-up. Bounded by HV_WFI_MAX_SPINS physical WFIs, each ended by the hypervisor tick at the latest.
 *
 * smp2e (review F2): TWI also traps WFIT (ESR.ISS.TI = 2). WFIT/WFE/WFET complete at once (an early
 * completion is architecturally allowed), and FEAT_WFxT is hidden from the guest while trapping is on
 * (hv_win_guest_id_reg), as the owner's Linux does on M4+ (idreg-override.c: wfxt=0).
 *
 * smp2e (review F3): the guest frame's x19-x28 and SP_EL0/1, and TPIDR_EL1, are compared across the
 * wait, so the first run measures whether the EL2 stack and those registers survive retention.
 *
 * Per-vCPU: PERCPU state, hv_wfi_fp[smp_id()] and this CPU's own stack frame only.
 */
#define HV_WFI_MAX_SPINS 64

static void hv_integ_report(struct exc_info *ctx, const char *what, int reg, u64 was, u64 now,
                            unsigned int reason);

struct hv_wfi_cx {
    int cpu;
    bool kvm_only;   /* the last virt_pending answer woke only under KVM's rule */
    bool bg_armed;   /* pre_idle pulled the EL2 timer in to a guest deadline */
    bool have_dl;    /* smp2g: a guest timer deadline was known before the physical WFI ... */
    bool probe;      /* ... this wait uses the long (1 ms) slack */
    u64 dl;          /* ... the deadline, in CNTPCT ticks */
    u64 t_idle;      /* CNTPCT at the start of the physical WFI */
    u32 lost;        /* WFI_VCTX_* classes found changed after any physical WFI of this trap */
    struct wfi_vctx saved;
};

static void hv_wfi_op_update(void *cx)
{
    UNUSED(cx);
    hv_update_fiq();
}

static bool hv_wfi_op_exit(void *cx)
{
    int cpu = ((struct hv_wfi_cx *)cx)->cpu;
    return hv_cpu_should_exit() || hv_want_cpu != -1 ||
           (hv_pinned_cpu != -1 && hv_pinned_cpu != cpu);
}

/* smp2f: KVM's wake rule (hv_vgic3_wfi_pending / wfi_lr_wakes); counts where smp2e's rule differs */
static bool hv_wfi_op_virt(void *cx_)
{
    struct hv_wfi_cx *cx = cx_;

    cx->kvm_only = false;
    if (mrs(HCR_EL2) & (HCR_VF | HCR_VI))
        return true;
    int p = hv_vgic3_wfi_pending();
    cx->kvm_only = (p & HV_VGIC3_WFI_WAKE) && !(p & (HV_VGIC3_WFI_STRICT | HV_VGIC3_WFI_DIRTY));
    return p & (HV_VGIC3_WFI_DIRTY | HV_VGIC3_WFI_WAKE);
}

static u64 hv_wfi_op_isr(void *cx)
{
    UNUSED(cx);
    return mrs(ISR_EL1);
}

static void hv_wfi_op_idle(void *cx)
{
    int cpu = ((struct hv_wfi_cx *)cx)->cpu;
    /* Preserve the guest's FP/SIMD across the retention WFI (Linux idle.c:117-122: FP does not survive) */
    get_simd_state(hv_wfi_fp[cpu]);
    u64 fpsr = mrs(SYS_FPSR), fpcr = mrs(SYS_FPCR);
    hv_wfi_idle();
    put_simd_state(hv_wfi_fp[cpu]);
    msr(SYS_FPSR, fpsr);
    msr(SYS_FPCR, fpcr);
}

/* smp2f: the vCPU's interface (hv_vgic3.c), guest timer and EL1 words, for wfi_vctx_check_restore() */
static void hv_vctx_read(void *cx, struct wfi_vctx *v)
{
    UNUSED(cx);
    hv_vgic3_vctx_read(v);
    v->cntv_ctl = mrs(CNTV_CTL_EL02);
    v->cntv_cval = mrs(SYS_CNTV_CVAL_EL02);
    v->cntp_ctl = mrs(CNTP_CTL_EL02);
    v->cntp_cval = mrs(SYS_CNTP_CVAL_EL02);
    v->cntvoff = mrs(CNTVOFF_EL2);
    v->el1[0] = mrs(SYS_SCTLR_EL12);
    v->el1[1] = mrs(SYS_VBAR_EL12);
    v->el1[2] = mrs(SYS_TTBR1_EL12);
    v->el1[3] = mrs(TPIDR_EL1);
    v->el2[0] = mrs(CNTHCTL_EL2); /* smp2g: compare-only */
    v->el2[1] = mrs(HCR_EL2);
}

static void hv_vctx_write(void *cx, const struct wfi_vctx *v, u32 classes)
{
    UNUSED(cx);
    if (classes & WFI_VCTX_CNTVOFF)
        msr(CNTVOFF_EL2, v->cntvoff);
    if (classes & (WFI_VCTX_TCVAL | WFI_VCTX_TCTL)) {
        msr(SYS_CNTV_CVAL_EL02, v->cntv_cval);
        msr(SYS_CNTP_CVAL_EL02, v->cntp_cval);
        msr(CNTV_CTL_EL02, v->cntv_ctl & ~CNTx_CTL_ISTATUS);
        msr(CNTP_CTL_EL02, v->cntp_ctl & ~CNTx_CTL_ISTATUS);
    }
    hv_vgic3_vctx_write(v, classes & (WFI_VCTX_HCR | WFI_VCTX_VMCR | WFI_VCTX_APR | WFI_VCTX_LR));
}

/* The guest's own IMASK of timer t (EL2 may be holding the hardware bit, hv_tmr_gate) */
static bool hv_tmr_guest_imask(int t, u64 ctl)
{
    if (PERCPU(tmr_hv_masked) & BIT(t))
        return PERCPU(tmr_guest_imask) & BIT(t);
    return ctl & CNTx_CTL_IMASK;
}

/*
 * smp2f pre_idle: (1) save the interface/timer state (KVM: vcpu_put before the host idles);
 * (2) KVM's background timer (kvm_timer_schedule): if a guest timer will fire before the EL2 tick,
 * pull the EL2 physical timer (CNTP_*_EL0 at EL2 = CNTHP, m1n1's tick) in to that deadline + 50 us, so
 * the core wakes then even if a guest timer's own FIQ does not end a WFI executed at EL2 (and
 * post_idle counts it as wfi_bg_fired when that is what happened). When it fires,
 * hv_exc_fiq() takes it as a tick (on a secondary: re-arm only; on the boot CPU: an early hv_tick).
 */
static void hv_wfi_op_pre_idle(void *cx_)
{
    struct hv_wfi_cx *cx = cx_;
    struct wfi_vctx *v = &cx->saved;

    hv_vctx_read(cx, v);

    u64 dl, best = 0;
    bool have = false;
    if (wfi_tmr_deadline(v->cntv_ctl, hv_tmr_guest_imask(HV_TMR_V, v->cntv_ctl), v->cntv_cval,
                         v->cntvoff, &dl)) {
        best = dl;
        have = true;
    }
    if (wfi_tmr_deadline(v->cntp_ctl, hv_tmr_guest_imask(HV_TMR_P, v->cntp_ctl), v->cntp_cval, 0,
                         &dl) &&
        (!have || dl < best)) {
        best = dl;
        have = true;
    }

    /* smp2g (W1): an enabled, unmasked, not-firing timer without a usable deadline is parked */
    if (!have && (((v->cntv_ctl & (CNTx_CTL_ENABLE | CNTx_CTL_ISTATUS)) == CNTx_CTL_ENABLE &&
                   !hv_tmr_guest_imask(HV_TMR_V, v->cntv_ctl)) ||
                  ((v->cntp_ctl & (CNTx_CTL_ENABLE | CNTx_CTL_ISTATUS)) == CNTx_CTL_ENABLE &&
                   !hv_tmr_guest_imask(HV_TMR_P, v->cntp_ctl))))
        PERCPU(wfi_bg_parked)++;

    /* smp2g (W2): every 8th decision uses a 1 ms slack instead of 50 us (see wfi_wake_source) */
    u64 frq = mrs(CNTFRQ_EL0), now = mrs(CNTPCT_EL0), prog = 0;
    cx->probe = have && (PERCPU(wfi_bg_seq)++ & 7) == 7;
    cx->have_dl = have;
    cx->dl = best;
    cx->bg_armed = false;
    switch (wfi_bg_deadline(now, mrs(CNTP_CTL_EL0), mrs(SYS_CNTP_CVAL_EL0), have, best,
                            cx->probe ? frq / 1000 : frq / 20000, &prog)) {
        case WFI_BG_ARM:
            cx->bg_armed = true;
            break;
        case WFI_BG_PASSED:
            /* Not in the future although ISTATUS read clear. Arm only if the timer really fires
             * now (it fired between the two reads); a deadline that never fires is never armed. */
            PERCPU(wfi_bg_passed)++;
            if ((mrs(CNTV_CTL_EL02) | mrs(CNTP_CTL_EL02)) & CNTx_CTL_ISTATUS) {
                prog = now;
                cx->bg_armed = true;
            } else {
                cx->have_dl = false;
            }
            break;
        default:
            if (have && best - now > WFI_BG_HORIZON) {
                cx->have_dl = false;
                PERCPU(wfi_bg_parked)++;
            }
            break;
    }
    if (cx->bg_armed) {
        msr(SYS_CNTP_CVAL_EL0, prog);
        msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
        sysop("isb");
        PERCPU(wfi_bg_armed)++;
    }
    PERCPU(vctx_idles)++;
    cx->t_idle = mrs(CNTPCT_EL0);
}

/* smp2f post_idle: compare with what pre_idle saved, write back what changed (KVM: vcpu_load), count */
static void hv_wfi_op_post_idle(void *cx_)
{
    struct hv_wfi_cx *cx = cx_;
    u64 dur = mrs(CNTPCT_EL0) - cx->t_idle;
    struct wfi_vctx now;
    const struct wfi_vctx_ops vo = {.cx = cx, .read = hv_vctx_read, .write = hv_vctx_write};
    u32 d = wfi_vctx_check_restore(&vo, &cx->saved, &now);

    if (d) {
        if (!PERCPU(vctx_lost)) {
            PERCPU(vctx_first_t) = cx->t_idle;
            PERCPU(vctx_first_dur) = dur;
            PERCPU(vctx_first_mask) = d;
            PERCPU(vctx_first_before) = cx->saved;
            PERCPU(vctx_first_after) = now;
        }
        PERCPU(vctx_lost)++;
        for (int c = 0; c < WFI_VCTX_CLASSES; c++)
            if (d & BIT(c))
                PERCPU(vctx_lost_class[c])++;
        cx->lost |= d;
    }
    bool el2_fired = mrs(CNTP_CTL_EL0) & CNTx_CTL_ISTATUS;
    if (cx->bg_armed && el2_fired)
        PERCPU(wfi_bg_fired)++;
    /* smp2g (W2): what ended this WFI relative to the guest deadline, and how late */
    if (cx->have_dl) {
        u64 t = cx->t_idle + dur;
        bool gt = ((now.cntv_ctl & (CNTx_CTL_ISTATUS | CNTx_CTL_IMASK)) == CNTx_CTL_ISTATUS) ||
                  ((now.cntp_ctl & (CNTx_CTL_ISTATUS | CNTx_CTL_IMASK)) == CNTx_CTL_ISTATUS);
        enum wfi_wsrc src =
            wfi_wake_source(el2_fired, mrs(SYS_IMP_APL_IPI_SR_EL1) & IPI_SR_PENDING, gt);
        if (t >= cx->dl)
            wfi_lat_note(&PERCPU(wfi_lat[cx->probe][src]), t - cx->dl);
        else if (PERCPU(wfi_early[src]) != 0xffffffffU)
            PERCPU(wfi_early[src])++;
    }
    if (dur > PERCPU(wfi_idle_longest))
        PERCPU(wfi_idle_longest) = dur;
    if (dur > mrs(CNTFRQ_EL0) / 100)
        PERCPU(wfi_idle_long)++;
}

/*
 * smp2f: publish this CPU's state for hv_win_smp_dump(). Only the owning CPU can read its ICH_* and
 * EL02 timer registers; the boot CPU prints. Cheap (about 40 register reads), no locks, no I/O.
 */
void hv_win_wfi_snap(int where, const struct exc_info *ctx, int wake)
{
    int cpu = smp_id();
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    struct hv_smp_snap *s = &hv_snap[cpu][where == HV_SNAP_FIQ ? 1 : 0];

    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELAXED);
    sysop("dmb ish");
    s->t = mrs(CNTPCT_EL0);
    s->where = where;
    s->elr = hv_get_elr();
    s->spsr = hv_get_spsr();
    s->hcr = mrs(HCR_EL2);
    s->isr = mrs(ISR_EL1);
    s->cnthp_ctl = mrs(CNTP_CTL_EL0);
    s->cnthp_cval = mrs(SYS_CNTP_CVAL_EL0);
    s->tmr_hv_masked = PERCPU(tmr_hv_masked);
    s->tmr_guest_imask = PERCPU(tmr_guest_imask);
    s->x30 = ctx ? ctx->regs[30] : 0; /* smp2g */
    s->sp_el1 = mrs(SP_EL1);
    s->sp_el0 = mrs(SP_EL0);
    s->wake = wake;
    hv_vctx_read(NULL, &s->v);
    s->misr = s->v.nr_lr ? mrs(SYS_ICH_MISR_EL2) : 0;
    s->elrsr = s->v.nr_lr ? mrs(SYS_ICH_ELRSR_EL2) : 0;
    sysop("dmb ish");
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
}

/*
 * cpuperf-G: accumulated trapped-WFI time per m1n1 CPU (CNTPCT ticks) and the start of a WFI in
 * progress (0 = none). Single writer (the CPU itself), read by the boot CPU's governor tick.
 */
u64 hv_win_wfi_idle_tk[MAX_CPUS];
u64 hv_win_wfi_since_tk[MAX_CPUS];

bool hv_win_wfi(struct exc_info *ctx)
{
    int cpu = smp_id();

    /* F2: WFIT (TI=2), and WFE/WFET if TWE were ever set: complete at once (allowed early completion) */
    if (FIELD_GET(ESR_ISS, ctx->esr) & 3) {
        PERCPU(wfx_other)++;
        return true;
    }

    PERCPU(wfi_traps)++;
    hv_win_diag_note(HV_WIN_DIAG_WFI, ctx, PERCPU(wfi_traps));
    hv_win_wfi_snap(HV_SNAP_WFI_ENTRY, ctx, 0);

    /* F3: what must survive the wait unchanged */
    u64 gpr[10];
    for (int i = 0; i < 10; i++)
        gpr[i] = ctx->regs[19 + i];
    u64 sp0 = ctx->sp[0], sp1 = ctx->sp[1], tpidr = mrs(TPIDR_EL1);

    struct hv_wfi_cx cx = {.cpu = cpu};
    const struct wfi_ops ops = {
        .cx = &cx,
        .update = hv_wfi_op_update,
        .exit_req = hv_wfi_op_exit,
        .virt_pending = hv_wfi_op_virt,
        .isr = hv_wfi_op_isr,
        .idle = hv_wfi_op_idle,
        .pre_idle = hv_wfi_op_pre_idle,   /* smp2f */
        .post_idle = hv_wfi_op_post_idle, /* smp2f */
    };

    /* sched1: only kicks sent from here on count as waking this wait */
    __atomic_store_n(&hv_kick_t[cpu], 0, __ATOMIC_RELAXED);

    u64 start = mrs(CNTPCT_EL0);
    int idles = 0;
    __atomic_store_n(&hv_win_wfi_since_tk[cpu], start, __ATOMIC_RELAXED);
    enum wfi_wake wake = wfi_wait(&ops, HV_WFI_MAX_SPINS, &idles);
    {
        u64 end = mrs(CNTPCT_EL0);
        __atomic_store_n(&hv_win_wfi_idle_tk[cpu], hv_win_wfi_idle_tk[cpu] + (end - start), __ATOMIC_RELAXED);
        __atomic_store_n(&hv_win_wfi_since_tk[cpu], 0, __ATOMIC_RELEASE);
    }

    u64 kick_t = __atomic_exchange_n(&hv_kick_t[cpu], 0, __ATOMIC_RELAXED);
    if (kick_t && wake == WFI_WAKE_VIRQ) {
        u64 t_end = mrs(CNTPCT_EL0);
        if (idles)
            wfi_lat_note(&PERCPU(kick_lat), t_end > kick_t ? t_end - kick_t : 0);
        else
            PERCPU(kick_immediate)++;
    }

    switch (wake) {
        case WFI_WAKE_EXIT:
            PERCPU(wfi_wake_exit)++;
            break;
        case WFI_WAKE_VIRQ:
            PERCPU(wfi_wake_virq)++;
            if (cx.kvm_only)
                PERCPU(wfi_wake_kvm_only)++;
            break;
        case WFI_WAKE_FIQ:
            PERCPU(wfi_wake_fiq)++;
            break;
        case WFI_WAKE_IRQ:
            PERCPU(wfi_wake_irq)++;
            break;
        default:
            PERCPU(wfi_wake_cap)++;
            break;
    }
    PERCPU(wfi_spins) += idles;
    if (!idles)
        PERCPU(wfi_immediate)++;

    u64 waited = mrs(CNTPCT_EL0) - start;
    if (waited > PERCPU(wfi_longest))
        PERCPU(wfi_longest) = waited;
    hv_win_diag_note(HV_WIN_DIAG_WFI_DONE, NULL,
                     (u64)wake | ((u64)(idles & 0xff) << 8) | ((u64)cx.lost << 16));
    hv_win_wfi_snap(HV_SNAP_WFI_EXIT, ctx, wake);

    if (idles) {
        for (int i = 0; i < 10; i++)
            if (ctx->regs[19 + i] != gpr[i]) {
                hv_integ_report(ctx, "WFI wait", 19 + i, gpr[i], ctx->regs[19 + i], 4);
                return true;
            }
        if (ctx->sp[0] != sp0)
            hv_integ_report(ctx, "WFI wait SP_EL0", -1, sp0, ctx->sp[0], 4);
        else if (ctx->sp[1] != sp1)
            hv_integ_report(ctx, "WFI wait SP_EL1", -1, sp1, ctx->sp[1], 4);
        else if (mrs(TPIDR_EL1) != tpidr)
            hv_integ_report(ctx, "WFI wait TPIDR_EL1", -1, tpidr, mrs(TPIDR_EL1), 4);
    }
    return true;
}

/*
 * smp2d/smp2e integrity check (bugcheck-0A diagnostic a). On an asynchronous exit (IRQ/FIQ/SError,
 * reasons 1-3) the guest executed no instruction, so its callee-saved registers, SPs and TPIDR_EL1
 * must come back unchanged; the WFI wait (reason 4) is checked the same way in hv_win_wfi(). Every
 * mismatch is counted; only the first per CPU is printed (review F4), with the exit reason, ELR, ESR.
 * A hit proves a vCPU's saved register was corrupted across an EL2 exit (suspect A).
 */
static void hv_integ_report(struct exc_info *ctx, const char *what, int reg, u64 was, u64 now,
                            unsigned int reason)
{
    if (++PERCPU(integ_mismatch) != 1)
        return;
    if (reg >= 0)
        printf("HV/win INTEGRITY: CPU %d x%d changed across %s (reason %u): 0x%lx -> 0x%lx "
               "(ELR 0x%lx ESR 0x%lx); further mismatches on this CPU are only counted\n",
               smp_id(), reg, what, reason, was, now, ctx->elr, ctx->esr);
    else
        printf("HV/win INTEGRITY: CPU %d %s changed (reason %u): 0x%lx -> 0x%lx (ELR 0x%lx ESR "
               "0x%lx); further mismatches on this CPU are only counted\n",
               smp_id(), what, reason, was, now, ctx->elr, ctx->esr);
}

static void hv_integ_snapshot(struct exc_info *ctx, u8 reason)
{
    for (int i = 0; i < 10; i++)
        PERCPU(integ_gpr[i]) = ctx->regs[19 + i];
    PERCPU(integ_sp0) = ctx->sp[0];
    PERCPU(integ_sp1) = ctx->sp[1];
    PERCPU(integ_tpidr) = mrs(TPIDR_EL1);
    PERCPU(integ_reason) = reason;
    PERCPU(integ_async) = true;
}

static void hv_integ_check(struct exc_info *ctx)
{
    if (!PERCPU(integ_async))
        return;
    PERCPU(integ_async) = false;

    unsigned int reason = PERCPU(integ_reason);
    for (int i = 0; i < 10; i++)
        if (ctx->regs[19 + i] != PERCPU(integ_gpr[i])) {
            hv_integ_report(ctx, "async exit", 19 + i, PERCPU(integ_gpr[i]), ctx->regs[19 + i],
                            reason);
            return;
        }
    if (ctx->sp[0] != PERCPU(integ_sp0))
        hv_integ_report(ctx, "async exit SP_EL0", -1, PERCPU(integ_sp0), ctx->sp[0], reason);
    else if (ctx->sp[1] != PERCPU(integ_sp1))
        hv_integ_report(ctx, "async exit SP_EL1", -1, PERCPU(integ_sp1), ctx->sp[1], reason);
    else if (mrs(TPIDR_EL1) != PERCPU(integ_tpidr))
        hv_integ_report(ctx, "async exit TPIDR_EL1", -1, PERCPU(integ_tpidr), mrs(TPIDR_EL1),
                        reason);
}

/* smp2d/smp2e/smp2f: per-CPU WFI/debug/integrity counters, printed on guest reset (hv_win_psci.c)
 * and in every hv_win_smp_dump(). */
void hv_win_wfi_report(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (!pcpu[cpu].wfi_traps && !pcpu[cpu].dbg_traps && !pcpu[cpu].integ_mismatch &&
            !pcpu[cpu].wfx_other)
            continue;
        printf("HV/win smp2g: CPU %d WFI traps %lu (wake virq %lu [kvm-rule only %lu] fiq %lu irq %lu "
               "exit %lu cap %lu; immediate %lu, physical WFIs %lu, longest %lu tk); WFIT/WFE completed "
               "%lu; debug-reg traps %lu; integrity mismatches %lu\n",
               cpu, pcpu[cpu].wfi_traps, pcpu[cpu].wfi_wake_virq, pcpu[cpu].wfi_wake_kvm_only,
               pcpu[cpu].wfi_wake_fiq, pcpu[cpu].wfi_wake_irq, pcpu[cpu].wfi_wake_exit,
               pcpu[cpu].wfi_wake_cap, pcpu[cpu].wfi_immediate, pcpu[cpu].wfi_spins,
               pcpu[cpu].wfi_longest, pcpu[cpu].wfx_other, pcpu[cpu].dbg_traps,
               pcpu[cpu].integ_mismatch);
        const u64 *c = pcpu[cpu].vctx_lost_class;
        printf("HV/win smp2g: CPU %d physical WFIs saved %lu, state changed across %lu (HCR %lu VMCR %lu "
               "APR %lu LR %lu TCTL %lu TCVAL %lu CNTVOFF %lu EL1 %lu); bg timer armed %lu fired %lu; "
               "idle >10ms %lu, longest idle %lu tk\n",
               cpu, pcpu[cpu].vctx_idles, pcpu[cpu].vctx_lost, c[0], c[1], c[2], c[3], c[4], c[5],
               c[6], c[7], pcpu[cpu].wfi_bg_armed, pcpu[cpu].wfi_bg_fired, pcpu[cpu].wfi_idle_long,
               pcpu[cpu].wfi_idle_longest);
        /* smp2g (W1/W2): what ended the physical WFIs that had a guest-timer deadline */
        static const char *const src[WFI_WSRC_N] = {"guest-timer", "el2-timer", "ipi", "other"};
        const struct hv_pcpu_data *p = &pcpu[cpu];
        printf("HV/win smp2g: CPU %d EL2CFG changed %lu; bg parked %lu passed %lu; woke before the "
               "deadline: guest-timer %u el2-timer %u ipi %u other %u\n",
               cpu, c[8], p->wfi_bg_parked, p->wfi_bg_passed, p->wfi_early[0], p->wfi_early[1],
               p->wfi_early[2], p->wfi_early[3]);
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < WFI_WSRC_N; i++)
                if (p->wfi_lat[k][i].n)
                    printf("HV/win smp2g: CPU %d woke after the deadline (%s slack) by %s: n %u, "
                           "late min %u max %u last %u tk\n",
                           cpu, k ? "1 ms probe" : "50 us", src[i], p->wfi_lat[k][i].n,
                           p->wfi_lat[k][i].min, p->wfi_lat[k][i].max, p->wfi_lat[k][i].last);
        /* sched1: vGIC kick (IPI sent by another CPU for this vCPU) to the end of the emulated WFI */
        if (p->kick_lat.n || p->kick_immediate)
            printf("HV/win sched1: CPU %d kick-to-wake: during a physical WFI n %u, min %u max %u last %u "
                   "tk; before it (no sleep) %lu\n",
                   cpu, p->kick_lat.n, p->kick_lat.min, p->kick_lat.max, p->kick_lat.last,
                   p->kick_immediate);
    }
}

static void hv_print_vctx(int cpu, const char *tag, const struct wfi_vctx *v, u64 now, u64 tk_ms)
{
    u64 vnow = now - v->cntvoff;
    printf("HV/win smp2g: CPU %d %s: CNTV ctl 0x%lx cval 0x%lx (%ld ms) CNTP ctl 0x%lx cval 0x%lx "
           "(%ld ms) CNTVOFF 0x%lx; ICH_HCR 0x%lx VMCR 0x%lx (VPMR 0x%lx VENG1 %ld) AP0R0 0x%x AP1R0 "
           "0x%x; EL1 SCTLR 0x%lx VBAR 0x%lx TTBR1 0x%lx TPIDR 0x%lx\n",
           cpu, tag, v->cntv_ctl, v->cntv_cval, (s64)(v->cntv_cval - vnow) / (s64)tk_ms, v->cntp_ctl,
           v->cntp_cval, (s64)(v->cntp_cval - now) / (s64)tk_ms, v->cntvoff, v->ich_hcr, v->ich_vmcr,
           (v->ich_vmcr >> 24) & 0xff, (long)((v->ich_vmcr >> 1) & 1), v->apr0[0], v->apr1[0],
           v->el1[0], v->el1[1], v->el1[2], v->el1[3]);
    printf("HV/win smp2g: CPU %d %s: CNTHCTL 0x%lx HCR 0x%lx\n", cpu, tag, v->el2[0], v->el2[1]);
    for (u32 i = 0; i < v->nr_lr; i++)
        if (v->lr[i])
            printf("HV/win smp2g: CPU %d %s: LR%u 0x%016lx (state %lu intid %lu prio 0x%lx)\n", cpu,
                   tag, i, v->lr[i], v->lr[i] >> 62, v->lr[i] & 0xffffffff, (v->lr[i] >> 48) & 0xff);
}

/*
 * smp2f: the SMP diagnostic dump, without a guest reset. Boot CPU only: every other CPU publishes its
 * own state (hv_win_wfi_snap) and never prints, so CPU 1 never reaches the USB console path.
 * Callers: the P_HV_WIN_SMP_DUMP proxy command (host, from the hypervisor shell, e.g. while the guest
 * is hung), the automatic stall watch in hv_win_tick(), and a guest SYSTEM_RESET/OFF.
 */
int hv_win_smp_dump(const char *why)
{
    if (smp_id() != boot_cpu_idx)
        return -1;

    u64 now = mrs(CNTPCT_EL0), frq = mrs(CNTFRQ_EL0);
    u64 tk_ms = frq / 1000 ? frq / 1000 : 1;
    printf("HV/win smp2g: ==== SMP dump (%s) at CNTPCT 0x%lx, CNTFRQ %lu ====\n", why, now, frq);
    hv_win_wfi_report();

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct hv_pcpu_data *p = &pcpu[cpu];
        if (p->vctx_lost) {
            printf("HV/win smp2g: CPU %d FIRST state change across a physical WFI: classes 0x%x, WFI "
                   "of %lu tk started %ld ms ago\n",
                   cpu, p->vctx_first_mask, p->vctx_first_dur,
                   (s64)(now - p->vctx_first_t) / (s64)tk_ms);
            hv_print_vctx(cpu, "before", &p->vctx_first_before, p->vctx_first_t, tk_ms);
            hv_print_vctx(cpu, "after", &p->vctx_first_after, p->vctx_first_t, tk_ms);
        }
        for (int slot = 0; slot < 2; slot++) {
            struct hv_smp_snap s;
            u64 seq0 = __atomic_load_n(&hv_snap[cpu][slot].seq, __ATOMIC_ACQUIRE);
            if (!seq0)
                continue;
            memcpy(&s, &hv_snap[cpu][slot], sizeof(s));
            sysop("dmb ish");
            u64 seq1 = __atomic_load_n(&hv_snap[cpu][slot].seq, __ATOMIC_RELAXED);
            const char *where = s.where == HV_SNAP_WFI_ENTRY ? "wfi-entry"
                                : s.where == HV_SNAP_WFI_EXIT ? "wfi-exit"
                                : s.where == HV_SNAP_FIQ      ? "fiq-entry"
                                                              : "?";
            printf("HV/win smp2g: CPU %d snapshot %s%s, %ld ms ago: ELR 0x%lx SPSR 0x%lx (DAIF %lx) "
                   "HCR VF %d VI %d ISR 0x%lx; EL2 timer ctl 0x%lx (%ld ms); timer gate held 0x%x "
                   "guest-imask 0x%x; ICH_MISR 0x%lx ELRSR 0x%lx\n",
                   cpu, where, (seq0 & 1) || seq0 != seq1 ? " (torn)" : "",
                   (s64)(now - s.t) / (s64)tk_ms, s.elr, s.spsr, (s.spsr >> 6) & 0xf,
                   !!(s.hcr & HCR_VF), !!(s.hcr & HCR_VI), s.isr, s.cnthp_ctl,
                   (s64)(s.cnthp_cval - now) / (s64)tk_ms, s.tmr_hv_masked, s.tmr_guest_imask, s.misr,
                   s.elrsr);
            /* smp2g: one line that answers "where is this vCPU": state, PC, LR, SP, DAIF, VPMR */
            static const char *const wk[] = {"wait", "exit", "virq", "fiq", "irq"};
            printf("HV/win smp2g: CPU %d %s: %s; guest PC 0x%lx x30 0x%lx SP_EL1 0x%lx SP_EL0 0x%lx "
                   "DAIF %lx VPMR 0x%lx\n",
                   cpu, where,
                   s.where == HV_SNAP_WFI_ENTRY  ? "IN EL2 WAIT since this snapshot (unless a newer "
                                                   "fiq-entry follows)"
                   : s.where == HV_SNAP_WFI_EXIT ? "left the EL2 wait, back in the guest"
                                                 : "interrupted while running guest code",
                   s.elr, s.x30, s.sp_el1, s.sp_el0, (s.spsr >> 6) & 0xf,
                   (s.v.ich_vmcr >> 24) & 0xff);
            if (s.where == HV_SNAP_WFI_EXIT)
                printf("HV/win smp2g: CPU %d wfi-exit: wake reason %s\n", cpu,
                       s.wake < 5 ? wk[s.wake] : "?");
            hv_print_vctx(cpu, slot ? "fiq-entry regs" : "wfi regs", &s.v, s.t, tk_ms);
        }
    }

    hv_vgic3_smp_dump();
    hv_win_pl011_kd_dump(); /* smp2g */
    hv_win_diag_dump_all();
    hv_win_cppc_dump(); /* cpuperf-A: -1 (silent) when no CPPC page */
    printf("HV/win smp2g: ==== end of SMP dump ====\n");
    return 0;
}

/* r4.9: read passed through at EL2, write ignored (read-only registers; the proxy's "ro" set) */
#define SYSREG_PASS_RO(sr)                                                                         \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr));                                                           \
        return true;

#define SYSREG_MAP(sr, to)                                                                         \
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
    hv_integ_check(ctx); /* smp2d: async-exit frame must be unchanged (bugcheck-0A suspect A) */
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

static bool hv_exc_sync_body(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('S');
    hv_get_context(ctx);
    hv_win_diag_note(HV_WIN_DIAG_TRAP, ctx, 0);
    bool handled = false;
    u32 ec = FIELD_GET(ESR_EC, ctx->esr);
    /* r4.11 diagnostics (lock-free counters). u1: atomic, so concurrent vCPUs do not lose counts */
    __atomic_fetch_add(&hv_exits_sync, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&hv_trap_ec[ec], 1, __ATOMIC_RELAXED);
    hv_trap_ec_pc[ec] = ctx->elr;
    hv_trap_ec_far[ec] = ctx->far;

    bool do_gexit = false;

    switch (ec) {
        case ESR_EC_ERET:
            handled = hv_sprr_handle_eret(ctx, false);
            break;
        case ESR_EC_WFI:
            /* smp2d: emulate the guest's trapped WFI (HCR_EL2.TWI, Windows >1 vCPU). Lock-free: the
             * wait must not hold bhl. The guest frame is already safe on the EL2 stack. */
            hv_wdt_breadcrumb('w');
            if (hv_win_wfi_trapping())
                handled = hv_win_wfi(ctx);
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

static void hv_exc_irq_body(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('I');
    __atomic_fetch_add(&hv_exits_irq, 1, __ATOMIC_RELAXED); /* u1: before bhl; atomic for > 1 vCPU */
    hv_get_context(ctx);
    hv_integ_snapshot(ctx, 1); /* smp2d: IRQ */
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

static void hv_exc_fiq_body(struct exc_info *ctx)
{
    bool tick = false;

    __atomic_fetch_add(&hv_exits_fiq, 1, __ATOMIC_RELAXED); /* u1: lock-free path; atomic for > 1 vCPU */
    /* smp2f: publish this CPU's interface/timer state as the FIQ found it (before maintenance) */
    if (hv_win_wfi_trapping())
        hv_win_wfi_snap(HV_SNAP_FIQ, ctx, 0);
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
        hv_win_diag_note(HV_WIN_DIAG_FIQ_FAST, NULL, tick); /* smp2d: fast-path exit log */
        hv_update_fiq();
        hv_arm_tick(true);
        return;
    }

    // Slow (single threaded) path
    hv_wdt_breadcrumb('F');
    hv_get_context(ctx);
    if (smp_id() != boot_cpu_idx) /* smp2f: secondaries only (the boot CPU's 1 kHz tick would flood it) */
        hv_win_diag_note(HV_WIN_DIAG_FIQ_SLOW, ctx, mrs(ISR_EL1));
    hv_integ_snapshot(ctx, 2); /* smp2d: FIQ */
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

/*
 * perf-exits entry points. With no option set these only call the bodies above (the old paths). With
 * HV_PERF_STATS they add the C time of each exit kind and note which system register a sync trap was
 * for (an emulated guest WFI wait is idle time and is not counted as EL2 work).
 */
bool hv_exc_sync(struct exc_info *ctx)
{
    u64 t0 = hv_perf_t0();
    u64 esr = t0 ? hv_get_esr() : 0;
    u32 ec = FIELD_GET(ESR_EC, esr);
    if (t0 && ec == ESR_EC_MSR)
        hv_perf_sr_note(hv_perf_sr_tab, &hv_perf_sr_other, esr);
    bool r = hv_exc_sync_body(ctx);
    if (ec != ESR_EC_WFI)
        hv_perf_t1(&hv_perf_el2_sync, t0);
    return r;
}

void hv_exc_irq(struct exc_info *ctx)
{
    u64 t0 = hv_perf_t0();
    hv_exc_irq_body(ctx);
    hv_perf_t1(&hv_perf_el2_irq, t0);
}

void hv_exc_fiq(struct exc_info *ctx)
{
    u64 t0 = hv_perf_t0();
    hv_exc_fiq_body(ctx);
    hv_perf_t1(&hv_perf_el2_fiq, t0);
}

/*
 * perf-exits HV_PERF_FAST_MDSCR: called from the EL2 sync vector (hv_asm.S _v_hv_sync_fast) with the
 * guest's GPRs saved in ctx->regs and nothing else read yet. Handles only a trapped MRS/MSR MDSCR_EL1,
 * with exactly the slow path's semantics (hv_handle_msr_unlocked SYS_MDSCR_EL1 case: the per-CPU
 * shadow), and returns 1: ELR advanced, the vector restores the frame and ERETs. Anything else returns
 * 0 and the vector runs the unchanged slow path (barriers, hv_exc_sync) on the same frame.
 * Skipped on purpose for these exits: hv_get_context, the diag ring and breadcrumbs, hv_update_fiq.
 * The access changes no timer, vGIC or IPI state, so the resync the slow path does after every handled
 * trap has nothing to fold; pending virtual interrupts stay in the list registers as they were, and any
 * physical FIQ/IRQ that arrived meanwhile is taken right after the ERET as before.
 * hv_win_perf_configure only sets the option with SPTM emulation off and GXF disabled, so the shadow is
 * PERCPU(mdscr) and ESR/ELR are the EL2 registers.
 */
u64 hv_exc_sync_fast(struct exc_info *ctx)
{
    u64 esr = mrs(ESR_EL2);
    if (!hv_perf_mdscr_emulate(esr, ctx->regs, &PERCPU(mdscr)))
        return 0;
    msr(ELR_EL2, mrs(ELR_EL2) + 4);
    hv_perf_fast_mdscr[mrs(TPIDR_EL2)]++;
    /* keep the ST line's exit counts comparable with the slow path */
    __atomic_fetch_add(&hv_exits_sync, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&hv_trap_ec[ESR_EC_MSR], 1, __ATOMIC_RELAXED);
    hv_trap_ec_pc[ESR_EC_MSR] = mrs(ELR_EL2) - 4;
    if (hv_perf_flags & HV_PERF_STATS)
        hv_perf_sr_note(hv_perf_sr_tab, &hv_perf_sr_other, esr);
    return 1;
}

void hv_exc_serr(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('E');
    hv_get_context(ctx);
    hv_integ_snapshot(ctx, 3); /* smp2d: SError */
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

/*
 * perf-exits PF line (only with a P_HV_WIN_PERF option set), per second over the interval since the
 * previous PF line: exits by kind and by exception class, the C time EL2 spent in each exit kind and in
 * the display sweep (parts per thousand of wall time; exception entry/return itself is not included, so
 * these are lower bounds), fast-path hits, the busiest trapped system registers, and the trap config.
 *   PF dt <ms> | rate irq <n> fiq <n> sync <n> | ec <ec>:<n> ... | el2 permille sync <p> irq <p> fiq <p>
 *      fb <p> | fast mdscr <n> | sr <mrs|msr> s<op0>_<op1>_c<n>_c<m>_<op2> <n> ... other <n> | flags 0x<f>
 *      mdcr 0x<v> hdfgrtr 0x<v>
 */
#define HV_FB_ST_FAULTS 15 /* hv_win_fb.c stat indices (0..34 keep their meaning) */
#define HV_FB_ST_PAGES  17
#define HV_FB_ST_BUSY   30
static u64 pf_last_t, pf_prev_ec[64], pf_prev_irq, pf_prev_fiq, pf_prev_sync, pf_prev_fast;
static u64 pf_prev_el2[3], pf_prev_fb_busy, pf_prev_fb_faults, pf_prev_fb_pages;

static void hv_perf_report(const char *P, u64 now, u64 freq)
{
    u64 dt = pf_last_t ? now - pf_last_t : 0;
    pf_last_t = now;
    u64 fast = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        fast += hv_perf_fast_mdscr[i];
    u64 el2[3] = {hv_perf_el2_sync, hv_perf_el2_irq, hv_perf_el2_fiq};
    u64 fbb = hv_win_fb_stat(HV_FB_ST_BUSY), fbf = hv_win_fb_stat(HV_FB_ST_FAULTS),
        fbp = hv_win_fb_stat(HV_FB_ST_PAGES);
    if (fbb == UINT64_MAX)
        fbb = fbf = fbp = 0;
    printf("%s PF dt %lu | rate irq %lu fiq %lu sync %lu | ec", P, (unsigned long)(dt * 1000 / (freq ? freq : 1)),
           (unsigned long)hv_perf_rate(hv_exits_irq - pf_prev_irq, dt, freq),
           (unsigned long)hv_perf_rate(hv_exits_fiq - pf_prev_fiq, dt, freq),
           (unsigned long)hv_perf_rate(hv_exits_sync - pf_prev_sync, dt, freq));
    for (int ec = 0; ec < 64; ec++) {
        u64 d = hv_trap_ec[ec] - pf_prev_ec[ec];
        if (d)
            printf(" %02x:%lu", ec, (unsigned long)hv_perf_rate(d, dt, freq));
        pf_prev_ec[ec] = hv_trap_ec[ec];
    }
    printf(" | el2 permille sync %lu irq %lu fiq %lu fb %lu | fb pages %lu faults %lu | fast mdscr %lu | sr",
           (unsigned long)hv_perf_permille(el2[0] - pf_prev_el2[0], dt),
           (unsigned long)hv_perf_permille(el2[1] - pf_prev_el2[1], dt),
           (unsigned long)hv_perf_permille(el2[2] - pf_prev_el2[2], dt),
           (unsigned long)hv_perf_permille(fbb - pf_prev_fb_busy, dt),
           (unsigned long)hv_perf_rate(fbp - pf_prev_fb_pages, dt, freq),
           (unsigned long)hv_perf_rate(fbf - pf_prev_fb_faults, dt, freq),
           (unsigned long)hv_perf_rate(fast - pf_prev_fast, dt, freq));
    bool taken[HV_PERF_SR_N] = {false};
    for (int k = 0; k < 6; k++) {
        int i = hv_perf_sr_top(hv_perf_sr_tab, taken);
        if (i < 0)
            break;
        taken[i] = true;
        u32 op0, op1, crn, crm, op2;
        bool rd;
        hv_perf_sr_decode(hv_perf_sr_tab[i].key, &op0, &op1, &crn, &crm, &op2, &rd);
        printf(" %s s%u_%u_c%u_c%u_%u %lu", rd ? "mrs" : "msr", op0, op1, crn, crm, op2,
               (unsigned long)hv_perf_rate(hv_perf_sr_tab[i].count - hv_perf_sr_tab[i].reported, dt, freq));
    }
    for (int i = 0; i < HV_PERF_SR_N; i++)
        hv_perf_sr_tab[i].reported = hv_perf_sr_tab[i].count;
    printf(" other %lu | flags 0x%x mdcr 0x%lx\n", (unsigned long)hv_perf_sr_other, hv_perf_flags,
           mrs(MDCR_EL2));
    pf_prev_irq = hv_exits_irq; pf_prev_fiq = hv_exits_fiq; pf_prev_sync = hv_exits_sync; pf_prev_fast = fast;
    for (int i = 0; i < 3; i++)
        pf_prev_el2[i] = el2[i];
    pf_prev_fb_busy = fbb; pf_prev_fb_faults = fbf; pf_prev_fb_pages = fbp;
}

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
        {HV_WIN_PMU_STUCK, "stuck"}, {HV_WIN_PMU_DMA_RESETS, "dma"},
        {HV_WIN_PMU_DATA_SKIPS, "dskip"}, {HV_WIN_PMU_FRAMES_SCANNED, "fscan"},
        {HV_WIN_PMU_FRAMES_MODIFIED, "fmod"}, {HV_WIN_PMU_WORDS_MODIFIED, "wmod"},
        {HV_WIN_PMU_WORDS_SKIP_EXEC, "skexec"}, {HV_WIN_PMU_WORDS_SKIP_BOUNDARY, "skbnd"},
        {HV_WIN_PMU_WORDS_SKIP_OTHER, "skother"}, {HV_WIN_PMU_MODIFIED_NEVER_EXECUTED, "MNE"},
        {HV_WIN_PMU_EXEC_FAULTS, "xflt"}, {HV_WIN_PMU_EXEC_DENIED, "xunproven"},
        {HV_WIN_PMU_REPROTECTS, "reprot"}};
    for (unsigned i = 0; i < sizeof(pk) / sizeof(pk[0]); i++) {
        u64 v = hv_win_pmu_stat(pk[i].idx);
        printf(" %s %lu(+%lu)", pk[i].name, (unsigned long)v, (unsigned long)(v - st_prev_pmu[pk[i].idx]));
        st_prev_pmu[pk[i].idx] = v;
    }
    printf(" gran %s\n", (hv_win_pmu_stat(HV_WIN_PMU_ID) & HV_WIN_PMU_ID_4K) ? "4k" : "16k");
    if (hv_perf_flags)
        hv_perf_report(P, now, freq);
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
