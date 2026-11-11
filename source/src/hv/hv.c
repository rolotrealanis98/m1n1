/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "hv.h"
#include "adt.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "gxf.h"
#include "hv_sprr.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"
#include "wdt.h"

#include "win/hv_win.h"
#include "win/hv_win_storage.h"
#include "hv_win_hidm.h"
#include "hv_win_smcp.h"
#include "hv_win_m5wn.h"
#include "hv_win_n1.h"

#define HV_TICK_RATE      1000
#define HV_SLOW_TICK_RATE 1

DECLARE_SPINLOCK(bhl);

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry);
void hv_exit_guest(void) __attribute__((noreturn));

extern char _hv_vectors_start[0];

u64 hv_tick_interval;
u64 hv_secondary_tick_interval;

int hv_pinned_cpu;
int hv_want_cpu;

enum hv_sptm_mode hv_sptm_mode = SPTM_NONE;
bool hv_genter_trap_enabled = false;

static bool hv_has_ecv;
static bool hv_should_exit[MAX_CPUS];
bool hv_started_cpus[MAX_CPUS];
u64 hv_cpus_in_guest;
u64 hv_saved_sp[MAX_CPUS];

static u64 hv_cpu_state_regs[MAX_CPUS];

static bool hv_cpu_state_access(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    if (width != 2 && width != 3)
        return false;
    if (write)
        return hv_pa_rw(ctx, ipa, val, write, width);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 base = hv_cpu_state_regs[cpu];
        if (!base)
            continue;
        if (ipa != base)
            continue;

        /* Clear out the lower byte when the core hasn't been started by the guest yet */
        if (!hv_pa_rw(ctx, ipa, val, write, width))
            return false;
        if (!hv_started_cpus[cpu])
            *val &= ~0xffUL;
        return true;
    }

    return false;
}

static bool hv_cpu_state_setup_hook(void)
{
    int node;

    memset(hv_cpu_state_regs, 0, sizeof(hv_cpu_state_regs));

    node = adt_path_offset(adt, "/cpus");
    if (node < 0)
        return false;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 cpu;
        u64 reg[2];

        if (ADT_GETPROP(adt, node, "cpu-id", &cpu) != sizeof(cpu))
            return false;
        if (ADT_GETPROP_ARRAY(adt, node, "cpu-impl-reg", reg) != sizeof(reg))
            return false;

        hv_cpu_state_regs[cpu] = reg[0] + 0x100;
        if (hv_map_hook(hv_cpu_state_regs[cpu], hv_cpu_state_access, 8) < 0)
            return false;
    }

    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    return true;
}

struct hv_secondary_info_t {
    uint64_t hcr;
    uint64_t hacr;
    uint64_t vtcr, vttbr;
    uint64_t mdcr;
    uint64_t mdscr;
    uint64_t amx_ctl;
    uint64_t apvmkeylo, apvmkeyhi, apsts;
    uint64_t actlr_el2;
    uint64_t actlr_el1;
    uint64_t cnthctl;
    uint64_t sprr_config;
    uint64_t gxf_config;
    uint64_t agt_cnt_rdir_el1;
    uint64_t agt_cnt_rdir_el12;
    uint64_t cptr; /* smp2b */
};

static struct hv_secondary_info_t hv_secondary_info;

u64 nvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));
u64 anvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));

static void hv_enable_sme_zt0(void)
{
    u64 sme = FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1));
    if (sme != ID_AA64PFR1_SME2)
        return;

    /* Don't trap SME instructions to EL2 */
    reg_set(SYS_SMCR_EL2, SMCR_EZT0);
}

/* must be called before hv_init */
void hv_sptm_set_mode(enum hv_sptm_mode mode)
{
    printf("HV: SPTM mode %d\n", mode);
    hv_sptm_mode = mode;
}

void hv_init(void)
{
    hv_win_hidm_retire();
    hv_win_m5wn_retire();
    hv_win_n1_retire();
    hv_win_storage_retire();
    pcie_shutdown();
    // Make sure we wake up DCP if we put it to sleep, just quiesce it to match ADT
    if (display_is_external && display_start_dcp() >= 0)
        display_shutdown(DCP_QUIESCED);
    // reenable hpm interrupts for the guest for unused iodevs
    usb_hpm_restore_irqs(0);
    smp_start_secondaries();
    smp_set_wfe_mode(true);
    hv_wdt_init();

    hv_pt_init();
    hv_sprr_init();
    hv_aic_init();

    // Configure hypervisor defaults
    hv_write_hcr(HCR_API | // Allow PAuth instructions
                 HCR_APK | // Allow PAuth key registers
                 HCR_TEA | // Trap external aborts
                 HCR_E2H | // VHE mode (forced)
                 HCR_RW |  // AArch64 guest
                 HCR_AMO | // Trap SError exceptions
                 HCR_VM);  // Enable stage 2 translation
    sysop("isb");

    if (hv_sptm_mode == SPTM_EMULATION) {
        hv_write_hcr(mrs(HCR_EL2) | HCR_EnSCXT);
        if (FIELD_GET(ID_AA64PFR1_MTE, mrs(SYS_ID_AA64PFR1_EL1)) >= ID_AA64PFR1_MTE2) {
            hv_write_hcr(mrs(HCR_EL2) | HCR_ATA);
            sysop("isb");
        }
    }

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        printf("Enabling NV\n");
        hv_write_hcr(mrs(SYS_HCR_EL2) | HCR_NV2 | HCR_NV);
        msr(VNCR_EL2, &nvMem[0]);
        hv_write_avncr((u64)&anvMem[0]);
    }

    // No guest vectors initially
    msr(VBAR_EL12, 0);

    // Compute tick interval
    hv_tick_interval = mrs(CNTFRQ_EL0) / HV_TICK_RATE;

    hv_has_ecv = mrs(ID_AA64MMFR0_EL1) & (0xfULL << 60);

    if (hv_has_ecv) {
        printf("HV: ECV enabled\n");
        reg_set(CNTHCTL_EL2,
                CNTHCTL_EL1NVVCT | CNTHCTL_EL1NVPCT | CNTHCTL_EL1TVT | CNTHCTL_EL1PCTEN);
        hv_secondary_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
    } else {
        printf("HV: No ECV supported\n");
        // Enable physical timer for EL1
        msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);

        hv_secondary_tick_interval = hv_tick_interval;
    }

    /*
     * Is the guest timer FIQ gate writable? Write back what it reads under the
     * exception guard; on T6050 any write is UNDEFINED. hv_exc.c then masks
     * firing guest timers via IMASK, which needs ECV to trap the guest's view.
     */
    int exc_before = exc_count;
    exc_guard = GUARD_SKIP | GUARD_SILENT;
    msr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, mrs(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2));
    sysop("isb");
    exc_guard = GUARD_OFF;
    hv_vm_tmr_fiq_locked = exc_count != exc_before;
    if (hv_vm_tmr_fiq_locked)
        printf("HV: VM_TMR_FIQ_ENA is locked; gating guest timers via IMASK%s\n",
               hv_has_ecv ? "" : " - BROKEN without ECV, guest timer FIQs will storm");

    // Set deep WFI back to defaults
    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    sysop("dsb ishst");
    sysop("tlbi alle1is");
    sysop("dsb ish");
    sysop("isb");
}

static void hv_set_gxf_vbar(void)
{
    msr(SYS_IMP_APL_VBAR_GL1, _hv_vectors_start);
}

void hv_start(void *entry, u64 regs[4])
{
    if (boot_cpu_idx == -1) {
        printf("Boot CPU has not been found, can't start hypervisor\n");
        return;
    }

    if (!hv_win_fb_start((const struct boot_args *)regs[0])) {
        printf("HV: framebuffer shadow validation failed; guest refused\n");
        return;
    }

    if (!hv_cpu_power_init()) {
        printf("HV: CPU power protection hooks failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_amcc_init()) {
        printf("HV: AMCC interrupt hook failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_win_gpu_start()) { /* gpu-pt: no-op unless HV_WIN_F_GPU_PS_GUARD */
        printf("HV: GPU power-state guard failed; guest refused\n");
        return;
    }

    if (!hv_cpu_state_setup_hook()) {
        printf("CPU state hook setup failed, can't start the hypervisor\n");
        return;
    }

    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));

    hv_started_cpus[boot_cpu_idx] = true;

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_secondary_info.hcr = mrs(HCR_EL2);
    hv_secondary_info.hacr = mrs(HACR_EL2);
    hv_secondary_info.vtcr = mrs(VTCR_EL2);
    hv_secondary_info.vttbr = mrs(VTTBR_EL2);
    hv_secondary_info.mdcr = mrs(MDCR_EL2);
    hv_secondary_info.mdscr = mrs(MDSCR_EL1);
    hv_secondary_info.amx_ctl = mrs(SYS_IMP_APL_AMX_CTL_EL2);
    hv_secondary_info.apvmkeylo = mrs(SYS_IMP_APL_APVMKEYLO_EL2);
    hv_secondary_info.apvmkeyhi = mrs(SYS_IMP_APL_APVMKEYHI_EL2);
    hv_secondary_info.apsts = mrs(SYS_IMP_APL_APSTS_EL12);
    hv_secondary_info.actlr_el2 = mrs(ACTLR_EL2);
    if (cpu_features->actlr_el2)
        hv_secondary_info.actlr_el1 = mrs(SYS_ACTLR_EL12);
    else
        hv_secondary_info.actlr_el1 = mrs(SYS_IMP_APL_ACTLR_EL12);
    hv_secondary_info.cnthctl = mrs(CNTHCTL_EL2);
    hv_secondary_info.sprr_config = mrs(SYS_IMP_APL_SPRR_CONFIG_EL1);
    hv_secondary_info.gxf_config = mrs(SYS_IMP_APL_GXF_CONFIG_EL1);
    if (cpu_features->counter_redirect) {
        hv_secondary_info.agt_cnt_rdir_el1 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL1);
        hv_secondary_info.agt_cnt_rdir_el12 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL12);
    }
    /*
     * smp2b: the boot CPU runs the guest with the CPTR_EL2 iBoot left it (m1n1 never writes it);
     * a secondary would keep its reset value, which may trap FP/SIMD (EC 0x07) on the guest's first
     * NEON instruction. Read under HCR_EL2.E2H = 1 (VHE layout), written back after HCR below.
     */
    hv_secondary_info.cptr = mrs(CPTR_EL2);

    hv_arm_tick(false);
    hv_pinned_cpu = -1;
    hv_want_cpu = -1;
    hv_cpus_in_guest = BIT(smp_id());

    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);

    hv_wdt_stop();

    printf("HV: Exiting hypervisor (main CPU)\n");

    spin_unlock(&bhl);
    // Wait a bit for the guest CPUs to exit on their own if they are in the process.
    udelay(200000);
    spin_lock(&bhl);

    hv_started_cpus[boot_cpu_idx] = false;

    for (int i = 0; i < MAX_CPUS; i++) {
        if (i == boot_cpu_idx) {
            continue;
        }
        hv_should_exit[i] = true;
        if (hv_started_cpus[i]) {
            printf("HV: Waiting for CPU %d to exit\n", i);
            spin_unlock(&bhl);
            smp_wait(i);
            spin_lock(&bhl);
            hv_started_cpus[i] = false;
        }
    }

    hv_win_hidm_retire();
    hv_win_smcp_retire();
    hv_win_m5wn_retire();
    hv_win_n1_retire();
    hv_win_storage_retire();
    printf("HV: All CPUs exited\n");
    spin_unlock(&bhl);
    /* smcp: a guest SYSTEM_OFF/RESET becomes a real power-off/restart, only with the flag */
    hv_win_smcp_after_exit();
    /* nightly-7: after a host stop or the dead-man (or a guest exit nobody collects), reset without the host */
    hv_win_session_after_exit();
}

static void hv_init_secondary(struct hv_secondary_info_t *info)
{
    if (cpu_features->apple_sysregs_unlocked)
        gxf_init();

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    msr(HCR_EL2, info->hcr);
    msr(HACR_EL2, info->hacr);
    msr(VTCR_EL2, info->vtcr);
    msr(VTTBR_EL2, info->vttbr);
    msr(MDCR_EL2, info->mdcr);
    msr(MDSCR_EL1, info->mdscr);
    /*
     * smp2b: parity with the boot CPU. CPTR_EL2 = the boot CPU's value (after HCR_EL2, so the VHE
     * layout applies, as when it was read). Guest VBAR_EL1 = 0 as hv_init() sets it on the boot
     * CPU ("No guest vectors initially"); exception_initialize() left m1n1's EL1 vectors here.
     */
    msr(CPTR_EL2, info->cptr);
    msr(VBAR_EL12, 0);

    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_AMX_CTL_EL2, info->amx_ctl);
        msr(SYS_IMP_APL_APVMKEYLO_EL2, info->apvmkeylo);
        msr(SYS_IMP_APL_APVMKEYHI_EL2, info->apvmkeyhi);
        msr(SYS_IMP_APL_APSTS_EL12, info->apsts);
    }
    msr(ACTLR_EL2, info->actlr_el2);
    if (cpu_features->actlr_el2)
        msr(SYS_ACTLR_EL12, info->actlr_el1);
    else
        msr(SYS_IMP_APL_ACTLR_EL12, info->actlr_el1);
    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_SPRR_CONFIG_EL1, info->sprr_config);
        /* GXF_CONFIG is UNDEF unless SPRR is enabled */
        if (info->sprr_config & SPRR_CONFIG_EN)
            msr(SYS_IMP_APL_GXF_CONFIG_EL1, info->gxf_config);
    }
    if (cpu_features->counter_redirect) {
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, info->agt_cnt_rdir_el1);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, info->agt_cnt_rdir_el12);
    }

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        msr(VNCR_EL2, &nvMem[smp_id()]);
        hv_write_avncr((u64)&anvMem[smp_id()]);
    }

    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    // For M3 and up, CNTHCTL_EL2 must be written after the counter redirection
    sysop("isb");
    msr(CNTHCTL_EL2, info->cnthctl);
    /* smp2: the guest's CNTVCT_EL0 must match the boot CPU's from the first instruction */
    hv_sync_cntvoff();

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_win_cpu_init();

    hv_arm_tick(true);

    /*
     * smp2c: printed, not flushed. A flush from a secondary runs usb_dwc3_handle_events() under the
     * iodev lock only, which races the boot CPU's hv_tick() event handling (console_lock); the line
     * is queued under console_lock and the boot CPU drains it (its flush after the bounded wait in
     * hv_start_secondary_bounded(), its printf/flush on every failure path, then every tick).
     */
    if (hv_win_smp_diag())
        printf("HV: CPU %d EL2 init done\n", smp_id());
}

static void hv_enter_secondary(void *entry, u64 regs[4])
{
    /* smp2c: no flush here (delta review F1): the boot CPU is already back in the guest */
    if (hv_win_smp_diag())
        printf("HV: CPU %d entering guest at %p, x0 0x%lx\n", smp_id(), entry, regs[0]);

    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    spin_lock(&bhl);

    printf("HV: Exiting from CPU %d\n", smp_id());

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);

    hv_started_cpus[smp_id()] = false;
    spin_unlock(&bhl);
}

void hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    printf("HV: Initializing secondary %d\n", cpu);
    iodev_console_flush();

    if (hv_sptm_mode == SPTM_EMULATION)
        hv_sprr_init_secondary(cpu);

    mmu_init_secondary(cpu);
    iodev_console_flush();
    smp_call4(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0);
    smp_wait(cpu);
    iodev_console_flush();

    printf("HV: Entering guest secondary %d at %p\n", cpu, entry);
    hv_started_cpus[cpu] = true;
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);

    iodev_console_flush();
    smp_call4(cpu, hv_enter_secondary, (u64)entry, (u64)regs, 0, 0);
}

/*
 * smp2b: hv_start_secondary() with every wait bounded, for the guest's PSCI CPU_ON (boot CPU, bhl
 * held, DAIF masked: an unbounded wait freezes the guest and the primary WDT kick with it). The
 * steps are the same as above. The core sits in m1n1's spin table (WFE) and answers a call within
 * microseconds; hv_init_secondary() also flushes its console lines (hv_win_cpu_init()), which waits
 * for the host to read them, hence the longer bound for the init step.
 * Returns 0 = the core entered the guest; -1 = it never took a call (withdrawn: it will not run
 * guest code); -2 = it took a call and did not finish it. On failure the core must not be used
 * again in this boot.
 */
#define HV_SECONDARY_CALL_US 200000
#define HV_SECONDARY_INIT_US 2000000

int hv_start_secondary_bounded(int cpu, void *entry, u64 regs[4])
{
    printf("HV: Initializing secondary %d\n", cpu);
    iodev_console_flush();

    if (hv_sptm_mode == SPTM_EMULATION)
        hv_sprr_init_secondary(cpu);

    int ret = mmu_init_secondary_timeout(cpu, HV_SECONDARY_CALL_US);
    if (ret) {
        printf("HV: secondary %d: %s (MMU setup, %d ms bound); giving up on this core\n", cpu,
               ret == -1 ? "no answer to the kick" : "MMU setup did not finish",
               HV_SECONDARY_CALL_US / 1000);
        iodev_console_flush();
        return ret;
    }
    printf("HV: secondary %d answered the kick (MMU on); starting its EL2 init\n", cpu);
    iodev_console_flush();

    ret = smp_call4_timeout(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0,
                            HV_SECONDARY_CALL_US);
    if (ret) {
        printf("HV: secondary %d: EL2 init call %s; giving up on this core\n", cpu,
               ret < 0 ? "not taken (withdrawn)" : "taken late");
        iodev_console_flush();
        return ret < 0 ? -1 : -2;
    }
    if (!smp_wait_timeout(cpu, HV_SECONDARY_INIT_US, NULL)) {
        printf("HV: secondary %d: EL2 init did not finish within %d ms; giving up on this core\n",
               cpu, HV_SECONDARY_INIT_US / 1000);
        iodev_console_flush();
        return -2;
    }
    iodev_console_flush();

    printf("HV: Entering guest secondary %d at %p\n", cpu, entry);
    hv_started_cpus[cpu] = true;
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);

    iodev_console_flush();
    ret = smp_call4_timeout(cpu, hv_enter_secondary, (u64)entry, (u64)regs, 0, 0,
                            HV_SECONDARY_CALL_US);
    if (ret < 0) {
        hv_started_cpus[cpu] = false;
        __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(cpu), __ATOMIC_ACQUIRE);
        printf("HV: secondary %d: guest entry call not taken (withdrawn); giving up on this core\n",
               cpu);
        iodev_console_flush();
        return -1;
    }
    return 0;
}

void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = smp_id();

    printf("HV: Requesting exit of CPU#%d from the guest\n", cpu);
    hv_should_exit[cpu] = true;
}

void hv_rendezvous(void)
{
    int timeout = 1000000;

    if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
        return;

    /* IPI all CPUs. This might result in spurious IPIs to the guest... */
    for (int i = 0; i < MAX_CPUS; i++) {
        if (i != smp_id() && hv_started_cpus[i]) {
            smp_send_ipi(i);
        }
    }

    while (timeout--) {
        if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
            return;
    }

    hv_panic("HV: Failed to rendezvous, missing CPUs: 0x%lx (current: %d)\n",
             __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE), smp_id());
}

bool hv_switch_cpu(int cpu)
{
    if (cpu > MAX_CPUS || cpu < 0 || !hv_started_cpus[cpu]) {
        printf("HV: CPU #%d is inactive or invalid\n", cpu);
        return false;
    }
    printf("HV: switching to CPU #%d\n", cpu);
    hv_want_cpu = cpu;
    hv_rendezvous();
    return true;
}

void hv_pin_cpu(int cpu)
{
    hv_pinned_cpu = cpu;
}

void hv_write_hcr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hcr, val, 0, 0, 0);
    else
        msr(HCR_EL2, val);
}

void hv_write_hacr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hacr, val, 0, 0, 0);
    else
        msr(HACR_EL2, val);
}

void hv_write_avncr(u64 val)
{
    // On T6050 (M5) AVNCR_EL2 is only writable from GL2; a direct EL2 write
    // UNDEFs. Route it like hv_write_hcr/hv_write_hacr.
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_avncr, val, 0, 0, 0);
    else
        msr(SYS_IMP_APL_AVNCR_EL2, val);
}

u64 hv_get_spsr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_SPSR_GL1);
    else
        return mrs(SPSR_EL2);
}

void hv_set_spsr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_SPSR_GL1, val);
    else
        return msr(SPSR_EL2, val);
}

u64 hv_get_esr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ESR_GL1);
    else
        return mrs(ESR_EL2);
}

u64 hv_get_far(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_FAR_GL1);
    else
        return mrs(FAR_EL2);
}

u64 hv_get_afsr1(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_AFSR1_GL1);
    else
        return mrs(AFSR1_EL2);
}

u64 hv_get_elr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ELR_GL1);
    else
        return mrs(ELR_EL2);
}

void hv_set_elr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_ELR_GL1, val);
    else
        return msr(ELR_EL2, val);
}

void hv_arm_tick(bool secondary)
{
    if (secondary)
        msr(CNTP_TVAL_EL0, hv_secondary_tick_interval);
    else
        msr(CNTP_TVAL_EL0, hv_tick_interval);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    hv_sprr_timer_arm();
}

void hv_maybe_exit(void)
{
    if (hv_should_exit[smp_id()]) {
        hv_exit_guest();
    }
}

/* smp2d: read-only "exit requested for this CPU", for the guest-WFI emulation wait (hv_exc.c) */
bool hv_cpu_should_exit(void)
{
    return hv_should_exit[smp_id()];
}

void hv_tick(struct exc_info *ctx)
{
    HV_DOING(HV_DO_TICK); /* nightly-7b */
    /* nightly-7b: with the progress watchdog armed, the SoC watchdog is petted only when every started CPU made
     * progress (hv_win_session.c); otherwise as before */
    if (!hv_win_progress_wdt_tick())
        wdt_kick();
    hv_wdt_pet();
    iodev_handle_events(uartproxy_iodev);
    if (iodev_can_read(uartproxy_iodev)) {
        printf("HV: User interrupt\n");
        iodev_console_flush();
        if (hv_pinned_cpu == -1 || hv_pinned_cpu == smp_id())
            hv_exc_proxy(ctx, START_HV, HV_USER_INTERRUPT, NULL);
    }
    hv_vuart_poll();
    /* Windows personality only: a no-op unless P_HV_WIN_PL011 configured the emulated PL011 */
    hv_win_pl011_poll();
    /* Windows PMU rewrite: periodic re-arm of pinned pages; a no-op unless it is armed */
    HV_DOING(HV_DO_PMU_TICK);
    hv_win_pmu_tick();
    HV_DOING(HV_DO_FB_TICK);
    hv_win_fb_tick();
    HV_DOING(HV_DO_TICK);
    /* smp2: per-vCPU timer/SGI report every 10 s with N > 1 vCPUs; no-op otherwise */
    hv_win_tick();
    /* r3: native read-only storage keeps the ANS mailbox drained; no-op otherwise */
    HV_DOING(HV_DO_NVME_TICK);
    hv_win_storage_tick();
    HV_DOING(HV_DO_TICK);
    hv_win_hidm_tick();
    hv_win_smcp_tick();
    hv_win_n1_tick();
    hv_win_m5wn_tick();
    /* nightly-7: host-heartbeat dead-man; a no-op until the launcher arms it */
    HV_DOING(HV_DO_SESSION);
    hv_win_session_tick();
    /* r4.12: re-present a declined, still-high NVMe INTx line (vGIC hold-off) */
    HV_DOING(HV_DO_VGIC_TICK);
    hv_vgic3_tick();
    /* r4.11 diagnostics: periodic guest/trap/vGIC/NVMe status on the console */
    HV_DOING(HV_DO_STATUS);
    hv_status_tick(ctx);
    HV_DOING(HV_DO_TICK);
    /* cpuperf-B: Desired -> cluster p-state; no-op unless P_HV_WIN_CPPC_DVFS enabled it */
    hv_win_cppc_tick();
}

bool hv_is_nested(void)
{
    u64 hcr = mrs(HCR_EL2);
    return (hcr | HCR_NV2 | HCR_NV) == hcr;
}

void hv_enable_genter_trap(void)
{
    hv_write_hacr(mrs(HACR_EL2) | HACR_TRAP_GENTER);
}

void hv_disable_genter_trap(void)
{
    hv_write_hacr(mrs(HACR_EL2) & ~HACR_TRAP_GENTER);
}

void hv_set_genter_trap(bool enabled)
{
    hv_genter_trap_enabled = enabled;
}
