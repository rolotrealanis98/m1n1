/* SPDX-License-Identifier: MIT */
/*
 * Windows EL2 runtime: configuration, HCR policy and exception hooks.
 * See hv_win.h for the model overview. Nothing here runs unless the host calls P_HV_WIN_CONFIGURE.
 */

#include "hv_win.h"
#include "aic.h"
#include "aic_regs.h"
#include "cpu_regs.h"
#include "hv_win_regs.h"
#include "smp.h"
#include "utils.h"

#include "../hv.h"

struct hv_win_state hv_win = {
    .irq_model = HV_WIN_IRQ_NONE,
    .reserved_cpu = -1,
};

u64 hv_win_caps(void)
{
    u64 caps = HV_WIN_CAP_PSCI | HV_WIN_CAP_AIC_PT | HV_WIN_CAP_SYSTEM_OFF;
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
    u64 hcr = mrs(HCR_EL2);
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
    return 0;
}

void hv_win_cpu_init(void)
{
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

bool hv_win_handle_msr(struct exc_info *ctx, u64 iss)
{
    if (!hv_win_vgic_enabled())
        return false;

    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;
    u64 val = rt == 31 ? 0 : ctx->regs[rt];

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

int hv_win_map_irq(u64 aic_line, u64 gsiv)
{
    if (!hv_win_vgic_enabled() || !aic)
        return -1;

    u64 die = aic_line / aic->max_irq;
    if (die >= aic->nr_die || aic_line % aic->max_irq >= aic->nr_irq) {
        printf("HV/win: AIC line %ld does not exist\n", aic_line);
        return -1;
    }

    if (hv_vgic3_map_irq(aic_line, gsiv) < 0) {
        printf("HV/win: cannot route AIC line %ld to INTID %ld (range 32..%d, each once)\n",
               aic_line, gsiv, GIC_MAX_SPI);
        return -1;
    }

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

    int cpu = smp_id();
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTP, ptimer_firing);
    hv_vgic3_set_ppi_level(cpu, GIC_PPI_CNTV, vtimer_firing);
    hv_vgic3_flush();
}
