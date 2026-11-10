/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * smp2e/smp2f host unit test for the guest-WFI emulation (src/hv/hv_wfi_logic.h: the wait loop wfi_wait(),
 * wfi_decide(), the GICv3 wake rule wfi_lr_signals()/wfi_running_priority()) and the debug-register
 * shadow classification. wfi_wait() is the firmware's own loop (hv_exc.c hv_win_wfi() calls it with
 * real callbacks); here its callbacks drive a simulated CPU with virtual and physical pending state,
 * so the interleavings of a wake source with the loop can be placed exactly:
 *   - injected before the check   (inside the update step of an iteration),
 *   - injected between check and wait (after the decision, before the physical WFI),
 *   - injected during the wait     (while the physical WFI sleeps).
 * The physical WFI model follows the architecture: it completes at once if a physical interrupt is
 * pending at entry (even masked), otherwise it sleeps until one arrives; with nothing scheduled the
 * only thing left is the hypervisor tick (1 s on CPU 1), which the tests treat as a lost wake-up
 * whenever the scenario had a real wake source.
 *   make -C tests/win-wfi check
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define BIT(x) (1UL << (x))

#define HV_WFI_LOGIC_TYPES_PROVIDED
#include "../../src/hv/hv_wfi_logic.h"
#include "../../src/hv/win/hv_win_kdlog.h" /* smp2g */

static int fails;
#define CHECK(name, cond)                                                                          \
    do {                                                                                           \
        bool _c = (cond); /* evaluate once: cond may have side effects */                          \
        printf("%-64s %s\n", name, _c ? "ok" : "FAILED");                                          \
        fails += !_c;                                                                              \
    } while (0)

/* --- simulated CPU -------------------------------------------------------------------------- */

enum when { AT_UPDATE, AT_BETWEEN, AT_DURING };
enum what {
    EV_SGI,         /* another CPU: vgic_mark (dirty) then its kick IPI (physical FIQ) */
    EV_TIMER,       /* the guest timer reaches its deadline */
    EV_DEVICE,      /* a routed device line (physical IRQ) */
    EV_EXIT,        /* guest/host exit request, then its IPI */
    EV_SPURIOUS,    /* the physical WFI returns with nothing pending */
};

struct event {
    enum when when;
    int iter; /* loop iteration the event belongs to */
    enum what what;
    bool done;
};

struct sim {
    /* physical */
    u64 isr;           /* ISR_EL1 level */
    bool timer_firing; /* guest EL02 timer ISTATUS && !guest IMASK */
    bool timer_held;   /* EL2 holds IMASK after injecting it (hv_tmr_gate) */
    /* virtual (this vCPU) */
    bool dirty;        /* another CPU changed our vGIC state, not folded yet */
    bool lr_pending;   /* an LR holds a pending interrupt */
    bool lr_signals;   /* ... and it passes the GICv3 priority/group rule */
    bool exit;
    /* bookkeeping */
    int iter;          /* current wfi_wait iteration (number of update calls - 1) */
    int idles, ticks, wfi_fell_through;
    struct event ev[8];
    int nev;
    bool virt_check_enabled; /* false = the smp2d behaviour (physical-only), for the regression */
};

static void fire(struct sim *s, struct event *e)
{
    e->done = true;
    switch (e->what) {
        case EV_SGI:
            s->dirty = true;    /* vgic_mark under vgic_lock ... */
            s->isr |= WFI_ISR_F; /* ... then smp_send_ipi after the unlock */
            break;
        case EV_TIMER:
            s->timer_firing = true;
            if (!s->timer_held)
                s->isr |= WFI_ISR_F; /* VM_TMR_FIQ is locked on: a firing EL02 timer raises FIQ */
            break;
        case EV_DEVICE:
            s->isr |= WFI_ISR_I;
            break;
        case EV_EXIT:
            s->exit = true;
            s->isr |= WFI_ISR_F;
            break;
        case EV_SPURIOUS:
            break;
    }
}

static void fire_due(struct sim *s, enum when w)
{
    for (int i = 0; i < s->nev; i++)
        if (!s->ev[i].done && s->ev[i].when == w && s->ev[i].iter == s->iter)
            fire(s, &s->ev[i]);
}

/* hv_update_fiq(): gate a firing guest timer (hold IMASK, PPI level -> LR), rebuild a dirty vCPU */
static void op_update(void *cx)
{
    struct sim *s = cx;
    s->iter++;
    fire_due(s, AT_UPDATE);
    if (s->timer_firing && !s->timer_held) {
        s->timer_held = true;
        s->isr &= ~WFI_ISR_F; /* IMASK set: the timer FIQ deasserts (no other FIQ source modelled) */
        s->lr_pending = s->lr_signals = true;
    }
    if (s->dirty) {
        s->dirty = false;
        s->lr_pending = s->lr_signals = true;
    }
}

static bool op_exit(void *cx)
{
    return ((struct sim *)cx)->exit;
}

static bool op_virt(void *cx)
{
    struct sim *s = cx;
    if (!s->virt_check_enabled)
        return false;
    return s->dirty || (s->lr_pending && s->lr_signals);
}

static u64 op_isr(void *cx)
{
    return ((struct sim *)cx)->isr;
}

/* One physical WFI. Fires the between-check-and-wait events first (they happen before the WFI
 * instruction executes), then follows the WFI rule. */
static void op_idle(void *cx)
{
    struct sim *s = cx;
    fire_due(s, AT_BETWEEN);
    s->idles++;
    if (s->isr) { /* a pending (even masked) physical interrupt: WFI completes at once */
        s->wfi_fell_through++;
        return;
    }
    for (int i = 0; i < s->nev; i++) {
        if (!s->ev[i].done && s->ev[i].when == AT_DURING && s->ev[i].iter == s->iter) {
            fire(s, &s->ev[i]);
            if (s->isr || s->ev[i].what == EV_SPURIOUS)
                return; /* woken during the wait */
        }
    }
    /* nothing else will happen: the hypervisor tick ends the WFI */
    s->ticks++;
    s->isr |= WFI_ISR_F;
}

static enum wfi_wake run(struct sim *s, int max)
{
    s->iter = -1;
    const struct wfi_ops ops = {
        .cx = s, .update = op_update, .exit_req = op_exit, .virt_pending = op_virt,
        .isr = op_isr, .idle = op_idle,
    };
    int idles = 0;
    enum wfi_wake w = wfi_wait(&ops, max, &idles);
    if (idles != s->idles)
        printf("  (idle count mismatch %d vs %d)\n", idles, s->idles);
    return w;
}

static void sim_init(struct sim *s)
{
    memset(s, 0, sizeof(*s));
    s->virt_check_enabled = true;
}

static void add(struct sim *s, enum when w, int iter, enum what what)
{
    s->ev[s->nev++] = (struct event){.when = w, .iter = iter, .what = what};
}

/* --- debug shadow model (mirrors hv_dbg_shadow) ---------------------------------------------- */

#define NCPU         2
#define DBG_SHADOW_N 64
static u64 dbg_shadow_mem[NCPU][DBG_SHADOW_N];

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

static u64 lr(int state, bool grp1, u8 prio)
{
    return ((u64)state << WFI_LR_STATE_SHIFT) | (grp1 ? WFI_LR_GROUP1 : 0) |
           ((u64)prio << WFI_LR_PRIO_SHIFT) | 41;
}

/* ==== smp2f: the guest-timer lifecycle across trapped WFIs, on a timed CPU model ============== */
/*
 * A time-stepped model of one vCPU (CNTFRQ 1 GHz, times in ns) driven through the firmware's own
 * wfi_wait() and, for smp2f, its own save/compare/restore (wfi_vctx_check_restore) and background
 * timer (wfi_tmr_deadline / wfi_bg_deadline) logic. The guest arms its virtual timer, WFIs, takes the
 * timer interrupt when the virtual CPU interface delivers it, re-arms (CVAL then CTL, trapped) and
 * WFIs again: fire -> inject -> guest handles and re-arms -> fire again.
 *
 * Hardware behaviours that hardware has not confirmed are switches, so each candidate root cause of
 * run 20261110-122939-smp2e is one scenario:
 *   deep_loses      a physical WFI longer than deep_after loses ICH_HCR_EL2.En, the LRs and the guest
 *                   timer's CNTV_CTL (the EL2 physical timer, which Linux's idle relies on, survives);
 *   vt_fiq_el2      the guest virtual timer raises its FIQ (ISR.F, WFI wake-up) while the core is at EL2;
 *   wfi_in_handler  the guest WFIs with a higher-priority interrupt active (running priority 0x10).
 * "Hung" = the physical WFI has no wake source left (no hypervisor tick on this vCPU).
 */
struct m {
    /* configuration */
    bool smp2f;          /* pre/post_idle ops and KVM's wake rule (else the smp2e loop and rule) */
    bool deep_loses, vt_fiq_el2 /* default true */, wfi_in_handler;
    u64 deep_after;      /* ns */
    u64 tick_period;     /* 0 = no hypervisor tick on this vCPU */
    u64 sgi_period;      /* 0 = none: another vCPU sends this one an SGI every sgi_period ns */
    u64 period;          /* the guest's timer period */
    /* guest virtual timer (CNTVOFF 0) */
    u64 now, cval;
    u64 cntvoff; /* smp2g: only used for the deadline arithmetic (W1); the model's timer ignores it */
    bool t_en, t_imask_hw;
    /* EL2 gate (hv_tmr_gate) */
    bool held;
    /* virtual CPU interface: LR0 = timer PPI 27 (prio 0x20), LR1 = SGI 1 (prio 0x30) */
    bool ich_en;
    u64 vmcr, lr[2];
    u32 apr;
    bool line, sgi_dirty, sgi_latched;
    /* EL2 physical timer (the tick) and the IPI */
    bool hp_en;
    u64 hp_cval, next_sgi;
    bool ipi;
    /* results */
    u64 edges, delivered, sgis_sent, sgis_taken, lost, bg_armed, bg_fired;
    bool hung;
    struct wfi_vctx saved;
};

#define M_LR(state, prio, intid) (((u64)(state) << WFI_LR_STATE_SHIFT) | WFI_LR_GROUP1 | ((u64)(prio) << WFI_LR_PRIO_SHIFT) | (intid))
#define M_ST(lr) (((lr) >> WFI_LR_STATE_SHIFT) & 3)

static bool m_istatus(struct m *m)
{
    return m->t_en && m->now >= m->cval;
}

static void m_events(struct m *m)
{
    if (m->sgi_period && m->now >= m->next_sgi) {
        m->next_sgi += m->sgi_period;
        m->sgis_sent++;
        m->sgi_dirty = true; /* vgic_mark ... */
        m->ipi = true;       /* ... then the kick IPI */
    }
}

/* hv_update_fiq: timer gate, PPI level, LR rebuild (a level PPI is re-listed while its line is high;
 * ICH_HCR is rewritten only when its value changes, so a lost En is not repaired here) */
static void m_update(void *cx)
{
    struct m *m = cx;
    m_events(m); /* SGIs another vCPU sent while this one ran or spun */
    bool firing = m_istatus(m);
    if (firing && !m->held) {
        m->held = true;
        m->t_imask_hw = true;
    } else if (!firing && m->held) {
        m->held = false;
        m->t_imask_hw = false;
    }
    if (firing && !m->line)
        m->edges++;
    m->line = firing;
    if (m->line && M_ST(m->lr[0]) == 0)
        m->lr[0] = M_LR(1, 0x20, 27);
    else if (!m->line && M_ST(m->lr[0]) == 1)
        m->lr[0] = 0;
    if (m->sgi_dirty) {
        m->sgi_dirty = false;
        m->sgi_latched = true;
    }
    if (m->sgi_latched && M_ST(m->lr[1]) == 0) {
        m->lr[1] = M_LR(1, 0x30, 1);
        m->sgi_latched = false;
    }
}

static bool m_exit(void *cx)
{
    return ((struct m *)cx)->hung;
}

static u32 m_rpr(struct m *m)
{
    return m->apr ? 0x10 : 0x100;
}

static bool m_virt(void *cx)
{
    struct m *m = cx;
    if (m->sgi_dirty)
        return true;
    for (int i = 0; i < 2; i++)
        if (m->smp2f ? wfi_lr_wakes(m->lr[i], m->vmcr) : wfi_lr_signals(m->lr[i], m->vmcr, m_rpr(m)))
            return true;
    return false;
}

static bool m_hp_due(struct m *m)
{
    return m->hp_en && m->now >= m->hp_cval;
}

static u64 m_isr(void *cx)
{
    struct m *m = cx;
    bool vt = m->vt_fiq_el2 && m_istatus(m) && !m->t_imask_hw;
    return (m->ipi || m_hp_due(m) || vt) ? WFI_ISR_F : 0;
}

static void m_idle(void *cx)
{
    struct m *m = cx;
    if (m_isr(m))
        return; /* a pending physical interrupt: WFI completes at once */
    u64 start = m->now, wake = ~0UL;
    if (m->sgi_period && m->next_sgi < wake)
        wake = m->next_sgi;
    if (m->hp_en && m->hp_cval < wake)
        wake = m->hp_cval;
    if (m->vt_fiq_el2 && m->t_en && !m->t_imask_hw && m->cval < wake)
        wake = m->cval;
    if (wake == ~0UL) {
        m->hung = true; /* nothing will ever end this WFI */
        return;
    }
    m->now = wake > m->now ? wake : m->now;
    if (m->deep_loses && m->now - start > m->deep_after) {
        m->ich_en = false; /* retention lost the virtual interface ... */
        m->lr[0] = m->lr[1] = 0;
        m->t_en = false;   /* ... and the guest timer's CTL */
    }
    m_events(m);
}

static void m_vread(void *cx, struct wfi_vctx *v)
{
    struct m *m = cx;
    memset(v, 0, sizeof(*v));
    v->ich_hcr = m->ich_en;
    v->ich_vmcr = m->vmcr;
    v->nr_apr = 1;
    v->apr1[0] = m->apr;
    v->nr_lr = 2;
    v->lr[0] = m->lr[0];
    v->lr[1] = m->lr[1];
    v->cntv_ctl = (m->t_en ? WFI_CTL_ENABLE : 0) | (m->t_imask_hw ? WFI_CTL_IMASK : 0) |
                  (m_istatus(m) ? WFI_CTL_ISTATUS : 0);
    v->cntv_cval = m->cval;
}

static void m_vwrite(void *cx, const struct wfi_vctx *v, u32 classes)
{
    struct m *m = cx;
    if (classes & WFI_VCTX_HCR)
        m->ich_en = v->ich_hcr & 1;
    if (classes & WFI_VCTX_VMCR)
        m->vmcr = v->ich_vmcr;
    if (classes & WFI_VCTX_APR)
        m->apr = v->apr1[0];
    if (classes & WFI_VCTX_LR) {
        m->lr[0] = v->lr[0];
        m->lr[1] = v->lr[1];
    }
    if (classes & (WFI_VCTX_TCTL | WFI_VCTX_TCVAL)) {
        m->t_en = v->cntv_ctl & WFI_CTL_ENABLE;
        m->t_imask_hw = v->cntv_ctl & WFI_CTL_IMASK;
        m->cval = v->cntv_cval;
    }
}

/* smp2f pre_idle / post_idle, as hv_exc.c does them with the header's helpers */
static void m_pre(void *cx)
{
    struct m *m = cx;
    m_vread(m, &m->saved);
    u64 dl, prog;
    bool have = wfi_tmr_deadline(m->saved.cntv_ctl, false, m->saved.cntv_cval, m->cntvoff, &dl);
    if (wfi_bg_deadline(m->now, m->hp_en ? WFI_CTL_ENABLE : 0, m->hp_cval, have, dl, 50000, &prog) ==
        WFI_BG_ARM) {
        m->hp_cval = prog;
        m->hp_en = true;
        m->bg_armed++;
    }
}

static void m_post(void *cx)
{
    struct m *m = cx;
    struct wfi_vctx now;
    const struct wfi_vctx_ops vo = {.cx = m, .read = m_vread, .write = m_vwrite};
    if (wfi_vctx_check_restore(&vo, &m->saved, &now))
        m->lost++;
    if (m_hp_due(m) && m->tick_period == 0)
        m->bg_fired++; /* hv_exc.c: the EL2 timer fired, so the guest timer did not end the WFI */
}

/* Return to the guest: the FIQ exit acknowledges the IPI and re-arms the tick; then the guest's
 * idle loop unmasks interrupts and takes what the virtual CPU interface delivers. */
static void m_guest(struct m *m)
{
    m->ipi = false;
    if (m_hp_due(m)) {
        m->hp_en = m->tick_period != 0;
        m->hp_cval = m->now + m->tick_period;
    }
    m_update(m);
    if (m->wfi_in_handler)
        m->apr = 0; /* the handler finishes: EOI of the higher-priority interrupt */
    for (int guard = 0; guard < 4; guard++) {
        int best = -1;
        for (int i = 0; i < 2; i++)
            if (m->ich_en && wfi_lr_signals(m->lr[i], m->vmcr, m_rpr(m)) &&
                (best < 0 || ((m->lr[i] >> WFI_LR_PRIO_SHIFT) & 0xff) <
                                 ((m->lr[best] >> WFI_LR_PRIO_SHIFT) & 0xff)))
                best = i;
        if (best < 0)
            break;
        m->lr[best] = 0; /* acknowledge + EOI (deactivation folded by the next sync) */
        if (best == 0) {
            m->delivered++;
            m->cval = m->now + m->period; /* re-arm: CVAL, then CTL (both trapped) */
            m->t_en = true;
            m_update(m);
        } else {
            m->sgis_taken++;
        }
        m->now += 1000; /* 1 us of handler */
    }
    if (m->wfi_in_handler)
        m->apr = 1; /* the next WFI again runs inside a handler */
}

static void m_init(struct m *m, bool smp2f)
{
    memset(m, 0, sizeof(*m));
    m->smp2f = smp2f;
    m->vt_fiq_el2 = true;
    m->deep_after = 1000000; /* 1 ms */
    m->period = 4000000;     /* 4 ms */
    m->now = 1000;
    m->ich_en = true;
    m->vmcr = (0xf0UL << WFI_VMCR_VPMR_SHIFT) | WFI_VMCR_VENG1;
    m->cval = m->now + m->period;
    m->t_en = true;
}

/* Run the guest for <ns> of simulated time or until <fires> timer interrupts were taken */
static void m_run(struct m *m, u64 ns, u64 fires)
{
    if (m->tick_period) {
        m->hp_en = true;
        m->hp_cval = m->now + m->tick_period;
    }
    if (m->sgi_period)
        m->next_sgi = m->now + m->sgi_period;
    if (m->wfi_in_handler)
        m->apr = 1;
    u64 end = m->now + ns;
    const struct wfi_ops ops = {
        .cx = m, .update = m_update, .exit_req = m_exit, .virt_pending = m_virt, .isr = m_isr,
        .idle = m_idle, .pre_idle = m->smp2f ? m_pre : NULL, .post_idle = m->smp2f ? m_post : NULL,
    };
    for (int traps = 0; traps < 2000000 && m->now < end && m->delivered < fires && !m->hung; traps++) {
        int idles;
        wfi_wait(&ops, 64, &idles); /* the guest's WFI, trapped */
        if (m->hung)
            break;
        m_guest(m);
        m->now += 50000; /* 50 us of guest idle-loop work between WFIs */
    }
}

static void test_timer_lifecycle(void)
{
    struct m e, f;
    const u64 S = 1000000000UL;
    u64 p = 0, dl = 0;

    /* A: hardware as smp2e assumed it, no hypervisor tick: both deliver every fire */
    m_init(&e, false);
    m_init(&f, true);
    m_run(&e, S, 5);
    m_run(&f, S, 5);
    CHECK("lifecycle A (state survives, vtimer FIQ wakes EL2), no tick: smp2e 5 fires",
          e.delivered == 5 && e.edges == 5 && !e.hung);
    CHECK("lifecycle A: smp2f 5 fires, nothing lost, guest timer woke it each time (bg never fired)",
          f.delivered == 5 && f.lost == 0 && !f.hung && f.bg_fired == 0);

    /* B: a long physical WFI loses the virtual interface and the guest timer CTL (leading cause) */
    m_init(&e, false);
    m_init(&f, true);
    e.deep_loses = f.deep_loses = true;
    m_run(&e, S, 5);
    m_run(&f, S, 5);
    CHECK("lifecycle B (deep WFI loses ICH/CNTV state), no tick: smp2e FAILS (hangs, <5 fires)",
          e.hung && e.delivered < 5);
    CHECK("lifecycle B: smp2f restores it after every WFI: 5 fires",
          f.delivered == 5 && !f.hung && f.lost >= 5);

    /* B': the same with CPU 1's 1 s hypervisor tick and a 1 Hz SGI from vCPU 0, for 20 s: smp2e
     * shows the run's signature (timer edges stop, SGIs keep being sent), smp2f keeps ticking */
    m_init(&e, false);
    m_init(&f, true);
    e.deep_loses = f.deep_loses = true;
    e.tick_period = f.tick_period = S;
    e.sgi_period = f.sgi_period = S;
    m_run(&e, 20 * S, ~0UL);
    m_run(&f, 20 * S, ~0UL);
    printf("  B' smp2e: edges %lu delivered %lu sgis sent %lu taken %lu hung %d now %lu; smp2f: edges %lu delivered %lu sgis taken %lu lost %lu\n",
           (unsigned long)e.edges, (unsigned long)e.delivered, (unsigned long)e.sgis_sent, (unsigned long)e.sgis_taken, e.hung, (unsigned long)e.now,
           (unsigned long)f.edges, (unsigned long)f.delivered, (unsigned long)f.sgis_taken, (unsigned long)f.lost);
    CHECK("lifecycle B' with 1 s tick + 1 Hz SGIs, 20 s: smp2e timer edges stop (<= 1), SGIs sent",
          e.edges <= 1 && e.delivered == 0 && e.sgis_sent >= 19 && e.sgis_taken == 0);
    CHECK("lifecycle B': smp2f keeps the timer going (~250 Hz) and takes the SGIs",
          f.delivered > 4000 && f.sgis_taken >= 19);

    /* C: the guest timer's FIQ does not end a WFI executed at EL2 (review's unverified item) */
    m_init(&e, false);
    m_init(&f, true);
    e.vt_fiq_el2 = f.vt_fiq_el2 = false;
    m_run(&e, S, 5);
    m_run(&f, S, 5);
    CHECK("lifecycle C (no vtimer FIQ at EL2), no tick: smp2e FAILS (hangs)", e.hung && e.delivered == 0);
    CHECK("lifecycle C: smp2f's background EL2 timer wakes 50 us after the guest deadline: 5 fires, "
          "each counted as bg fired",
          f.delivered == 5 && !f.hung && f.bg_armed >= 5 && f.bg_fired == 5);

    /* D: the guest WFIs inside a handler (running priority above the timer's) */
    m_init(&e, false);
    m_init(&f, true);
    e.wfi_in_handler = f.wfi_in_handler = true;
    m_run(&e, S, 5);
    m_run(&f, S, 5);
    CHECK("lifecycle D (WFI with a higher-priority interrupt active), no tick: smp2e FAILS (hangs)",
          e.hung && e.delivered == 0);
    CHECK("lifecycle D: smp2f wakes under KVM's rule: 5 fires", f.delivered == 5 && !f.hung);

    /* bg timer arithmetic */
    CHECK("tmr deadline: enabled, unmasked, not yet firing", wfi_tmr_deadline(WFI_CTL_ENABLE, false, 500, 100, &dl) && dl == 600);
    CHECK("tmr deadline: disabled / guest-masked / already firing: none",
          !wfi_tmr_deadline(0, false, 500, 0, &dl) && !wfi_tmr_deadline(WFI_CTL_ENABLE, true, 500, 0, &dl) &&
              !wfi_tmr_deadline(WFI_CTL_ENABLE | WFI_CTL_ISTATUS, false, 500, 0, &dl));
#define BG(now, tctl, tcval, have, gdl, slack) wfi_bg_deadline(now, tctl, tcval, have, gdl, slack, &p)
    CHECK("bg: guest deadline + slack before the tick: pull the tick in",
          BG(100, WFI_CTL_ENABLE, 1000, true, 600, 50) == WFI_BG_ARM && p == 650);
    CHECK("bg: tick first (or within the slack): leave it",
          BG(100, WFI_CTL_ENABLE, 500, true, 600, 50) == WFI_BG_NONE &&
              BG(100, WFI_CTL_ENABLE, 640, true, 600, 50) == WFI_BG_NONE);
    CHECK("bg: tick masked or off: program the guest deadline + slack",
          BG(100, WFI_CTL_ENABLE | WFI_CTL_IMASK, 500, true, 600, 50) == WFI_BG_ARM && p == 650 &&
              BG(100, 0, 0, true, 700, 0) == WFI_BG_ARM && p == 700);
    CHECK("bg: no guest deadline: nothing", BG(100, 0, 0, false, 700, 50) == WFI_BG_NONE);

    /* ---- smp2g (review W1): wrap-safe deadlines ---- */
    const u64 NOW = 5000000000UL, MAX = ~(u64)0, OFF = 123456;
    p = 0x5a5a;
    CHECK("W1 deadline 0: PASSED, nothing programmed", BG(NOW, 0, 0, true, 0, 1200) == WFI_BG_PASSED && p == 0x5a5a);
    CHECK("W1 deadline == now: PASSED", BG(NOW, 0, 0, true, NOW, 1200) == WFI_BG_PASSED);
    CHECK("W1 deadline just past now (now-1): PASSED", BG(NOW, 0, 0, true, NOW - 1, 1200) == WFI_BG_PASSED);
    CHECK("W1 deadline just ahead (now+1): ARM at deadline+slack",
          BG(NOW, 0, 0, true, NOW + 1, 1200) == WFI_BG_ARM && p == NOW + 1201);
    p = 0x5a5a;
    CHECK("W1 far future (now + horizon + 1): NONE", BG(NOW, 0, 0, true, NOW + WFI_BG_HORIZON + 1, 1200) == WFI_BG_NONE);
    CHECK("W1 at the horizon: still ARM", BG(NOW, 0, 0, true, NOW + WFI_BG_HORIZON, 1200) == WFI_BG_ARM);
    p = 0x5a5a;
    CHECK("W1 deadline 2^64-1: NONE, nothing programmed", BG(NOW, 0, 0, true, MAX, 1200) == WFI_BG_NONE && p == 0x5a5a);
    CHECK("W1 deadline 2^64-1-slack..2^64-1 with now near the top: NONE (sum would wrap)",
          BG(MAX - 5000, 0, 0, true, MAX - 100, 1200) == WFI_BG_NONE &&
              BG(MAX - 5000, 0, 0, true, MAX - 1200, 1200) == WFI_BG_ARM && p == MAX);
    CHECK("W1 tmr: CVAL 2^64-1 with a non-zero offset wraps: no deadline",
          !wfi_tmr_deadline(WFI_CTL_ENABLE, false, MAX, OFF, &dl) &&
              !wfi_tmr_deadline(WFI_CTL_ENABLE, false, MAX - OFF + 1, OFF, &dl));
    CHECK("W1 tmr: CVAL + offset == 2^64-1 exactly: a deadline, then NONE (far future)",
          wfi_tmr_deadline(WFI_CTL_ENABLE, false, MAX - OFF, OFF, &dl) && dl == MAX &&
              BG(NOW, WFI_CTL_ENABLE, NOW + 1000, true, dl, 1200) == WFI_BG_NONE);
    CHECK("W1 tmr: CVAL 2^64-1, offset 0: a deadline of 2^64-1, NONE",
          wfi_tmr_deadline(WFI_CTL_ENABLE, false, MAX, 0, &dl) && dl == MAX && BG(NOW, 0, 0, true, dl, 1200) == WFI_BG_NONE);
    CHECK("W1 tmr: CVAL 0, offset 0: deadline 0 -> PASSED",
          wfi_tmr_deadline(WFI_CTL_ENABLE, false, 0, 0, &dl) && dl == 0 && BG(NOW, 0, 0, true, dl, 1200) == WFI_BG_PASSED);

    /* a parked timer (CVAL = 2^64-1, CNTVOFF != 0) with a 1 s tick: smp2f's arithmetic programmed a
     * deadline in the past on every WFI (busy loop, bg fired inflated); smp2g never arms */
    {
        struct m k;
        m_init(&k, true);
        k.cval = MAX;
        k.cntvoff = OFF;
        k.tick_period = S;
        m_run(&k, 5 * S, ~0UL);
        CHECK("W1 lifecycle: parked timer (CVAL 2^64-1, CNTVOFF != 0): never armed, sleeps tick to tick",
              k.bg_armed == 0 && k.bg_fired == 0 && k.delivered == 0 && !k.hung && k.now >= 5 * S);
    }

    /* ---- smp2g (review W2): wake source and lateness ---- */
    CHECK("W2 source: EL2 timer fired -> BG (even with the guest timer firing)",
          wfi_wake_source(true, false, true) == WFI_WSRC_BG && wfi_wake_source(true, true, true) == WFI_WSRC_BG);
    CHECK("W2 source: IPI pending, EL2 timer not fired -> IPI", wfi_wake_source(false, true, true) == WFI_WSRC_IPI);
    CHECK("W2 source: only the guest timer firing -> GT", wfi_wake_source(false, false, true) == WFI_WSRC_GT);
    CHECK("W2 source: nothing -> OTHER", wfi_wake_source(false, false, false) == WFI_WSRC_OTHER);
    struct wfi_lat L;
    memset(&L, 0, sizeof(L));
    wfi_lat_note(&L, 700);
    wfi_lat_note(&L, 30);
    wfi_lat_note(&L, 90);
    CHECK("W2 lateness: n/min/max/last", L.n == 3 && L.min == 30 && L.max == 700 && L.last == 90);
    wfi_lat_note(&L, 0x1ffffffffUL);
    CHECK("W2 lateness saturates at 2^32-1", L.max == 0xffffffffU && L.last == 0xffffffffU && L.min == 30);

    /* ---- smp2g: KD break/continue recogniser ---- */
    {
        struct kdlog_parser kp;
        struct kdlog_event ev;
        memset(&kp, 0, sizeof(kp));
        u8 pkt[64];
        int n = 0, hits = 0;
        const char *noise = "Image at 0000 text 00 000";
        for (const char *c = noise; *c; c++)
            hits += kdlog_feed(&kp, (u8)*c, &ev); /* a false leader in text: no event */
        for (int i = 0; i < 12; i++)
            hits += kdlog_feed(&kp, 0xee, &ev); /* flush the false header */
        CHECK("KD: console text with '0000' produces no event", hits == 0);
        memset(pkt, 0, sizeof(pkt));
        memset(pkt, 0x30, 4);
        pkt[4] = 7; /* type */
        pkt[6] = 240; /* byte count */
        n = 16;
        pkt[n + 0] = 0x30, pkt[n + 1] = 0x30; /* NewState 0x3030 */
        pkt[n + 6] = 1;                       /* processor 1 */
        pkt[n + 8] = 2;                       /* 2 processors */
        for (int i = 0; i < 8; i++)
            pkt[n + 24 + i] = 0x11 * (i + 1);
        memset(&ev, 0, sizeof(ev));
        hits = 0;
        for (int i = 0; i < 16 + 32; i++)
            hits += kdlog_feed(&kp, pkt[i], &ev);
        CHECK("KD: state change (exception) from processor 1 recognised with its PC",
              hits == 1 && ev.type == 7 && ev.code == 0x3030 && ev.proc == 1 && ev.pc == 0x8877665544332211UL);
        memset(pkt, 0, sizeof(pkt));
        memset(pkt, 0x30, 4);
        pkt[4] = 2;
        pkt[6] = 56;
        pkt[n + 0] = 0x3c, pkt[n + 1] = 0x31; /* DbgKdContinueApi2 */
        pkt[n + 16] = 0x02, pkt[n + 18] = 0x01; /* DBG_CONTINUE 0x00010002 */
        hits = 0;
        for (int i = 0; i < 16 + 32; i++)
            hits += kdlog_feed(&kp, pkt[i], &ev);
        CHECK("KD: continue (0x313c, DBG_CONTINUE) recognised",
              hits == 1 && ev.type == 2 && ev.code == 0x313c && ev.pc == 0x00010002);
        pkt[n + 0] = 0x30; /* ReadVirtualMemory 0x3130: a manipulate that is not a continue */
        hits = 0;
        for (int i = 0; i < 16 + 32; i++)
            hits += kdlog_feed(&kp, pkt[i], &ev);
        CHECK("KD: other manipulate packets are ignored", hits == 0);
        hits = 0;
        for (int i = 0; i < 100000; i++)
            hits += kdlog_feed(&kp, (u8)(i * 7 + (i >> 3)), &ev);
        for (int i = 0; i < 4096; i++)
            hits += kdlog_feed(&kp, 0x30, &ev); /* a long run of '0' */
        CHECK("KD: arbitrary bytes and a run of '0' never overrun or report", hits == 0 && kp.n <= sizeof(kp.buf));
    }

    /* vctx diff: ISTATUS is not state; ENABLE/IMASK, CVAL, LR, HCR are */
    struct wfi_vctx a, b;
    memset(&a, 0, sizeof(a));
    a.nr_lr = 2;
    a.nr_apr = 1;
    b = a;
    b.cntv_ctl |= WFI_CTL_ISTATUS;
    CHECK("vctx diff ignores ISTATUS", wfi_vctx_diff(&a, &b) == 0);
    b.cntv_ctl |= WFI_CTL_ENABLE;
    b.lr[1] = 1;
    b.ich_hcr = 1;
    b.el1[3] = 5;
    CHECK("vctx diff classes", wfi_vctx_diff(&a, &b) == (WFI_VCTX_TCTL | WFI_VCTX_LR | WFI_VCTX_HCR | WFI_VCTX_EL1));
    b = a;
    b.lr[2] = 9; /* beyond nr_lr: not compared */
    CHECK("vctx diff only over the LRs in use", wfi_vctx_diff(&a, &b) == 0);
    CHECK("KVM rule: active or invalid never wakes; pending under VPMR does",
          !wfi_lr_wakes(lr(2, true, 0x10), 0xf0UL << 24) && !wfi_lr_wakes(lr(0, true, 0x10), 0xf0UL << 24) &&
              wfi_lr_wakes(lr(1, false, 0x10), 0xf0UL << 24) && !wfi_lr_wakes(lr(1, true, 0xf0), 0xf0UL << 24));
}

int main(void)
{
    struct sim s;
    enum wfi_wake w;

    /* ==== F1: virtual interrupt already pending for the vCPU ===================================== */
    sim_init(&s);
    s.lr_pending = s.lr_signals = true; /* e.g. the timer EL2 injected and holds with IMASK */
    s.timer_held = s.timer_firing = true;
    w = run(&s, 64);
    CHECK("F1: LR already pending -> wakes at once (VIRQ), no physical WFI",
          w == WFI_WAKE_VIRQ && s.idles == 0 && s.ticks == 0);

    sim_init(&s);
    s.virt_check_enabled = false; /* the smp2d loop: physical ISR only */
    s.lr_pending = s.lr_signals = true;
    s.timer_held = s.timer_firing = true;
    w = run(&s, 64);
    CHECK("F1 regression: smp2d physical-only check sleeps to the tick", s.ticks == 1);

    sim_init(&s);
    s.timer_firing = true; /* deadline already due when the WFI traps (FIQ asserted, not yet gated) */
    s.isr = WFI_ISR_F;
    w = run(&s, 64);
    CHECK("timer already due: update gates it, wakes VIRQ, no physical WFI",
          w == WFI_WAKE_VIRQ && s.idles == 0 && s.timer_held);

    sim_init(&s);
    s.dirty = true; /* another CPU's change landed before the trap */
    w = run(&s, 64);
    CHECK("vCPU dirty before the trap: wakes VIRQ at once", w == WFI_WAKE_VIRQ && s.idles == 0);

    /* ==== interleavings: SGI from the other vCPU =============================================== */
    sim_init(&s);
    add(&s, AT_UPDATE, 0, EV_SGI);
    w = run(&s, 64);
    CHECK("SGI before the check: VIRQ, no physical WFI", w == WFI_WAKE_VIRQ && s.idles == 0);

    sim_init(&s);
    add(&s, AT_BETWEEN, 0, EV_SGI);
    w = run(&s, 64);
    CHECK("SGI between check and wait: WFI falls through, then VIRQ",
          w == WFI_WAKE_VIRQ && s.idles == 1 && s.wfi_fell_through == 1 && s.ticks == 0);

    sim_init(&s);
    add(&s, AT_DURING, 0, EV_SGI);
    w = run(&s, 64);
    CHECK("SGI during the wait: woken, then VIRQ, no tick",
          w == WFI_WAKE_VIRQ && s.idles == 1 && s.ticks == 0);

    /* ==== interleavings: the vCPU's own timer ================================================== */
    sim_init(&s);
    add(&s, AT_UPDATE, 0, EV_TIMER);
    w = run(&s, 64);
    CHECK("timer fires before the check: VIRQ, no physical WFI", w == WFI_WAKE_VIRQ && s.idles == 0);

    sim_init(&s);
    add(&s, AT_BETWEEN, 0, EV_TIMER);
    w = run(&s, 64);
    CHECK("timer fires between check and wait: falls through, then VIRQ",
          w == WFI_WAKE_VIRQ && s.idles == 1 && s.wfi_fell_through == 1 && s.ticks == 0);

    sim_init(&s);
    add(&s, AT_DURING, 0, EV_TIMER);
    w = run(&s, 64);
    CHECK("timer fires during the wait: woken by its FIQ, then VIRQ",
          w == WFI_WAKE_VIRQ && s.idles == 1 && s.ticks == 0);

    /* ==== interleavings: device SPI (physical IRQ, delivered by the normal IRQ exit) ============ */
    sim_init(&s);
    add(&s, AT_BETWEEN, 0, EV_DEVICE);
    w = run(&s, 64);
    CHECK("device IRQ between check and wait: falls through, IRQ",
          w == WFI_WAKE_IRQ && s.idles == 1 && s.ticks == 0);

    sim_init(&s);
    add(&s, AT_DURING, 0, EV_DEVICE);
    w = run(&s, 64);
    CHECK("device IRQ during the wait: IRQ, no tick", w == WFI_WAKE_IRQ && s.ticks == 0);

    /* ==== exit request ========================================================================== */
    sim_init(&s);
    add(&s, AT_DURING, 0, EV_EXIT);
    w = run(&s, 64);
    CHECK("exit request during the wait: EXIT", w == WFI_WAKE_EXIT && s.ticks == 0);

    sim_init(&s);
    s.exit = true;
    s.lr_pending = s.lr_signals = true;
    w = run(&s, 64);
    CHECK("exit wins over pending virtual work", w == WFI_WAKE_EXIT && s.idles == 0);

    /* ==== masked virtual interrupt: architecturally not a wake source ========================== */
    sim_init(&s);
    s.lr_pending = true;
    s.lr_signals = false; /* below VPMR / group off / running priority higher */
    w = run(&s, 64);
    CHECK("masked LR (not signalled) does not wake; core idles until a real source",
          s.idles == 1 && s.ticks == 1 && w == WFI_WAKE_FIQ);

    /* ==== spurious physical wakes: bounded ===================================================== */
    sim_init(&s);
    for (int i = 0; i < 3; i++)
        add(&s, AT_DURING, i, EV_SPURIOUS);
    add(&s, AT_DURING, 3, EV_SGI);
    w = run(&s, 64);
    CHECK("three spurious wakes then an SGI: re-idles, then VIRQ",
          w == WFI_WAKE_VIRQ && s.idles == 4 && s.ticks == 0);

    sim_init(&s);
    for (int i = 0; i < 8; i++)
        add(&s, AT_DURING, i, EV_SPURIOUS);
    w = run(&s, 4);
    CHECK("only spurious wakes: returns after <max> physical WFIs (WAIT)",
          w == WFI_WAIT && s.idles == 4);

    /* ==== per-vCPU isolation: an SGI for vCPU 0 does not wake vCPU 1 =========================== */
    struct sim a, b;
    sim_init(&a);
    sim_init(&b);
    add(&a, AT_DURING, 0, EV_SGI); /* only CPU 0 receives the kick */
    enum wfi_wake wa = run(&a, 64), wb = run(&b, 64);
    CHECK("per-vCPU: kicked CPU wakes VIRQ, the other idles to its own tick",
          wa == WFI_WAKE_VIRQ && a.ticks == 0 && wb == WFI_WAKE_FIQ && b.ticks == 1);

    /* ==== GICv3 wake rule (wfi_lr_signals / wfi_running_priority) ============================== */
    const u64 vmcr = (0xf0UL << WFI_VMCR_VPMR_SHIFT) | WFI_VMCR_VENG1;
    CHECK("LR pending grp1 prio 0x50, VPMR 0xf0, idle: signals",
          wfi_lr_signals(lr(1, true, 0x50), vmcr, 0x100));
    CHECK("LR pending prio == VPMR: masked", !wfi_lr_signals(lr(1, true, 0xf0), vmcr, 0x100));
    CHECK("LR pending, VENG1 off: masked",
          !wfi_lr_signals(lr(1, true, 0x50), 0xf0UL << WFI_VMCR_VPMR_SHIFT, 0x100));
    CHECK("group-0 LR uses VENG0",
          wfi_lr_signals(lr(1, false, 0x50), (0xf0UL << WFI_VMCR_VPMR_SHIFT) | WFI_VMCR_VENG0, 0x100) &&
              !wfi_lr_signals(lr(1, false, 0x50), vmcr, 0x100));
    CHECK("active LR: not a wake", !wfi_lr_signals(lr(2, true, 0x50), vmcr, 0x100));
    CHECK("pending+active LR: not a wake (KVM: pending && !active)",
          !wfi_lr_signals(lr(3, true, 0x50), vmcr, 0x100));
    CHECK("invalid LR: not a wake", !wfi_lr_signals(lr(0, true, 0x50), vmcr, 0x100));
    CHECK("running priority 0x40 blocks prio 0x50", !wfi_lr_signals(lr(1, true, 0x50), vmcr, 0x40));
    CHECK("running priority 0x60 lets prio 0x50 through", wfi_lr_signals(lr(1, true, 0x50), vmcr, 0x60));

    u32 apr[4] = {0, 0, 0, 0};
    CHECK("RPR idle = 0x100", wfi_running_priority(apr, 1, 5) == 0x100);
    apr[0] = BIT(8) | BIT(12); /* groups 8 and 12 active, 5 preemption bits */
    CHECK("RPR = highest active group (8 << 3 = 0x40)", wfi_running_priority(apr, 1, 5) == 0x40);
    apr[0] = 0;
    apr[1] = BIT(0); /* group 32 with 6 bits -> 32 << 2 = 0x80 */
    CHECK("RPR from the second APR word (6 bits): 0x80", wfi_running_priority(apr, 2, 6) == 0x80);

    /* ==== wfi_decide ordering =================================================================== */
    CHECK("decide: masked-at-PE physical FIQ still wakes", wfi_decide(false, false, WFI_ISR_F) == WFI_WAKE_FIQ);
    CHECK("decide: virtual beats physical", wfi_decide(false, true, WFI_ISR_F) == WFI_WAKE_VIRQ);
    CHECK("decide: nothing -> WAIT", wfi_decide(false, false, 0) == WFI_WAIT);

    /* ==== debug-register shadow ================================================================= */
    memset(dbg_shadow_mem, 0, sizeof(dbg_shadow_mem));
    u64 regs[32] = {0};
    regs[3] = 0xdeadbeef;
    CHECK("DBGBVR0 write is shadowed", dbg_shadow(0, 2, 0, 0, 0, 4, 3, false, regs));
    regs[3] = 0;
    CHECK("DBGBVR0 reads back the written value",
          dbg_shadow(0, 2, 0, 0, 0, 4, 3, true, regs) && regs[3] == 0xdeadbeef);
    regs[3] = 0x1234;
    dbg_shadow(1, 2, 0, 0, 0, 4, 3, true, regs);
    CHECK("DBGBVR0 is per-CPU (CPU1 unset)", regs[3] == 0);
    CHECK("DBGBCR5 shadowed, slot 21", wfi_is_bp_reg(2, 0, 0, 5) && wfi_bp_index(5, 5) == 21);
    CHECK("DBGWVR3 shadowed, slot 35", wfi_is_bp_reg(2, 0, 0, 6) && wfi_bp_index(6, 3) == 35);
    CHECK("DBGWCR0 shadowed, slot 48", wfi_is_bp_reg(2, 0, 0, 7) && wfi_bp_index(7, 0) == 48);
    CHECK("MDSCR_EL1 not shadowed here", !wfi_is_bp_reg(2, 0, 0, 2));
    CHECK("MDCCINT_EL1 not shadowed here", !wfi_is_bp_reg(2, 0, 0, 0));
    CHECK("OSLSR_EL1 (CRn=1) not shadowed here", !wfi_is_bp_reg(2, 0, 1, 4));
    CHECK("a normal sysreg (op0=3) not shadowed", !wfi_is_bp_reg(3, 0, 0, 4));
    dbg_shadow_mem[0][wfi_bp_index(4, 1)] = 0x55;
    dbg_shadow(0, 2, 0, 0, 1, 4, 31, false, regs);
    CHECK("write from xzr stores 0", dbg_shadow_mem[0][wfi_bp_index(4, 1)] == 0);
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

    /* ==== smp2f: timer lifecycle across trapped WFIs ============================================ */
    test_timer_lifecycle();

    printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
