/* SPDX-License-Identifier: MIT */
/*
 * GICv3 distributor/redistributor emulation for the Windows guest (interrupt route R1, M1+).
 *
 * Apple cores implement the GICv3 *virtual* CPU interface in hardware (ICH_*_EL2, 8 list registers
 * and 5 priority bits on the T6050: ICH_VTR_EL2 = 0x90580007). There is no distributor, no
 * redistributor and no physical CPU interface, so EL2 models GICD and one GICR frame pair per vCPU
 * as MMIO hooks in the synthetic IPA window and delivers through the list registers. The guest's
 * ICC_* accesses go to the hardware ICV_* registers and never trap, except ICC_SGI*R_EL1
 * (HCR_EL2.IMO=1), which hv_vgic3_handle_sgi1r() turns into pending SGIs.
 *
 * Model (Linux KVM vgic-v3 semantics unless noted):
 *  - GICD_CTLR.DS=1, ARE RAO/WI, Group 1 only (IGROUPR RAO/WI, IGRPMODR/NSACR RAZ/WI), no LPIs/ITS,
 *    ITLinesNumber 31 (SPIs 32..1019), IROUTER Aff2.Aff1.Aff0 only (IRM and Aff3 RAZ/WI, No1N=1),
 *    PIDR2 0x30.
 *  - Per-IRQ software state: enable, pending latch, line level, active, priority (masked to the
 *    implemented priority bits), edge/level, route. The list registers are a cache of that state:
 *    every guest entry on a vCPU with something listed folds its LRs back into software state and
 *    rebuilds them from the highest-priority candidates (active first). Changes made by another
 *    CPU mark the owning vCPU dirty and kick it with an m1n1 IPI; its FIQ path re-enters through
 *    hv_update_fiq() and rebuilds.
 *  - HW=0 in every LR (no physical GIC to deactivate). Level interrupts get ICH_LR_EOI and are
 * never listed pending+active, so their deactivation raises the maintenance interrupt and the line
 * is resampled (KVM's software resampling for Apple cores). AIC-backed SPIs latch: the AIC masks
 * the line on acknowledge, the SPI carries ICH_LR_EOI, and the line is unmasked once the guest has
 *    deactivated the SPI, so a still-asserted device line simply raises a new event.
 *  - Overflow (more candidates than LRs): ICH_HCR_EL2.UIE. Active interrupts that did not fit get
 *    ICH_HCR_EL2.LRENPIE; their deactivations arrive as EOIcount and retire the highest-priority
 *    unlisted active interrupt each.
 *  - Emulated sources: timer PPIs (hv_win_update_virq) and the PL011 (hv_win_pl011_raise) drive
 *    line levels; the PL011 line is resampled on deactivation.
 *  - The maintenance interrupt is a FIQ on Apple cores (no AIC status bit): hv_exc_fiq() calls
 *    hv_vgic3_maintenance() first, which rebuilds and so clears ICH_MISR_EL2.
 *
 * Locking: one spinlock for all distributor/redistributor state. The list registers of a vCPU are
 * only touched by its own CPU. IPIs are sent after the lock is dropped.
 *
 * Plan and numbering: m5pro-windows docs/EL2-RUNTIME.md items 4, 5a-5d.
 */

#ifdef VGIC3_HOST_TEST
#include "vgic3_host.h" /* tests/win-vgic3: sysreg, AIC, SMP and stage-2 mocks */
#else
#include "aic.h"
#include "aic_regs.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

#include "../hv.h"
#endif

#include "hv_win.h"
#include "hv_win_storage.h"
#include "hv_win_regs.h"
#include "../hv_wfi_logic.h" /* smp2e: wfi_lr_signals(), wfi_running_priority() */

extern bool hv_started_cpus[MAX_CPUS];

#define VGIC_NR_IRQS   (GIC_MAX_SPI + 1) /* INTIDs 0..1019 */
#define VGIC_NR_SPIS   (VGIC_NR_IRQS - GIC_NR_PRIVATE)
#define VGIC_MAP_WORDS ((VGIC_NR_IRQS + 63) / 64)
#define VGIC_MAX_LRS   16
#define VGIC_NONE      0xff
#define VGIC_NO_LINE   0xffffffffU
#define VGIC_AIC_LINES (AIC_MAX_DIES * AIC_MAX_HW_NUM)

#define VGIC_GICD_TYPER                                                                            \
    (FIELD_PREP(GICD_TYPER_ITLINES, 31) | FIELD_PREP(GICD_TYPER_IDBITS, 9) | GICD_TYPER_No1N)
#define VGIC_IIDR  (FIELD_PREP(GICD_IIDR_PRODUCTID, 'M') | FIELD_PREP(GICD_IIDR_IMPLEMENTER, 0x43b))
#define VGIC_PIDR2 GIC_PIDR2_ARCH_GICV3 /* CONTRACT.md s9 (KVM/GIC-600: 0x3b, same ArchRev) */

/* State changes made while an IRQ sits in a list register, applied when that LR is folded */
#define REQ_CLR_PEND BIT(0)
#define REQ_SET_ACT  BIT(1)
#define REQ_CLR_ACT  BIT(2)

enum vgic_op { OP_SET_EN, OP_CLR_EN, OP_SET_PEND, OP_CLR_PEND, OP_SET_ACT, OP_CLR_ACT };
enum vgic_field { F_EN, F_PEND, F_ACT };

struct vgic_irq {
    u32 aic_line; /* VGIC_NO_LINE unless an AIC line is routed here */
    u32 mpidr;    /* SPI: GICD_IROUTER Aff2.Aff1.Aff0 */
    u16 intid;
    u8 priority;
    u8 target;   /* vCPU the IRQ is routed to */
    u8 owner;    /* vCPU whose candidate set holds it (vgic_oracle), or VGIC_NONE */
    u8 lr_vcpu;  /* vCPU whose list register holds it, or VGIC_NONE */
    u8 lr_idx;   /* LR index on lr_vcpu */
    u8 act_vcpu; /* vCPU it is active on */
    u8 req;      /* REQ_* */
    bool enabled : 1;
    bool pending_latch : 1;
    bool line_level : 1;
    bool active : 1;
    bool edge : 1;
    bool aic_masked : 1;
};

struct vgic_vcpu {
    struct vgic_irq priv[GIC_NR_PRIVATE];
    u64 cand[VGIC_MAP_WORDS]; /* INTIDs whose owner is this vCPU */
    u64 lr_val[VGIC_MAX_LRS]; /* LR contents as last read or written by EL2 */
    u64 hcr;                  /* ICH_HCR_EL2 as last written */
    u32 aff;                  /* Aff3.Aff2.Aff1.Aff0 */
    int cpu;                  /* m1n1 CPU index */
    u32 lr_used;              /* list registers holding an IRQ (this vCPU's CPU only) */
    u32 dirty;                /* rebuild needed; written under the lock, peeked without it */
    u16 lr_intid[VGIC_MAX_LRS];
    u8 num_lrs;
    u8 nr_apr;
    bool ready; /* virtual CPU interface enabled on this vCPU's CPU */
    bool sleep; /* GICR_WAKER.ProcessorSleep */
    u64 nr_timer_edges; /* smp2: CNTP/CNTV rising edges (written by this vCPU's CPU only) */
    u64 nr_sgi_rx;      /* smp2: SGIs made pending here (under the lock) */
    u64 report_edges;   /* smp2b: nr_timer_edges at the last periodic report (boot CPU only) */
    /* smp2f diagnostics (statistics only; nothing reads them back into the model) */
    u64 t_last_edge;    /* CNTPCT of the last timer PPI rising edge */
    u64 nr_sgi_from[4]; /* SGIs made pending here, by sending vCPU (3 = vCPU 3 or more) */
    u32 nr_sgi_id[16];  /* SGIs made pending here, by SGI number */
    u64 nr_sgi_ack;     /* SGI LRs the guest acknowledged (pending -> not pending at a fold) */
    u64 nr_timer_ack;   /* timer PPI LRs the guest acknowledged */
    u64 nr_syncs;       /* vgic_sync() runs on this vCPU */
    u64 stall_edges;    /* nr_timer_edges when the stall watch last saw progress (boot CPU only) */
    u64 stall_since;    /* CNTPCT of that progress */
    bool stall_dumped;  /* auto dump already done for this stall episode */
};

static struct {
    bool ready;
    bool grp1; /* GICD_CTLR.EnableGrp1 */
    u8 pri_mask;
    u32 nr_vcpus;
    u32 kick; /* vCPUs to IPI once the lock is dropped */
    u64 gicd_base;
    u64 gicr_base;
    u8 cpu_vcpu[MAX_CPUS];
    struct vgic_vcpu vcpu[MAX_CPUS];
    struct vgic_irq spi[VGIC_NR_SPIS];
    u32 nr_map;
    struct {
        u32 line;
        u32 intid;
    } map[VGIC_NR_SPIS];
    u32 unrouted_seen[VGIC_AIC_LINES / 32];
    u32 pl011_intid; /* SPI of the emulated PL011 (hv_win_pl011.c), 0 = none */
    u64 nr_sgi, nr_aic, nr_maint, nr_overflow;
    u64 report_next; /* smp2: next periodic per-vCPU report (hv_vgic3_smp_report) */
} vgic;

static spinlock_t vgic_lock = SPINLOCK_INIT;

/* --- helpers ---------------------------------------------------------------------------------- */

/*
 * smp2g diagnostics (statistics only), for every vCPU (smpN: was the first 4, so the dump showed
 * "interrupts taken 0" for vCPU 4 and up whatever they took): SGIs made pending ("rx") and
 * SGIs the guest took ("taken"), by sending vCPU (3 = vCPU 3 or more) and SGI number, and the last
 * interrupt of any kind the guest took. A take is attributed to the SGI's most recent sender (the
 * model, like GICv3, keeps one pending bit per SGI, not one per sender). Written under vgic_lock.
 */
#define VGIC_STAT_N MAX_CPUS
static struct vgic_stat {
    u32 rx[4][16];
    u32 taken[4][16];
    u8 last_src[16];
    u64 nr_taken;      /* interrupts of any kind taken (listed pending -> not pending at a fold) */
    u64 t_last_taken;  /* CNTPCT when the fold saw the last one */
    u32 last_taken_intid;
} vgic_stat[VGIC_STAT_N];

static void vgic_stat_sgi_rx(u32 v, u32 from, u32 sgi)
{
    if (v >= VGIC_STAT_N)
        return;
    u32 src = from < 3 ? from : 3;
    vgic_stat[v].rx[src][sgi & 15]++;
    vgic_stat[v].last_src[sgi & 15] = src;
}

static u8 vgic_cur_vcpu(void)
{
    int cpu = smp_id();

    if (cpu < 0 || cpu >= MAX_CPUS)
        return VGIC_NONE;
    return vgic.cpu_vcpu[cpu];
}

static struct vgic_irq *vgic_get_irq(u8 v, u32 intid)
{
    if (intid < GIC_NR_PRIVATE)
        return (v < MAX_CPUS && v < vgic.nr_vcpus) ? &vgic.vcpu[v].priv[intid] : NULL;
    if (intid < VGIC_NR_IRQS)
        return &vgic.spi[intid - GIC_NR_PRIVATE];
    return NULL;
}

static u8 vgic_find_vcpu(u32 aff)
{
    for (u32 v = 0; v < vgic.nr_vcpus; v++)
        if (vgic.vcpu[v].aff == aff)
            return v;
    return VGIC_NONE;
}

/* Edge interrupts and AIC-backed lines latch; level interrupts follow their line (KVM) */
static bool vgic_latched(const struct vgic_irq *irq)
{
    return irq->edge || irq->aic_line != VGIC_NO_LINE;
}

static bool vgic_pending(const struct vgic_irq *irq)
{
    return irq->pending_latch || (!vgic_latched(irq) && irq->line_level);
}

/* The vCPU that must list this IRQ: where it is active, else its target when it can be signalled */
static u8 vgic_oracle(const struct vgic_irq *irq)
{
    if (irq->active)
        return irq->act_vcpu;
    if (irq->enabled && vgic.grp1 && vgic_pending(irq))
        return irq->target;
    return VGIC_NONE;
}

static void vgic_mark(u8 v)
{
    if (v >= vgic.nr_vcpus)
        return;
    __atomic_store_n(&vgic.vcpu[v].dirty, 1, __ATOMIC_RELEASE);
    if (v != vgic_cur_vcpu())
        vgic.kick |= BIT(v);
}

static void vgic_unlock(void)
{
    u32 kick = vgic.kick;

    vgic.kick = 0;
    spin_unlock(&vgic_lock);

    while (kick) {
        int v = __builtin_ctz(kick);
        int cpu = vgic.vcpu[v].cpu;

        kick &= kick - 1;
        if (cpu != smp_id() && hv_started_cpus[cpu]) {
            hv_win_kick_note(cpu); /* sched1: kick-to-wake latency (hv_exc.c) */
            smp_send_ipi(cpu);
        }
    }
}

/* AIC line masked unless the SPI is enabled and idle (not pending, active or listed) */
static void vgic_aic_sync(struct vgic_irq *irq)
{
    if (irq->aic_line == VGIC_NO_LINE)
        return;

    bool mask = !irq->enabled || irq->pending_latch || irq->active || irq->lr_vcpu != VGIC_NONE;
    if (mask == irq->aic_masked)
        return;
    if (mask)
        aic_mask(irq->aic_line);
    else
        aic_unmask(irq->aic_line);
    irq->aic_masked = mask;
}

/* Re-evaluate an IRQ after any state change (lock held) */
static void vgic_update(struct vgic_irq *irq)
{
    u8 owner = vgic_oracle(irq);
    u32 w = irq->intid / 64;
    u64 bit = BIT(irq->intid % 64);

    if (irq->lr_vcpu != VGIC_NONE)
        vgic_mark(irq->lr_vcpu);

    if (owner != irq->owner) {
        if (irq->owner != VGIC_NONE)
            vgic.vcpu[irq->owner].cand[w] &= ~bit;
        if (owner != VGIC_NONE)
            vgic.vcpu[owner].cand[w] |= bit;
        irq->owner = owner;
    }

    if (owner != VGIC_NONE && irq->lr_vcpu == VGIC_NONE)
        vgic_mark(owner);

    vgic_aic_sync(irq);
}

static void vgic_update_all(void)
{
    for (u32 v = 0; v < vgic.nr_vcpus; v++)
        for (u32 i = 0; i < GIC_NR_PRIVATE; i++)
            vgic_update(&vgic.vcpu[v].priv[i]);
    for (u32 i = 0; i < VGIC_NR_SPIS; i++)
        vgic_update(&vgic.spi[i]);
}

/* Input line change from an emulated source (timers, emulated devices) */
static void vgic_set_level(struct vgic_irq *irq, bool level)
{
    if (irq->line_level == level)
        return;
    irq->line_level = level;
    if (irq->edge && level)
        irq->pending_latch = true;
    vgic_update(irq);
}

/* --- list registers --------------------------------------------------------------------------- */

#define LR_CASE_R(n)                                                                               \
    case n:                                                                                        \
        return mrs(SYS_ICH_LR##n##_EL2)
#define LR_CASE_W(n)                                                                               \
    case n:                                                                                        \
        msr(SYS_ICH_LR##n##_EL2, val);                                                             \
        break

static u64 vgic_read_lr(int i)
{
    switch (i) {
        LR_CASE_R(0);
        LR_CASE_R(1);
        LR_CASE_R(2);
        LR_CASE_R(3);
        LR_CASE_R(4);
        LR_CASE_R(5);
        LR_CASE_R(6);
        LR_CASE_R(7);
        LR_CASE_R(8);
        LR_CASE_R(9);
        LR_CASE_R(10);
        LR_CASE_R(11);
        LR_CASE_R(12);
        LR_CASE_R(13);
        LR_CASE_R(14);
        LR_CASE_R(15);
        default:
            return 0;
    }
}

static void vgic_write_lr(int i, u64 val)
{
    switch (i) {
        LR_CASE_W(0);
        LR_CASE_W(1);
        LR_CASE_W(2);
        LR_CASE_W(3);
        LR_CASE_W(4);
        LR_CASE_W(5);
        LR_CASE_W(6);
        LR_CASE_W(7);
        LR_CASE_W(8);
        LR_CASE_W(9);
        LR_CASE_W(10);
        LR_CASE_W(11);
        LR_CASE_W(12);
        LR_CASE_W(13);
        LR_CASE_W(14);
        LR_CASE_W(15);
        default:
            break;
    }
}

static void vgic_clear_aprs(int nr)
{
    msr(SYS_ICH_AP0R0_EL2, 0);
    msr(SYS_ICH_AP1R0_EL2, 0);
    if (nr > 1) {
        msr(SYS_ICH_AP0R1_EL2, 0);
        msr(SYS_ICH_AP1R1_EL2, 0);
    }
    if (nr > 2) {
        msr(SYS_ICH_AP0R2_EL2, 0);
        msr(SYS_ICH_AP0R3_EL2, 0);
        msr(SYS_ICH_AP1R2_EL2, 0);
        msr(SYS_ICH_AP1R3_EL2, 0);
    }
}

static u64 vgic_make_lr(struct vgic_irq *irq)
{
    u64 lr = FIELD_PREP(ICH_LR_VINTID, irq->intid) | ICH_LR_GROUP1 |
             FIELD_PREP(ICH_LR_PRIORITY, irq->priority);
    bool allow_pend = irq->enabled && vgic.grp1;

    if (irq->active)
        lr |= ICH_LR_ACTIVE_BIT;

    if (!vgic_latched(irq)) {
        /* Level: learn the deactivation to resample; no pending+active (KVM) */
        lr |= ICH_LR_EOI;
        if (irq->active)
            allow_pend = false;
    } else if (irq->aic_line != VGIC_NO_LINE) {
        /* AIC-backed: the deactivation unmasks the AIC line */
        lr |= ICH_LR_EOI;
    }

    if (allow_pend && vgic_pending(irq)) {
        lr |= ICH_LR_PENDING_BIT;
        if (vgic_latched(irq))
            irq->pending_latch = false; /* the LR holds the pending state now */
    }

    return lr;
}

/* Fold one LR back into the IRQ's software state (KVM vgic_v3_fold_lr_state) */
static void vgic_fold_lr(struct vgic_irq *irq, u64 lr, u8 me)
{
    bool lr_pend = (lr & ICH_LR_PENDING_BIT) && !(irq->req & REQ_CLR_PEND);
    bool lr_act =
        ((lr & ICH_LR_ACTIVE_BIT) && !(irq->req & REQ_CLR_ACT)) || (irq->req & REQ_SET_ACT);

    irq->active = lr_act;
    if (lr_act)
        irq->act_vcpu = me;

    if (vgic_latched(irq)) {
        if (lr_pend)
            irq->pending_latch = true;
    } else if (!(lr & (ICH_LR_PENDING_BIT | ICH_LR_ACTIVE_BIT))) {
        /* soft pending state of a level IRQ ends with its deactivation; resample emulated lines */
        irq->pending_latch = false;
        if (irq->intid >= GIC_NR_PRIVATE && irq->intid == vgic.pl011_intid)
            irq->line_level = hv_win_pl011_irq_level();
    }

    irq->req = 0;
    irq->lr_vcpu = VGIC_NONE;
}

/*
 * Deactivations of active interrupts that did not fit in the LRs arrive as ICH_HCR_EL2.EOIcount
 * (LRENP maintenance). The guest completes interrupts in reverse acknowledge order, so each one
 * retires the highest-priority active interrupt that is not listed.
 */
static void vgic_retire_unlisted(struct vgic_vcpu *vc, u8 me)
{
    u32 count = FIELD_GET(ICH_HCR_EOICOUNT, mrs(SYS_ICH_HCR_EL2));

    while (count--) {
        struct vgic_irq *best = NULL;

        for (u32 w = 0; w < VGIC_MAP_WORDS; w++) {
            u64 bits = vc->cand[w];
            while (bits) {
                u32 intid = w * 64 + __builtin_ctzll(bits);
                struct vgic_irq *irq = vgic_get_irq(me, intid);

                bits &= bits - 1;
                if (!irq || !irq->active || irq->lr_vcpu != VGIC_NONE)
                    continue;
                if (!best || irq->priority < best->priority)
                    best = irq;
            }
        }

        if (!best)
            break;
        best->active = false;
        vgic_update(best);
    }
}

/* Rebuild this vCPU's list registers (lock held, running on the vCPU's own CPU) */
static void vgic_sync(struct vgic_vcpu *vc, u8 me)
{
    u32 prev = vc->lr_used;
    u64 old[VGIC_MAX_LRS];

    vc->nr_syncs++;

    if (vc->hcr & ICH_HCR_LRENPIE)
        vgic_retire_unlisted(vc, me);

    /* 1. fold */
    vc->lr_used = 0;
    for (u32 used = prev; used; used &= used - 1) {
        int i = __builtin_ctz(used);
        struct vgic_irq *irq = vgic_get_irq(me, vc->lr_intid[i]);

        old[i] = vgic_read_lr(i);
        if (!irq)
            continue;
        /* smp2f: the guest took it if what EL2 listed as pending is no longer pending */
        if ((vc->lr_val[i] & ICH_LR_PENDING_BIT) && !(old[i] & ICH_LR_PENDING_BIT)) {
            if (me < VGIC_STAT_N) { /* smp2g */
                struct vgic_stat *st = &vgic_stat[me];
                st->nr_taken++;
                st->t_last_taken = mrs(CNTPCT_EL0);
                st->last_taken_intid = irq->intid;
                if (irq->intid < GIC_NR_SGI)
                    st->taken[st->last_src[irq->intid]][irq->intid]++;
            }
            if (irq->intid < GIC_NR_SGI)
                vc->nr_sgi_ack++;
            else if (irq->intid == GIC_PPI_CNTV || irq->intid == GIC_PPI_CNTP)
                vc->nr_timer_ack++;
        }
        vgic_fold_lr(irq, old[i], me);
        vgic_update(irq);
    }

    /* 2. pick the best candidates: active first, then priority, then INTID */
    struct vgic_irq *best[VGIC_MAX_LRS];
    u32 key[VGIC_MAX_LRS];
    u32 n = 0, total = 0, unlisted_active = 0;

    for (u32 w = 0; w < VGIC_MAP_WORDS; w++) {
        u64 bits = vc->cand[w];
        while (bits) {
            u32 intid = w * 64 + __builtin_ctzll(bits);
            struct vgic_irq *irq = vgic_get_irq(me, intid);

            bits &= bits - 1;
            if (!irq || irq->lr_vcpu != VGIC_NONE)
                continue; /* still in another vCPU's LR; that vCPU hands it over */

            u32 k = (irq->active ? 0 : BIT(24)) | ((u32)irq->priority << 16) | intid;
            u32 pos = n;

            total++;
            while (pos > 0 && key[pos - 1] > k)
                pos--;
            if (pos >= vc->num_lrs) {
                unlisted_active += irq->active;
                continue;
            }
            if (n == vc->num_lrs) {
                unlisted_active += best[n - 1]->active;
                n--;
            }
            for (u32 j = n; j > pos; j--) {
                best[j] = best[j - 1];
                key[j] = key[j - 1];
            }
            best[pos] = irq;
            key[pos] = k;
            n++;
        }
    }

    /* 3. write the list registers */
    u32 used = 0;
    for (u32 i = 0; i < n; i++) {
        struct vgic_irq *irq = best[i];
        u64 lr = vgic_make_lr(irq);

        irq->lr_vcpu = me;
        irq->lr_idx = i;
        vc->lr_intid[i] = irq->intid;
        vc->lr_val[i] = lr;
        used |= BIT(i);
        if (!(prev & BIT(i)) || old[i] != lr)
            vgic_write_lr(i, lr);
        vgic_aic_sync(irq);
    }
    for (u32 i = n; i < vc->num_lrs; i++)
        if (prev & BIT(i))
            vgic_write_lr(i, 0);
    vc->lr_used = used;

    /* 4. maintenance requests */
    u64 hcr = ICH_HCR_EN;
    if (total > n) {
        hcr |= ICH_HCR_UIE;
        vgic.nr_overflow++;
    }
    if (unlisted_active)
        hcr |= ICH_HCR_LRENPIE;
    if (hcr != vc->hcr || (vc->hcr & ICH_HCR_LRENPIE)) {
        msr(SYS_ICH_HCR_EL2, hcr); /* also zeroes EOIcount */
        vc->hcr = hcr;
    }

    __atomic_store_n(&vc->dirty, 0, __ATOMIC_RELAXED);
}

/* --- register state machine ------------------------------------------------------------------- */

/* Pending/active as the guest would see them, including what sits in a list register */
static void vgic_live(struct vgic_irq *irq, bool *pend, bool *act)
{
    *pend = vgic_pending(irq);
    *act = irq->active;

    if (irq->lr_vcpu == VGIC_NONE)
        return;

    u64 lr = vgic.vcpu[irq->lr_vcpu].lr_val[irq->lr_idx];
    if (irq->lr_vcpu == vgic_cur_vcpu())
        lr = vgic_read_lr(irq->lr_idx);

    *pend |= (lr & ICH_LR_PENDING_BIT) && !(irq->req & REQ_CLR_PEND);
    *act = ((lr & ICH_LR_ACTIVE_BIT) && !(irq->req & REQ_CLR_ACT)) || (irq->req & REQ_SET_ACT);
}

static u32 vgic_read_bits(u8 rv, u32 first, enum vgic_field f)
{
    u32 val = 0;

    for (u32 i = 0; i < 32; i++) {
        struct vgic_irq *irq = vgic_get_irq(rv, first + i);
        bool pend, act, bit;

        if (!irq)
            continue;
        vgic_live(irq, &pend, &act);
        bit = f == F_EN ? irq->enabled : f == F_PEND ? pend : act;
        if (bit)
            val |= BIT(i);
    }

    return val;
}

static void vgic_write_bits(u8 rv, u32 first, u32 val, enum vgic_op op)
{
    for (; val; val &= val - 1) {
        struct vgic_irq *irq = vgic_get_irq(rv, first + __builtin_ctz(val));
        bool listed;

        if (!irq)
            continue;
        listed = irq->lr_vcpu != VGIC_NONE;

        switch (op) {
            case OP_SET_EN:
                irq->enabled = true;
                break;
            case OP_CLR_EN:
                irq->enabled = false;
                break;
            case OP_SET_PEND:
                irq->pending_latch = true;
                break;
            case OP_CLR_PEND:
                irq->pending_latch = false;
                if (listed)
                    irq->req |= REQ_CLR_PEND;
                break;
            case OP_SET_ACT:
                irq->active = true;
                irq->act_vcpu = listed ? irq->lr_vcpu : irq->target;
                if (listed)
                    irq->req = (irq->req & ~REQ_CLR_ACT) | REQ_SET_ACT;
                break;
            case OP_CLR_ACT:
                irq->active = false;
                if (listed)
                    irq->req = (irq->req & ~REQ_SET_ACT) | REQ_CLR_ACT;
                break;
        }

        vgic_update(irq);
    }
}

static void vgic_write_priority(u8 rv, u32 intid, u8 val)
{
    struct vgic_irq *irq = vgic_get_irq(rv, intid);

    if (!irq)
        return;
    irq->priority = val & vgic.pri_mask;
    vgic_update(irq);
}

static u32 vgic_read_priorities(u8 rv, u32 first)
{
    u32 val = 0;

    for (u32 i = 0; i < 4; i++) {
        struct vgic_irq *irq = vgic_get_irq(rv, first + i);
        if (irq)
            val |= (u32)irq->priority << (8 * i);
    }
    return val;
}

/* GICD_ICFGR<n>: two bits per INTID, bit 1 = edge-triggered */
static u32 vgic_read_cfg(u32 first)
{
    u32 val = 0;

    for (u32 i = 0; i < 16; i++) {
        struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, first + i);
        if (irq && irq->edge)
            val |= BIT(2 * i + 1);
    }
    return val;
}

static void vgic_write_cfg(u32 first, u32 val)
{
    for (u32 i = 0; i < 16; i++) {
        struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, first + i);
        bool edge = val & BIT(2 * i + 1);

        if (!irq || irq->edge == edge)
            continue;
        irq->edge = edge;
        vgic_update(irq);
    }
}

/* Bits of GICD_IGROUPR<n> etc. that name implemented INTIDs */
static u32 vgic_valid_bits(u32 n)
{
    u32 first = n * 32;

    if (first >= VGIC_NR_IRQS)
        return 0;
    if (VGIC_NR_IRQS - first >= 32)
        return 0xffffffff;
    return (u32)GENMASK(VGIC_NR_IRQS - first - 1, 0);
}

/* Indexed by enum vgic_op; enum vgic_field is the index / 2 */
static const u32 vgic_bitregs[] = {GICD_ISENABLER, GICD_ICENABLER, GICD_ISPENDR,
                                   GICD_ICPENDR,   GICD_ISACTIVER, GICD_ICACTIVER};

static bool in_range(u32 off, u32 base, u32 size)
{
    return off >= base && off < base + size;
}

/* Registers shared by GICD (INTIDs 32+, rv = VGIC_NONE) and the GICR SGI_base frame (INTIDs 0..31)
 */
static bool vgic_irq_read32(u8 rv, u32 off, u32 *val)
{
    u32 limit = rv == VGIC_NONE ? GICD_BITREG_SIZE : 4;
    u32 n;

    if (in_range(off, GICD_IGROUPR, limit)) {
        n = (off - GICD_IGROUPR) / 4;
        *val = rv != VGIC_NONE ? 0xffffffff : n ? vgic_valid_bits(n) : 0;
        return true;
    }

    for (u32 i = 0; i < ARRAY_SIZE(vgic_bitregs); i++) {
        if (!in_range(off, vgic_bitregs[i], limit))
            continue;
        n = (off - vgic_bitregs[i]) / 4;
        /* ARE=1: GICD word 0 (SGIs/PPIs) is RAZ/WI, those live in the redistributors */
        *val =
            (rv == VGIC_NONE && n == 0) ? 0 : vgic_read_bits(rv, n * 32, (enum vgic_field)(i / 2));
        return true;
    }

    if (in_range(off, GICD_IPRIORITYR, rv == VGIC_NONE ? GICD_IPRIO_SIZE : GIC_NR_PRIVATE)) {
        u32 first = off - GICD_IPRIORITYR;
        *val = (rv == VGIC_NONE && first < GIC_NR_PRIVATE) ? 0 : vgic_read_priorities(rv, first);
        return true;
    }

    if (in_range(off, GICD_ICFGR, rv == VGIC_NONE ? GICD_ICFGR_SIZE : 8)) {
        n = (off - GICD_ICFGR) / 4;
        if (rv != VGIC_NONE)
            *val = n == 0 ? 0xaaaaaaaa : 0; /* SGIs edge, PPIs level; read-only (KVM) */
        else
            *val = n < 2 ? 0 : vgic_read_cfg(n * 16);
        return true;
    }

    return false;
}

static bool vgic_irq_write32(u8 rv, u32 off, u32 val)
{
    u32 limit = rv == VGIC_NONE ? GICD_BITREG_SIZE : 4;
    u32 n;

    for (u32 i = 0; i < ARRAY_SIZE(vgic_bitregs); i++) {
        if (!in_range(off, vgic_bitregs[i], limit))
            continue;
        n = (off - vgic_bitregs[i]) / 4;
        if (rv != VGIC_NONE || n != 0)
            vgic_write_bits(rv, n * 32, val & vgic_valid_bits(n), (enum vgic_op)i);
        return true;
    }

    if (in_range(off, GICD_IPRIORITYR, rv == VGIC_NONE ? GICD_IPRIO_SIZE : GIC_NR_PRIVATE)) {
        u32 first = off - GICD_IPRIORITYR;
        if (rv != VGIC_NONE || first >= GIC_NR_PRIVATE)
            for (u32 i = 0; i < 4; i++)
                vgic_write_priority(rv, first + i, val >> (8 * i));
        return true;
    }

    if (in_range(off, GICD_ICFGR, GICD_ICFGR_SIZE)) {
        n = (off - GICD_ICFGR) / 4;
        if (rv == VGIC_NONE && n >= 2)
            vgic_write_cfg(n * 16, val);
        return true;
    }

    return false; /* IGROUPR (RAO/WI) and everything else: WI */
}

static u32 vgic_dist_read32(u32 off)
{
    u32 val = 0;

    switch (off) {
        case GICD_CTLR:
            return (vgic.grp1 ? GICD_CTLR_ENABLEGRP1 : 0) | GICD_CTLR_ARE | GICD_CTLR_DS;
        case GICD_TYPER:
            return VGIC_GICD_TYPER;
        case GICD_IIDR:
            return VGIC_IIDR;
        case GICD_PIDR2:
            return VGIC_PIDR2;
    }

    if (vgic_irq_read32(VGIC_NONE, off, &val))
        return val;

    if (in_range(off, GICD_IROUTER, GICD_IROUTER_END - GICD_IROUTER) && !(off & 4)) {
        struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, (off - GICD_IROUTER) / 8);
        if (irq && irq->intid >= GIC_NR_PRIVATE)
            return irq->mpidr;
    }

    return 0; /* STATUSR, ITARGETSR, IGRPMODR, NSACR, SGIR, *SGIR, IROUTER Aff3, IDs: RAZ */
}

static void vgic_dist_write32(u32 off, u32 val)
{
    if (off == GICD_CTLR) {
        bool grp1 = val & GICD_CTLR_ENABLEGRP1;
        if (grp1 != vgic.grp1) {
            vgic.grp1 = grp1;
            vgic_update_all();
        }
        return;
    }

    if (vgic_irq_write32(VGIC_NONE, off, val))
        return;

    if (in_range(off, GICD_IROUTER, GICD_IROUTER_END - GICD_IROUTER) && !(off & 4)) {
        struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, (off - GICD_IROUTER) / 8);
        if (irq && irq->intid >= GIC_NR_PRIVATE) {
            irq->mpidr = val & GICD_IROUTER_AFF;
            irq->target = vgic_find_vcpu(irq->mpidr);
            vgic_update(irq);
        }
    }
}

static u64 vgic_gicr_typer(u8 v)
{
    return ((u64)vgic.vcpu[v].aff << 32) | FIELD_PREP(GICR_TYPER_PROCNUM, v) |
           (v == vgic.nr_vcpus - 1 ? GICR_TYPER_LAST : 0);
}

static u32 vgic_redist_read32(u8 v, u32 off)
{
    u32 val = 0;

    if (off >= GICR_SGI_BASE) {
        vgic_irq_read32(v, off - GICR_SGI_BASE, &val);
        return val;
    }

    switch (off) {
        case GICR_IIDR:
            return VGIC_IIDR;
        case GICR_TYPER:
            return vgic_gicr_typer(v);
        case GICR_TYPER + 4:
            return vgic_gicr_typer(v) >> 32;
        case GICR_WAKER:
            return vgic.vcpu[v].sleep ? GICR_WAKER_PROCESSORSLEEP | GICR_WAKER_CHILDRENASLEEP : 0;
        case GICR_PIDR2:
            return VGIC_PIDR2;
        default:
            return 0; /* CTLR (no LPIs, RWP=0), STATUSR, PROPBASER, PENDBASER, SYNCR (idle) */
    }
}

static void vgic_redist_write32(u8 v, u32 off, u32 val)
{
    if (off >= GICR_SGI_BASE)
        vgic_irq_write32(v, off - GICR_SGI_BASE, val);
    else if (off == GICR_WAKER)
        vgic.vcpu[v].sleep = val & GICR_WAKER_PROCESSORSLEEP;
}

/* rv: VGIC_NONE for the distributor, else the redistributor's vCPU */
static bool vgic_mmio(u8 rv, u32 off, u64 *val, bool write, int width)
{
    u32 bytes = 1U << width;

    spin_lock(&vgic_lock);

    if (bytes == 1) {
        /* Byte access exists only for the priority registers; everything else RAZ/WI */
        u32 base = rv == VGIC_NONE ? GICD_IPRIORITYR : GICR_SGI_BASE + GICD_IPRIORITYR;
        u32 size = rv == VGIC_NONE ? GICD_IPRIO_SIZE : GIC_NR_PRIVATE;
        bool prio = in_range(off, base, size) && (rv != VGIC_NONE || off - base >= GIC_NR_PRIVATE);

        if (write && prio)
            vgic_write_priority(rv, off - base, *val);
        else if (!write)
            *val = prio ? vgic_read_priorities(rv, off - base) & 0xff : 0;
    } else if (bytes == 2) {
        if (!write)
            *val = 0;
    } else {
        /* 32-bit registers; a 64-bit access covers two (IROUTER, GICR_TYPER, ...) */
        for (u32 i = 0; i < bytes / 4; i++) {
            u32 o = off + 4 * i;
            u64 *word = &val[i / 2];
            u32 shift = 32 * (i % 2);

            if (write) {
                u32 w = *word >> shift;
                if (rv == VGIC_NONE)
                    vgic_dist_write32(o, w);
                else
                    vgic_redist_write32(rv, o, w);
            } else {
                u64 r = rv == VGIC_NONE ? vgic_dist_read32(o) : vgic_redist_read32(rv, o);
                if (!shift)
                    *word = 0;
                *word |= r << shift;
            }
        }
    }

    vgic_unlock();
    return true;
}

static bool vgic_gicd_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    return vgic_mmio(VGIC_NONE, addr - vgic.gicd_base, val, write, width);
}

static bool vgic_gicr_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 off = addr - vgic.gicr_base;
    u64 v = off / GICR_STRIDE;

    UNUSED(ctx);
    if (v >= vgic.nr_vcpus) {
        if (!write)
            *val = 0;
        return true;
    }
    return vgic_mmio(v, off % GICR_STRIDE, val, write, width);
}

/* --- setup ------------------------------------------------------------------------------------ */

static u32 vgic_aff(u64 mpidr)
{
    return ((mpidr >> 8) & 0xff000000) | (mpidr & 0xffffff);
}

/* The GIC frames are EL2's alone: refuse when anything (RAM, MMIO, another hook) is mapped there */
static bool vgic_ipa_busy(u64 base, u64 size)
{
    for (u64 off = 0; off < size; off += 0x4000)
        if (hv_pt_walk(base + off))
            return true;
    return false;
}

int hv_vgic3_init(u64 gicd_base, u64 gicr_base, u32 nr_vcpus)
{
    u64 gicr_size = (u64)nr_vcpus * GICR_STRIDE;

    vgic.ready = false;

    if (nr_vcpus < 1 || nr_vcpus > MAX_CPUS || (gicd_base & 0xffff) || (gicr_base & 0xffff))
        return -1;
    if (gicd_base < gicr_base + gicr_size && gicr_base < gicd_base + GICD_SIZE) {
        printf("HV/win vGIC3: GICD and GICR frames overlap\n");
        return -1;
    }
    if (vgic_ipa_busy(gicd_base, GICD_SIZE) || vgic_ipa_busy(gicr_base, gicr_size)) {
        printf("HV/win vGIC3: GICD/GICR IPAs are already mapped (RAM or MMIO?)\n");
        return -1;
    }

    memset(&vgic, 0, sizeof(vgic));
    memset(vgic_stat, 0, sizeof(vgic_stat));
    memset(vgic.cpu_vcpu, VGIC_NONE, sizeof(vgic.cpu_vcpu));
    vgic.gicd_base = gicd_base;
    vgic.gicr_base = gicr_base;
    vgic.nr_vcpus = nr_vcpus;

    u32 pri_bits = FIELD_GET(ICH_VTR_PRIBITS, mrs(SYS_ICH_VTR_EL2)) + 1;
    vgic.pri_mask = (0xff << (8 - pri_bits)) & 0xff;

    for (u32 v = 0; v < nr_vcpus; v++) {
        struct vgic_vcpu *vc = &vgic.vcpu[v];
        int cpu = hv_win.vcpu_cpu[v];

        if (cpu < 0 || cpu >= MAX_CPUS || vgic.cpu_vcpu[cpu] != VGIC_NONE) {
            printf("HV/win vGIC3: bad vCPU table entry %u (CPU %d)\n", v, cpu);
            return -1;
        }
        vgic.cpu_vcpu[cpu] = v;
        vc->cpu = cpu;
        vc->aff = vgic_aff(smp_get_mpidr(cpu));
        vc->sleep = true;

        for (u32 i = 0; i < GIC_NR_PRIVATE; i++) {
            struct vgic_irq *irq = &vc->priv[i];
            irq->intid = i;
            irq->aic_line = VGIC_NO_LINE;
            irq->target = v;
            irq->owner = irq->lr_vcpu = irq->act_vcpu = VGIC_NONE;
            irq->edge = i < GIC_NR_SGI;
            irq->enabled = i < GIC_NR_SGI; /* SGIs reset enabled, as in KVM */
        }
    }

    for (u32 i = 0; i < VGIC_NR_SPIS; i++) {
        struct vgic_irq *irq = &vgic.spi[i];
        irq->intid = GIC_NR_PRIVATE + i;
        irq->aic_line = VGIC_NO_LINE;
        /*
         * smp2 (SMP-PLAN #12): affinity 0 is no vCPU on the T6050 (the boot CPU is 0.4.0), so an
         * SPI the guest never routed would have no target. With more than one vCPU route it to
         * vCPU 0 (the boot CPU) and read that affinity back from GICD_IROUTER, as KVM's vgic does
         * (target vCPU 0 by default; reference only). One vCPU keeps the hardware-proven parent
         * behaviour unchanged.
         */
        if (nr_vcpus > 1) {
            irq->target = 0;
            irq->mpidr = vgic.vcpu[0].aff & GICD_IROUTER_AFF;
        } else {
            irq->target = vgic_find_vcpu(0);
        }
        irq->owner = irq->lr_vcpu = irq->act_vcpu = VGIC_NONE;
    }

    if (hv_map_hook(gicd_base, vgic_gicd_hook, GICD_SIZE) < 0 ||
        hv_map_hook(gicr_base, vgic_gicr_hook, gicr_size) < 0) {
        printf("HV/win vGIC3: failed to hook the GIC frames\n");
        return -1;
    }
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");

    vgic.ready = true;
    printf("HV/win vGIC3: GICD 0x%lx, %u GICR frames at 0x%lx, SPIs 32..%u, %u priority bits\n",
           gicd_base, nr_vcpus, gicr_base, GIC_MAX_SPI, pri_bits);
    return 0;
}

void hv_vgic3_cpu_init(void)
{
    u8 me = vgic_cur_vcpu();

    if (!vgic.ready || me == VGIC_NONE)
        return;

    struct vgic_vcpu *vc = &vgic.vcpu[me];
    u64 vtr = mrs(SYS_ICH_VTR_EL2);
    u64 sre = mrs(SYS_ICC_SRE_EL2);
    u64 need = ICC_SRE_EL2_SRE | ICC_SRE_EL2_ENABLE;

    /* 0xf at hand-off on the T6050; only write it if something is missing */
    if ((sre & need) != need) {
        msr(SYS_ICC_SRE_EL2, sre | need);
        sysop("isb");
    }

    spin_lock(&vgic_lock);

    /* CPU_OFF then CPU_ON: take back whatever the previous incarnation left listed */
    for (u32 used = vc->ready ? vc->lr_used : 0; used; used &= used - 1) {
        int i = __builtin_ctz(used);
        struct vgic_irq *irq = vgic_get_irq(me, vc->lr_intid[i]);
        if (irq) {
            vgic_fold_lr(irq, vgic_read_lr(i), me);
            vgic_update(irq);
        }
    }

    vc->num_lrs = FIELD_GET(ICH_VTR_LISTREGS, vtr) + 1;
    if (vc->num_lrs > VGIC_MAX_LRS)
        vc->num_lrs = VGIC_MAX_LRS;
    u32 pre_bits = FIELD_GET(ICH_VTR_PREBITS, vtr) + 1; /* 5..7: 1, 2 or 4 AP registers */
    vc->nr_apr = pre_bits > 5 ? 1 << (pre_bits - 5) : 1;

    for (u32 i = 0; i < vc->num_lrs; i++)
        vgic_write_lr(i, 0);
    vgic_clear_aprs(vc->nr_apr);
    msr(SYS_ICH_VMCR_EL2, 0); /* reset view; the guest programs PMR/BPR/IGRPEN1 itself (KVM) */
    msr(SYS_ICH_HCR_EL2, ICH_HCR_EN);
    sysop("isb");

    vc->hcr = ICH_HCR_EN;
    vc->lr_used = 0;
    vc->ready = true;
    __atomic_store_n(&vc->dirty, 1, __ATOMIC_RELAXED);

    vgic_unlock();

    printf("HV/win vGIC3: vCPU %u on CPU %d (affinity 0x%x): %u LRs, interface enabled\n", me,
           vc->cpu, vc->aff, vc->num_lrs);
}

/* r3: AIC lines owned by a host-side driver (native ANS); never routed to the guest. */
static u32 vgic_denied_lines[VGIC_AIC_LINES / 32];

int hv_vgic3_deny_aic_line(u32 aic_line)
{
    int ret = -1;

    if (!vgic.ready || aic_line >= VGIC_AIC_LINES)
        return -1;
    spin_lock(&vgic_lock);
    bool mapped = false;
    for (u32 i = 0; i < vgic.nr_map && !mapped; i++)
        mapped = vgic.map[i].line == aic_line;
    if (!mapped) {
        vgic_denied_lines[aic_line / 32] |= BIT(aic_line % 32);
        ret = 0;
    }
    vgic_unlock();
    return ret;
}

int hv_vgic3_map_irq(u32 aic_line, u32 intid)
{
    int ret = -1;

    if (!vgic.ready || intid < GIC_NR_PRIVATE || intid > GIC_MAX_SPI || aic_line >= VGIC_AIC_LINES ||
        (vgic_denied_lines[aic_line / 32] & BIT(aic_line % 32)))
        return -1;

    spin_lock(&vgic_lock);

    struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, intid);
    bool dup = irq->aic_line != VGIC_NO_LINE;
    for (u32 i = 0; i < vgic.nr_map && !dup; i++)
        dup = vgic.map[i].line == aic_line;

    if (!dup && vgic.nr_map < VGIC_NR_SPIS) {
        vgic.map[vgic.nr_map].line = aic_line;
        vgic.map[vgic.nr_map].intid = intid;
        vgic.nr_map++;
        irq->aic_line = aic_line;
        irq->aic_masked = false;
        vgic_aic_sync(irq); /* masked until the guest enables the SPI */
        ret = 0;
    }

    vgic_unlock();
    return ret;
}

/* --- run time --------------------------------------------------------------------------------- */

bool hv_vgic3_handle_sgi1r(u64 val)
{
    if (!vgic.ready)
        return false;

    u8 me = vgic_cur_vcpu();
    u32 sgi = FIELD_GET(ICC_SGI1R_INTID, val);

    spin_lock(&vgic_lock);
    vgic.nr_sgi++;

    if (val & ICC_SGI1R_IRM) {
        /* all PEs but the sender */
        for (u32 v = 0; v < vgic.nr_vcpus; v++) {
            if (v == me)
                continue;
            vgic.vcpu[v].priv[sgi].pending_latch = true;
            vgic.vcpu[v].nr_sgi_rx++;
            vgic.vcpu[v].nr_sgi_from[me < 3 ? me : 3]++;
            vgic.vcpu[v].nr_sgi_id[sgi & 15]++;
            vgic_stat_sgi_rx(v, me, sgi);
            vgic_update(&vgic.vcpu[v].priv[sgi]);
        }
    } else {
        /* Aff3.Aff2.Aff1 plus Aff0 = RangeSelector * 16 + TargetList bit */
        u32 aff = (FIELD_GET(ICC_SGI1R_AFF3, val) << 24) | (FIELD_GET(ICC_SGI1R_AFF2, val) << 16) |
                  (FIELD_GET(ICC_SGI1R_AFF1, val) << 8);
        u32 rs = FIELD_GET(ICC_SGI1R_RS, val) * 16;

        for (u32 tl = FIELD_GET(ICC_SGI1R_TARGETLIST, val); tl; tl &= tl - 1) {
            u8 v = vgic_find_vcpu(aff | (rs + __builtin_ctz(tl)));
            if (v == VGIC_NONE)
                continue;
            vgic.vcpu[v].priv[sgi].pending_latch = true;
            vgic.vcpu[v].nr_sgi_rx++;
            vgic.vcpu[v].nr_sgi_from[me < 3 ? me : 3]++;
            vgic.vcpu[v].nr_sgi_id[sgi & 15]++;
            vgic_stat_sgi_rx(v, me, sgi);
            vgic_update(&vgic.vcpu[v].priv[sgi]);
        }
    }

    vgic_unlock();
    return true;
}

void hv_vgic3_handle_aic_irq(u32 aic_line)
{
    struct vgic_irq *irq = NULL;
    bool first_unrouted = false;

    if (!vgic.ready)
        return;

    spin_lock(&vgic_lock);
    vgic.nr_aic++;

    for (u32 i = 0; i < vgic.nr_map; i++) {
        if (vgic.map[i].line == aic_line) {
            irq = vgic_get_irq(VGIC_NONE, vgic.map[i].intid);
            break;
        }
    }

    if (irq) {
        irq->aic_masked = true; /* the event read masked it */
        irq->pending_latch = true;
        vgic_update(irq);
    } else if (aic_line < VGIC_AIC_LINES &&
               !(vgic.unrouted_seen[aic_line / 32] & BIT(aic_line % 32))) {
        vgic.unrouted_seen[aic_line / 32] |= BIT(aic_line % 32);
        first_unrouted = true;
    }

    vgic_unlock();

    if (first_unrouted)
        printf("HV/win vGIC3: AIC line %u is not in m5w,irq-map; left masked\n", aic_line);
}

void hv_vgic3_set_ppi_level(int cpu, u32 intid, bool level)
{
    if (!vgic.ready || cpu < 0 || cpu >= MAX_CPUS || intid >= GIC_NR_PRIVATE)
        return;

    u8 v = vgic.cpu_vcpu[cpu];
    if (v == VGIC_NONE)
        return;

    struct vgic_irq *irq = &vgic.vcpu[v].priv[intid];
    if (irq->line_level == level) /* timers: only this CPU drives its own PPIs */
        return;
    if (level && (intid == GIC_PPI_CNTP || intid == GIC_PPI_CNTV)) {
        vgic.vcpu[v].nr_timer_edges++;
        vgic.vcpu[v].t_last_edge = mrs(CNTPCT_EL0);
    }

    spin_lock(&vgic_lock);
    vgic_set_level(irq, level);
    vgic_unlock();
}

void hv_vgic3_set_spi_level(u32 intid, bool level)
{
    if (!vgic.ready || intid < GIC_NR_PRIVATE || intid > GIC_MAX_SPI)
        return;

    spin_lock(&vgic_lock);
    vgic_set_level(vgic_get_irq(VGIC_NONE, intid), level);
    vgic_unlock();
}

/*
 * The emulated PL011 (hv_win_pl011.c) reports rising edges of its interrupt here, overriding its
 * weak no-op; the line is resampled with hv_win_pl011_irq_level() when the guest deactivates it.
 */
void hv_win_pl011_raise(u32 gsiv)
{
    if (!vgic.ready || gsiv < GIC_NR_PRIVATE || gsiv > GIC_MAX_SPI)
        return;

    spin_lock(&vgic_lock);
    vgic.pl011_intid = gsiv;
    vgic_set_level(vgic_get_irq(VGIC_NONE, gsiv), true);
    vgic_unlock();
}

void hv_vgic3_inject_spi(u32 intid)
{
    if (!vgic.ready || intid < GIC_NR_PRIVATE || intid > GIC_MAX_SPI)
        return;

    spin_lock(&vgic_lock);
    struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, intid);
    irq->pending_latch = true;
    vgic_update(irq);
    vgic_unlock();
}

/* Guest entry on this CPU (hv_update_fiq): bring the list registers up to date */
void hv_vgic3_flush(void)
{
    u8 me = vgic_cur_vcpu();

    if (!vgic.ready || me == VGIC_NONE)
        return;

    struct vgic_vcpu *vc = &vgic.vcpu[me];
    if (!vc->ready || (!vc->lr_used && !__atomic_load_n(&vc->dirty, __ATOMIC_ACQUIRE)))
        return;

    spin_lock(&vgic_lock);
    vgic_sync(vc, me);
    vgic_unlock();
}

/*
 * smp2e (review F1) / smp2f: should the emulated WFI on this vCPU complete now? EL2 emulates the guest's
 * WFI (HCR_EL2.TWI) and a pending list register does not wake a WFI executed at EL2, so the WFI wait
 * asks here before each physical WFI, after hv_update_fiq() has folded timer state and rebuilt a dirty
 * vCPU. Returns HV_VGIC3_WFI_* flags:
 *   DIRTY  another CPU left this vCPU dirty (its kick IPI is pending too);
 *   WAKE   a list register wakes under KVM's rule wfi_lr_wakes() (pending, priority above VPMR) -
 *          smp2f wakes on DIRTY or WAKE;
 *   STRICT a list register signals under the full GICv3 rule wfi_lr_signals() (also group enable and
 *          running priority) - smp2e's rule, kept to count where the two differ.
 * Lock-free: a vCPU's list registers, lr_used and APRs are only touched by its own CPU; dirty is read
 * atomically.
 */
int hv_vgic3_wfi_pending(void)
{
    u8 me = vgic_cur_vcpu();

    if (!vgic.ready || me == VGIC_NONE)
        return 0;

    struct vgic_vcpu *vc = &vgic.vcpu[me];
    if (!vc->ready)
        return 0;
    if (__atomic_load_n(&vc->dirty, __ATOMIC_ACQUIRE))
        return HV_VGIC3_WFI_DIRTY;
    if (!vc->lr_used)
        return 0;

    u32 apr[4] = {0};
    apr[0] = mrs(SYS_ICH_AP0R0_EL2) | mrs(SYS_ICH_AP1R0_EL2);
    if (vc->nr_apr > 1)
        apr[1] = mrs(SYS_ICH_AP0R1_EL2) | mrs(SYS_ICH_AP1R1_EL2);
    if (vc->nr_apr > 2) {
        apr[2] = mrs(SYS_ICH_AP0R2_EL2) | mrs(SYS_ICH_AP1R2_EL2);
        apr[3] = mrs(SYS_ICH_AP0R3_EL2) | mrs(SYS_ICH_AP1R3_EL2);
    }
    u32 pre_bits = FIELD_GET(ICH_VTR_PREBITS, mrs(SYS_ICH_VTR_EL2)) + 1;
    u32 rpr = wfi_running_priority(apr, vc->nr_apr > 2 ? 4 : vc->nr_apr, pre_bits);
    u64 vmcr = mrs(SYS_ICH_VMCR_EL2);
    int ret = 0;

    for (u32 used = vc->lr_used; used; used &= used - 1) {
        u64 lr = vgic_read_lr(__builtin_ctz(used));
        if (wfi_lr_wakes(lr, vmcr))
            ret |= HV_VGIC3_WFI_WAKE;
        if (wfi_lr_signals(lr, vmcr, rpr))
            ret |= HV_VGIC3_WFI_STRICT;
    }
    return ret;
}

/*
 * smp2f: this CPU's virtual-interface registers into / out of a wfi_vctx (hv_exc.c adds the timer and
 * EL1 words). Only the vCPU's own CPU calls these, before and after a physical WFI.
 */
void hv_vgic3_vctx_read(struct wfi_vctx *v)
{
    u8 me = vgic_cur_vcpu();

    v->nr_lr = v->nr_apr = 0;
    v->ich_hcr = v->ich_vmcr = 0;
    if (!vgic.ready || me == VGIC_NONE || !vgic.vcpu[me].ready)
        return;

    struct vgic_vcpu *vc = &vgic.vcpu[me];
    v->ich_hcr = mrs(SYS_ICH_HCR_EL2);
    v->ich_vmcr = mrs(SYS_ICH_VMCR_EL2);
    v->nr_apr = vc->nr_apr > 2 ? 4 : vc->nr_apr;
    v->apr0[0] = mrs(SYS_ICH_AP0R0_EL2);
    v->apr1[0] = mrs(SYS_ICH_AP1R0_EL2);
    if (v->nr_apr > 1) {
        v->apr0[1] = mrs(SYS_ICH_AP0R1_EL2);
        v->apr1[1] = mrs(SYS_ICH_AP1R1_EL2);
    }
    if (v->nr_apr > 2) {
        v->apr0[2] = mrs(SYS_ICH_AP0R2_EL2);
        v->apr0[3] = mrs(SYS_ICH_AP0R3_EL2);
        v->apr1[2] = mrs(SYS_ICH_AP1R2_EL2);
        v->apr1[3] = mrs(SYS_ICH_AP1R3_EL2);
    }
    v->nr_lr = vc->num_lrs < WFI_VCTX_LRS ? vc->num_lrs : WFI_VCTX_LRS;
    for (u32 i = 0; i < v->nr_lr; i++)
        v->lr[i] = vgic_read_lr(i);
}

void hv_vgic3_vctx_write(const struct wfi_vctx *v, u32 classes)
{
    if (classes & WFI_VCTX_LR)
        for (u32 i = 0; i < v->nr_lr; i++)
            vgic_write_lr(i, v->lr[i]);
    if (classes & WFI_VCTX_APR) {
        msr(SYS_ICH_AP0R0_EL2, v->apr0[0]);
        msr(SYS_ICH_AP1R0_EL2, v->apr1[0]);
        if (v->nr_apr > 1) {
            msr(SYS_ICH_AP0R1_EL2, v->apr0[1]);
            msr(SYS_ICH_AP1R1_EL2, v->apr1[1]);
        }
        if (v->nr_apr > 2) {
            msr(SYS_ICH_AP0R2_EL2, v->apr0[2]);
            msr(SYS_ICH_AP0R3_EL2, v->apr0[3]);
            msr(SYS_ICH_AP1R2_EL2, v->apr1[2]);
            msr(SYS_ICH_AP1R3_EL2, v->apr1[3]);
        }
    }
    if (classes & WFI_VCTX_VMCR)
        msr(SYS_ICH_VMCR_EL2, v->ich_vmcr);
    if (classes & WFI_VCTX_HCR)
        msr(SYS_ICH_HCR_EL2, v->ich_hcr); /* last: En on with the rest already in place */
    sysop("isb");
}

void hv_vgic3_maintenance(void)
{
    u8 me = vgic_cur_vcpu();

    if (!vgic.ready || me == VGIC_NONE || !vgic.vcpu[me].ready)
        return;
    if (!(mrs(SYS_ICH_HCR_EL2) & ICH_HCR_EN) || !mrs(SYS_ICH_MISR_EL2))
        return;

    struct vgic_vcpu *vc = &vgic.vcpu[me];

    spin_lock(&vgic_lock);
    vgic.nr_maint++;
    vgic_sync(vc, me);
    vgic_unlock();

    u64 misr = mrs(SYS_ICH_MISR_EL2);
    if (misr) {
        /* Never return to the guest with a maintenance FIQ that cannot be cleared (Linux does the
         * same) */
        printf("HV/win vGIC3: maintenance IRQ stuck on vCPU %u (MISR 0x%lx HCR 0x%lx), disabling "
               "its virtual CPU interface\n",
               me, misr, mrs(SYS_ICH_HCR_EL2));
        msr(SYS_ICH_HCR_EL2, 0);
        vc->ready = false;
    }
}

/*
 * smp2: with more than one vCPU, one console line per vCPU every <period> counter ticks (called from
 * the boot CPU's hypervisor tick under bhl): the M4 pass signal "both vCPUs take timer interrupts"
 * and "SGIs reach the other vCPU". Read-only; the counters are statistics.
 */
void hv_vgic3_smp_report(u64 now, u64 period)
{
    if (!vgic.ready || vgic.nr_vcpus < 2 || !period)
        return;
    if (!vgic.report_next) {
        vgic.report_next = now + period;
        return;
    }
    if (now < vgic.report_next)
        return;
    vgic.report_next = now + period;
    /*
     * smp2b guest-progress signal: a timer edge needs the guest to re-arm its timer after each
     * interrupt, so "no timer IRQ" on a vCPU across a report period means that vCPU stopped taking
     * timer interrupts (hung, masked, or gone) while EL2 itself still ticks. The primary WDT does
     * not see that case (only the boot CPU's EL2 tick kicks it).
     */
    for (u32 v = 0; v < vgic.nr_vcpus; v++) {
        u64 edges = vgic.vcpu[v].nr_timer_edges;
        u64 delta = edges - vgic.vcpu[v].report_edges;
        vgic.vcpu[v].report_edges = edges;
        printf("HV/win vGIC3: vCPU %u (CPU %d): timer PPI edges %lu, SGIs received %lu, %s, %s "
               "(+%lu)\n",
               v, vgic.vcpu[v].cpu, edges, vgic.vcpu[v].nr_sgi_rx,
               vgic.vcpu[v].ready ? "interface on" : "interface off",
               delta ? "timer IRQ since last report" : "NO timer IRQ since last report", delta);
    }
}

/*
 * smp2f: per-vCPU software view for the on-demand / automatic SMP dump (hv_exc.c hv_win_smp_dump,
 * boot CPU only). Copies under the lock, prints after dropping it.
 */
void hv_vgic3_smp_dump(void)
{
    if (!vgic.ready)
        return;

    u64 now = mrs(CNTPCT_EL0);
    for (u32 v = 0; v < vgic.nr_vcpus; v++) {
        struct {
            u64 edges, sgi_rx, sgi_ack, timer_ack, syncs, t_edge, hcr;
            u64 from[4];
            u32 id[16];
            u64 lr_val[VGIC_MAX_LRS];
            u16 lr_intid[VGIC_MAX_LRS];
            u32 lr_used, dirty, sgi_pend, sgi_act;
            u8 num_lrs;
            bool ready;
            struct {
                bool en, lvl, latch, act;
                u8 prio, lr_vcpu, owner;
            } t[2];
        } c;

        spin_lock(&vgic_lock);
        struct vgic_vcpu *vc = &vgic.vcpu[v];
        c.edges = vc->nr_timer_edges;
        c.sgi_rx = vc->nr_sgi_rx;
        c.sgi_ack = vc->nr_sgi_ack;
        c.timer_ack = vc->nr_timer_ack;
        c.syncs = vc->nr_syncs;
        c.t_edge = vc->t_last_edge;
        c.hcr = vc->hcr;
        memcpy(c.from, vc->nr_sgi_from, sizeof(c.from));
        memcpy(c.id, vc->nr_sgi_id, sizeof(c.id));
        memcpy(c.lr_val, vc->lr_val, sizeof(c.lr_val));
        memcpy(c.lr_intid, vc->lr_intid, sizeof(c.lr_intid));
        c.lr_used = vc->lr_used;
        c.dirty = __atomic_load_n(&vc->dirty, __ATOMIC_RELAXED);
        c.num_lrs = vc->num_lrs;
        c.ready = vc->ready;
        c.sgi_pend = c.sgi_act = 0;
        for (u32 i = 0; i < GIC_NR_SGI; i++) {
            if (vgic_pending(&vc->priv[i]))
                c.sgi_pend |= BIT(i);
            if (vc->priv[i].active)
                c.sgi_act |= BIT(i);
        }
        const u32 tid[2] = {GIC_PPI_CNTV, GIC_PPI_CNTP};
        for (u32 k = 0; k < 2; k++) {
            struct vgic_irq *irq = &vc->priv[tid[k]];
            c.t[k].en = irq->enabled;
            c.t[k].lvl = irq->line_level;
            c.t[k].latch = irq->pending_latch;
            c.t[k].act = irq->active;
            c.t[k].prio = irq->priority;
            c.t[k].lr_vcpu = irq->lr_vcpu;
            c.t[k].owner = irq->owner;
        }
        struct vgic_stat st;
        memset(&st, 0, sizeof(st));
        if (v < VGIC_STAT_N)
            st = vgic_stat[v];
        spin_unlock(&vgic_lock);

        printf("HV/win smp2g vGIC: vCPU %u CPU %d %s dirty %u: timer edges %lu (last %lu ms ago), "
               "timer acks %lu; SGIs rx %lu (from v0 %lu v1 %lu v2 %lu v3+ %lu) acks %lu; syncs %lu; "
               "HCR(sw) 0x%lx\n",
               v, vc->cpu, c.ready ? "on" : "OFF", c.dirty, c.edges,
               c.t_edge ? (now - c.t_edge) / (mrs(CNTFRQ_EL0) / 1000) : 0, c.timer_ack, c.sgi_rx,
               c.from[0], c.from[1], c.from[2], c.from[3], c.sgi_ack, c.syncs, c.hcr);
        printf("HV/win smp2g vGIC: vCPU %u SGI by id:", v);
        for (u32 i = 0; i < 16; i++)
            if (c.id[i])
                printf(" %u:%u", i, c.id[i]);
        printf("; SGI pending 0x%x active 0x%x\n", c.sgi_pend, c.sgi_act);
        /* smp2g: received versus taken, by sender and SGI number; the last interrupt taken */
        for (u32 src = 0; src < 4; src++)
            for (u32 i = 0; i < 16; i++)
                if (st.rx[src][i] || st.taken[src][i])
                    printf("HV/win smp2g vGIC: vCPU %u SGI %u from v%u%s: rx %u taken %u%s\n", v, i,
                           src, src == 3 ? "+" : "", st.rx[src][i], st.taken[src][i],
                           st.rx[src][i] > st.taken[src][i] + 1 ? " (NOT TAKEN)" : "");
        printf("HV/win smp2g vGIC: vCPU %u interrupts taken %lu, last intid %u %ld ms ago\n", v,
               st.nr_taken, st.last_taken_intid,
               st.t_last_taken ? (s64)(now - st.t_last_taken) / (s64)(mrs(CNTFRQ_EL0) / 1000) : -1L);
        for (u32 k = 0; k < 2; k++)
            printf("HV/win smp2g vGIC: vCPU %u PPI %u: enabled %d line %d latch %d active %d prio 0x%x "
                   "lr_vcpu %d owner %d\n",
                   v, tid[k], c.t[k].en, c.t[k].lvl, c.t[k].latch, c.t[k].act, c.t[k].prio,
                   c.t[k].lr_vcpu == VGIC_NONE ? -1 : c.t[k].lr_vcpu,
                   c.t[k].owner == VGIC_NONE ? -1 : c.t[k].owner);
        for (u32 i = 0; i < c.num_lrs && i < VGIC_MAX_LRS; i++)
            if (c.lr_used & BIT(i))
                printf("HV/win smp2g vGIC: vCPU %u LR%u (sw) intid %u val 0x%016lx\n", v, i,
                       c.lr_intid[i], c.lr_val[i]);
    }
    printf("HV/win smp2g vGIC: totals SGI %lu AIC %lu maint %lu overflow %lu\n", vgic.nr_sgi,
           vgic.nr_aic, vgic.nr_maint, vgic.nr_overflow);
}

/*
 * smp2f: stall watch, from the boot CPU's hypervisor tick (bhl held). Returns a vCPU whose timer PPI
 * has had no rising edge for <period> counter ticks since it last made progress, once per stall
 * episode (re-armed when the vCPU makes progress again), or -1.
 */
int hv_vgic3_stall_check(u64 now, u64 period)
{
    if (!vgic.ready || vgic.nr_vcpus < 2 || !period)
        return -1;

    for (u32 v = 0; v < vgic.nr_vcpus; v++) {
        struct vgic_vcpu *vc = &vgic.vcpu[v];
        u64 edges = __atomic_load_n(&vc->nr_timer_edges, __ATOMIC_RELAXED);

        if (!vc->ready || !vc->stall_since || edges != vc->stall_edges) {
            vc->stall_edges = edges;
            vc->stall_since = now;
            vc->stall_dumped = false;
            continue;
        }
        /*
         * smpN: with more than two vCPUs only vCPU 0 (the boot CPU, Windows' clock owner) can trigger
         * the automatic dump. An idle secondary legitimately takes no timer tick for tens of seconds
         * (run 20261110-165343-smp2h a8: three false stall dumps for idle vCPU 1), and with 17 of them
         * the four dumps would all go to idle CPUs, each printing N vCPUs under bhl. The others
         * stay tracked; the on-demand dump covers them. Two vCPUs keep the smp2g rule.
         */
        if (vgic.nr_vcpus > 2 && v != 0)
            continue;
        if (!vc->stall_dumped && now - vc->stall_since >= period) {
            vc->stall_dumped = true;
            return v;
        }
    }
    return -1;
}

/* Read-only pre-start ownership check for the isolated S1 synthetic SPI. */
bool hv_vgic3_spi_unassigned(u32 intid)
{
    if (!vgic.ready || intid < GIC_NR_PRIVATE || intid > GIC_MAX_SPI)
        return false;
    struct vgic_irq *irq = vgic_get_irq(VGIC_NONE, intid);
    return irq->aic_line == VGIC_NO_LINE && !irq->enabled && !irq->active &&
           !irq->pending_latch && !irq->line_level && intid != vgic.pl011_intid;
}
