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
 * FIQs injected as PPIs, SGIs routed from trapped ICC_SGI1R_EL1, emulated GICD/GICR frames, Apple's
 *     hardware virtual CPU interface (ICH list registers) for delivery. Scaffold only: refused by
 *     hv_win_configure() until HV_WIN_VGIC3_READY is defined.
 */

#ifndef HV_WIN_H
#define HV_WIN_H

#include "exception.h"
#include "types.h"

#define HV_WIN_MAGIC       0x57454c32UL /* "WEL2" */
#define HV_WIN_ABI_VERSION 1

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

/* P_HV_WIN_CAPS: (MAGIC << 32) | (ABI << 16) | caps */
#define HV_WIN_CAP_PSCI       BIT(0) /* PSCI 1.1 / SMCCC 1.1 over HVC (and SMC with TSC) */
#define HV_WIN_CAP_AIC_PT     BIT(1) /* AIC passthrough model (M0) */
#define HV_WIN_CAP_VGIC3      BIT(2) /* GICv3 emulation usable (not set by the scaffold) */
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

struct hv_win_state {
    u32 irq_model;
    u32 flags;
    u64 gicd_base;
    u64 gicr_base;
    u32 nr_vcpus;
    int reserved_cpu;
    u32 exit_reason;
    u64 psci_calls;
};

extern struct hv_win_state hv_win;

u64 hv_win_caps(void);
int hv_win_configure(u64 irq_model, u64 gicd_base, u64 gicr_base, u64 nr_vcpus, u64 flags);
u64 hv_win_status(void);

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

/* GICv3 emulation (hv_vgic3.c) */
int hv_vgic3_init(u64 gicd_base, u64 gicr_base, u32 nr_vcpus);
void hv_vgic3_cpu_init(void);
bool hv_vgic3_handle_sgi1r(u64 val);
void hv_vgic3_set_ppi_level(int cpu, u32 intid, bool level);
void hv_vgic3_inject_spi(u32 intid);
void hv_vgic3_flush(void);

#endif
