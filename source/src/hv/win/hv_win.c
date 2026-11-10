/* SPDX-License-Identifier: MIT */
/*
 * Windows EL2 runtime: configuration, HCR policy and exception hooks.
 * See hv_win.h for the model overview. Nothing here runs unless the host calls P_HV_WIN_CONFIGURE.
 */

#include "hv_win.h"
#include "aic.h"
#include "aic_regs.h"
#include "cpu_regs.h"
#include "dart.h"
#include "hv_win_pmu_rewrite.h"
#include "hv_win_regs.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"

#include "../hv.h"

struct hv_win_state hv_win = {
    .irq_model = HV_WIN_IRQ_NONE,
    .reserved_cpu = -1,
};

/* Diagnostic-only ring: no I/O on the trap path, no status-register writes. */
#define WIN_DIAG_COUNT 16
struct win_diag_entry {
    u64 sequence;
    u64 stage;
    u64 pc;
    u64 far;
    u64 esr;
    u64 spsr;
    u64 detail;
};
static volatile struct {
    u64 next;
    u64 pc, far, esr, spsr;
    struct win_diag_entry entries[WIN_DIAG_COUNT];
} win_diag[MAX_CPUS];
static volatile bool win_diag_frozen;

void hv_win_diag_note(enum hv_win_diag_stage stage, const struct exc_info *ctx, u64 detail)
{
    if (!hv_win_enabled() || win_diag_frozen)
        return;
    unsigned int cpu = smp_id();
    if (cpu >= MAX_CPUS)
        return;
    if (ctx) {
        win_diag[cpu].pc = ctx->elr;
        win_diag[cpu].far = ctx->far;
        win_diag[cpu].esr = ctx->esr;
        win_diag[cpu].spsr = ctx->spsr;
    }
    u64 sequence = ++win_diag[cpu].next;
    volatile struct win_diag_entry *entry = &win_diag[cpu].entries[(sequence - 1) % WIN_DIAG_COUNT];
    entry->sequence = 0; /* A nested fatal exception skips a partially written slot. */
    entry->stage = stage;
    entry->pc = win_diag[cpu].pc;
    entry->far = win_diag[cpu].far;
    entry->esr = win_diag[cpu].esr;
    entry->spsr = win_diag[cpu].spsr;
    entry->detail = detail;
    entry->sequence = sequence;
}

void hv_win_diag_dump(void)
{
    static const char *const stages[] = {
        "trap",           "vuart", "usb-events", "usb-events-done", "can-write",
        "can-write-done", "write", "write-done", "vuart-done",
    };
    if (!hv_win_enabled() || win_diag_frozen)
        return;
    win_diag_frozen = true;
    unsigned int cpu = smp_id();
    if (cpu >= MAX_CPUS)
        return;
    /* Snapshot before printing: console output itself can process USB events. */
    u64 sts = mrs(SYS_IMP_APL_L2C_ERR_STS);
    u64 adr = mrs(SYS_IMP_APL_L2C_ERR_ADR);
    u64 inf = mrs(SYS_IMP_APL_L2C_ERR_INF);
    u64 last = win_diag[cpu].next;
    u64 first = last > WIN_DIAG_COUNT ? last - WIN_DIAG_COUNT + 1 : 1;
    printf("HV/win diag: CPU %u L2 sts=%lx adr=%lx inf=%lx last=%lu\n", cpu, sts, adr, inf, last);
    for (u64 sequence = first; sequence <= last; sequence++) {
        volatile struct win_diag_entry *entry =
            &win_diag[cpu].entries[(sequence - 1) % WIN_DIAG_COUNT];
        if (entry->sequence != sequence)
            continue;
        const char *stage =
            entry->stage < sizeof(stages) / sizeof(stages[0]) ? stages[entry->stage] : "?";
        printf("HV/win diag: %lu %s pc=%lx far=%lx esr=%lx spsr=%lx detail=%lx\n", sequence, stage,
               entry->pc, entry->far, entry->esr, entry->spsr, entry->detail);
    }
}
/* End diagnostic-only ring. */

/*
 * PMUv3 traps (EL2-RUNTIME item 6). The T6050 has no PMUv3, but Windows' boot manager, winload and
 * the kernel access PMCCNTR_EL0 and friends without looking at ID_AA64DFR0_EL1.PMUVer. MDCR_EL2.TPM
 * traps every PMU register, TPMCR PMCR_EL0; with FEAT_FGT the HDFGRTR_EL2/HDFGWTR_EL2 PMU bits trap
 * them as well (bit numbers from the Arm ARM register descriptions, positive polarity only).
 * Whether an EL1 access on an implementation without PMUv3 traps at all or UNDEFs at EL1 first is
 * what the smoke guest's pmu probe answers on hardware.
 */
#define MDCR_TPMCR BIT(5)
#define MDCR_TPM   BIT(6)

#define SYS_HDFGRTR_EL2 sys_reg(3, 4, 3, 1, 4)
#define SYS_HDFGWTR_EL2 sys_reg(3, 4, 3, 1, 5)
/* bits 12..19: PMEVCNTRn, PMEVTYPERn, PMCCFILTR, PMCCNTR, PMCNTEN, PMINTEN, PMOVS, PMSELR */
#define HDFGxTR_PMU_COMMON GENMASK(19, 12)
#define HDFGRTR_PMU                                                                                \
    (HDFGxTR_PMU_COMMON | BIT(22) /* PMMIR */ | BIT(57) /* PMUSERENR */ | BIT(58) /* PMCEIDn */)
#define HDFGWTR_PMU                                                                                \
    (HDFGxTR_PMU_COMMON | BIT(20) /* PMSWINC */ | BIT(21) /* PMCR */ | BIT(57) /* PMUSERENR */)

#define SYS_PMCR_EL0    sys_reg(3, 3, 9, 12, 0)
#define SYS_PMCCNTR_EL0 sys_reg(3, 3, 9, 13, 0)
/* PMCR_EL0 bits that read back: E, D, X, DP, LC, LP (P and C are write-only; N, IMP, IDCODE = 0) */
#define PMCR_RW_MASK (BIT(0) | GENMASK(7, 3))

static u64 hv_win_pmu_count[MAX_CPUS];
static u64 hv_win_pmu_esr[MAX_CPUS];
static u64 hv_win_pmcr[MAX_CPUS];

u64 hv_win_caps(void)
{
    u64 caps = HV_WIN_CAP_PSCI | HV_WIN_CAP_AIC_PT | HV_WIN_CAP_SYSTEM_OFF | HV_WIN_CAP_PMU_RW | HV_WIN_CAP_FB_SHADOW | HV_WIN_CAP_S1_STORAGE;
#ifdef HV_WIN_VGIC3_READY
    caps |= HV_WIN_CAP_VGIC3;
#endif
    return ((u64)HV_WIN_MAGIC << 32) | ((u64)HV_WIN_ABI_VERSION << 16) | caps;
}

u64 hv_win_status(void)
{
    return ((u64)hv_win.exit_reason << 32) | (hv_win.psci_calls & 0xffffffff);
}

static int hv_win_highest_cpu(void)
{
    int top = -1;

    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (cpu == boot_cpu_idx || smp_is_alive(cpu))
            top = cpu;

    return top;
}

static int hv_win_mpidr_to_cpu(u64 mpidr)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (cpu != boot_cpu_idx && !smp_is_alive(cpu))
            continue;
        if ((smp_get_mpidr(cpu) & HV_WIN_MPIDR_AFF_MASK) == mpidr)
            return cpu;
    }
    return -1;
}

/*
 * vCPU table = /chosen/m5w,cpus (CONTRACT.md s9): vCPU index = GICR frame = ACPI processor UID.
 * The launcher may pass its list to P_HV_WIN_CONFIGURE as the address of nr_vcpus u64 MPIDRs, boot
 * CPU first. Without one, EL2 uses the rule run_windows.py publishes: the boot CPU, then the other
 * CPUs in m1n1 (ADT cpu-id) order, without the CPU reserved for the host.
 */
static int hv_win_add_vcpu(u32 index, int cpu, int reserved)
{
    if (cpu < 0 || cpu == reserved || hv_win.cpu_vcpu[cpu] >= 0 ||
        (index == 0) != (cpu == boot_cpu_idx))
        return -1;
    hv_win.vcpu_cpu[index] = cpu;
    hv_win.cpu_vcpu[cpu] = index;
    return 0;
}

static int hv_win_build_vcpus(u32 nr_vcpus, int reserved, u64 cpus)
{
    u32 n = 0;

    for (int i = 0; i < MAX_CPUS; i++)
        hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;

    if (!cpus) {
        if (hv_win_add_vcpu(n++, boot_cpu_idx, reserved) < 0)
            return -1;
        for (int cpu = 0; cpu < MAX_CPUS && n < nr_vcpus; cpu++)
            if (cpu != boot_cpu_idx && smp_is_alive(cpu) && !hv_win_add_vcpu(n, cpu, reserved))
                n++;
        if (n < nr_vcpus) {
            printf("HV/win: fewer than %u CPUs available to the guest\n", nr_vcpus);
            return -1;
        }
        return 0;
    }

    if ((cpus & 7) || !is_heap((void *)cpus) || !is_heap((void *)(cpus + 8 * nr_vcpus - 1))) {
        printf("HV/win: bad MPIDR list address 0x%lx\n", cpus);
        return -1;
    }

    for (u32 i = 0; i < nr_vcpus; i++) {
        u64 mpidr = read64(cpus + 8 * i) & HV_WIN_MPIDR_AFF_MASK;
        if (hv_win_add_vcpu(i, hv_win_mpidr_to_cpu(mpidr), reserved) < 0) {
            printf("HV/win: m5w,cpus[%u] = 0x%lx is not usable (unknown, reserved, repeated, or "
                   "not the boot CPU first)\n",
                   i, mpidr);
            return -1;
        }
    }

    return 0;
}

int hv_win_configure(u64 irq_model, u64 gicd_base, u64 gicr_base, u64 nr_vcpus, u64 flags, u64 cpus)
{
    if (hv_win.irq_model != HV_WIN_IRQ_NONE) {
        printf("HV/win: already configured\n");
        return -1;
    }

    if (flags & ~(u64)(HV_WIN_F_PSCI_SMC | HV_WIN_F_RESERVE_CPU | HV_WIN_F_KEEP_STEAL)) {
        printf("HV/win: unknown flags 0x%lx\n", flags);
        return -1;
    }

    if (nr_vcpus < 1 || nr_vcpus > MAX_CPUS) {
        printf("HV/win: invalid vCPU count %ld\n", nr_vcpus);
        return -1;
    }

    switch (irq_model) {
        case HV_WIN_IRQ_AIC_PASSTHROUGH:
            if (nr_vcpus != 1) {
                printf("HV/win: AIC passthrough is single-CPU (M0)\n");
                return -1;
            }
            break;

        case HV_WIN_IRQ_VGIC3:
#ifndef HV_WIN_VGIC3_READY
            printf("HV/win: GICv3 emulation is scaffolding only in this build\n");
            return -1;
#else
            if (!gicd_base || !gicr_base || (gicd_base & 0xffff) || (gicr_base & 0xffff)) {
                printf("HV/win: GICD/GICR IPAs must be non-zero and 64 KiB aligned\n");
                return -1;
            }
            break;
#endif

        default:
            printf("HV/win: unknown interrupt model %ld\n", irq_model);
            return -1;
    }

    int reserved = (flags & HV_WIN_F_RESERVE_CPU) ? hv_win_highest_cpu() : -1;
    if (reserved >= 0 && reserved == boot_cpu_idx) {
        printf("HV/win: cannot reserve the boot CPU\n");
        return -1;
    }
    if (hv_win_build_vcpus(nr_vcpus, reserved, cpus) < 0)
        return -1;

    hv_win.flags = flags;
    hv_win.gicd_base = gicd_base;
    hv_win.gicr_base = gicr_base;
    hv_win.nr_vcpus = nr_vcpus;
    hv_win.exit_reason = HV_WIN_EXIT_NONE;
    hv_win.psci_calls = 0;
    hv_win.reserved_cpu = reserved;

    if (irq_model == HV_WIN_IRQ_VGIC3 && hv_vgic3_init(gicd_base, gicr_base, nr_vcpus) < 0)
        return -1;

    /*
     * HCR policy. hv_init() and the Python HV.init() have already set the stock bits (VM, AMO, TEA,
     * E2H, RW, API/APK, FMO=1, IMO=0, TACR, TTLBOS). hv_start() snapshots HCR_EL2 for the
     * secondaries, so everything set here propagates to every guest CPU.
     */
    u64 hcr = mrs(HCR_EL2) | HCR_TID3; /* both models: hide PMUv3 in ID_AA64DFR0_EL1 */
    if (flags & HV_WIN_F_PSCI_SMC)
        hcr |= HCR_TSC;
    if (irq_model == HV_WIN_IRQ_VGIC3)
        hcr |= HCR_IMO | HCR_FMO;
    hv_write_hcr(hcr);
    sysop("isb");

    /*
     * Windows programs CNTV/CNTP and reads both counters; m1n1's time stealing moves CNTVOFF_EL2 on
     * every exit, which would make CNTVCT drift against CNTPCT. Off unless asked for.
     */
    if (!(flags & HV_WIN_F_KEEP_STEAL))
        hv_set_time_stealing(false, true);

    hv_win.irq_model = irq_model;
    hv_win_cpu_init();

    printf("HV/win: model %ld, %ld vCPU(s), GICD 0x%lx GICR 0x%lx, flags 0x%lx, reserved CPU %d, "
           "HCR 0x%lx\n",
           irq_model, nr_vcpus, gicd_base, gicr_base, flags, hv_win.reserved_cpu, mrs(HCR_EL2));
    bool fgt = FIELD_GET(ID_AA64MMFR0_FGT, mrs(ID_AA64MMFR0_EL1));
    printf("HV/win: PMU traps: ID_AA64DFR0_EL1 0x%lx, MDCR_EL2 0x%lx, FGT %s (HDFGRTR_EL2 0x%lx "
           "HDFGWTR_EL2 "
           "0x%lx)\n",
           mrs(ID_AA64DFR0_EL1), mrs(MDCR_EL2), fgt ? "yes" : "no", fgt ? mrs(SYS_HDFGRTR_EL2) : 0,
           fgt ? mrs(SYS_HDFGWTR_EL2) : 0);
    return 0;
}

/* Per CPU; MDCR_EL2 is also part of the state hv_start() hands to the secondaries */
static void hv_win_pmu_traps_init(void)
{
    msr(MDCR_EL2, mrs(MDCR_EL2) | MDCR_TPM | MDCR_TPMCR);
    if (FIELD_GET(ID_AA64MMFR0_FGT, mrs(ID_AA64MMFR0_EL1))) {
        msr(SYS_HDFGRTR_EL2, mrs(SYS_HDFGRTR_EL2) | HDFGRTR_PMU);
        msr(SYS_HDFGWTR_EL2, mrs(SYS_HDFGWTR_EL2) | HDFGWTR_PMU);
    }
    sysop("isb");
}

void hv_win_cpu_init(void)
{
    if (!hv_win_enabled())
        return;

    hv_win_pmu_traps_init();
    if (hv_win_vgic_enabled())
        hv_vgic3_cpu_init();
}

/* SMCCC calling convention: function ID in w0, arguments x1..x3, results in x0..x3. */
#define SMCCC_NOT_SUPPORTED ((u64)(s64) - 1)

bool hv_win_handle_hvc(struct exc_info *ctx)
{
    if (!hv_win_enabled())
        return false;

    u16 imm = FIELD_GET(ESR_ISS, ctx->esr) & 0xffff;

    switch (imm) {
        case 0:
            /* SMCCC/PSCI conduit; function ID in x0 */
            if (!hv_win_psci_call(ctx))
                ctx->regs[0] = SMCCC_NOT_SUPPORTED;
            return true;

        case M5W_HVC_DEBUG_PUTC:
            printf("%c", (char)(ctx->regs[0] & 0xff));
            return true;

        case M5W_HVC_FAULT_REPORT:
            printf("HV/win: Mu fault report kind=0x%lx ESR=0x%lx addr=0x%lx (CPU %d)\n",
                   ctx->regs[0], ctx->regs[1], ctx->regs[2], smp_id());
            return true;

        case M5W_HVC_PMU_TRAPS:
            ctx->regs[0] = hv_win_pmu_count[smp_id()];
            ctx->regs[1] = hv_win_pmu_esr[smp_id()];
            return true;

        default:
            /* Other immediates stay with the stock/Python HVC handlers (SPTM emulation etc.) */
            return false;
    }
}

bool hv_win_handle_smc(struct exc_info *ctx)
{
    if (!hv_win_enabled() || !(hv_win.flags & HV_WIN_F_PSCI_SMC))
        return false;

    if (FIELD_GET(ESR_ISS, ctx->esr) & 0xffff)
        return false;

    if (!hv_win_psci_call(ctx))
        ctx->regs[0] = SMCCC_NOT_SUPPORTED;
    return true;
}

/*
 * HCR_EL2.TID3 (both models) traps reads of the ID register space (op0 3, op1 0, CRn 0, CRm 1..7).
 * Everything reads as the hardware does, except:
 *   ID_AA64DFR0_EL1.PMUVer = 0: no PMUv3 (the PMU registers are emulated as absent when they trap);
 *   ID_AA64PFR0_EL1.GIC (VGIC3 model): the T6050 reports 0 (no physical GIC CPU interface), while
 * the guest does get the GICv3 system-register interface through ICH_HCR_EL2.En, so it reads 1, as
 * KVM reports to its vgic-v3 guests.
 */
#define ID_AA64PFR0_GIC    GENMASK(27, 24)
#define ID_AA64DFR0_PMUVER GENMASK(11, 8)

#define ID_CASE(crm, op2)                                                                          \
    case (crm) << 3 | (op2):                                                                       \
        return mrs(sys_reg(3, 0, 0, crm, op2))
#define ID_CASES(crm)                                                                              \
    ID_CASE(crm, 0);                                                                               \
    ID_CASE(crm, 1);                                                                               \
    ID_CASE(crm, 2);                                                                               \
    ID_CASE(crm, 3);                                                                               \
    ID_CASE(crm, 4);                                                                               \
    ID_CASE(crm, 5);                                                                               \
    ID_CASE(crm, 6);                                                                               \
    ID_CASE(crm, 7)

static u64 hv_win_read_id_reg(u32 crm, u32 op2)
{
    switch (crm << 3 | op2) {
        ID_CASES(1);
        ID_CASES(2);
        ID_CASES(3);
        ID_CASES(4);
        ID_CASES(5);
        ID_CASES(6);
        ID_CASES(7);
        default:
            return 0;
    }
}

static u64 hv_win_guest_id_reg(u32 crm, u32 op2)
{
    u64 val = hv_win_read_id_reg(crm, op2);

    if (crm == 4 && op2 == 0 && hv_win_vgic_enabled()) /* ID_AA64PFR0_EL1 */
        val = (val & ~ID_AA64PFR0_GIC) | FIELD_PREP(ID_AA64PFR0_GIC, 1);
    if (crm == 5 && op2 == 0) /* ID_AA64DFR0_EL1 */
        val &= ~ID_AA64DFR0_PMUVER;
    return val;
}

/* PMUv3 registers: the same set as the stage-2 rewrite (pmu_rw_is_pmu_reg) */
static bool hv_win_is_pmu_reg(u32 op0, u32 op1, u32 crn, u32 crm)
{
    return op0 == 3 && pmu_rw_is_pmu_reg(op1, crn, crm);
}

/* Lock-free (per-CPU state only): runs in the unlocked MSR fast path */
static void hv_win_pmu_access(struct exc_info *ctx, u64 reg, u64 rt, bool is_read, u64 val)
{
    int cpu = smp_id();

    hv_win_pmu_count[cpu]++;
    hv_win_pmu_esr[cpu] = ctx->esr;

    if (!is_read) {
        if (reg == SYSREG_ISS(SYS_PMCR_EL0))
            hv_win_pmcr[cpu] = val & PMCR_RW_MASK;
        return; /* everything else: writes ignored */
    }

    if (rt == 31)
        return;
    if (reg == SYSREG_ISS(SYS_PMCCNTR_EL0))
        ctx->regs[rt] = mrs(CNTVCT_EL0); /* monotonic: Windows times with it */
    else if (reg == SYSREG_ISS(SYS_PMCR_EL0))
        ctx->regs[rt] = hv_win_pmcr[cpu]; /* N = 0, IMP = IDCODE = 0 */
    else
        ctx->regs[rt] = 0;
}

bool hv_win_handle_msr(struct exc_info *ctx, u64 iss)
{
    if (!hv_win_enabled())
        return false;

    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;
    u64 val = rt == 31 ? 0 : ctx->regs[rt];
    u32 crm = FIELD_GET(ESR_ISS_MSR_CRm, iss);

    if (FIELD_GET(ESR_ISS_MSR_OP0, iss) == 3 && !FIELD_GET(ESR_ISS_MSR_OP1, iss) &&
        !FIELD_GET(ESR_ISS_MSR_CRn, iss) && crm >= 1 && crm <= 7) {
        if (!is_read)
            return false;
        if (rt != 31)
            ctx->regs[rt] = hv_win_guest_id_reg(crm, FIELD_GET(ESR_ISS_MSR_OP2, iss));
        return true;
    }

    if (hv_win_is_pmu_reg(FIELD_GET(ESR_ISS_MSR_OP0, iss), FIELD_GET(ESR_ISS_MSR_OP1, iss),
                          FIELD_GET(ESR_ISS_MSR_CRn, iss), crm)) {
        hv_win_pmu_access(ctx, reg, rt, is_read, val);
        return true;
    }

    if (!hv_win_vgic_enabled())
        return false;

    switch (reg) {
        case SYSREG_ISS(SYS_ICC_SGI1R_EL1):
            if (is_read)
                return false;
            return hv_vgic3_handle_sgi1r(val);

        case SYSREG_ISS(SYS_ICC_ASGI1R_EL1):
        case SYSREG_ISS(SYS_ICC_SGI0R_EL1):
            /* Group 0 / alternate-group SGIs: the emulated GIC has DS=1 and only Group 1 */
            return !is_read;

        default:
            return false;
    }
}

/* T6050 Windows USB: the boot firmware leaves the AIC globally disabled.
 * Arm only after the measured route has been installed (initially masked), then
 * enable on the first guest exception return. Enabling at MAP_IRQ time would
 * expose unrelated pending lines before hv_start installs the guest vectors.
 */
#define HV_WIN_T6050_AIC_BASE 0x280400000UL
#define HV_WIN_AIC_CONFIG    0x14
#define HV_WIN_AIC_ENABLE    BIT(0)
static bool hv_win_aic_enable_pending;
extern bool hv_started_cpus[MAX_CPUS];
extern char _hv_vectors_start[];

static bool hv_win_aic_layout_valid(void)
{
    return aic && aic->base == HV_WIN_T6050_AIC_BASE && aic->version == 3 &&
           aic->nr_die == 1 && aic->nr_irq == 3104 && aic->max_irq == 4096 &&
           aic->regs.config == 0x10000 && aic->regs.event == 0x40000 &&
           aic->regs.reg_size >= 0x40004 && aic->extintrcfg_stride == 0x4a00 &&
           aic->intmaskset_stride == 0x4a00 && aic->intmaskclear_stride == 0x4a00;
}

static bool hv_win_aic_enable_on_return(void)
{
    if (!hv_win_aic_enable_pending)
        return true;

    int cpu = smp_id();
    if (!hv_win_aic_layout_valid() || hv_win.nr_vcpus != 1 || cpu != boot_cpu_idx ||
        cpu < 0 || cpu >= MAX_CPUS || !hv_started_cpus[cpu] || !(mrs(DAIF) & SPSR_I) ||
        (mrs(HCR_EL2) & (HCR_IMO | HCR_FMO)) != (HCR_IMO | HCR_FMO) ||
        mrs(VBAR_EL1) != (u64)_hv_vectors_start) {
        printf("HV/win: AIC enable refused outside prepared guest exception return\n");
        return false;
    }

    u32 before = read32(aic->base + HV_WIN_AIC_CONFIG);
    u32 expected = before | HV_WIN_AIC_ENABLE;
    if (!(before & HV_WIN_AIC_ENABLE)) {
        write32(aic->base + HV_WIN_AIC_CONFIG, expected);
        u32 after = read32(aic->base + HV_WIN_AIC_CONFIG);
        if (after != expected) {
            printf("HV/win: AIC enable readback failed: 0x%x -> 0x%x (wanted 0x%x)\n",
                   before, after, expected);
            return false;
        }
    }
    hv_win_aic_enable_pending = false;
    printf("HV/win: T6050 AIC enabled for guest route: CONFIG 0x%x -> 0x%x\n",
           before, expected);
    return true;
}

int hv_win_map_irq(u64 aic_line, u64 gsiv)
{
    if (!hv_win_vgic_enabled() || !aic)
        return -1;

    u64 die = aic_line / aic->max_irq;
    if (die >= aic->nr_die || aic_line % aic->max_irq >= aic->nr_irq) {
        printf("HV/win: AIC line %ld does not exist\n", aic_line);
        return -1;
    }

    bool t6050 = aic->base == HV_WIN_T6050_AIC_BASE;
    if (t6050 && (!hv_win_aic_layout_valid() || hv_win.nr_vcpus != 1 ||
                  aic_line != 1451 || gsiv != 65)) {
        printf("HV/win: unsupported T6050 AIC layout or Windows USB route\n");
        return -1;
    }

    if (hv_vgic3_map_irq(aic_line, gsiv) < 0) {
        printf("HV/win: cannot route AIC line %ld to INTID %ld (range 32..%d, each once)\n",
               aic_line, gsiv, GIC_MAX_SPI);
        return -1;
    }

    if (t6050)
        hv_win_aic_enable_pending = true;
    return 0;
}

/* AIC lines are numbered die * max_irq + num, as in m1n1's IRQ tracing and /chosen/m5w,irq-map */
#define HV_WIN_IRQ_BUDGET 32

bool hv_win_handle_irq(struct exc_info *ctx)
{
    UNUSED(ctx);

    if (!hv_win_vgic_enabled() || !aic)
        return false;

    /* Each read acknowledges and masks one event; hv_vgic3 unmasks after the guest's EOI */
    for (int i = 0; i < HV_WIN_IRQ_BUDGET; i++) {
        u32 event = aic_ack();

        if (!event)
            break;
        if (FIELD_GET(AIC_EVENT_TYPE, event) == AIC_EVENT_TYPE_HW)
            hv_vgic3_handle_aic_irq(FIELD_GET(AIC_EVENT_DIE, event) * aic->max_irq +
                                    FIELD_GET(AIC_EVENT_NUM, event));
    }

    return true;
}

void hv_win_update_virq(bool ptimer_firing, bool vtimer_firing)
{
    if (!hv_win_vgic_enabled())
        return;

    if (!hv_win_aic_enable_on_return())
        hv_panic("T6050 AIC enable failed; refusing guest resume\n");

    int cpu = smp_id();
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, ptimer_firing);
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTV, vtimer_firing);
    hv_vgic3_flush();
}

extern bool hv_started_cpus[MAX_CPUS];

s64 hv_win_usb_prep(u64 port, u64 flags, u64 ranges, u64 count)
{
    struct dart_range r[64];

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (hv_started_cpus[cpu]) {
            printf("HV/win: USB host preparation refused while a guest is running\n");
            return USB_HOST_E_BUSY;
        }
    }
    if (!count || count > ARRAY_SIZE(r) || (ranges & 7) || !is_heap((void *)ranges) ||
        !is_heap((void *)(ranges + count * sizeof(r[0]) - 1))) {
        printf("HV/win: bad USB identity range list 0x%lx (%lu entries)\n", ranges, count);
        return USB_HOST_E_RANGES;
    }

    memcpy(r, (void *)ranges, count * sizeof(r[0]));
    return usb_host_prepare(port, flags, r, count);
}
