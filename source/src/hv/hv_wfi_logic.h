/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * smp2d/smp2e: pure logic for guest-WFI emulation and the guest debug-register shadow, shared by
 * hv_exc.c / hv_vgic3.c and the host unit test (tests/win-wfi), so the test drives the exact loop
 * and decisions the firmware runs. No system-register access here: callers pass what they read, and
 * the wait loop reaches hardware only through the wfi_ops callbacks.
 */
#ifndef HV_WFI_LOGIC_H
#define HV_WFI_LOGIC_H

/* The host unit test defines its own fixed-width types and BIT(); firmware uses src/types.h. */
#ifndef HV_WFI_LOGIC_TYPES_PROVIDED
#include "types.h"
#endif

/* ISR_EL1 bits (pending physical interrupt, reported regardless of PSTATE.DAIF masking) */
#define WFI_ISR_F BIT(6) /* FIQ: guest/hv timer, fast IPI (SGI kick), PMC */
#define WFI_ISR_I BIT(7) /* IRQ: routed device SPI (USB) */

enum wfi_wake {
    WFI_WAIT = 0,  /* nothing pending for this vCPU: idle the core once more */
    WFI_WAKE_EXIT, /* guest/host exit or CPU-switch request */
    WFI_WAKE_VIRQ, /* smp2e: a virtual interrupt is pending for this vCPU (LR, vFIQ/vIRQ, dirty) */
    WFI_WAKE_FIQ,  /* a physical FIQ is pending */
    WFI_WAKE_IRQ,  /* a physical IRQ is pending */
};

/*
 * One decision of the WFI wait. An exit/switch request wins (prompt teardown); then virtual work the
 * vCPU can take (smp2e F1: a native WFI at EL1 completes at once on a pending virtual interrupt even
 * with PSTATE.I set, while a pending list register does NOT wake a WFI executed at EL2, so EL2 must
 * check it itself); then any pending physical interrupt. WFI_WAIT means idle the core once more.
 */
static inline enum wfi_wake wfi_decide(bool exit_req, bool virt_pending, u64 isr)
{
    if (exit_req)
        return WFI_WAKE_EXIT;
    if (virt_pending)
        return WFI_WAKE_VIRQ;
    if (isr & WFI_ISR_F)
        return WFI_WAKE_FIQ;
    if (isr & WFI_ISR_I)
        return WFI_WAKE_IRQ;
    return WFI_WAIT;
}

/*
 * GICv3 virtual CPU interface: does this list register signal a virtual IRQ to the PE, i.e. would a
 * native WFI at EL1 wake on it? A virtual interrupt is signalled when it is pending, its group is
 * enabled in ICH_VMCR_EL2 (VENG0/VENG1), and its priority is higher (numerically lower) than both the
 * virtual priority mask VPMR and the running priority (GICv3 architecture, virtual CPU interface).
 * PSTATE.I does not matter: WFI wakes on a signalled interrupt even when it is masked at the PE.
 * Pending+active entries are excluded (the active instance holds the running priority), as in KVM's
 * kvm_vgic_vcpu_pending_irq (arch/arm64/kvm/vgic/vgic.c:1264-1267: pending && !active && prio < pmr).
 *   lr:   ICH_LR<n>_EL2; State [63:62] (01 pending, 10 active, 11 both), Group [60], Priority [55:48]
 *   vmcr: ICH_VMCR_EL2; VPMR [31:24], VENG1 [1], VENG0 [0]
 *   rpr:  running priority (0x100 = nothing active), from the active-priority registers
 */
#define WFI_LR_STATE_SHIFT  62
#define WFI_LR_GROUP1       BIT(60)
#define WFI_LR_PRIO_SHIFT   48
#define WFI_VMCR_VPMR_SHIFT 24
#define WFI_VMCR_VENG1      BIT(1)
#define WFI_VMCR_VENG0      BIT(0)

static inline bool wfi_lr_signals(u64 lr, u64 vmcr, u32 rpr)
{
    if (((lr >> WFI_LR_STATE_SHIFT) & 3) != 1) /* only Pending (not active, not invalid) */
        return false;
    bool grp1 = lr & WFI_LR_GROUP1;
    if (!(vmcr & (grp1 ? WFI_VMCR_VENG1 : WFI_VMCR_VENG0)))
        return false;
    u32 prio = (lr >> WFI_LR_PRIO_SHIFT) & 0xff;
    u32 pmr = (vmcr >> WFI_VMCR_VPMR_SHIFT) & 0xff;
    return prio < pmr && prio < rpr;
}

/*
 * smp2f: the wake rule actually used for the emulated WFI, KVM's kvm_vgic_vcpu_pending_irq()
 * (arch/arm64/kvm/vgic/vgic.c; the owner's Linux runs KVM with kvm-arm.wfi_trap_policy=trap):
 * pending && !active && priority < VPMR. Unlike wfi_lr_signals() it ignores the running priority and
 * the group enables. A WFI may complete spuriously at any time (Arm ARM, WFI wake-up events), so
 * waking where the strict GICv3 rule would sleep is always correct; sleeping where KVM (and the
 * hypervisors Windows is validated on) would wake is what can stall a guest. smp2e used the strict
 * rule; it is kept only to count how often the two differ (wfi_wake_kvm_only).
 */
static inline bool wfi_lr_wakes(u64 lr, u64 vmcr)
{
    if (((lr >> WFI_LR_STATE_SHIFT) & 3) != 1)
        return false;
    return ((lr >> WFI_LR_PRIO_SHIFT) & 0xff) < ((vmcr >> WFI_VMCR_VPMR_SHIFT) & 0xff);
}

/*
 * smp2f: generic-timer helpers for KVM's "background timer" while a vCPU blocks in WFI
 * (kvm_timer_schedule()/kvm_timer_earliest_exp(), arch/arm64/kvm/arch_timer.c: when a vCPU blocks,
 * KVM does not rely on the guest timer's own interrupt to wake the host; it arms a host timer for the
 * earliest guest deadline). Here the host timer is the EL2 physical timer m1n1 already uses for its
 * tick (CNTHP, the timer Linux's own idle relies on to wake these cores).
 *   ctl:         the timer's CNTx_CTL as read from hardware (ISTATUS valid)
 *   guest_imask: the guest's own IMASK (EL2 may be holding the hardware IMASK)
 *   cval/offset: CNTx_CVAL and the offset that maps it to the physical counter (CNTVOFF_EL2 for the
 *                virtual timer, 0 for the physical timer: CNTHCTL_EL2.ECV is clear, no CNTPOFF)
 * Returns true with the physical-counter deadline if the timer will fire later and the guest wants it.
 */
#define WFI_CTL_ENABLE  BIT(0)
#define WFI_CTL_IMASK   BIT(1)
#define WFI_CTL_ISTATUS BIT(2)

static inline bool wfi_tmr_deadline(u64 ctl, bool guest_imask, u64 cval, u64 offset, u64 *deadline)
{
    if (!(ctl & WFI_CTL_ENABLE) || guest_imask || (ctl & WFI_CTL_ISTATUS))
        return false;
    /* smp2g (review W1): a "parked" timer (CVAL near 2^64) plus the offset would wrap to a small
     * value; such a deadline is never reached, so it is no deadline. */
    if (cval > ~(u64)0 - offset)
        return false;
    *deadline = cval + offset;
    return true;
}

/*
 * The EL2 timer deadline to program before the physical WFI: the guest deadline plus <slack> if that
 * is earlier than the pending EL2 tick (or the tick is not armed); otherwise leave the tick alone.
 * smp2g (review W1): all arithmetic is wrap-safe and relative to <now> (as KVM's is):
 *   WFI_BG_NONE    no guest deadline, a deadline further away than WFI_BG_HORIZON (a parked timer:
 *                  the tick wakes the core long before), a sum that would wrap, or the tick is
 *                  earlier: nothing to program;
 *   WFI_BG_PASSED  the deadline is not in the future although the caller saw ISTATUS clear: nothing
 *                  is programmed here (a deadline in the past would fire at once on every WFI); the
 *                  caller re-reads the timer and arms only if it really fires now;
 *   WFI_BG_ARM     program *program.
 */
#define WFI_BG_HORIZON BIT(40) /* counter ticks: 18 min at 1 GHz, 12 h at 24 MHz */
enum wfi_bg { WFI_BG_NONE, WFI_BG_ARM, WFI_BG_PASSED };

static inline enum wfi_bg wfi_bg_deadline(u64 now, u64 tick_ctl, u64 tick_cval, bool have_guest,
                                          u64 guest_dl, u64 slack, u64 *program)
{
    if (!have_guest)
        return WFI_BG_NONE;
    if (guest_dl <= now)
        return WFI_BG_PASSED;
    if (guest_dl - now > WFI_BG_HORIZON || guest_dl > ~(u64)0 - slack)
        return WFI_BG_NONE;
    bool tick_armed = (tick_ctl & WFI_CTL_ENABLE) && !(tick_ctl & WFI_CTL_IMASK);
    if (tick_armed && tick_cval <= guest_dl + slack)
        return WFI_BG_NONE;
    *program = guest_dl + slack;
    return WFI_BG_ARM;
}

/*
 * smp2g (review W2): what ended a physical WFI whose guest-timer deadline had passed when it returned,
 * and how late after the deadline. Classified from what is pending right after the WFI:
 *   BG     the EL2 timer has fired (the background deadline = guest deadline + slack, or the tick);
 *   IPI    an IPI is pending (another vCPU's kick; cannot be told apart from a simultaneous timer);
 *   GT     neither, and the guest timer is firing unmasked: its own FIQ ended the wait;
 *   OTHER  none of these.
 * With the 50 us slack a slow wake-up from retention looks like BG. So every 8th armed wait uses a
 * long slack (the "probe", 1 ms): there GT with a lateness under 1 ms shows the guest timer does end
 * the wait and how late; BG at about 1 ms shows it does not.
 */
enum wfi_wsrc { WFI_WSRC_GT, WFI_WSRC_BG, WFI_WSRC_IPI, WFI_WSRC_OTHER, WFI_WSRC_N };
struct wfi_lat {
    u32 n, min, max, last; /* counter ticks, saturating */
};

static inline enum wfi_wsrc wfi_wake_source(bool el2_fired, bool ipi, bool gt_firing)
{
    if (el2_fired)
        return WFI_WSRC_BG;
    if (ipi)
        return WFI_WSRC_IPI;
    return gt_firing ? WFI_WSRC_GT : WFI_WSRC_OTHER;
}

static inline void wfi_lat_note(struct wfi_lat *l, u64 late)
{
    u32 v = late > 0xffffffffUL ? 0xffffffffU : (u32)late;
    if (!l->n || v < l->min)
        l->min = v;
    if (v > l->max)
        l->max = v;
    l->last = v;
    if (l->n != 0xffffffffU)
        l->n++;
}

/*
 * smp2f: the vCPU's virtual-interface and guest-timer state held in hardware registers while EL2
 * idles the core. KVM never leaves it in hardware across a host idle: a trapped WFI blocks the vCPU,
 * and vcpu_put() (vgic_v3_put: VMCR/APRs; the LRs and ICH_HCR are saved on every guest exit;
 * kvm_timer_vcpu_put: CNTV/CNTP CTL and CVAL) runs before the host idles, vcpu_load() restores it.
 * smp2e kept all of it live in the registers across hv_wfi_idle(), whose retention WFI on M4+ is
 * known to lose GPRs and FP (m1n1 kboot.c: "M4 and later lose architectural state in WFI and WFIT");
 * whether these registers survive it was never established. smp2f saves them before the physical
 * WFI, compares after it, writes back only what changed and counts each change per class.
 * The EL1 words are compared only (a loss there is reported, not repaired).
 */
#define WFI_VCTX_LRS 16
struct wfi_vctx {
    u64 ich_hcr, ich_vmcr;
    u32 apr0[4], apr1[4];
    u32 nr_apr; /* AP0R/AP1R words in use (1, 2 or 4) */
    u32 nr_lr;  /* list registers in use */
    u64 lr[WFI_VCTX_LRS];
    u64 cntv_ctl, cntv_cval, cntp_ctl, cntp_cval, cntvoff;
    u64 el1[4]; /* compare-only: SCTLR_EL12, VBAR_EL12, TTBR1_EL12, TPIDR_EL1 */
    u64 el2[2]; /* smp2g, compare-only: CNTHCTL_EL2 (timer trap config), HCR_EL2 */
};

enum {
    WFI_VCTX_HCR = BIT(0),
    WFI_VCTX_VMCR = BIT(1),
    WFI_VCTX_APR = BIT(2),
    WFI_VCTX_LR = BIT(3),
    WFI_VCTX_TCTL = BIT(4), /* CNTV/CNTP CTL ENABLE or IMASK (ISTATUS is status, not state) */
    WFI_VCTX_TCVAL = BIT(5),
    WFI_VCTX_CNTVOFF = BIT(6),
    WFI_VCTX_EL1 = BIT(7),
    WFI_VCTX_EL2CFG = BIT(8), /* smp2g: CNTHCTL_EL2 or HCR_EL2 */
};
#define WFI_VCTX_CLASSES 9
#define WFI_VCTX_RESTORE                                                                           \
    (WFI_VCTX_HCR | WFI_VCTX_VMCR | WFI_VCTX_APR | WFI_VCTX_LR | WFI_VCTX_TCTL | WFI_VCTX_TCVAL |   \
     WFI_VCTX_CNTVOFF)

static inline u32 wfi_vctx_diff(const struct wfi_vctx *a, const struct wfi_vctx *b)
{
    const u64 tmask = WFI_CTL_ENABLE | WFI_CTL_IMASK;
    u32 d = 0;

    if (a->ich_hcr != b->ich_hcr)
        d |= WFI_VCTX_HCR;
    if (a->ich_vmcr != b->ich_vmcr)
        d |= WFI_VCTX_VMCR;
    for (u32 i = 0; i < a->nr_apr && i < 4; i++)
        if (a->apr0[i] != b->apr0[i] || a->apr1[i] != b->apr1[i])
            d |= WFI_VCTX_APR;
    for (u32 i = 0; i < a->nr_lr && i < WFI_VCTX_LRS; i++)
        if (a->lr[i] != b->lr[i])
            d |= WFI_VCTX_LR;
    if ((a->cntv_ctl & tmask) != (b->cntv_ctl & tmask) ||
        (a->cntp_ctl & tmask) != (b->cntp_ctl & tmask))
        d |= WFI_VCTX_TCTL;
    if (a->cntv_cval != b->cntv_cval || a->cntp_cval != b->cntp_cval)
        d |= WFI_VCTX_TCVAL;
    if (a->cntvoff != b->cntvoff)
        d |= WFI_VCTX_CNTVOFF;
    for (u32 i = 0; i < 4; i++)
        if (a->el1[i] != b->el1[i])
            d |= WFI_VCTX_EL1;
    if (a->el2[0] != b->el2[0] || a->el2[1] != b->el2[1])
        d |= WFI_VCTX_EL2CFG;
    return d;
}

/* Hardware access for the save/compare/restore, so the test drives the same sequence. */
struct wfi_vctx_ops {
    void *cx;
    void (*read)(void *cx, struct wfi_vctx *v);
    void (*write)(void *cx, const struct wfi_vctx *v, u32 classes); /* WFI_VCTX_RESTORE subset */
};

/* After the physical WFI: compare with what was saved, write back the restorable classes that changed */
static inline u32 wfi_vctx_check_restore(const struct wfi_vctx_ops *o, const struct wfi_vctx *saved,
                                         struct wfi_vctx *now)
{
    o->read(o->cx, now);
    u32 d = wfi_vctx_diff(saved, now);
    if (d & WFI_VCTX_RESTORE)
        o->write(o->cx, saved, d & WFI_VCTX_RESTORE);
    return d;
}

/*
 * Running priority from the active-priority words (ICH_AP0R<n> | ICH_AP1R<n>): bit b of word n set
 * means priority group n*32+b is active; with <pre_bits> preemption bits a group number is the
 * priority >> (8 - pre_bits). The lowest set bit is the highest active priority. 0x100 = idle.
 */
static inline u32 wfi_running_priority(const u32 *apr, u32 nr_apr, u32 pre_bits)
{
    for (u32 n = 0; n < nr_apr; n++)
        if (apr[n])
            return ((n * 32 + (u32)__builtin_ctz(apr[n])) << (8 - pre_bits)) & 0xff;
    return 0x100;
}

/*
 * The wait loop itself (smp2e: shared with the test, not a copy). Interrupts are masked at EL2 for
 * the whole loop. Each iteration first folds hardware state into the vCPU (update = hv_update_fiq:
 * timer gating, PPI levels, rebuild of a dirty vCPU's list registers), then decides, and only then
 * idles. A wake source that arrives after the decision is necessarily physical: another CPU's change
 * is signalled by its IPI after vgic_mark() set dirty; a guest timer not held by EL2 raises its FIQ; a
 * device raises an IRQ. It stays pending in ISR, so the physical WFI falls straight through and the
 * next iteration sees it: no lost wake-up. Returns the wake reason, or WFI_WAIT once <max> physical
 * WFIs have run (the guest then sees a spurious WFI completion, which the architecture allows).
 */
struct wfi_ops {
    void *cx;
    void (*update)(void *cx);       /* hv_update_fiq() */
    bool (*exit_req)(void *cx);     /* should_exit / want_cpu / pinned elsewhere */
    bool (*virt_pending)(void *cx); /* signalled LR, vFIQ/vIRQ in HCR, or vGIC dirty */
    u64 (*isr)(void *cx);           /* ISR_EL1 */
    void (*idle)(void *cx);         /* one physical WFI, register- and FP-safe */
    /*
     * smp2f, optional (NULL = the smp2e loop): around each physical WFI, as KVM does around a
     * blocking vCPU (vcpu_put and the background timer before, vcpu_load after):
     *   pre_idle:  save the vCPU's interface/timer registers, arm the EL2 timer for the earliest guest
     *              timer deadline (wfi_bg_deadline);
     *   post_idle: compare, restore what changed (wfi_vctx_check_restore), count it.
     */
    void (*pre_idle)(void *cx);
    void (*post_idle)(void *cx);
};

static inline enum wfi_wake wfi_wait(const struct wfi_ops *o, int max, int *idles)
{
    *idles = 0;
    for (;;) {
        o->update(o->cx);
        enum wfi_wake w = wfi_decide(o->exit_req(o->cx), o->virt_pending(o->cx), o->isr(o->cx));
        if (w != WFI_WAIT || *idles >= max)
            return w;
        if (o->pre_idle)
            o->pre_idle(o->cx);
        o->idle(o->cx);
        if (o->post_idle)
            o->post_idle(o->cx);
        (*idles)++;
    }
}

/*
 * Guest debug-register classification. A breakpoint/watchpoint value or control register
 * (DBGB{V,C}Rn_EL1, DBGW{V,C}Rn_EL1) is op0=2 op1=0 CRn=0 op2 in 4..7, CRm=n (0..15). These are
 * shadowed per CPU in EL2. MDCCINT (op2 0), OSDTR (op2 2), MDSCR (CRm=2 op2=2) and the OS-lock/status
 * registers (CRn=1) are not breakpoint registers and are handled elsewhere.
 */
/* u1: identical copy in hv_sysreg_logic.h (storage line); whichever header comes first defines them */
#ifndef HV_BP_REG_LOGIC_DEFINED
#define HV_BP_REG_LOGIC_DEFINED
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

#endif
