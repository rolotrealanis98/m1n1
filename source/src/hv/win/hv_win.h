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
#define HV_WIN_CAP_FB_SHADOW  BIT(5) /* opt-in BGRA shadow, one vCPU */
#define HV_WIN_CAP_S1_STORAGE BIT(6) /* isolated immutable RAM fixture */
#define HV_WIN_CAP_PMU_RW     BIT(4) /* P_HV_WIN_PMU_REWRITE/STATS: stage-2 first-execute rewrite */

enum hv_win_irq_model {
    HV_WIN_IRQ_NONE = 0,
    HV_WIN_IRQ_AIC_PASSTHROUGH = 1,
    HV_WIN_IRQ_VGIC3 = 2,
};

/* P_HV_WIN_CONFIGURE flags */
#define HV_WIN_F_PSCI_SMC    BIT(0) /* set HCR_EL2.TSC and accept PSCI over SMC as well */
#define HV_WIN_F_RESERVE_CPU BIT(1) /* keep the highest-numbered CPU in host context */
#define HV_WIN_F_KEEP_STEAL  BIT(2) /* keep m1n1 time stealing (CNTVOFF); default is off */
/*
 * smp2d: by default EL2 traps and emulates the guest's WFI (HCR_EL2.TWI) when nr_vcpus > 1, so a
 * retention WFI on a long-idle secondary cannot lose the guest's register state (bugcheck-0A).
 * HV_WIN_F_NO_WFI_TRAP (launcher --no-wfi-trap) disables it to reproduce the native-WFI behaviour as
 * a control. The 1-vCPU path never traps WFI (its boot-CPU WFIs stay short under the 1 kHz tick).
 */
#define HV_WIN_F_NO_WFI_TRAP BIT(3)

/* P_HV_WIN_STATUS */
enum hv_win_exit {
    HV_WIN_EXIT_NONE = 0,
    HV_WIN_EXIT_SYSTEM_OFF = 1,
    HV_WIN_EXIT_SYSTEM_RESET = 2,
    HV_WIN_EXIT_HOST_STOP = 3, /* nightly-7: P_HV_WIN_SESSION_END */
    HV_WIN_EXIT_DEADMAN = 4,   /* nightly-7: no host heartbeat within the armed timeout */
};
/* nightly-7: cable-independent session end (hv_win_session.c) */
void hv_win_system_exit(u32 reason);
s64 hv_win_session_end(u64 code, u64 flags);
s64 hv_win_session_heartbeat(u64 seconds);
void hv_win_session_tick(void);
void hv_win_session_after_exit(void);
u64 hv_win_session_stat(u64 index);

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
    bool wfi_trap; /* smp2d: EL2 is trapping and emulating the guest's WFI */
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

enum hv_win_diag_stage {
    HV_WIN_DIAG_TRAP,
    HV_WIN_DIAG_VUART,
    HV_WIN_DIAG_USB_EVENTS,
    HV_WIN_DIAG_USB_EVENTS_DONE,
    HV_WIN_DIAG_CAN_WRITE,
    HV_WIN_DIAG_CAN_WRITE_DONE,
    HV_WIN_DIAG_WRITE,
    HV_WIN_DIAG_WRITE_DONE,
    HV_WIN_DIAG_VUART_DONE,
    HV_WIN_DIAG_WFI,      /* smp2d: a trapped guest WFI entered emulation */
    HV_WIN_DIAG_FIQ_FAST, /* smp2d: the non-interruptible FIQ fast-path return */
    HV_WIN_DIAG_WFI_DONE, /* smp2f: emulated WFI completed; detail = wake | idles << 8 | lost << 16 */
    HV_WIN_DIAG_FIQ_SLOW, /* smp2f: FIQ slow path entered; detail = ISR_EL1 */
};

void hv_win_diag_note(enum hv_win_diag_stage stage, const struct exc_info *ctx, u64 detail);
void hv_win_diag_dump(void);

struct boot_args;
s64 hv_win_fb_prepare(u64 expected_native, u64 flags, u64 tuning);
bool hv_win_fb_start(const struct boot_args *gba);
void hv_win_fb_tick(void);
u64 hv_win_fb_stat(u64 idx);
struct exc_info;
bool hv_win_fb_handle_dabort(struct exc_info *ctx, u64 esr); /* true: re-execute */

u64 hv_win_caps(void);
int hv_win_configure(u64 irq_model, u64 gicd_base, u64 gicr_base, u64 nr_vcpus, u64 flags,
                     u64 cpus);
u64 hv_win_status(void);
int hv_win_map_irq(u64 aic_line, u64 gsiv); /* VGIC3 model: route an AIC line to SPI <gsiv> */

/*
 * P_HV_WIN_USB_PREP (0xc1d): hand a Type-C port's xHCI to the guest (m5pro-windows
 * docs/USB-HOST.md). <ranges> is the address of <count> {u64 base, u64 size} guest RAM ranges
 * (the /chosen/m5w,memory bytes) that the port's DART maps DVA == PA, nothing else; <flags> are
 * usb.h USB_HOST_F_*. Before P_HV_START only, never the proxy's port. Returns usb.h
 * USB_HOST_INFO_* (>= 0) or USB_HOST_E_* (< 0). Independent of P_HV_WIN_CONFIGURE, so the EL2
 * half can be tested on its own under the proxy.
 */
s64 hv_win_usb_prep(u64 port, u64 flags, u64 ranges, u64 count);

static inline bool hv_win_enabled(void)
{
    return hv_win.irq_model != HV_WIN_IRQ_NONE;
}

static inline bool hv_win_vgic_enabled(void)
{
    return hv_win.irq_model == HV_WIN_IRQ_VGIC3;
}

/* smp2b: secondary bring-up stage lines (hv.c); off with one vCPU and outside the Windows guest */
static inline bool hv_win_smp_diag(void)
{
    return hv_win_enabled() && hv_win.nr_vcpus > 1;
}

/* Per-CPU setup, called from hv_init_secondary() and from configure on the boot CPU */
void hv_win_cpu_init(void);
/* perf-exits: P_HV_WIN_PERF (hv_win_perf.h flags), before the guest starts; returns the flags applied */
s64 hv_win_perf_configure(u64 flags);

/* Exception hooks (return true when handled; ELR handling as documented per hook) */
bool hv_win_handle_hvc(struct exc_info *ctx);                    /* ELR already past the HVC */
bool hv_win_handle_smc(struct exc_info *ctx);                    /* caller advances ELR by 4 */
bool hv_win_handle_msr(struct exc_info *ctx, u64 iss);           /* caller advances ELR by 4 */
bool hv_win_handle_irq(struct exc_info *ctx);                    /* vGIC model only */
void hv_win_update_virq(bool ptimer_firing, bool vtimer_firing); /* vGIC model, on guest entry */

/* smp2d: guest-WFI emulation (hv_exc.c). Trapping is enabled by hv_win_configure when >1 vCPU. */
bool hv_win_wfi(struct exc_info *ctx);  /* emulate one trapped guest WFI on this vCPU */
bool hv_win_storage_smp_gate(const char *what);
s64 hv_win_storage_max_cpus_set(u64 n); /* smp-storage: P_HV_WIN_STORAGE_MAX_CPUS */
s64 hv_win_storage_smp_set(u64 n, u64 timeout_us); /* nightly-7: the cap + the SMP data-command wait bound */
bool hv_win_disk_path_used(void); /* nightly-2: a disk path was admitted or tried this session */ /* u1: false (and a console line) with > 1 vCPU */
bool hv_win_wfi_trapping(void);         /* true if EL2 is emulating guest WFI */
void hv_win_wfi_report(void);           /* per-CPU WFI/debug/integrity counters, on guest exit */
void hv_win_wfi_snap(int where, const struct exc_info *ctx, int wake); /* smp2g: + x30/SP, wake */
/* sched1 (hv_exc.c): the vGIC is about to IPI <cpu> for its vCPU. Timestamps the first unconsumed kick so
 * the target's emulated WFI can report kick-to-wake latency. Lock-free, callable from any CPU. */
void hv_win_kick_note(int cpu);
void hv_win_pl011_kd_dump(void); /* smp2g: KD break/continue log, boot CPU */
void hv_win_diag_dump_all(void);       /* smp2f: every CPU's exit ring, non-freezing, boot CPU */

/* PSCI (hv_win_psci.c) */
bool hv_win_psci_call(struct exc_info *ctx);

/* GICv3 emulation (hv_vgic3.c); <cpu> arguments are m1n1 CPU indices */
int hv_vgic3_init(u64 gicd_base, u64 gicr_base, u32 nr_vcpus);
void hv_vgic3_cpu_init(void);
int hv_vgic3_map_irq(u32 aic_line, u32 intid);
/* r3: refuse ever routing this AIC line to the guest (fails if it is already routed) */
int hv_vgic3_deny_aic_line(u32 aic_line);
bool hv_vgic3_handle_sgi1r(u64 val);
void hv_vgic3_handle_aic_irq(u32 aic_line);
void hv_vgic3_set_ppi_level(int cpu, u32 intid, bool level);
void hv_vgic3_set_spi_level(u32 intid, bool level); /* emulated level-triggered devices */
void hv_vgic3_inject_spi(u32 intid);                /* emulated edge: latch pending */
void hv_vgic3_flush(void);
void hv_vgic3_smp_report(u64 now, u64 period); /* smp2 */
/* smp2e/smp2f: should the emulated WFI on this vCPU complete now (HV_VGIC3_WFI_* flags) */
#define HV_VGIC3_WFI_DIRTY  1 /* another CPU changed this vCPU and it is not folded yet */
#define HV_VGIC3_WFI_WAKE   2 /* an LR wakes under KVM's rule (pending, priority above VPMR): smp2f */
#define HV_VGIC3_WFI_STRICT 4 /* an LR signals under the full GICv3 rule (smp2e's rule; statistics) */
int hv_vgic3_wfi_pending(void);
struct wfi_vctx;
void hv_vgic3_vctx_read(struct wfi_vctx *v);                /* smp2f: ICH_* of this CPU */
void hv_vgic3_vctx_write(const struct wfi_vctx *v, u32 classes);
void hv_vgic3_smp_dump(void);                                /* smp2f: boot CPU only */
int hv_vgic3_stall_check(u64 now, u64 period);               /* smp2f: boot CPU tick */
int hv_win_smp_dump(const char *why);                         /* smp2f (hv_exc.c): boot CPU only */
void hv_win_tick(void);                         /* smp2: boot CPU hv_tick */
void hv_vgic3_maintenance(void);
void hv_vgic3_status_line(const char *prefix); /* r4.11 diagnostics */
void hv_vgic3_tick(void); /* r4.12: hv_tick, ends the NVMe SPI re-presentation hold-off */

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

/* cpuperf-A (hv_win_cppc.c): EL2-emulated CPPC page behind the firmware _CPC; no DVFS access */
int hv_win_cppc_configure(u64 base, u64 ncpus);
int hv_win_cppc_dump(void);
/* cpuperf-B: Desired -> T6050 cluster p-state (off unless enabled; perf/WRITES.md lists every write) */
int hv_win_cppc_dvfs(u64 cfg_addr, u64 len);
void hv_win_cppc_tick(void);
void hv_win_pl011_poll(void);
bool hv_win_pl011_irq_level(void);
void hv_win_pl011_raise(u32 gsiv);
u64 hv_win_pl011_stats(void); /* tx[31:0] | rx[47:32] | tx_dropped[63:48] */

/*
 * ---- Stage-2 first-execute PMUv3 rewrite (hv_win_pmu.c, docs/PMU-EL2-REWRITE.md) ----------------
 * Off unless the host calls P_HV_WIN_PMU_REWRITE(base, size, flags) for each guest RAM range after
 * P_HV_WIN_CONFIGURE and after the RAM is mapped in stage 2, before the guest starts (launcher flag
 * --pmu-rewrite el2). Guest RAM is then non-executable in stage 2 until EL2 has rewritten the PMUv3
 * accesses (hv_win_pmu_rewrite.h): by default only in a 4 KiB frame whose fetch faulted and whose
 * stage-1 mapping is proven executable; with flags = HV_WIN_PMU_MODE_16K (launcher --pmu-granule
 * 16k) in the whole 16 KiB page, as before nightly-5. Stage-2 permissions are 16 KiB either way,
 * read-only while executable and back to writable and non-executable on a write.
 * P_HV_WIN_PMU_STATS(idx) reads one counter, or one entry of the ring of the last
 * HV_WIN_PMU_LOG_SIZE rewrites.
 */
enum hv_win_pmu_stat {
    HV_WIN_PMU_ID = 0,          /* HV_WIN_PMU_MAGIC << 32 | version << 16 | 4 KiB << 1 | armed */
    HV_WIN_PMU_PAGES,           /* 16 KiB pages execute-tracked */
    HV_WIN_PMU_RANGES,          /* ranges armed */
    HV_WIN_PMU_TABLES,          /* L3 tables (16 KiB each) allocated to split 32 MiB blocks */
    HV_WIN_PMU_SCANS,           /* scans (16 KiB pages, or 4 KiB frames in the 4 KiB mode) */
    HV_WIN_PMU_PAGES_REWRITTEN, /* scans that rewrote at least one word */
    HV_WIN_PMU_INSNS,           /* words rewritten (= rewrites logged) */
    HV_WIN_PMU_INSNS_PMCCNTR,   /* mrs PMCCNTR_EL0 -> mrs CNTVCT_EL0 */
    HV_WIN_PMU_INSNS_MRS,       /* other PMU mrs -> movz #0 */
    HV_WIN_PMU_INSNS_MSR,       /* PMU msr -> nop */
    HV_WIN_PMU_WRITE_FLIPS,     /* CODE -> DIRTY on a guest write */
    HV_WIN_PMU_WALK_FLIPS,      /* CODE -> DIRTY on a stage-1 table walker update */
    HV_WIN_PMU_PINS,            /* CODE -> PINNED (RWX) at the flip cap */
    HV_WIN_PMU_PIN_FULL,        /* flip cap reached while the pin list was full: kept flipping */
    HV_WIN_PMU_SWEEPS,          /* periodic re-arms of the pin list */
    HV_WIN_PMU_UNPINS,          /* PINNED -> DIRTY by a sweep */
    HV_WIN_PMU_SPURIOUS,        /* fault on a page already in the needed state (stale TLB) */
    HV_WIN_PMU_IPA_FAIL,        /* AT could not translate the fault address: access retried */
    HV_WIN_PMU_NOT_OURS,        /* permission faults on untracked IPAs, left to the stock path */
    HV_WIN_PMU_STUCK,           /* faults handed to the stock path after too many retries */
    HV_WIN_PMU_DMA_RESETS,      /* pages whose X was revoked because EL2 filled them (device DMA) */
    HV_WIN_PMU_DATA_SKIPS,      /* PMU-looking words left alone: next word not executable (data) */
    /* nightly-5 (4 KiB scan granule; the 16 KiB mode keeps them too, see each) */
    HV_WIN_PMU_FRAMES_SCANNED,      /* 4 KiB frames scanned (16 KiB mode: 4 per page scan) */
    HV_WIN_PMU_FRAMES_MODIFIED,     /* 4 KiB frames in which a scan rewrote at least one word */
    HV_WIN_PMU_WORDS_MODIFIED,      /* words rewritten (= INSNS) */
    HV_WIN_PMU_WORDS_SKIP_EXEC,     /* 1024 per fetch whose frame was not proven: never read */
    HV_WIN_PMU_WORDS_SKIP_BOUNDARY, /* PMU word in a frame's last slot with no next word known */
    HV_WIN_PMU_WORDS_SKIP_OTHER,    /* non-PMU words examined (4 KiB mode) */
    HV_WIN_PMU_MODIFIED_NEVER_EXECUTED, /* 4 KiB: scan of an unproven frame refused; must stay 0.
                                         * 16 KiB: frames rewritten other than the faulting one */
    HV_WIN_PMU_EXEC_FAULTS,         /* 4 KiB: fetch faults on DIRTY or re-protected blocks */
    HV_WIN_PMU_EXEC_DENIED,         /* 4 KiB: of those, frame not proven (block opened unscanned) */
    HV_WIN_PMU_REPROTECTS,          /* 4 KiB: CODE blocks with an unscanned frame re-protected */
    HV_WIN_PMU_STORMS,              /* nightly-7: blocks made sticky by the storm guard (write/execute ping-pong) */
    HV_WIN_PMU_NR_STATS,
};
#define HV_WIN_PMU_MAGIC        0x504d5552UL /* "PMUR" */
#define HV_WIN_PMU_VERSION      1
#define HV_WIN_PMU_ID_4K        2UL /* HV_WIN_PMU_ID bit 1: 4 KiB scan granule */
#define HV_WIN_PMU_MODE_16K     1UL /* P_HV_WIN_PMU_REWRITE flags: legacy 16 KiB scan granule */
#define HV_WIN_PMU_LOG_SIZE     256
#define HV_WIN_PMU_STAT_LOG_IPA 0x100 /* + slot: IPA of the word (rewrite n is in slot n % 256) */
#define HV_WIN_PMU_STAT_LOG_OP  0x200 /* + slot: old word << 32 | new word */
#define HV_WIN_PMU_STAT_LOG_ELR 0x300 /* + slot: guest PC whose fetch triggered the scan */
#define HV_WIN_PMU_STAT_HEAP    0x500 /* + 0 heap bytes left (n5c: primary + pool), + 1 arm margin, + 2 primary
                                       * left, + 3/4/5 pool base/size/used (n5c).
                                       * Arm needs, per range: 16 KiB per 32 MiB block split + (4 KiB granule)
                                       * 1 byte per 16 KiB of the range, plus the margin left over. */

int hv_win_pmu_arm(u64 base, u64 size); /* pages tracked, or -1 */
int hv_win_pmu_set_mode(u64 flags);     /* before the first arm: 0 = 4 KiB, MODE_16K; or -1 */
u64 hv_win_pmu_stat(u64 idx);
bool hv_win_handle_iabort(struct exc_info *ctx);          /* true: re-execute the instruction */
bool hv_win_handle_dabort(struct exc_info *ctx, u64 esr); /* true: re-execute the access */
void hv_win_pmu_tick(void);                               /* hv_tick(), under the big HV lock */
void hv_win_pmu_report(void);                             /* printf summary (guest exit) */
/* EL2 is about to store into guest RAM for a device (DMA write): re-arm the scan of what the range
 * touches (hv_win_pmu.c). Under the big HV lock, before the copy. Returns pages whose X was revoked. */
u32 hv_win_pmu_dma_write(u64 ipa, u64 size);

#endif
