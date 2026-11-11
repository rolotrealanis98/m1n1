/* SPDX-License-Identifier: MIT */
/*
 * Host unit test for the Windows EL2 runtime's GICv3 model (src/hv/win/hv_vgic3.c).
 *
 * hv_vgic3.c is compiled unchanged with VGIC3_HOST_TEST; this file mocks what it uses from m1n1:
 * per-CPU ICH_* registers including a small model of the hardware virtual CPU interface (guest
 * acknowledge/EOI, ICH_MISR_EL2 derived from the list registers as the architecture defines it),
 * the AIC mask state, IPIs, the stage-2 hook API and spinlocks. The tests drive the emulated
 * GICD/GICR frames through the hooks hv_vgic3_init() installs, exactly as guest MMIO would.
 *
 * Build and run: make -C tests/win-vgic3 && tests/win-vgic3/test_vgic3
 */

#define VGIC3_HOST_KEEP_PRINTF
#include <stdarg.h>
#include <stdlib.h>

#include "hv_win.h"
#include "hv_win_regs.h"
#include "vgic3_host.h"
#include "../hv_wfi_logic.h" /* smp2f: struct wfi_vctx, wfi_vctx_diff */

/* --- harness ---------------------------------------------------------------------------------- */

static int checks, fails;
static const char *cur_test;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            fails++;                                                                               \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #cond);                  \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        u64 _a = (u64)(a), _b = (u64)(b);                                                          \
        checks++;                                                                                  \
        if (_a != _b) {                                                                            \
            fails++;                                                                               \
            printf("FAIL [%s] %s:%d: %s = 0x%lx, expected 0x%lx\n", cur_test, __FILE__, __LINE__,  \
                   #a, _a, _b);                                                                    \
        }                                                                                          \
    } while (0)

/* --- mocks: m1n1 environment ------------------------------------------------------------------ */

int host_cpu;
bool hv_started_cpus[MAX_CPUS];
struct hv_win_state hv_win;

#define NR_TEST_CPUS 6
static u64 mpidr_of[MAX_CPUS];

u64 smp_get_mpidr(int cpu)
{
    return mpidr_of[cpu] & 0xffffff; /* m1n1's spin table keeps Aff2..Aff0 */
}

static u32 ipi_mask;
/* sched1: every vGIC IPI is preceded by exactly one hv_win_kick_note() for the same CPU */
static u32 kick_noted;
static u64 kick_notes, kick_ipis;
void hv_win_kick_note(int cpu)
{
    CHECK(cpu != host_cpu);
    CHECK(!(kick_noted & (1U << cpu))); /* a note never left without its IPI */
    kick_noted |= 1U << cpu;
    kick_notes++;
}

void smp_send_ipi(int cpu)
{
    CHECK(cpu != host_cpu);
    CHECK(kick_noted & (1U << cpu)); /* noted before the IPI is sent */
    kick_noted &= ~(1U << cpu);
    kick_ipis++;
    ipi_mask |= 1U << cpu;
}

static int lock_depth;
void spin_lock(spinlock_t *lock)
{
    lock->depth++;
    lock_depth++;
}

void spin_unlock(spinlock_t *lock)
{
    CHECK(lock->depth > 0);
    lock->depth--;
    lock_depth--;
}

static int log_lines;
static char last_log[256];
static char all_log[1 << 16]; /* smp2f: everything printed since the last log_clear() */
static size_t all_len;
static bool verbose;
static void log_clear(void)
{
    all_len = 0;
    all_log[0] = 0;
}
int host_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    size_t l = strlen(last_log);
    if (all_len + l + 1 < sizeof(all_log)) {
        memcpy(all_log + all_len, last_log, l + 1);
        all_len += l;
    }
    log_lines++;
    if (verbose)
        fputs(last_log, stdout);
    return n;
}

static bool aic_line_masked[AIC_MAX_DIES * AIC_MAX_HW_NUM];
static int aic_unmask_calls;
void aic_mask(u32 irq)
{
    aic_line_masked[irq] = true;
}

void aic_unmask(u32 irq)
{
    aic_line_masked[irq] = false;
    aic_unmask_calls++;
}

static bool pl011_line;
bool hv_win_pl011_irq_level(void)
{
    return pl011_line;
}

static struct {
    u64 base, size;
    hv_hook_t *fn;
} hooks[4];
static int nr_hooks;
static u64 busy_ipa = ~0UL;

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    if (nr_hooks == 4)
        return -1;
    hooks[nr_hooks].base = from;
    hooks[nr_hooks].size = size;
    hooks[nr_hooks].fn = hook;
    nr_hooks++;
    return 0;
}

u64 hv_pt_walk(u64 addr)
{
    return addr == busy_ipa ? 0x4000000000000001UL : 0;
}

/* --- mock: per-CPU virtual CPU interface (ICH_*_EL2) ------------------------------------------ */

struct ich {
    u64 lr[16];
    u64 hcr, vmcr, sre;
    u64 apr[2][4];
};
static struct ich ich[MAX_CPUS];

#define VTR_T6050 0x90580007UL
#define NR_LRS    8

static int lr_index(u32 reg)
{
    for (int i = 0; i < 16; i++)
        if (reg == (u32)sys_reg(3, 4, 12, 12 + i / 8, i % 8))
            return i;
    return -1;
}

static u64 lr_state(u64 lr)
{
    return FIELD_GET(ICH_LR_STATE, lr);
}

/* ICH_MISR_EL2 as the architecture derives it from the LRs and ICH_HCR_EL2 */
static u64 calc_misr(int cpu)
{
    struct ich *c = &ich[cpu];
    u64 misr = 0;
    int valid = 0, pend = 0;

    if (!(c->hcr & ICH_HCR_EN))
        return 0;
    for (int i = 0; i < NR_LRS; i++) {
        u64 lr = c->lr[i];
        if (lr_state(lr))
            valid++;
        if (lr & ICH_LR_PENDING_BIT)
            pend++;
        if (!lr_state(lr) && (lr & ICH_LR_EOI) && !(lr & ICH_LR_HW))
            misr |= ICH_MISR_EOI;
    }
    if ((c->hcr & ICH_HCR_UIE) && valid <= 1)
        misr |= ICH_MISR_U;
    if ((c->hcr & ICH_HCR_LRENPIE) && FIELD_GET(ICH_HCR_EOICOUNT, c->hcr))
        misr |= ICH_MISR_LRENP;
    if ((c->hcr & ICH_HCR_NPIE) && !pend)
        misr |= ICH_MISR_NP;
    return misr;
}

static int bad_sysreg;
static u64 host_now = 1000000; /* smp2f: CNTPCT */
u64 host_mrs(u32 reg)
{
    struct ich *c = &ich[host_cpu];
    int i = lr_index(reg);

    if (i >= 0)
        return i < NR_LRS ? c->lr[i] : 0;
    switch (reg) {
        case SYS_ICH_VTR_EL2:
            return VTR_T6050;
        case SYS_ICH_HCR_EL2:
            return c->hcr;
        case SYS_ICH_VMCR_EL2:
            return c->vmcr;
        case SYS_ICH_MISR_EL2:
            return calc_misr(host_cpu);
        case SYS_ICC_SRE_EL2:
            return c->sre;
        case CNTPCT_EL0: /* smp2f */
            return host_now;
        case CNTFRQ_EL0:
            return 1000000000;
        /* smp2e: active-priority registers, read by hv_vgic3_wfi_pending() */
        case SYS_ICH_AP0R0_EL2:
            return c->apr[0][0];
        case SYS_ICH_AP0R1_EL2:
            return c->apr[0][1];
        case SYS_ICH_AP0R2_EL2:
            return c->apr[0][2];
        case SYS_ICH_AP0R3_EL2:
            return c->apr[0][3];
        case SYS_ICH_AP1R0_EL2:
            return c->apr[1][0];
        case SYS_ICH_AP1R1_EL2:
            return c->apr[1][1];
        case SYS_ICH_AP1R2_EL2:
            return c->apr[1][2];
        case SYS_ICH_AP1R3_EL2:
            return c->apr[1][3];
    }
    bad_sysreg++;
    printf("unexpected mrs 0x%x\n", reg);
    return 0;
}

static int lr_writes;
void host_msr(u32 reg, u64 val)
{
    struct ich *c = &ich[host_cpu];
    int i = lr_index(reg);

    if (i >= 0) {
        CHECK(i < NR_LRS); /* never touch LRs the hardware does not have */
        if (i < NR_LRS)
            c->lr[i] = val;
        lr_writes++;
        return;
    }
    switch (reg) {
        case SYS_ICH_HCR_EL2:
            c->hcr = val;
            return;
        case SYS_ICH_VMCR_EL2:
            c->vmcr = val;
            return;
        case SYS_ICC_SRE_EL2:
            c->sre = val;
            return;
        case SYS_ICH_AP0R0_EL2:
            c->apr[0][0] = val;
            return;
        case SYS_ICH_AP1R0_EL2:
            c->apr[1][0] = val;
            return;
    }
    bad_sysreg++;
    printf("unexpected msr 0x%x\n", reg);
}

/* Guest side of the CPU interface: acknowledge the highest-priority pending LR */
static u32 guest_ack(int cpu)
{
    struct ich *c = &ich[cpu];
    int best = -1;

    for (int i = 0; i < NR_LRS; i++) {
        if (lr_state(c->lr[i]) != ICH_LR_STATE_PEND)
            continue;
        if (best < 0 ||
            FIELD_GET(ICH_LR_PRIORITY, c->lr[i]) < FIELD_GET(ICH_LR_PRIORITY, c->lr[best]))
            best = i;
    }
    if (best < 0)
        return GIC_SPURIOUS;
    c->lr[best] = (c->lr[best] & ~ICH_LR_STATE) | FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_ACTIVE);
    return FIELD_GET(ICH_LR_VINTID, c->lr[best]);
}

/* EOImode 0: priority drop and deactivation; no LR match counts in EOIcount */
static void guest_eoi(int cpu, u32 intid)
{
    struct ich *c = &ich[cpu];

    for (int i = 0; i < NR_LRS; i++) {
        if (FIELD_GET(ICH_LR_VINTID, c->lr[i]) == intid && (c->lr[i] & ICH_LR_ACTIVE_BIT)) {
            c->lr[i] &= ~ICH_LR_ACTIVE_BIT;
            return;
        }
    }
    u64 cnt = FIELD_GET(ICH_HCR_EOICOUNT, c->hcr) + 1;
    c->hcr = (c->hcr & ~ICH_HCR_EOICOUNT) | FIELD_PREP(ICH_HCR_EOICOUNT, cnt);
}

/* The LR holding <intid> on <cpu>, or 0 */
static u64 find_lr(int cpu, u32 intid)
{
    for (int i = 0; i < NR_LRS; i++)
        if (lr_state(ich[cpu].lr[i]) && FIELD_GET(ICH_LR_VINTID, ich[cpu].lr[i]) == intid)
            return ich[cpu].lr[i];
    return 0;
}

static int nr_valid_lrs(int cpu)
{
    int n = 0;
    for (int i = 0; i < NR_LRS; i++)
        n += lr_state(ich[cpu].lr[i]) != 0;
    return n;
}

/* --- topology --------------------------------------------------------------------------------- */

/*
 * m1n1 CPU -> MPIDR. The boot CPU is CPU 2, so vCPU index != m1n1 CPU index != Aff0 (the failure
 * model from Aura's windows-sgi-routing.md). CPU 4's Aff0 is 0x11 and needs RangeSelector 1.
 */
#define GICD     0xf00000000UL
#define GICR     0xf10000000UL
#define BOOT_CPU 2

static const u64 test_mpidr[NR_TEST_CPUS] = {
    0x80000000, /* CPU 0: 0.0.0 */
    0x80000001, /* CPU 1: 0.0.1 */
    0x80000100, /* CPU 2: 0.1.0 (boot) */
    0x80000101, /* CPU 3: 0.1.1 */
    0x80000111, /* CPU 4: 0.1.17 */
    0x80010000, /* CPU 5: 1.0.0 */
};
/* vCPU order: boot CPU first, then m1n1 order (hv_win_build_vcpus) */
static const int vcpu_cpu[NR_TEST_CPUS] = {2, 0, 1, 3, 4, 5};

static int cpu_of(int vcpu)
{
    return vcpu_cpu[vcpu];
}

static void reset_world(void)
{
    memset(ich, 0, sizeof(ich));
    memset(hooks, 0, sizeof(hooks));
    memset(aic_line_masked, 0, sizeof(aic_line_masked));
    memset(&hv_win, 0, sizeof(hv_win));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    nr_hooks = 0;
    ipi_mask = 0;
    aic_unmask_calls = 0;
    log_lines = 0;
    busy_ipa = ~0UL;
    bad_sysreg = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;
        ich[i].sre = 0xf;
        ich[i].vmcr = 0xf84c0008; /* hand-off value from PROBE-GIC-T6050.md */
    }
    for (int i = 0; i < NR_TEST_CPUS; i++) {
        mpidr_of[i] = test_mpidr[i];
        hv_win.vcpu_cpu[i] = vcpu_cpu[i];
        hv_win.cpu_vcpu[vcpu_cpu[i]] = i;
    }
    hv_win.nr_vcpus = NR_TEST_CPUS;
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
}

/* configure + every vCPU brought up (CPU_ON) */
static void bring_up(void)
{
    reset_world();
    host_cpu = BOOT_CPU;
    CHECK_EQ(hv_vgic3_init(GICD, GICR, NR_TEST_CPUS), 0);
    for (int v = 0; v < NR_TEST_CPUS; v++) {
        host_cpu = cpu_of(v);
        hv_vgic3_cpu_init();
        hv_started_cpus[cpu_of(v)] = true;
    }
    host_cpu = BOOT_CPU;
    ipi_mask = 0;
}

/* --- guest MMIO through the installed hooks --------------------------------------------------- */

static hv_hook_t *hook_for(u64 addr)
{
    for (int i = 0; i < nr_hooks; i++)
        if (addr >= hooks[i].base && addr < hooks[i].base + hooks[i].size)
            return hooks[i].fn;
    return NULL;
}

static u64 rd(u64 addr, int width)
{
    hv_hook_t *fn = hook_for(addr);
    u64 val[8] = {0xdeadbeefdeadbeefUL};

    CHECK(fn != NULL);
    if (!fn)
        return 0;
    CHECK(fn(NULL, addr, val, false, width));
    CHECK_EQ(lock_depth, 0);
    return val[0];
}

static void wr(u64 addr, u64 v, int width)
{
    hv_hook_t *fn = hook_for(addr);
    u64 val[8] = {v};

    CHECK(fn != NULL);
    if (!fn)
        return;
    CHECK(fn(NULL, addr, val, true, width));
    CHECK_EQ(lock_depth, 0);
}

#define rd32(a)    ((u32)rd((a), 2))
#define wr32(a, v) wr((a), (v), 2)
#define rd64(a)    rd((a), 3)
#define wr64(a, v) wr((a), (v), 3)

static u64 gicr_rd(int vcpu, u32 off)
{
    return GICR + (u64)vcpu * GICR_STRIDE + off;
}

static u64 gicr_sgi(int vcpu, u32 off)
{
    return gicr_rd(vcpu, GICR_SGI_BASE + off);
}

static u64 bitreg(u32 base, u32 intid)
{
    return GICD + base + 4 * (intid / 32);
}

static void spi_enable(u32 intid)
{
    wr32(bitreg(GICD_ISENABLER, intid), 1U << (intid % 32));
}

static void spi_route(u32 intid, int vcpu)
{
    wr64(GICD + GICD_IROUTER + 8 * intid, mpidr_of[cpu_of(vcpu)] & 0xffffff);
}

static void spi_prio(u32 intid, u8 prio)
{
    wr(GICD + GICD_IPRIORITYR + intid, prio, 0);
}

static void spi_edge(u32 intid)
{
    u64 a = GICD + GICD_ICFGR + 4 * (intid / 16);
    wr32(a, rd32(a) | (2U << (2 * (intid % 16))));
}

static void grp1_on(void)
{
    wr32(GICD + GICD_CTLR, GICD_CTLR_ENABLEGRP1);
}

/* A guest exit on <cpu>: the maintenance check of hv_exc_fiq and the flush of hv_update_fiq */
static void exit_on(int cpu)
{
    host_cpu = cpu;
    hv_vgic3_maintenance();
    hv_vgic3_flush();
    CHECK_EQ(lock_depth, 0);
    host_cpu = BOOT_CPU;
}

/* Only the maintenance FIQ (no other exit happened) */
static void maint_on(int cpu)
{
    host_cpu = cpu;
    hv_vgic3_maintenance();
    CHECK_EQ(lock_depth, 0);
    host_cpu = BOOT_CPU;
}

static u64 lr_bits(u32 intid, u8 prio, u64 state, bool eoi)
{
    return FIELD_PREP(ICH_LR_VINTID, intid) | ICH_LR_GROUP1 | FIELD_PREP(ICH_LR_PRIORITY, prio) |
           FIELD_PREP(ICH_LR_STATE, state) | (eoi ? ICH_LR_EOI : 0);
}

/* --- tests ------------------------------------------------------------------------------------ */

static void test_init_and_id_registers(void)
{
    bring_up();

    CHECK_EQ(nr_hooks, 2);
    CHECK_EQ(hooks[0].base, GICD);
    CHECK_EQ(hooks[0].size, GICD_SIZE);
    CHECK_EQ(hooks[1].base, GICR);
    CHECK_EQ(hooks[1].size, NR_TEST_CPUS * GICR_STRIDE);

    /* per-CPU interface: enabled, LRs clear, VMCR at its reset view, SRE untouched (already 0xf) */
    for (int v = 0; v < NR_TEST_CPUS; v++) {
        int cpu = cpu_of(v);
        CHECK_EQ(ich[cpu].hcr, ICH_HCR_EN);
        CHECK_EQ(ich[cpu].vmcr, 0);
        CHECK_EQ(ich[cpu].sre, 0xf);
        CHECK_EQ(nr_valid_lrs(cpu), 0);
    }

    CHECK_EQ(rd32(GICD + GICD_CTLR), GICD_CTLR_ARE | GICD_CTLR_DS);
    wr32(GICD + GICD_CTLR, GICD_CTLR_ENABLEGRP0 | GICD_CTLR_ENABLEGRP1 | GICD_CTLR_RWP);
    CHECK_EQ(rd32(GICD + GICD_CTLR), GICD_CTLR_ARE | GICD_CTLR_DS | GICD_CTLR_ENABLEGRP1);

    u32 typer = rd32(GICD + GICD_TYPER);
    CHECK_EQ(FIELD_GET(GICD_TYPER_ITLINES, typer), 31);
    CHECK_EQ(FIELD_GET(GICD_TYPER_IDBITS, typer), 9);
    CHECK(typer & GICD_TYPER_No1N);
    CHECK(!(typer & GICD_TYPER_A3V));
    CHECK(!(typer & BIT(17))); /* LPIS */
    CHECK_EQ(FIELD_GET(GICD_IIDR_IMPLEMENTER, rd32(GICD + GICD_IIDR)), 0x43b);
    CHECK_EQ(rd32(GICD + GICD_PIDR2), 0x30); /* CONTRACT.md s9 */
    CHECK_EQ(rd32(GICD + GICD_TYPER2), 0);

    for (int v = 0; v < NR_TEST_CPUS; v++) {
        u64 typer64 = rd64(gicr_rd(v, GICR_TYPER));
        u64 mpidr = mpidr_of[cpu_of(v)];
        CHECK_EQ(FIELD_GET(GICR_TYPER_AFFINITY, typer64), mpidr & 0xffffff);
        CHECK_EQ(FIELD_GET(GICR_TYPER_PROCNUM, typer64), v);
        CHECK_EQ(!!(typer64 & GICR_TYPER_LAST), v == NR_TEST_CPUS - 1);
        CHECK_EQ(typer64 & 0xf, 0); /* no PLPIS/VLPIS/DirectLPI */
        CHECK_EQ(rd32(gicr_rd(v, GICR_TYPER)), (u32)typer64);
        CHECK_EQ(rd32(gicr_rd(v, GICR_TYPER + 4)), typer64 >> 32);
        CHECK_EQ(FIELD_GET(GIC_PIDR2_ARCH, rd32(gicr_rd(v, GICR_PIDR2))) << 4,
                 GIC_PIDR2_ARCH_GICV3);
    }

    /* GICR_WAKER: ProcessorSleep at reset, ChildrenAsleep follows it */
    CHECK_EQ(rd32(gicr_rd(3, GICR_WAKER)), GICR_WAKER_PROCESSORSLEEP | GICR_WAKER_CHILDRENASLEEP);
    wr32(gicr_rd(3, GICR_WAKER), 0);
    CHECK_EQ(rd32(gicr_rd(3, GICR_WAKER)), 0);
    CHECK_EQ(rd32(gicr_rd(2, GICR_WAKER)), GICR_WAKER_PROCESSORSLEEP | GICR_WAKER_CHILDRENASLEEP);

    /* no LPIs: CTLR, PROPBASER and PENDBASER RAZ/WI, SYNCR idle */
    wr32(gicr_rd(0, GICR_CTLR), 1);
    wr64(gicr_rd(0, GICR_PROPBASER), 0x1234000);
    CHECK_EQ(rd32(gicr_rd(0, GICR_CTLR)), 0);
    CHECK_EQ(rd64(gicr_rd(0, GICR_PROPBASER)), 0);
    CHECK_EQ(rd64(gicr_rd(0, GICR_PENDBASER)), 0);
    CHECK_EQ(rd32(gicr_rd(0, GICR_SYNCR)), 0);

    CHECK_EQ(bad_sysreg, 0);

    /* refusals: IPA already mapped, overlapping frames, misaligned */
    reset_world();
    busy_ipa = GICR + 3 * GICR_STRIDE;
    CHECK_EQ(hv_vgic3_init(GICD, GICR, NR_TEST_CPUS), -1);
    CHECK_EQ(nr_hooks, 0);
    reset_world();
    CHECK_EQ(hv_vgic3_init(GICR + 0x20000, GICR, NR_TEST_CPUS), -1);
    CHECK_EQ(hv_vgic3_init(GICD + 0x100, GICR, NR_TEST_CPUS), -1);
    CHECK_EQ(nr_hooks, 0);
}

static void test_distributor_registers(void)
{
    bring_up();

    /* ARE=1: GICD word 0 (SGIs/PPIs) RAZ/WI */
    wr32(GICD + GICD_ISENABLER, 0xffffffff);
    CHECK_EQ(rd32(GICD + GICD_ISENABLER), 0);
    CHECK_EQ(rd32(GICD + GICD_IGROUPR), 0);
    wr(GICD + GICD_IPRIORITYR + 27, 0x80, 0);
    CHECK_EQ(rd(GICD + GICD_IPRIORITYR + 27, 0), 0);
    CHECK_EQ(rd32(GICD + GICD_ICFGR), 0);
    CHECK_EQ(rd32(GICD + GICD_ICFGR + 4), 0);

    /* Group 1 only: IGROUPR RAO/WI for implemented SPIs, INTIDs 1020..1023 RAZ */
    CHECK_EQ(rd32(GICD + GICD_IGROUPR + 4), 0xffffffff);
    wr32(GICD + GICD_IGROUPR + 4, 0);
    CHECK_EQ(rd32(GICD + GICD_IGROUPR + 4), 0xffffffff);
    CHECK_EQ(rd32(GICD + GICD_IGROUPR + 4 * 31), 0x0fffffff);

    /* enables */
    wr32(bitreg(GICD_ISENABLER, 32), 0x5);
    CHECK_EQ(rd32(bitreg(GICD_ISENABLER, 32)), 0x5);
    CHECK_EQ(rd32(bitreg(GICD_ICENABLER, 32)), 0x5);
    wr32(bitreg(GICD_ICENABLER, 32), 0x1);
    CHECK_EQ(rd32(bitreg(GICD_ISENABLER, 32)), 0x4);
    wr32(bitreg(GICD_ISENABLER, 992), 0xffffffff);
    CHECK_EQ(rd32(bitreg(GICD_ISENABLER, 992)), 0x0fffffff);

    /* pending and active (INTID 40 disabled, so nothing gets listed) */
    wr32(bitreg(GICD_ISPENDR, 40), BIT(8));
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), BIT(8));
    CHECK_EQ(rd32(bitreg(GICD_ICPENDR, 40)), BIT(8));
    wr32(bitreg(GICD_ICPENDR, 40), BIT(8));
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), 0);
    wr32(bitreg(GICD_ISACTIVER, 40), BIT(9));
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 40)), BIT(9));
    wr32(bitreg(GICD_ICACTIVER, 40), BIT(9));
    CHECK_EQ(rd32(bitreg(GICD_ICACTIVER, 40)), 0);

    /* priorities: 5 implemented bits (0xf8), byte and word access */
    spi_prio(33, 0xff);
    CHECK_EQ(rd(GICD + GICD_IPRIORITYR + 33, 0), 0xf8);
    wr32(GICD + GICD_IPRIORITYR + 36, 0x12345678);
    CHECK_EQ(rd32(GICD + GICD_IPRIORITYR + 36), 0x10305078);
    CHECK_EQ(rd(GICD + GICD_IPRIORITYR + 37, 0), 0x50);

    /* ICFGR: SPIs configurable, edge = odd bit */
    wr32(GICD + GICD_ICFGR + 8, 0xffffffff);
    CHECK_EQ(rd32(GICD + GICD_ICFGR + 8), 0xaaaaaaaa);
    wr32(GICD + GICD_ICFGR + 8, 0x2);
    CHECK_EQ(rd32(GICD + GICD_ICFGR + 8), 0x2);

    /* IROUTER: Aff2.Aff1.Aff0 kept, IRM and Aff3 RAZ/WI, 64-bit or 32-bit halves */
    wr64(GICD + GICD_IROUTER + 8 * 40, (1UL << 32) | BIT(31) | 0x010101);
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 40), 0x010101);
    CHECK_EQ(rd32(GICD + GICD_IROUTER + 8 * 40), 0x010101);
    CHECK_EQ(rd32(GICD + GICD_IROUTER + 8 * 40 + 4), 0);
    wr32(GICD + GICD_IROUTER + 8 * 41, 0x0101);
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 41), 0x0101);
    wr64(GICD + GICD_IROUTER + 8 * 20, 0x0101);
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 20), 0);

    /* odd widths and the reserved/RAZ space */
    CHECK_EQ(rd(GICD + GICD_CTLR, 1), 0);
    CHECK_EQ(rd(GICD + GICD_CTLR, 0), 0);
    wr32(GICD + GICD_ITARGETSR + 32, 0xffffffff);
    CHECK_EQ(rd32(GICD + GICD_ITARGETSR + 32), 0);
    CHECK_EQ(rd32(GICD + GICD_IGRPMODR + 4), 0);
    CHECK_EQ(rd32(GICD + GICD_STATUSR), 0);
    CHECK_EQ(rd32(GICD + 0xfff0), 0);

    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_redistributor_banking(void)
{
    bring_up();

    /* SGIs reset enabled (KVM), PPIs disabled; each frame has its own bank */
    for (int v = 0; v < NR_TEST_CPUS; v++)
        CHECK_EQ(rd32(gicr_sgi(v, GICR_ISENABLER0)), 0xffff);

    wr32(gicr_sgi(1, GICR_ISENABLER0), BIT(27) | BIT(30));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISENABLER0)), 0xffff | BIT(27) | BIT(30));
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISENABLER0)), 0xffff);
    CHECK_EQ(rd32(gicr_sgi(2, GICR_ISENABLER0)), 0xffff);

    /* ICENABLER0 disables (NT-for-ASi enabled here), only on its own frame */
    wr32(gicr_sgi(1, GICR_ICENABLER0), BIT(0) | BIT(30));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISENABLER0)), 0xfffe | BIT(27));
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISENABLER0)), 0xffff);

    wr(gicr_sgi(3, GICR_IPRIORITYR0 + 27), 0xa7, 0);
    CHECK_EQ(rd(gicr_sgi(3, GICR_IPRIORITYR0 + 27), 0), 0xa0);
    CHECK_EQ(rd32(gicr_sgi(3, GICR_IPRIORITYR0 + 24)), 0xa0000000);
    CHECK_EQ(rd32(gicr_sgi(4, GICR_IPRIORITYR0 + 24)), 0);

    /* SGIs edge, PPIs level, both read-only; Group 1 */
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ICFGR0)), 0xaaaaaaaa);
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ICFGR1)), 0);
    wr32(gicr_sgi(0, GICR_ICFGR1), 0xaaaaaaaa);
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ICFGR1)), 0);
    CHECK_EQ(rd32(gicr_sgi(0, GICR_IGROUPR0)), 0xffffffff);

    /* pending SGI through ISPENDR0 on frame 3 belongs to vCPU 3 only */
    wr32(gicr_sgi(3, GICR_ISPENDR0), BIT(7));
    CHECK_EQ(rd32(gicr_sgi(3, GICR_ISPENDR0)), BIT(7));
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISPENDR0)), 0);
}

static void test_lr_allocation_and_overflow(void)
{
    bring_up();
    grp1_on();

    /* ten edge SPIs on vCPU 0 (the boot CPU), INTID 40 + i at priority 0x80 - 8 * i */
    for (u32 i = 0; i < 10; i++) {
        spi_edge(40 + i);
        spi_route(40 + i, 0);
        spi_prio(40 + i, 0x80 - 8 * i);
        spi_enable(40 + i);
    }
    wr32(bitreg(GICD_ISPENDR, 40), 0x3ff << 8);
    CHECK_EQ(ipi_mask, 0); /* all for the CPU doing the MMIO: no IPI, flushed on its exit */

    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), NR_LRS);
    for (u32 i = 0; i < NR_LRS; i++) {
        u32 intid = 49 - i;
        CHECK_EQ(ich[BOOT_CPU].lr[i], lr_bits(intid, 0x80 - 8 * (intid - 40), 1, false));
    }
    CHECK_EQ(ich[BOOT_CPU].hcr, ICH_HCR_EN | ICH_HCR_UIE);
    CHECK_EQ(calc_misr(BOOT_CPU), 0);
    /* INTIDs 40, 41 wait in software, still pending as far as the guest can tell */
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), 0x3ff << 8);

    /* the guest handles seven: underflow fires with one LR left */
    for (int i = 0; i < 7; i++) {
        u32 intid = guest_ack(BOOT_CPU);
        CHECK_EQ(intid, 49 - i);
        guest_eoi(BOOT_CPU, intid);
    }
    CHECK_EQ(calc_misr(BOOT_CPU), ICH_MISR_U);
    maint_on(BOOT_CPU);
    CHECK_EQ(calc_misr(BOOT_CPU), 0);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 3);
    CHECK(find_lr(BOOT_CPU, 42) & ICH_LR_PENDING_BIT);
    CHECK(find_lr(BOOT_CPU, 41) & ICH_LR_PENDING_BIT);
    CHECK(find_lr(BOOT_CPU, 40) & ICH_LR_PENDING_BIT);
    CHECK_EQ(ich[BOOT_CPU].hcr, ICH_HCR_EN);

    for (int i = 0; i < 3; i++)
        guest_eoi(BOOT_CPU, guest_ack(BOOT_CPU));
    CHECK_EQ(guest_ack(BOOT_CPU), GIC_SPURIOUS);
    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 40)), 0);

    /* a level SPI pended by software carries EOI so its deactivation is seen */
    spi_route(60, 0);
    spi_prio(60, 0x40);
    spi_enable(60);
    wr32(bitreg(GICD_ISPENDR, 60), BIT(60 % 32));
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 60), lr_bits(60, 0x40, 1, true));

    /* nothing pending, nothing listed: an exit does not touch the LRs */
    guest_eoi(BOOT_CPU, guest_ack(BOOT_CPU));
    maint_on(BOOT_CPU);
    int before = lr_writes;
    exit_on(BOOT_CPU);
    CHECK_EQ(lr_writes, before);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_sgi_routing(void)
{
    bring_up();
    grp1_on();
    hv_started_cpus[5] = false; /* not brought up yet: pending is kept, no IPI */

    /* from vCPU 0 (CPU 2, 0.1.0): Aff1=1, TargetList {0,1} -> itself and CPU 3 (0.1.1) */
    host_cpu = BOOT_CPU;
    CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF1, 1) |
                                FIELD_PREP(ICC_SGI1R_TARGETLIST, 3) |
                                FIELD_PREP(ICC_SGI1R_INTID, 3)));
    CHECK_EQ(lock_depth, 0);
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISPENDR0)), BIT(3));
    CHECK_EQ(rd32(gicr_sgi(3, GICR_ISPENDR0)), BIT(3));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)), 0);
    CHECK_EQ(ipi_mask, BIT(3));

    /* RangeSelector 1, bit 1 -> Aff0 17 (CPU 4, vCPU 4) */
    ipi_mask = 0;
    hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF1, 1) | FIELD_PREP(ICC_SGI1R_RS, 1) |
                          FIELD_PREP(ICC_SGI1R_TARGETLIST, 2) | FIELD_PREP(ICC_SGI1R_INTID, 4));
    CHECK_EQ(rd32(gicr_sgi(4, GICR_ISPENDR0)), BIT(4));
    CHECK_EQ(rd32(gicr_sgi(3, GICR_ISPENDR0)), BIT(3)); /* 0.1.1 not hit by RS 1 */
    CHECK_EQ(ipi_mask, BIT(4));

    /* Aff2 = 1 -> CPU 5 (vCPU 5), not running: pending, no IPI */
    ipi_mask = 0;
    hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 1) | FIELD_PREP(ICC_SGI1R_TARGETLIST, 1) |
                          FIELD_PREP(ICC_SGI1R_INTID, 9));
    CHECK_EQ(rd32(gicr_sgi(5, GICR_ISPENDR0)), BIT(9));
    CHECK_EQ(ipi_mask, 0);

    /* nobody at 0.7.x */
    hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF1, 7) | FIELD_PREP(ICC_SGI1R_TARGETLIST, 0xffff) |
                          FIELD_PREP(ICC_SGI1R_INTID, 1));
    for (int v = 0; v < NR_TEST_CPUS; v++)
        CHECK(!(rd32(gicr_sgi(v, GICR_ISPENDR0)) & BIT(1)));
    CHECK_EQ(ipi_mask, 0);

    /* IRM: everyone but the sender (vCPU 3 on CPU 3) */
    host_cpu = 3;
    hv_vgic3_handle_sgi1r(ICC_SGI1R_IRM | FIELD_PREP(ICC_SGI1R_INTID, 5) |
                          FIELD_PREP(ICC_SGI1R_TARGETLIST, 0x1)); /* TargetList ignored */
    host_cpu = BOOT_CPU;
    for (int v = 0; v < NR_TEST_CPUS; v++)
        CHECK_EQ(!!(rd32(gicr_sgi(v, GICR_ISPENDR0)) & BIT(5)), v != 3);
    CHECK_EQ(ipi_mask, BIT(0) | BIT(1) | BIT(2) | BIT(4)); /* not 3 (self), not 5 (off) */

    /* the target's exit lists its SGIs: edge, no EOI bit, priority 0 */
    exit_on(3);
    CHECK_EQ(find_lr(3, 3), lr_bits(3, 0, 1, false));
    CHECK_EQ(nr_valid_lrs(3), 1);
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 3), lr_bits(3, 0, 1, false));
    CHECK_EQ(find_lr(BOOT_CPU, 5), lr_bits(5, 0, 1, false));

    /* a second SGI while the first is active: pending+active (edge) */
    CHECK_EQ(guest_ack(3), 3);
    host_cpu = BOOT_CPU;
    ipi_mask = 0;
    hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF1, 1) | FIELD_PREP(ICC_SGI1R_TARGETLIST, 2) |
                          FIELD_PREP(ICC_SGI1R_INTID, 3));
    CHECK_EQ(ipi_mask, BIT(3));
    exit_on(3);
    CHECK_EQ(find_lr(3, 3), lr_bits(3, 0, ICH_LR_STATE_PA, false));
    guest_eoi(3, 3);
    CHECK_EQ(guest_ack(3), 3);
    guest_eoi(3, 3);
    exit_on(3);
    CHECK_EQ(nr_valid_lrs(3), 0);

    /* a disabled SGI stays pending and is not listed */
    wr32(gicr_sgi(1, GICR_ICENABLER0), BIT(5));
    exit_on(cpu_of(1));
    CHECK_EQ(find_lr(cpu_of(1), 5), 0);
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)) & BIT(5), BIT(5));

    /* reads of ICC_SGI1R are not ours, SGI0R/ASGI1R are handled in hv_win.c */
    CHECK_EQ(bad_sysreg, 0);
}

static void test_aic_line_and_eoi_maintenance(void)
{
    bring_up();
    grp1_on();

    /* map: line 100 -> SPI 40; masked until the guest enables it */
    host_cpu = BOOT_CPU;
    CHECK_EQ(hv_vgic3_map_irq(100, 40), 0);
    CHECK(aic_line_masked[100]);
    CHECK_EQ(hv_vgic3_map_irq(100, 41), -1);   /* line twice */
    CHECK_EQ(hv_vgic3_map_irq(101, 40), -1);   /* SPI twice */
    CHECK_EQ(hv_vgic3_map_irq(102, 31), -1);   /* PPI */
    CHECK_EQ(hv_vgic3_map_irq(102, 1020), -1); /* special INTID */
    CHECK_EQ(hv_vgic3_map_irq(102, GIC_MAX_SPI), 0);

    /* SPI 40 to vCPU 1 (CPU 0), level (the AIC line latches regardless), enabled -> unmasked */
    spi_route(40, 1);
    spi_prio(40, 0x90);
    spi_enable(40);
    CHECK(!aic_line_masked[100]);

    /* the event lands on the boot CPU: the AIC masked the line on acknowledge */
    aic_line_masked[100] = true;
    host_cpu = BOOT_CPU;
    hv_vgic3_handle_aic_irq(100);
    CHECK_EQ(ipi_mask, BIT(0));
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), BIT(8));
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 40), 0);

    /* vCPU 1 lists it with EOI set (HW=0) */
    exit_on(0);
    CHECK_EQ(find_lr(0, 40), lr_bits(40, 0x90, 1, true));
    CHECK(aic_line_masked[100]);

    /* acknowledged: stays active across exits, line still masked */
    CHECK_EQ(guest_ack(0), 40);
    exit_on(0);
    CHECK_EQ(find_lr(0, 40), lr_bits(40, 0x90, ICH_LR_STATE_ACTIVE, true));
    CHECK(aic_line_masked[100]);
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 40)), BIT(8));
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 40)), 0);

    /* EOI: maintenance (EOI) fires, EL2 unmasks the line and frees the LR */
    guest_eoi(0, 40);
    CHECK_EQ(calc_misr(0), ICH_MISR_EOI);
    int unmasks = aic_unmask_calls;
    maint_on(0);
    CHECK_EQ(calc_misr(0), 0);
    CHECK(!aic_line_masked[100]);
    CHECK_EQ(aic_unmask_calls, unmasks + 1);
    CHECK_EQ(nr_valid_lrs(0), 0);
    CHECK_EQ(ich[0].lr[0], 0);
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 40)), 0);

    /* the device still asserts: the next event goes round again */
    aic_line_masked[100] = true;
    host_cpu = 0;
    hv_vgic3_handle_aic_irq(100);
    exit_on(0);
    CHECK_EQ(find_lr(0, 40), lr_bits(40, 0x90, 1, true));

    /* the guest clears the pending state instead of taking it: line resampled (unmasked) */
    wr32(bitreg(GICD_ICPENDR, 40), BIT(8));
    exit_on(0);
    CHECK_EQ(find_lr(0, 40), 0);
    CHECK(!aic_line_masked[100]);

    /* disabling masks the line at the AIC */
    wr32(bitreg(GICD_ICENABLER, 40), BIT(8));
    CHECK(aic_line_masked[100]);

    /* unrouted line: logged once, never unmasked */
    log_lines = 0;
    unmasks = aic_unmask_calls;
    hv_vgic3_handle_aic_irq(200);
    hv_vgic3_handle_aic_irq(200);
    CHECK_EQ(log_lines, 1);
    CHECK(strstr(last_log, "200") != NULL);
    CHECK_EQ(aic_unmask_calls, unmasks);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_timer_ppi_level(void)
{
    const int cpu = 0; /* vCPU 1 */

    bring_up();
    grp1_on();
    wr32(gicr_sgi(1, GICR_ISENABLER0), BIT(GIC_PPI_CNTP));
    wr(gicr_sgi(1, GICR_IPRIORITYR0 + GIC_PPI_CNTP), 0x20, 0);

    /* timer fires: level PPI with EOI so the deactivation resamples */
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, true);
    exit_on(cpu);
    CHECK_EQ(find_lr(cpu, GIC_PPI_CNTP), lr_bits(GIC_PPI_CNTP, 0x20, 1, true));

    /* acknowledged while the line is still high: active only, never pending+active */
    CHECK_EQ(guest_ack(cpu), GIC_PPI_CNTP);
    exit_on(cpu);
    CHECK_EQ(find_lr(cpu, GIC_PPI_CNTP), lr_bits(GIC_PPI_CNTP, 0x20, ICH_LR_STATE_ACTIVE, true));

    /* EOI with the line still high (guest did not reprogram): re-delivered */
    guest_eoi(cpu, GIC_PPI_CNTP);
    CHECK_EQ(calc_misr(cpu), ICH_MISR_EOI);
    maint_on(cpu);
    CHECK_EQ(calc_misr(cpu), 0);
    CHECK_EQ(find_lr(cpu, GIC_PPI_CNTP), lr_bits(GIC_PPI_CNTP, 0x20, 1, true));

    /* the handler reprograms CVAL (trapped exit, level drops), then EOIs: done */
    CHECK_EQ(guest_ack(cpu), GIC_PPI_CNTP);
    host_cpu = cpu;
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, false);
    exit_on(cpu);
    CHECK_EQ(find_lr(cpu, GIC_PPI_CNTP), lr_bits(GIC_PPI_CNTP, 0x20, ICH_LR_STATE_ACTIVE, true));
    guest_eoi(cpu, GIC_PPI_CNTP);
    maint_on(cpu);
    CHECK_EQ(nr_valid_lrs(cpu), 0);
    CHECK_EQ(calc_misr(cpu), 0);
    CHECK_EQ(ich[cpu].hcr, ICH_HCR_EN);

    /* level drops before the guest took it: withdrawn from the LR */
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, true);
    exit_on(cpu);
    CHECK(find_lr(cpu, GIC_PPI_CNTP) & ICH_LR_PENDING_BIT);
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, false);
    exit_on(cpu);
    CHECK_EQ(nr_valid_lrs(cpu), 0);

    /* ICPENDR cannot clear a level interrupt whose line is high */
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, true);
    wr32(gicr_sgi(1, GICR_ICPENDR0), BIT(GIC_PPI_CNTP));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)), BIT(GIC_PPI_CNTP));
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, false);
    exit_on(cpu);

    /* the virtual timer PPI is disabled: a firing CNTV is not delivered */
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTV, true);
    exit_on(cpu);
    CHECK_EQ(nr_valid_lrs(cpu), 0);
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)), BIT(GIC_PPI_CNTV));

    /* a PPI level for another CPU only affects that CPU's redistributor */
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISPENDR0)), 0);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_changes_while_listed(void)
{
    bring_up();
    grp1_on();

    spi_edge(50);
    spi_route(50, 0);
    spi_enable(50);
    wr32(bitreg(GICD_ISPENDR, 50), BIT(50 % 32));
    exit_on(BOOT_CPU);
    CHECK(find_lr(BOOT_CPU, 50) & ICH_LR_PENDING_BIT);

    /* another CPU disables it: the owner is kicked and withdraws the LR, pending stays */
    host_cpu = 0;
    ipi_mask = 0;
    wr32(bitreg(GICD_ICENABLER, 50), BIT(50 % 32));
    CHECK_EQ(ipi_mask, BIT(BOOT_CPU));
    host_cpu = BOOT_CPU;
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 50)), BIT(50 % 32)); /* seen through the live LR */
    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 50)), BIT(50 % 32));

    /* enabled again: listed; then ICPENDR from another CPU: withdrawn and not pending */
    spi_enable(50);
    exit_on(BOOT_CPU);
    CHECK(find_lr(BOOT_CPU, 50) & ICH_LR_PENDING_BIT);
    host_cpu = 0;
    wr32(bitreg(GICD_ICPENDR, 50), BIT(50 % 32));
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 50)), 0);
    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 50)), 0);

    /* ICACTIVER on an active listed interrupt from another CPU */
    wr32(bitreg(GICD_ISPENDR, 50), BIT(50 % 32));
    exit_on(BOOT_CPU);
    CHECK_EQ(guest_ack(BOOT_CPU), 50);
    host_cpu = 0;
    wr32(bitreg(GICD_ICACTIVER, 50), BIT(50 % 32));
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 50)), 0);
    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);

    /* priority change while listed: rewritten with the new priority */
    wr32(bitreg(GICD_ISPENDR, 50), BIT(50 % 32));
    exit_on(BOOT_CPU);
    spi_prio(50, 0x48);
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 50), lr_bits(50, 0x48, 1, false));

    /* re-route while listed and pending: handed over through the old owner's exit */
    host_cpu = 0;
    ipi_mask = 0;
    spi_route(50, 3); /* 0.1.1 = CPU 3 */
    CHECK_EQ(ipi_mask, BIT(BOOT_CPU));
    exit_on(3);
    CHECK_EQ(find_lr(3, 50), 0); /* still in the boot CPU's LR */
    ipi_mask = 0;
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 50), 0);
    CHECK_EQ(ipi_mask, BIT(3));
    exit_on(3);
    CHECK_EQ(find_lr(3, 50), lr_bits(50, 0x48, 1, false));

    /* re-route while active: stays on the CPU that took it until deactivated */
    CHECK_EQ(guest_ack(3), 50);
    spi_route(50, 0);
    exit_on(3);
    CHECK_EQ(find_lr(3, 50), lr_bits(50, 0x48, ICH_LR_STATE_ACTIVE, false));
    guest_eoi(3, 50);
    exit_on(3);
    CHECK_EQ(nr_valid_lrs(3), 0);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_group_enable_gate(void)
{
    bring_up();

    spi_edge(70);
    spi_route(70, 0);
    spi_enable(70);
    wr32(bitreg(GICD_ISPENDR, 70), BIT(70 % 32));
    exit_on(BOOT_CPU);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0); /* EnableGrp1 = 0 */

    host_cpu = 0;
    ipi_mask = 0;
    grp1_on();
    CHECK(ipi_mask & BIT(BOOT_CPU));
    exit_on(BOOT_CPU);
    CHECK(find_lr(BOOT_CPU, 70) & ICH_LR_PENDING_BIT);

    /* unrouted SPI (IROUTER names no vCPU): pending, never listed */
    spi_edge(71);
    wr64(GICD + GICD_IROUTER + 8 * 71, 0x020000);
    spi_enable(71);
    wr32(bitreg(GICD_ISPENDR, 71), BIT(71 % 32));
    for (int v = 0; v < NR_TEST_CPUS; v++) {
        exit_on(cpu_of(v));
        CHECK_EQ(find_lr(cpu_of(v), 71), 0);
    }
}

static void test_active_overflow_eoicount(void)
{
    bring_up();
    grp1_on();

    /* nine interrupts active at once on vCPU 0: the lowest priority one cannot be listed */
    for (u32 i = 0; i < 8; i++) {
        spi_edge(80 + i);
        spi_route(80 + i, 0);
        spi_prio(80 + i, 0x10 + 8 * i);
        spi_enable(80 + i);
    }
    spi_edge(88);
    spi_route(88, 0);
    spi_prio(88, 0xf0);
    spi_enable(88);
    wr32(bitreg(GICD_ISACTIVER, 88), BIT(88 % 32)); /* acknowledged earlier, still active */

    wr32(bitreg(GICD_ISPENDR, 80), 0xff << (80 % 32));
    exit_on(BOOT_CPU);
    /* the active one goes first, then seven of the eight pending */
    CHECK_EQ(find_lr(BOOT_CPU, 88), lr_bits(88, 0xf0, ICH_LR_STATE_ACTIVE, false));
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), NR_LRS);
    CHECK(ich[BOOT_CPU].hcr & ICH_HCR_UIE);

    for (int i = 0; i < 7; i++)
        CHECK_EQ(guest_ack(BOOT_CPU), 80 + i);
    /* the eighth arrives through the underflow refill once enough has drained... */
    exit_on(BOOT_CPU);
    /* ...but with 8 active (7 acked + 88) and one pending, the pending one cannot get an LR */
    CHECK_EQ(find_lr(BOOT_CPU, 87), 0);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), NR_LRS);

    /* take 87 in by making room: the guest finishes 86 (listed) */
    guest_eoi(BOOT_CPU, 86);
    exit_on(BOOT_CPU);
    CHECK(find_lr(BOOT_CPU, 87) & ICH_LR_PENDING_BIT);
    CHECK_EQ(guest_ack(BOOT_CPU), 87);

    /* now 8 active and 88 is the ninth: force it out by activating one more higher-priority one */
    spi_edge(89);
    spi_route(89, 0);
    spi_prio(89, 0x08);
    spi_enable(89);
    wr32(bitreg(GICD_ISACTIVER, 89), BIT(89 % 32));
    exit_on(BOOT_CPU);
    CHECK_EQ(find_lr(BOOT_CPU, 88), 0); /* lowest-priority active left out */
    CHECK(ich[BOOT_CPU].hcr & ICH_HCR_LRENPIE);

    /* the guest completes everything without an exit in between, 88 last (reverse order) */
    u32 order[] = {89, 87, 85, 84, 83, 82, 81, 80, 88};
    for (u32 i = 0; i < ARRAY_SIZE(order); i++)
        guest_eoi(BOOT_CPU, order[i]);
    CHECK_EQ(FIELD_GET(ICH_HCR_EOICOUNT, ich[BOOT_CPU].hcr), 1);
    CHECK(calc_misr(BOOT_CPU) & ICH_MISR_LRENP);
    maint_on(BOOT_CPU);
    CHECK_EQ(calc_misr(BOOT_CPU), 0);
    CHECK_EQ(FIELD_GET(ICH_HCR_EOICOUNT, ich[BOOT_CPU].hcr), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISACTIVER, 88)), 0);
    CHECK_EQ(nr_valid_lrs(BOOT_CPU), 0);
    CHECK_EQ(ich[BOOT_CPU].hcr, ICH_HCR_EN);
}

static void test_emulated_level_spi(void)
{
    bring_up();
    grp1_on();

    /* e.g. the PL011 model: a level SPI driven by software */
    spi_route(90, 4);
    spi_enable(90);
    host_cpu = BOOT_CPU;
    ipi_mask = 0;
    hv_vgic3_set_spi_level(90, true);
    CHECK_EQ(ipi_mask, BIT(4));
    exit_on(4);
    CHECK_EQ(find_lr(4, 90), lr_bits(90, 0, 1, true));
    CHECK_EQ(guest_ack(4), 90);
    host_cpu = 4;
    hv_vgic3_set_spi_level(90, false); /* the guest drained the FIFO */
    exit_on(4);
    guest_eoi(4, 90);
    maint_on(4);
    CHECK_EQ(nr_valid_lrs(4), 0);

    /* edge SPI injection */
    spi_edge(91);
    spi_route(91, 4);
    spi_enable(91);
    hv_vgic3_inject_spi(91);
    exit_on(4);
    CHECK_EQ(find_lr(4, 91), lr_bits(91, 0, 1, false));
}

static void test_pl011_resample(void)
{
    const int cpu = 1; /* vCPU 2 */

    bring_up();
    grp1_on();
    spi_route(95, 2);
    spi_prio(95, 0x60);
    spi_enable(95);

    /* RX data: hv_win_pl011.c reports the 0 -> 1 edge from wherever the access happened */
    pl011_line = true;
    host_cpu = BOOT_CPU;
    ipi_mask = 0;
    hv_win_pl011_raise(95);
    CHECK_EQ(lock_depth, 0);
    CHECK_EQ(ipi_mask, BIT(cpu));
    exit_on(cpu);
    CHECK_EQ(find_lr(cpu, 95), lr_bits(95, 0x60, 1, true));

    /* the guest drains the FIFO (line drops, no callback), EOIs: resampled low, done */
    CHECK_EQ(guest_ack(cpu), 95);
    pl011_line = false;
    guest_eoi(cpu, 95);
    maint_on(cpu);
    CHECK_EQ(nr_valid_lrs(cpu), 0);
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 95)), 0);

    /* more data arrived before the EOI: resampled high, delivered again */
    pl011_line = true;
    hv_win_pl011_raise(95);
    exit_on(cpu);
    CHECK_EQ(guest_ack(cpu), 95);
    guest_eoi(cpu, 95);
    maint_on(cpu);
    CHECK_EQ(find_lr(cpu, 95), lr_bits(95, 0x60, 1, true));
    pl011_line = false;
    guest_eoi(cpu, guest_ack(cpu));
    maint_on(cpu);
    CHECK_EQ(nr_valid_lrs(cpu), 0);

    /* out-of-range GSIVs are ignored */
    hv_win_pl011_raise(20);
    hv_win_pl011_raise(1020);
    CHECK_EQ(lock_depth, 0);
}

static void test_cpu_off_on(void)
{
    bring_up();
    grp1_on();

    spi_edge(100);
    spi_route(100, 3);
    spi_enable(100);
    wr32(bitreg(GICD_ISPENDR, 100), BIT(100 % 32));
    exit_on(3);
    CHECK(find_lr(3, 100) & ICH_LR_PENDING_BIT);

    /* CPU_OFF then CPU_ON: the new incarnation starts clean and gets the pending IRQ back */
    hv_started_cpus[3] = false;
    host_cpu = 3;
    hv_vgic3_cpu_init();
    hv_started_cpus[3] = true;
    CHECK_EQ(nr_valid_lrs(3), 0);
    CHECK_EQ(ich[3].hcr, ICH_HCR_EN);
    CHECK_EQ(rd32(bitreg(GICD_ISPENDR, 100)), BIT(100 % 32));
    exit_on(3);
    CHECK(find_lr(3, 100) & ICH_LR_PENDING_BIT);

    /* a CPU that is not a vCPU (reserved/host) is ignored everywhere */
    hv_win.cpu_vcpu[7] = -1;
    host_cpu = 7;
    hv_vgic3_cpu_init();
    hv_vgic3_flush();
    hv_vgic3_maintenance();
    hv_vgic3_set_ppi_level(7, GIC_PPI_CNTP, true);
    CHECK_EQ(ich[7].hcr, 0);
    host_cpu = BOOT_CPU;
}

static void test_stuck_maintenance_guard(void)
{
    bring_up();

    /* an LR EL2 does not know about (should never happen) keeps EOI asserted: disable, don't storm
     */
    ich[BOOT_CPU].lr[7] = ICH_LR_EOI | FIELD_PREP(ICH_LR_VINTID, 33);
    CHECK_EQ(calc_misr(BOOT_CPU), ICH_MISR_EOI);
    log_lines = 0;
    maint_on(BOOT_CPU);
    CHECK_EQ(ich[BOOT_CPU].hcr & ICH_HCR_EN, 0);
    CHECK_EQ(log_lines, 1);
    exit_on(BOOT_CPU); /* no further LR traffic on that vCPU */
    CHECK_EQ(ich[BOOT_CPU].hcr & ICH_HCR_EN, 0);
}

/* --- smp2: the J716s topology with one and two vCPUs (SMP-PLAN M4) ------------------------------ */

/* boot CPU 0 = MPIDR 0x80040000 (0.4.0), CPU 1 = 0x80040001 (0.4.1), as in the hardware hv.log */
static void bring_up_j716s(u32 n)
{
    reset_world();
    for (int i = 0; i < MAX_CPUS; i++)
        hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;
    mpidr_of[0] = 0x80040000;
    mpidr_of[1] = 0x80040001;
    for (u32 v = 0; v < n; v++) {
        hv_win.vcpu_cpu[v] = v;
        hv_win.cpu_vcpu[v] = v;
    }
    hv_win.nr_vcpus = n;
    host_cpu = 0;
    CHECK_EQ(hv_vgic3_init(GICD, GICR, n), 0);
    for (u32 v = 0; v < n; v++) {
        host_cpu = v;
        hv_vgic3_cpu_init();
        hv_started_cpus[v] = true;
    }
    host_cpu = 0;
    ipi_mask = 0;
}

static void exit_on_cpu(int cpu)
{
    int saved = host_cpu;
    host_cpu = cpu;
    hv_vgic3_maintenance();
    hv_vgic3_flush();
    CHECK_EQ(lock_depth, 0);
    host_cpu = saved;
}

static void test_smp2_j716s_two_vcpus(void)
{
    bring_up_j716s(2);
    grp1_on();

    /* GICR_TYPER: vCPU 1 = 0.4.1, Last on the second frame */
    CHECK_EQ(FIELD_GET(GICR_TYPER_AFFINITY, rd64(gicr_rd(1, GICR_TYPER))), 0x040001);
    CHECK(!(rd64(gicr_rd(0, GICR_TYPER)) & GICR_TYPER_LAST));
    CHECK(rd64(gicr_rd(1, GICR_TYPER)) & GICR_TYPER_LAST);

    /* an SPI the guest never routed: IROUTER reads the boot vCPU, and it is delivered there */
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 40), 0x040000);
    spi_prio(40, 0x40);
    spi_enable(40);
    wr32(bitreg(GICD_ISPENDR, 40), BIT(40 % 32));
    CHECK_EQ(ipi_mask, 0); /* written from CPU 0, its own vCPU: no kick */
    exit_on_cpu(0);
    CHECK_EQ(find_lr(0, 40), lr_bits(40, 0x40, 1, true));
    CHECK_EQ(nr_valid_lrs(1), 0);

    /* routed to 0.4.1: kicked and listed on CPU 1 only */
    wr64(GICD + GICD_IROUTER + 8 * 41, 0x040001);
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 41), 0x040001);
    spi_prio(41, 0x50);
    spi_enable(41);
    ipi_mask = 0;
    wr32(bitreg(GICD_ISPENDR, 41), BIT(41 % 32));
    CHECK_EQ(ipi_mask, BIT(1));
    exit_on_cpu(1);
    CHECK_EQ(find_lr(1, 41), lr_bits(41, 0x50, 1, true));
    exit_on_cpu(0);
    CHECK_EQ(find_lr(0, 41), 0);

    /* SGI 2 from vCPU 0 to Aff2=4 Aff1=0 TargetList bit 1 (0.4.1) */
    host_cpu = 0;
    ipi_mask = 0;
    CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 4) | FIELD_PREP(ICC_SGI1R_TARGETLIST, 2) |
                                FIELD_PREP(ICC_SGI1R_INTID, 2)));
    CHECK_EQ(ipi_mask, BIT(1));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)) & BIT(2), BIT(2));
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISPENDR0)) & BIT(2), 0);
    exit_on_cpu(1);
    CHECK_EQ(find_lr(1, 2), lr_bits(2, 0, 1, false));

    /* SGI 3 broadcast (IRM) from vCPU 1: only vCPU 0 */
    host_cpu = 1;
    ipi_mask = 0;
    CHECK(hv_vgic3_handle_sgi1r(ICC_SGI1R_IRM | FIELD_PREP(ICC_SGI1R_INTID, 3)));
    CHECK_EQ(ipi_mask, BIT(0));
    CHECK_EQ(rd32(gicr_sgi(0, GICR_ISPENDR0)) & BIT(3), BIT(3));
    CHECK_EQ(rd32(gicr_sgi(1, GICR_ISPENDR0)) & BIT(3), 0);
    host_cpu = 0;

    /* timer PPIs stay per CPU: CPU 1's CNTV does not touch vCPU 0 */
    wr32(gicr_sgi(0, GICR_ISENABLER0), BIT(GIC_PPI_CNTV));
    wr32(gicr_sgi(1, GICR_ISENABLER0), BIT(GIC_PPI_CNTV));
    host_cpu = 1;
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, true);
    exit_on_cpu(1);
    host_cpu = 0;
    CHECK(find_lr(1, GIC_PPI_CNTV) != 0);
    exit_on_cpu(0);
    CHECK_EQ(find_lr(0, GIC_PPI_CNTV), 0);

    /* periodic per-vCPU report (hv_win_tick): armed on the first call, then one line per vCPU */
    int lines = log_lines;
    hv_vgic3_smp_report(1000, 100);
    hv_vgic3_smp_report(1099, 100);
    CHECK_EQ(log_lines, lines);
    hv_vgic3_smp_report(1100, 100);
    CHECK_EQ(log_lines, lines + 2);
    CHECK(strstr(last_log, "vCPU 1 (CPU 1): timer PPI edges 1, SGIs received 1") != NULL);
    /* smp2b guest-progress signal: the edge since the last report, then none in the next period */
    CHECK(strstr(last_log, ", timer IRQ since last report (+1)") != NULL);
    hv_vgic3_smp_report(1200, 100);
    CHECK_EQ(log_lines, lines + 4);
    CHECK(strstr(last_log, "vCPU 1 (CPU 1): timer PPI edges 1,") != NULL);
    CHECK(strstr(last_log, "NO timer IRQ since last report (+0)") != NULL);
    host_cpu = 1;
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, false);
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, true);
    host_cpu = 0;
    hv_vgic3_smp_report(1300, 100);
    CHECK(strstr(last_log, "timer PPI edges 2,") != NULL);
    CHECK(strstr(last_log, ", timer IRQ since last report (+1)") != NULL);
    CHECK_EQ(bad_sysreg, 0);
}

/*
 * smp2e (review F1): hv_vgic3_wfi_pending() answers "would a native WFI on this vCPU complete now":
 * vCPU dirty (another CPU's change, not yet folded), or a list register that signals under the GICv3
 * rule (pending, group 1 enabled in VMCR, priority above VPMR and above the running priority).
 */
static void test_smp2e_wfi_pending(void)
{
    const int W = HV_VGIC3_WFI_WAKE, S = HV_VGIC3_WFI_STRICT;

    bring_up_j716s(2);
    grp1_on();
    ich[1].vmcr = (0xf0UL << 24) | BIT(1); /* VPMR 0xf0, VENG1 */

    exit_on_cpu(1); /* fold the cpu_init dirty mark */
    host_cpu = 1;
    CHECK_EQ(hv_vgic3_wfi_pending(), 0); /* nothing pending: the wait may idle */

    /* SPI 41 routed to vCPU 1, made pending from CPU 0: vCPU 1 is dirty and kicked */
    host_cpu = 0;
    wr64(GICD + GICD_IROUTER + 8 * 41, 0x040001);
    spi_prio(41, 0x50);
    spi_enable(41);
    ipi_mask = 0;
    wr32(bitreg(GICD_ISPENDR, 41), BIT(41 % 32));
    CHECK_EQ(ipi_mask, BIT(1));
    host_cpu = 1;
    CHECK_EQ(hv_vgic3_wfi_pending(), HV_VGIC3_WFI_DIRTY); /* dirty: wake even before the rebuild */

    exit_on_cpu(1); /* hv_update_fiq's rebuild puts it in an LR */
    host_cpu = 1;
    CHECK_EQ(find_lr(1, 41), lr_bits(41, 0x50, 1, true));
    CHECK_EQ(hv_vgic3_wfi_pending(), W | S); /* LR pending, 0x50 < VPMR 0xf0: both rules wake */

    /* priority mask: equal VPMR masks it under both rules, a higher VPMR does not */
    ich[1].vmcr = (0x50UL << 24) | BIT(1);
    CHECK_EQ(hv_vgic3_wfi_pending(), 0);
    ich[1].vmcr = (0x58UL << 24) | BIT(1);
    CHECK_EQ(hv_vgic3_wfi_pending(), W | S);

    /* smp2f: group 1 disabled at the virtual CPU interface: the strict rule sleeps, KVM's wakes */
    ich[1].vmcr = 0xf0UL << 24;
    CHECK_EQ(hv_vgic3_wfi_pending(), W);
    ich[1].vmcr = (0xf0UL << 24) | BIT(1);

    /* smp2f: running priority: an active 0x40 blocks 0x50 under the strict rule only */
    ich[1].apr[1][0] = BIT(0x40 >> 3);
    CHECK_EQ(hv_vgic3_wfi_pending(), W);
    ich[1].apr[1][0] = BIT(0x60 >> 3);
    CHECK_EQ(hv_vgic3_wfi_pending(), W | S);
    ich[1].apr[1][0] = 0;

    /* per-vCPU: CPU 0 has nothing of its own */
    exit_on_cpu(0);
    host_cpu = 0;
    CHECK_EQ(hv_vgic3_wfi_pending(), 0);

    /* once the guest has acknowledged it (active, not pending): no longer a wake condition */
    CHECK_EQ(guest_ack(1), 41);
    host_cpu = 1;
    CHECK_EQ(hv_vgic3_wfi_pending(), 0);

    /* a firing timer PPI folded by the exit path is a wake condition (held with IMASK at EL2) */
    wr32(gicr_sgi(1, GICR_ISENABLER0), BIT(GIC_PPI_CNTV));
    wr(gicr_sgi(1, GICR_IPRIORITYR0 + GIC_PPI_CNTV), 0x20, 0);
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, true);
    exit_on_cpu(1);
    host_cpu = 1;
    CHECK_EQ(hv_vgic3_wfi_pending(), W | S);
    host_cpu = 0;
    CHECK_EQ(bad_sysreg, 0);
}

/*
 * smp2f: the interface save/compare/restore around the EL2 physical WFI (hv_vgic3_vctx_read/write with
 * wfi_vctx_diff), the acknowledge / sender statistics and the boot-CPU dump, and the stall watch.
 */
static void test_smp2f_vctx_and_diag(void)
{
    bring_up_j716s(2);
    grp1_on();
    ich[1].vmcr = (0xf0UL << 24) | BIT(1);

    /* the timer PPI of vCPU 1, firing and listed */
    wr32(gicr_sgi(1, GICR_ISENABLER0), BIT(GIC_PPI_CNTV));
    wr(gicr_sgi(1, GICR_IPRIORITYR0 + GIC_PPI_CNTV), 0x20, 0);
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, true);
    exit_on_cpu(1);
    host_cpu = 1;
    CHECK(find_lr(1, GIC_PPI_CNTV) != 0);

    struct wfi_vctx saved, now;
    memset(&saved, 0, sizeof(saved));
    memset(&now, 0, sizeof(now));
    hv_vgic3_vctx_read(&saved);
    CHECK_EQ(saved.nr_lr, NR_LRS);
    CHECK_EQ(saved.nr_apr, 1);
    CHECK_EQ(saved.ich_hcr, ich[1].hcr);
    CHECK(saved.ich_hcr & ICH_HCR_EN);
    CHECK_EQ(saved.ich_vmcr, ich[1].vmcr);

    /* nothing changed: no difference, nothing to write */
    hv_vgic3_vctx_read(&now);
    CHECK_EQ(wfi_vctx_diff(&saved, &now), 0);

    /* a retention WFI that loses the interface: En, the LRs and VMCR */
    struct ich keep = ich[1];
    ich[1].hcr = 0;
    ich[1].vmcr = 0;
    memset(ich[1].lr, 0, sizeof(ich[1].lr));
    hv_vgic3_vctx_read(&now);
    u32 d = wfi_vctx_diff(&saved, &now);
    CHECK_EQ(d, WFI_VCTX_HCR | WFI_VCTX_VMCR | WFI_VCTX_LR);
    hv_vgic3_vctx_write(&saved, d);
    CHECK_EQ(ich[1].hcr, keep.hcr);
    CHECK_EQ(ich[1].vmcr, keep.vmcr);
    CHECK(!memcmp(ich[1].lr, keep.lr, sizeof(keep.lr)));
    CHECK_EQ(hv_vgic3_wfi_pending() & HV_VGIC3_WFI_WAKE, HV_VGIC3_WFI_WAKE);

    /* the guest takes the timer: counted as a timer acknowledge at the next fold */
    CHECK_EQ(guest_ack(1), GIC_PPI_CNTV);
    exit_on_cpu(1);

    /* SGI 3 from vCPU 0 to vCPU 1: counted by sender and number; the guest takes it */
    host_cpu = 0;
    CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 4) | FIELD_PREP(ICC_SGI1R_INTID, 3) |
                                FIELD_PREP(ICC_SGI1R_TARGETLIST, BIT(1))));
    exit_on_cpu(1);
    CHECK_EQ(guest_ack(1), 3);
    exit_on_cpu(1);

    host_cpu = 0;
    log_clear();
    hv_vgic3_smp_dump();
    CHECK(strstr(all_log, "vCPU 1 CPU 1 on") != NULL);
    CHECK(strstr(all_log, "timer acks 1;") != NULL);
    CHECK(strstr(all_log, "(from v0 1 v1 0 v2 0 v3+ 0) acks 1;") != NULL);
    CHECK(strstr(all_log, "vCPU 1 SGI by id: 3:1;") != NULL);
    CHECK(strstr(all_log, "vCPU 1 PPI 27: enabled 1 line 1") != NULL);
    /* smp2g: received versus taken by sender and SGI number; the last interrupt taken */
    CHECK(strstr(all_log, "vCPU 1 SGI 3 from v0: rx 1 taken 1\n") != NULL);
    CHECK(strstr(all_log, "vCPU 1 interrupts taken 2, last intid 3 ") != NULL);
    CHECK(strstr(all_log, "NOT TAKEN") == NULL);
    /* three more SGI 5 from vCPU 1 to vCPU 0 that vCPU 0 never takes: flagged */
    host_cpu = 1;
    for (int i = 0; i < 3; i++)
        CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 4) | FIELD_PREP(ICC_SGI1R_INTID, 5) |
                                    FIELD_PREP(ICC_SGI1R_TARGETLIST, BIT(0))));
    exit_on_cpu(0);
    host_cpu = 0;
    log_clear();
    hv_vgic3_smp_dump();
    CHECK(strstr(all_log, "vCPU 0 SGI 5 from v1: rx 3 taken 0 (NOT TAKEN)") != NULL);
    CHECK(strstr(all_log, "vCPU 0 interrupts taken 0, last intid 0 -1 ms ago") != NULL);

    /* stall watch: a vCPU without timer edges for the whole period is reported once per episode */
    const u64 P = 20000000000UL;
    CHECK_EQ(hv_vgic3_stall_check(1000, P), -1); /* first look: baseline */
    CHECK_EQ(hv_vgic3_stall_check(1000 + P - 1, P), -1);
    host_cpu = 0;
    hv_vgic3_set_ppi_level(0, GIC_PPI_CNTV, true); /* vCPU 0 makes progress, vCPU 1 does not */
    CHECK_EQ(hv_vgic3_stall_check(1000 + P / 2, P), -1);
    CHECK_EQ(hv_vgic3_stall_check(1000 + P, P), 1);
    CHECK_EQ(hv_vgic3_stall_check(1000 + 2 * P, P), 0); /* vCPU 0 now stalled too: reported once */
    CHECK_EQ(hv_vgic3_stall_check(1000 + 3 * P, P), -1); /* both already reported */
    host_cpu = 1;
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, false);
    hv_vgic3_set_ppi_level(1, GIC_PPI_CNTV, true); /* vCPU 1 progresses: re-armed */
    host_cpu = 0;
    CHECK_EQ(hv_vgic3_stall_check(1000 + 4 * P, P), -1);
    CHECK_EQ(hv_vgic3_stall_check(1000 + 5 * P, P), 1);

    /* one vCPU: no stall watch */
    bring_up_j716s(1);
    CHECK_EQ(hv_vgic3_stall_check(1000, P), -1);
    CHECK_EQ(hv_vgic3_stall_check(1000 + 10 * P, P), -1);
    CHECK_EQ(bad_sysreg, 0);
}

/* one vCPU: the hardware-proven parent behaviour (no default SPI target) is unchanged */
static void test_smp2_j716s_one_vcpu_unchanged(void)
{
    bring_up_j716s(1);
    grp1_on();
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 40), 0);
    spi_enable(40);
    wr32(bitreg(GICD_ISPENDR, 40), BIT(40 % 32));
    exit_on_cpu(0);
    CHECK_EQ(find_lr(0, 40), 0); /* pending, no target until the guest writes IROUTER */
    wr64(GICD + GICD_IROUTER + 8 * 40, 0x040000);
    exit_on_cpu(0);
    CHECK(find_lr(0, 40) != 0);
    CHECK_EQ(ipi_mask, 0);
    int lines = log_lines;
    hv_vgic3_smp_report(1000, 100);
    hv_vgic3_smp_report(5000, 100);
    CHECK_EQ(log_lines, lines); /* no periodic report with one vCPU */
}

/* --- smpN: 6 and 18 vCPUs on the real J716s topology (SMP-PLAN M5a/M5b) ------------------------- */

/* hv.log spin table: cluster 0 = 0.4.0-5 (boot cluster, M), cluster 1 = 0.4.1xx (M), cluster 2 = 0.1.2xx (P) */
static u64 j716s_mpidr(int cpu)
{
    static const u64 cl[3] = {0x80040000, 0x80040100, 0x80010200};
    return cl[cpu / 6] | (cpu % 6);
}

/* vCPU v = m1n1 CPU v (the launcher's ADT order, boot CPU 0 first) */
static void bring_up_j716s_n(u32 n)
{
    reset_world();
    for (int i = 0; i < MAX_CPUS; i++) {
        hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;
        mpidr_of[i] = i < 18 ? j716s_mpidr(i) : 0;
    }
    for (u32 v = 0; v < n; v++)
        hv_win.vcpu_cpu[v] = hv_win.cpu_vcpu[v] = v;
    hv_win.nr_vcpus = n;
    host_cpu = 0;
    CHECK_EQ(hv_vgic3_init(GICD, GICR, n), 0);
    for (u32 v = 0; v < n; v++) {
        host_cpu = v;
        hv_vgic3_cpu_init();
        hv_started_cpus[v] = true;
    }
    host_cpu = 0;
    for (u32 v = 0; v < n; v++)
        exit_on_cpu(v); /* fold the cpu_init dirty marks */
    ipi_mask = 0;
}

static u64 sgi1r_to(u32 v, u32 sgi)
{
    u64 m = j716s_mpidr(v);
    return FIELD_PREP(ICC_SGI1R_AFF2, (m >> 16) & 0xff) | FIELD_PREP(ICC_SGI1R_AFF1, (m >> 8) & 0xff) |
           FIELD_PREP(ICC_SGI1R_TARGETLIST, BIT(m & 0xf)) | FIELD_PREP(ICC_SGI1R_INTID, sgi);
}

static void smpn_case(u32 n)
{
    bring_up_j716s_n(n);
    grp1_on();

    /* redistributors: one frame per vCPU with its own affinity and processor number, Last on the final one */
    for (u32 v = 0; v < n; v++) {
        u64 t = rd64(gicr_rd(v, GICR_TYPER));
        CHECK_EQ(FIELD_GET(GICR_TYPER_AFFINITY, t), j716s_mpidr(v) & 0xffffff);
        CHECK_EQ(FIELD_GET(GICR_TYPER_PROCNUM, t), v);
        CHECK_EQ(!!(t & GICR_TYPER_LAST), v == n - 1);
    }

    /* SGIs between every ordered pair, inside and across clusters: only the target pends, only it is kicked */
    for (u32 a = 0; a < n; a++) {
        for (u32 b = 0; b < n; b++) {
            if (a == b)
                continue;
            u32 sgi = (a + b) % 16;
            host_cpu = a;
            ipi_mask = 0;
            CHECK(hv_vgic3_handle_sgi1r(sgi1r_to(b, sgi)));
            CHECK_EQ(ipi_mask, BIT(b));
            for (u32 v = 0; v < n; v++)
                CHECK_EQ(rd32(gicr_sgi(v, GICR_ISPENDR0)) & BIT(sgi), v == b ? BIT(sgi) : 0);
            exit_on_cpu(b);
            CHECK_EQ(find_lr(b, sgi), lr_bits(sgi, 0, 1, false));
            wr32(gicr_sgi(b, GICR_ICPENDR0), BIT(sgi));
            exit_on_cpu(b);
            CHECK_EQ(find_lr(b, sgi), 0);
        }
    }
    host_cpu = 0;

    /* one SGI1R naming several CPUs of cluster 1 (Aff1 = 1): all of them, nobody else */
    if (n > 8) {
        host_cpu = 0;
        ipi_mask = 0;
        CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 4) | FIELD_PREP(ICC_SGI1R_AFF1, 1) |
                                    FIELD_PREP(ICC_SGI1R_TARGETLIST, BIT(0) | BIT(2) | BIT(5)) |
                                    FIELD_PREP(ICC_SGI1R_INTID, 9)));
        CHECK_EQ(ipi_mask, BIT(6) | BIT(8) | BIT(11));
        for (u32 v = 0; v < n; v++) {
            CHECK_EQ(rd32(gicr_sgi(v, GICR_ISPENDR0)) & BIT(9), (v == 6 || v == 8 || v == 11) ? BIT(9) : 0);
            wr32(gicr_sgi(v, GICR_ICPENDR0), BIT(9));
        }
        /* cluster 2 is Aff2 = 1: the cluster-0 affinity with Aff1 = 2 is nobody */
        ipi_mask = 0;
        CHECK(hv_vgic3_handle_sgi1r(FIELD_PREP(ICC_SGI1R_AFF2, 4) | FIELD_PREP(ICC_SGI1R_AFF1, 2) |
                                    FIELD_PREP(ICC_SGI1R_TARGETLIST, 0x3f) | FIELD_PREP(ICC_SGI1R_INTID, 9)));
        CHECK_EQ(ipi_mask, 0);
    }

    /* broadcast (IRM) from the last vCPU: every other vCPU */
    host_cpu = n - 1;
    ipi_mask = 0;
    CHECK(hv_vgic3_handle_sgi1r(ICC_SGI1R_IRM | FIELD_PREP(ICC_SGI1R_INTID, 1)));
    CHECK_EQ(ipi_mask, (u32)(BIT(n - 1) - 1));
    for (u32 v = 0; v < n; v++) {
        CHECK_EQ(rd32(gicr_sgi(v, GICR_ISPENDR0)) & BIT(1), v == n - 1 ? 0 : BIT(1));
        wr32(gicr_sgi(v, GICR_ICPENDR0), BIT(1));
        exit_on_cpu(v);
    }
    host_cpu = 0;

    /* timer wake on each vCPU: its own CNTV PPI lists on it and nowhere else */
    for (u32 v = 0; v < n; v++) {
        wr32(gicr_sgi(v, GICR_ISENABLER0), BIT(GIC_PPI_CNTV));
        host_cpu = v;
        hv_vgic3_set_ppi_level(v, GIC_PPI_CNTV, true);
        exit_on_cpu(v);
        CHECK(find_lr(v, GIC_PPI_CNTV) != 0);
        for (u32 w = 0; w < n; w++)
            if (w != v)
                CHECK(lr_state(find_lr(w, GIC_PPI_CNTV)) == 0 || w < v);
        hv_vgic3_set_ppi_level(v, GIC_PPI_CNTV, false);
        exit_on_cpu(v);
    }
    host_cpu = 0;

    /* an SPI routed to the last vCPU (0.1.2.5 for 18): kicked and listed there only */
    u64 last = j716s_mpidr(n - 1) & 0xffffff;
    wr64(GICD + GICD_IROUTER + 8 * 45, last);
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 45), last);
    spi_prio(45, 0x60);
    spi_enable(45);
    ipi_mask = 0;
    wr32(bitreg(GICD_ISPENDR, 45), BIT(45 % 32));
    CHECK_EQ(ipi_mask, BIT(n - 1));
    for (u32 v = 0; v < n; v++)
        exit_on_cpu(v);
    CHECK_EQ(find_lr(n - 1, 45), lr_bits(45, 0x60, 1, true));
    for (u32 v = 0; v + 1 < n; v++)
        CHECK_EQ(find_lr(v, 45), 0);
    /* an unrouted SPI still goes to vCPU 0 (smp2 rule, unchanged for N > 2) */
    CHECK_EQ(rd64(GICD + GICD_IROUTER + 8 * 46), 0x040000);

    /* the dump: every vCPU, with its own taken statistics (vCPU 4 and up had none before smpN) */
    host_cpu = 0;
    CHECK(hv_vgic3_handle_sgi1r(sgi1r_to(n - 1, 7)));
    exit_on_cpu(n - 1);
    CHECK_EQ(guest_ack(n - 1), 7); /* SPI 45 (0x60) and SGI 7 (0) are both listed; priority 0 wins */
    log_clear();
    hv_vgic3_smp_dump();
    char want[160];
    for (u32 v = 0; v < n; v++) {
        snprintf(want, sizeof(want), "vCPU %u CPU %u on", v, v);
        CHECK(strstr(all_log, want) != NULL);
    }
    exit_on_cpu(n - 1);
    log_clear();
    hv_vgic3_smp_dump();
    snprintf(want, sizeof(want), "vCPU %u interrupts taken ", n - 1);
    char *p = strstr(all_log, want);
    CHECK(p != NULL);
    CHECK(p && strncmp(p + strlen(want), "0,", 2) != 0);
    snprintf(want, sizeof(want), "vCPU %u SGI 7 from v0: rx 1", n - 1);
    CHECK(strstr(all_log, want) != NULL);

    /* periodic report: one line per vCPU */
    int lines = log_lines;
    hv_vgic3_smp_report(1000, 100);
    hv_vgic3_smp_report(1100, 100);
    CHECK_EQ(log_lines, lines + (int)n);

    /* stall watch with N > 2: an idle secondary never triggers the automatic dump, vCPU 0 does */
    const u64 P = 20000000000UL;
    host_cpu = 0;
    CHECK_EQ(hv_vgic3_stall_check(1000, P), -1);
    CHECK_EQ(hv_vgic3_stall_check(1000 + P, P), 0);
    CHECK_EQ(hv_vgic3_stall_check(1000 + 2 * P, P), -1); /* once per episode */
    CHECK_EQ(lock_depth, 0);
    CHECK_EQ(bad_sysreg, 0);
}

static void test_smpn_j716s_6_vcpus(void)
{
    smpn_case(6);
}

static void test_smpn_j716s_18_vcpus(void)
{
    smpn_case(18);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");

#define RUN(t)                                                                                     \
    do {                                                                                           \
        int before = fails;                                                                        \
        cur_test = #t;                                                                             \
        t();                                                                                       \
        printf("%-40s %s\n", #t, fails == before ? "ok" : "FAILED");                               \
    } while (0)

    RUN(test_init_and_id_registers);
    RUN(test_distributor_registers);
    RUN(test_redistributor_banking);
    RUN(test_lr_allocation_and_overflow);
    RUN(test_sgi_routing);
    RUN(test_aic_line_and_eoi_maintenance);
    RUN(test_timer_ppi_level);
    RUN(test_changes_while_listed);
    RUN(test_group_enable_gate);
    RUN(test_active_overflow_eoicount);
    RUN(test_emulated_level_spi);
    RUN(test_pl011_resample);
    RUN(test_cpu_off_on);
    RUN(test_stuck_maintenance_guard);
    RUN(test_smp2_j716s_two_vcpus);
    RUN(test_smp2_j716s_one_vcpu_unchanged);
    RUN(test_smp2e_wfi_pending);
    RUN(test_smp2f_vctx_and_diag);
    RUN(test_smpn_j716s_6_vcpus);
    RUN(test_smpn_j716s_18_vcpus);

    /* sched1: kick-to-wake timestamps cover every IPI the model sent, and nothing else */
    cur_test = "sched1_kick_note";
    CHECK(kick_ipis > 0);
    CHECK_EQ(kick_notes, kick_ipis);
    CHECK_EQ(kick_noted, 0);
    printf("%-40s %s (%lu IPIs)\n", "sched1_kick_note", kick_notes == kick_ipis && !kick_noted ? "ok" : "FAILED",
           (unsigned long)kick_ipis);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
