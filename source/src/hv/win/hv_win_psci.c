/* SPDX-License-Identifier: MIT */
/*
 * PSCI 1.1 and SMCCC 1.1 for the Windows guest, over HVC (FADT ARM_BOOT_ARCH PSCI_COMPLIANT |
 * PSCI_USE_HVC) and optionally SMC (HCR_EL2.TSC).
 *
 * There is no EL3 on Apple silicon, so this is the only PSCI implementation the guest sees. CPU_ON
 * maps onto m1n1's existing guest secondary start (hv_start_secondary: the target core is parked
 * in m1n1's spin table since hv_init()). SYSTEM_OFF/RESET end the guest and report to the host;
 * they do not power off or reset the Mac.
 *
 * smp2 (SMP-PLAN M4): first build that runs CPU_ON. The start itself is m1n1's proven path
 * (hv_start_secondary(), hv.c; upstream reaches it from the CPU_START trap, proxyclient
 * m1n1/hv/__init__.py start_secondary -> p.hv_start_secondary), called here from the locked half of
 * hv_exc_sync() exactly as upstream calls it from the proxy (also under bhl). The per-CPU affinity
 * state follows the community pattern in aura-winboot src/hv/windows/hv_psci.c
 * hv_psci_turn_on_cpu() (SPDX MIT OR BSD-3-Clause; pattern only, no code copied): OFF -> ON_PENDING
 * -> ON, and a CPU that cannot be started again stays refused. No power-down or suspend state is
 * offered (SMP-PLAN hazards: cpuidle off).
 */

#ifdef PSCI_HOST_TEST
#include "psci_host.h" /* tests/win-psci: SMP, stage-2 and cache mocks */
#else
#include "../hv.h"
#include "iodev.h"
#include "memory.h"
#include "smp.h"
#include "utils.h"
#endif
#include "hv_win.h"

extern bool hv_started_cpus[MAX_CPUS];

#define SMCCC_VERSION           0x80000000
#define SMCCC_ARCH_FEATURES     0x80000001
#define SMCCC_ARCH_WORKAROUND_1 0x80008000
#define SMCCC_ARCH_WORKAROUND_2 0x80007fff
#define SMCCC_ARCH_WORKAROUND_3 0x80003fff

#define PSCI_VERSION           0x84000000
#define PSCI_CPU_SUSPEND_32    0x84000001
#define PSCI_CPU_OFF           0x84000002
#define PSCI_CPU_ON_32         0x84000003
#define PSCI_AFFINITY_INFO_32  0x84000004
#define PSCI_MIGRATE_INFO_TYPE 0x84000006
#define PSCI_SYSTEM_OFF        0x84000008
#define PSCI_SYSTEM_RESET      0x84000009
#define PSCI_FEATURES          0x8400000a
#define PSCI_CPU_SUSPEND_64    0xc4000001
#define PSCI_CPU_ON_64         0xc4000003
#define PSCI_AFFINITY_INFO_64  0xc4000004

#define PSCI_RET_SUCCESS            0
#define PSCI_RET_NOT_SUPPORTED      -1
#define PSCI_RET_INVALID_PARAMETERS -2
#define PSCI_RET_DENIED             -3
#define PSCI_RET_ALREADY_ON         -4
#define PSCI_RET_INTERNAL_FAILURE   -6
#define PSCI_RET_INVALID_ADDRESS    -9

#define PSCI_VERSION_1_1  0x00010001
#define SMCCC_VERSION_1_1 0x00010001

#define PSCI_AFF_ON  0
#define PSCI_AFF_OFF 1

/* smp2: per-CPU start state, only changed under bhl (every PSCI call runs in the locked path) */
enum psci_cpu_state {
    PSCI_CPU_OFF_STATE = 0, /* parked in m1n1's spin table, never started by the guest */
    PSCI_CPU_ON_PENDING,    /* hv_start_secondary() in progress */
    PSCI_CPU_ON_STATE,      /* entered the guest */
    PSCI_CPU_RETIRED,       /* left with CPU_OFF: hv_should_exit stays set, cannot re-enter */
};
static u8 psci_state[MAX_CPUS];
#ifdef PSCI_HOST_TEST
void psci_host_reset(void)
{
    memset(psci_state, 0, sizeof(psci_state));
}
#endif

static bool psci_verbose(void)
{
    return hv_win.nr_vcpus > 1; /* the single-vCPU console stays exactly as before */
}

#define PSCI_POWER_STATE_TYPE BIT(30)
#define MIGRATE_INFO_NO_TOS   2

#define MPIDR_AFF_MASK 0xff00ffffffUL

/* CPU_ON arguments must stay valid until the target core has read them (it does so asynchronously)
 */
static u64 cpu_on_regs[MAX_CPUS][4];

static void psci_ret(struct exc_info *ctx, s64 val)
{
    ctx->regs[0] = (u64)val;
}

static int psci_mpidr_to_cpu(u64 mpidr)
{
    mpidr &= MPIDR_AFF_MASK;

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (cpu != boot_cpu_idx && !smp_is_alive(cpu))
            continue;
        if ((smp_get_mpidr(cpu) & 0xffffff) == (mpidr & 0xffffff) && !(mpidr & ~0xffffffUL))
            return cpu;
    }

    return -1;
}

/* Only the CPUs published in /chosen/m5w,cpus (the vCPU table) may be brought up */
static bool psci_cpu_allowed(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS && cpu != hv_win.reserved_cpu && hv_win.cpu_vcpu[cpu] >= 0;
}

static s64 psci_cpu_on_checked(int cpu, u64 entry, u64 context)
{
    if (!psci_cpu_allowed(cpu))
        return PSCI_RET_INVALID_PARAMETERS;
    if (hv_started_cpus[cpu] || psci_state[cpu] == PSCI_CPU_ON_STATE)
        return PSCI_RET_ALREADY_ON;
    if (psci_state[cpu] == PSCI_CPU_ON_PENDING)
        return -5; /* PSCI_RET_ON_PENDING */
    if (psci_state[cpu] == PSCI_CPU_RETIRED)
        return PSCI_RET_INTERNAL_FAILURE;
    if (!smp_is_alive(cpu))
        return PSCI_RET_INTERNAL_FAILURE; /* hv_init() never got this core into the spin table */
    if (!entry || (entry & 3) || !hv_pt_is_ram(entry))
        return PSCI_RET_INVALID_ADDRESS;

    psci_state[cpu] = PSCI_CPU_ON_PENDING;

    /*
     * PSCI: entry at EL1, MMU and caches off (hv_win_cpu_init()), x0 = context_id. DAIF: as in
     * upstream m1n1, hv_enter_guest() (hv_asm.S) copies the secondary's EL2 DAIF into SPSR_EL2,
     * and exception_initialize() leaves a secondary with I and F masked, D and A clear. PSCI asks
     * for all four; with HCR_EL2.AMO and MDCR_EL2.TDE set, SErrors and debug exceptions go to EL2
     * whatever the guest's D/A bits say, and Windows sets its own DAIF before unmasking.
     */
    cpu_on_regs[cpu][0] = context;
    cpu_on_regs[cpu][1] = 0;
    cpu_on_regs[cpu][2] = 0;
    cpu_on_regs[cpu][3] = 0;
    dc_civac_range(cpu_on_regs[cpu], sizeof(cpu_on_regs[cpu]));

    /* smp2b: before the start, so the request is in the log even if the start hangs */
    printf("HV/win PSCI: CPU_ON received on CPU %d: starting CPU %d (MPIDR 0x%lx), entry 0x%lx, "
           "context 0x%lx\n",
           smp_id(), cpu, smp_get_mpidr(cpu), entry, context);
    iodev_console_flush();

    /* smp2b: every wait bounded; a core that does not answer is retired, CPU_ON fails */
    int ret = hv_start_secondary_bounded(cpu, (void *)entry, cpu_on_regs[cpu]);
    if (ret) {
        psci_state[cpu] = PSCI_CPU_RETIRED;
        printf("HV/win PSCI: CPU %d did not start (%s); CPU_ON returns INTERNAL_FAILURE, the core "
               "stays retired\n",
               cpu, ret == -1 ? "no answer" : "hung during EL2 init");
        return PSCI_RET_INTERNAL_FAILURE;
    }
    psci_state[cpu] = PSCI_CPU_ON_STATE;
    return PSCI_RET_SUCCESS;
}

static s64 psci_cpu_on(u64 target, u64 entry, u64 context)
{
    int cpu = psci_mpidr_to_cpu(target);
    s64 ret = psci_cpu_on_checked(cpu, entry, context);

    printf("HV/win PSCI: CPU_ON from CPU %d: MPIDR 0x%lx -> CPU %d, entry 0x%lx, context 0x%lx: "
           "%ld\n",
           smp_id(), target, cpu, entry, context, ret);
    return ret;
}

static s64 psci_cpu_off(void)
{
    /* Offlining the boot CPU would end hv_start() and with it the whole guest */
    if (smp_id() == boot_cpu_idx)
        return PSCI_RET_DENIED;

    psci_state[smp_id()] = PSCI_CPU_RETIRED;
    printf("HV/win PSCI: CPU_OFF on CPU %d (the core will not be restarted in this boot)\n", smp_id());
    hv_exit_cpu(-1); /* leaves the guest in hv_exc_exit(); the core returns to the spin table */
    return PSCI_RET_SUCCESS;
}

static s64 psci_affinity_info(u64 target, u64 level)
{
    if (level != 0)
        return PSCI_RET_INVALID_PARAMETERS;

    int cpu = psci_mpidr_to_cpu(target);
    if (cpu < 0)
        return PSCI_RET_INVALID_PARAMETERS;

    return hv_started_cpus[cpu] ? PSCI_AFF_ON : PSCI_AFF_OFF;
}

static s64 psci_cpu_suspend(u64 power_state)
{
    /*
     * Standby states only: returning is a valid wake-up ("woken by an interrupt"). Power-down
     * states would need a resume at the entry point with the context restored; the firmware must
     * not advertise them (_LPI / idle states) until that exists.
     */
    if (power_state & PSCI_POWER_STATE_TYPE)
        return PSCI_RET_INVALID_PARAMETERS;
    return PSCI_RET_SUCCESS;
}

static void psci_system_exit(u32 reason)
{
    hv_win.exit_reason = reason;
    if (reason == HV_WIN_EXIT_HOST_STOP || reason == HV_WIN_EXIT_DEADMAN) /* nightly-7: not the guest's request */
        printf("HV/win: session end (%s) on CPU %d\n", reason == HV_WIN_EXIT_HOST_STOP ? "host stop" : "dead-man",
               smp_id());
    else
        printf("HV/win: guest requested %s on CPU %d\n",
               reason == HV_WIN_EXIT_SYSTEM_OFF ? "SYSTEM_OFF" : "SYSTEM_RESET", smp_id());
    hv_win_pmu_report();
    /* smp2d: on a guest reset/bugcheck dump the resetting CPU's exit ring and the WFI/debug/integrity
     * counters, so a single hardware run settles the WFI-state-loss question. */
    hv_win_wfi_report();
    hv_win_diag_dump();
    /* smp2f: the full SMP dump too, when the resetting CPU is the boot CPU (no-op otherwise) */
    if (hv_win.nr_vcpus > 1)
        hv_win_smp_dump("guest exit");

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (!hv_started_cpus[cpu])
            continue;
        hv_exit_cpu(cpu);
        if (cpu != smp_id())
            smp_send_ipi(cpu);
    }
}

/* nightly-7 (hv_win_session.c): end the guest exactly as a guest SYSTEM_OFF does (every started vCPU exits; the
 * boot CPU then runs hv.c's unchanged exit path: retire HIDM/SMCP/M5WN/N1 and the storage session) */
void hv_win_system_exit(u32 reason)
{
    psci_system_exit(reason);
}

static s64 psci_features(u64 fid)
{
    switch (fid) {
        case PSCI_VERSION:
        case PSCI_CPU_OFF:
        case PSCI_CPU_ON_32:
        case PSCI_CPU_ON_64:
        case PSCI_AFFINITY_INFO_32:
        case PSCI_AFFINITY_INFO_64:
        case PSCI_MIGRATE_INFO_TYPE:
        case PSCI_SYSTEM_OFF:
        case PSCI_SYSTEM_RESET:
        case PSCI_FEATURES:
        case SMCCC_VERSION:
            return PSCI_RET_SUCCESS;
        case PSCI_CPU_SUSPEND_32:
        case PSCI_CPU_SUSPEND_64:
            return 0; /* original power_state format, no OS-initiated mode */
        default:
            return PSCI_RET_NOT_SUPPORTED;
    }
}

static bool psci_dispatch(struct exc_info *ctx, u64 fid, u64 a1, u64 a2, u64 a3)
{
    /* SMC32 calls carry 32-bit arguments */
    if (!(fid & BIT(30))) {
        a1 &= 0xffffffff;
        a2 &= 0xffffffff;
        a3 &= 0xffffffff;
    }

    hv_win.psci_calls++;

    switch (fid) {
        case SMCCC_VERSION:
            psci_ret(ctx, SMCCC_VERSION_1_1);
            return true;
        case SMCCC_ARCH_FEATURES:
            /* No firmware mitigations to offer; WORKAROUND_1/2/3 report NOT_SUPPORTED */
            psci_ret(ctx, (a1 == SMCCC_VERSION) ? PSCI_RET_SUCCESS : PSCI_RET_NOT_SUPPORTED);
            return true;
        case PSCI_VERSION:
            psci_ret(ctx, PSCI_VERSION_1_1);
            return true;
        case PSCI_FEATURES:
            psci_ret(ctx, psci_features(a1));
            return true;
        case PSCI_CPU_ON_32:
        case PSCI_CPU_ON_64:
            psci_ret(ctx, psci_cpu_on(a1, a2, a3));
            return true;
        case PSCI_CPU_OFF:
            psci_ret(ctx, psci_cpu_off());
            return true;
        case PSCI_AFFINITY_INFO_32:
        case PSCI_AFFINITY_INFO_64:
            psci_ret(ctx, psci_affinity_info(a1, a2));
            return true;
        case PSCI_CPU_SUSPEND_32:
        case PSCI_CPU_SUSPEND_64:
            psci_ret(ctx, psci_cpu_suspend(a1));
            return true;
        case PSCI_MIGRATE_INFO_TYPE:
            psci_ret(ctx, MIGRATE_INFO_NO_TOS);
            return true;
        case PSCI_SYSTEM_OFF:
            psci_system_exit(HV_WIN_EXIT_SYSTEM_OFF);
            psci_ret(ctx, PSCI_RET_INTERNAL_FAILURE); /* only seen if the exit does not happen */
            return true;
        case PSCI_SYSTEM_RESET:
            psci_system_exit(HV_WIN_EXIT_SYSTEM_RESET);
            psci_ret(ctx, PSCI_RET_INTERNAL_FAILURE);
            return true;
        default:
            hv_win.psci_calls--;
            return false;
    }
}

bool hv_win_psci_call(struct exc_info *ctx)
{
    u64 fid = ctx->regs[0] & 0xffffffff;
    u64 a1 = ctx->regs[1], a2 = ctx->regs[2], a3 = ctx->regs[3];
    bool handled = psci_dispatch(ctx, fid, a1, a2, a3);

    /* smp2: one console line per call with N vCPUs (Windows makes about 30 per boot) */
    if (psci_verbose() && fid != PSCI_CPU_ON_32 && fid != PSCI_CPU_ON_64)
        printf("HV/win PSCI: CPU %d fid 0x%lx x1 0x%lx -> 0x%lx%s\n", smp_id(), fid,
               ctx->regs[1], ctx->regs[0], handled ? "" : " (not ours: NOT_SUPPORTED)");
    return handled;
}
