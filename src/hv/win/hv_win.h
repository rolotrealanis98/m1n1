/* SPDX-License-Identifier: MIT */
/*
 * Windows EL2 runtime for m1n1's hypervisor (the open counterpart of Aurora's "x1n1").
 *
 * Personality layered on the stock m1n1 hypervisor, off unless the host configures it with
 * P_HV_WIN_CONFIGURE after P_HV_INIT and before P_HV_START. Design and plan:
 * m5pro-windows docs/EL2-RUNTIME.md and docs/M0-RUNTIME-PLAN.md.
 *
 * Interrupt models:
 *   AIC_PASSTHROUGH (M0): the guest (Mu built with AIC_BUILD=TRUE) drives the physical AIC itself;
 *     IRQs stay routed to EL1 (HCR_EL2.IMO=0) and the Apple timer FIQs keep reaching the guest as
 *     virtual FIQs through the existing hv_update_fiq() path. One guest CPU.
 *   VGIC3 (M1+): HCR_EL2.IMO=FMO=1, AIC events taken at EL2 and injected as virtual SPIs, timer
 *     FIQs injected as PPIs, SGIs routed from trapped ICC_SGI1R_EL1, emulated GICD/GICR frames,
 *     Apple's hardware virtual CPU interface (ICH list registers) for delivery (hv_vgic3.c).
 *     Offered when HV_WIN_VGIC3_READY is defined (below).
 *
 * Both models: HCR_EL2.TID3 (ID registers emulated: ID_AA64DFR0_EL1.PMUVer reads 0, and in VGIC3
 * ID_AA64PFR0_EL1.GIC reads 1) and MDCR_EL2.TPM|TPMCR plus, with FEAT_FGT, the HDFGRTR/HDFGWTR_EL2
 * PMU bits: Windows' boot manager reads PMCCNTR_EL0 without checking PMUVer, so EL2 emulates an
 * absent PMUv3 (PMCCNTR_EL0 = CNTVCT_EL0, PMCR_EL0.N = 0, everything else RAZ/WI) when the access
 * traps.
 */

#ifndef HV_WIN_H
#define HV_WIN_H

#include "exception.h"
#include "smp_asm.h"
#include "types.h"

#define HV_WIN_MAGIC       0x57454c32UL /* "WEL2" */
#define HV_WIN_ABI_VERSION 1

/* hv_vgic3.c is complete: advertise HV_WIN_CAP_VGIC3 and accept the VGIC3 model */
#define HV_WIN_VGIC3_READY 1

/*
 * M5W hypercalls: HVC immediates owned by this runtime, recorded in m5pro-windows docs/CONTRACT.md
 * s5/s6. HVC #0 stays the SMCCC/PSCI conduit (function ID in x0); these use a distinctive 'M5' base
 * so they cannot be confused with SMCCC calls or Mu's own HVC usage. M5W_HVC_DEBUG_PUTC  (x0 =
 * byte): early console before Mu's UART library is up. M5W_HVC_FAULT_REPORT(x0 = kind, x1 = ESR, x2
 * = address/PC): Mu's vector forwards a fault it cannot handle so the resident can log/decide,
 * instead of patching Microsoft binaries.
 */
#define M5W_HVC_BASE         0x4d35 /* 'M5' */
#define M5W_HVC_DEBUG_PUTC   (M5W_HVC_BASE + 0)
#define M5W_HVC_FAULT_REPORT (M5W_HVC_BASE + 1)
/*
 * M5W_HVC_PMU_TRAPS (no arguments): x0 = how many PMUv3 register accesses EL2 has emulated on the
 * calling CPU, x1 = the ESR_EL2 of the latest one. A probe for the smoke guest (trapped vs UNDEF at
 * EL1).
 */
#define M5W_HVC_PMU_TRAPS (M5W_HVC_BASE + 2)

/* P_HV_WIN_CAPS: (MAGIC << 32) | (ABI << 16) | caps */
#define HV_WIN_CAP_PSCI       BIT(0) /* PSCI 1.1 / SMCCC 1.1 over HVC (and SMC with TSC) */
#define HV_WIN_CAP_AIC_PT     BIT(1) /* AIC passthrough model (M0) */
#define HV_WIN_CAP_VGIC3      BIT(2) /* GICv3 emulation usable (route R1) */
#define HV_WIN_CAP_SYSTEM_OFF BIT(3) /* SYSTEM_OFF/RESET end the guest and report to the host */

enum hv_win_irq_model {
    HV_WIN_IRQ_NONE = 0,
    HV_WIN_IRQ_AIC_PASSTHROUGH = 1,
    HV_WIN_IRQ_VGIC3 = 2,
};

/* P_HV_WIN_CONFIGURE flags */
#define HV_WIN_F_PSCI_SMC    BIT(0) /* set HCR_EL2.TSC and accept PSCI over SMC as well */
#define HV_WIN_F_RESERVE_CPU BIT(1) /* keep the highest-numbered CPU in host context */
#define HV_WIN_F_KEEP_STEAL  BIT(2) /* keep m1n1 time stealing (CNTVOFF); default is off */

/* P_HV_WIN_STATUS */
enum hv_win_exit {
    HV_WIN_EXIT_NONE = 0,
    HV_WIN_EXIT_SYSTEM_OFF = 1,
    HV_WIN_EXIT_SYSTEM_RESET = 2,
};

/* MPIDR affinity fields: Aff3 (bits 39:32) and Aff2..Aff0 (bits 23:0) */
#define HV_WIN_MPIDR_AFF_MASK 0xff00ffffffUL

struct hv_win_state {
    u32 irq_model;
    u32 flags;
    u64 gicd_base;
    u64 gicr_base;
    u32 nr_vcpus;
    int reserved_cpu;
    u32 exit_reason;
    u64 psci_calls;
    /*
     * Guest vCPU table, from the MPIDR list the launcher passes to hv_win_configure(): vCPU index
     * (= /chosen/m5w,cpus order = GICR frame order = ACPI processor UID) <-> m1n1 CPU index.
     * vCPU 0 is the boot CPU.
     */
    int vcpu_cpu[MAX_CPUS]; /* m1n1 CPU for each vCPU, -1 past nr_vcpus */
    int cpu_vcpu[MAX_CPUS]; /* vCPU for each m1n1 CPU, -1 if the CPU is not offered to the guest */
};

extern struct hv_win_state hv_win;

u64 hv_win_caps(void);
int hv_win_configure(u64 irq_model, u64 gicd_base, u64 gicr_base, u64 nr_vcpus, u64 flags,
                     u64 cpus);
u64 hv_win_status(void);
int hv_win_map_irq(u64 aic_line, u64 gsiv); /* VGIC3 model: route an AIC line to SPI <gsiv> */

static inline bool hv_win_enabled(void)
{
    return hv_win.irq_model != HV_WIN_IRQ_NONE;
}

static inline bool hv_win_vgic_enabled(void)
{
    return hv_win.irq_model == HV_WIN_IRQ_VGIC3;
}

/* Per-CPU setup, called from hv_init_secondary() and from configure on the boot CPU */
void hv_win_cpu_init(void);

/* Exception hooks (return true when handled; ELR handling as documented per hook) */
bool hv_win_handle_hvc(struct exc_info *ctx);                    /* ELR already past the HVC */
bool hv_win_handle_smc(struct exc_info *ctx);                    /* caller advances ELR by 4 */
bool hv_win_handle_msr(struct exc_info *ctx, u64 iss);           /* caller advances ELR by 4 */
bool hv_win_handle_irq(struct exc_info *ctx);                    /* vGIC model only */
void hv_win_update_virq(bool ptimer_firing, bool vtimer_firing); /* vGIC model, on guest entry */

/* PSCI (hv_win_psci.c) */
bool hv_win_psci_call(struct exc_info *ctx);

/* GICv3 emulation (hv_vgic3.c); <cpu> arguments are m1n1 CPU indices */
int hv_vgic3_init(u64 gicd_base, u64 gicr_base, u32 nr_vcpus);
void hv_vgic3_cpu_init(void);
int hv_vgic3_map_irq(u32 aic_line, u32 intid);
bool hv_vgic3_handle_sgi1r(u64 val);
void hv_vgic3_handle_aic_irq(u32 aic_line);
void hv_vgic3_set_ppi_level(int cpu, u32 intid, bool level);
void hv_vgic3_set_spi_level(u32 intid, bool level); /* emulated level-triggered devices */
void hv_vgic3_inject_spi(u32 intid);                /* emulated edge: latch pending */
void hv_vgic3_flush(void);
void hv_vgic3_maintenance(void);

/*
 * First thing in hv_exc_fiq(): on Apple cores the vGIC maintenance interrupt is a FIQ with no AIC
 * status bit, recognised by a non-zero ICH_MISR_EL2 while ICH_HCR_EL2.En is set (Linux
 * irq-apple-aic.c does the same).
 */
static inline void hv_win_fiq_entry(void)
{
    if (hv_win_vgic_enabled())
        hv_vgic3_maintenance();
}

/*
 * ---- Emulated PL011 for Windows KD (hv_win_pl011.c) ------------------------------------------
 * Configured by the host with P_HV_WIN_PL011(base, gsiv) after P_HV_INIT (it shares the vuart CDC
 * pipe that HV.init() binds). gsiv = 0: no interrupt. Independent of P_HV_WIN_CONFIGURE.
 * Hooks for the vGIC side:
 *   hv_win_pl011_raise(gsiv)  weak no-op here; called on a 0 -> 1 edge of PL011 MIS
 *   hv_win_pl011_irq_level()  current MIS != 0, for level re-sampling (EOI/deactivate)
 *   hv_win_pl011_poll()       call from the hypervisor tick next to hv_vuart_poll() so host input
 *                             raises RX without a guest access
 */
int hv_win_pl011_configure(u64 base, u64 gsiv);
void hv_win_pl011_poll(void);
bool hv_win_pl011_irq_level(void);
void hv_win_pl011_raise(u32 gsiv);
u64 hv_win_pl011_stats(void); /* tx[31:0] | rx[47:32] | tx_dropped[63:48] */

#endif
