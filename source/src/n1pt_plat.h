/* SPDX-License-Identifier: MIT */
#ifndef N1PT_PLAT_H
#define N1PT_PLAT_H

/*
 * n1pt (wireless/n1-native-20261111): the hardware half of the N1 passthrough below the resident glue
 * (hv/win/hv_win_n1pt.c): programming of the real root port, functions, port MSI and RID2SID, the DART for the
 * pool, and the three service-page commands with the storage guard. Every register it touches is listed in
 * wireless/n1-native-20261111/platform/REGISTERS.md. Host-tested in tests/win-n1pt against a register model.
 * Needs u32/u64 from the includer.
 */

#include "m5wn/n1/n1_hal.h"
#include "n1pt_dart.h"

struct n1pt;

/* stage-2 and AIC actions the commands need from the resident (host test: recorded) */
struct n1pt_plat_cb {
    void *ctx;
    int (*map_bars)(void *ctx, int bar0, int bar2); /* 1 = device map, 0 = back to the all-ones hook */
    void (*link_irq)(void *ctx, int on);            /* AIC line of the port interrupt */
};

struct n1pt_plat {
    struct n1_addrs a;  /* pcie_t6050_n1_addrs() */
    u64 win;            /* 1 MiB window: BAR0 +0x10000, BAR2 +0x20000 (fn1, fn2 at +64 KiB), BAR4 +0x40000 */
    struct n1pt_dart_tables dart;
    int four_level;
    int fns_enabled;
    int pre_train_failed; /* cycle: RID2SID / port MSI restore before training failed */
    const struct n1pt_plat_cb *cb;
};

#define N1PT_RP_CFG(p)          ((p)->a.ecam)                                  /* root port 00:00.0 */
#define N1PT_FN_CFG(p, fn)      ((p)->a.ecam + ((u64)1 << 20) + ((u64)(fn) << 12)) /* 01:00.fn */
#define N1PT_FN_BAR(p, fn)      ((p)->win + (u64)0x10000 * (1 + (fn)))

/* config access to the real bus 1 functions (dword) */
u32 n1pt_plat_cfg_read(struct n1pt_plat *p, unsigned fn, unsigned off);
void n1pt_plat_cfg_write(struct n1pt_plat *p, unsigned fn, unsigned off, u32 v);
int n1pt_plat_link_up(struct n1pt_plat *p);
/* fn0 BAR0 + 0x8000 (ROM/IPC stage), all-ones without a link */
u32 n1pt_plat_rom_stage(struct n1pt_plat *p);

/*
 * Pre-guest, after pcie_t6050_n1_train_ans(): checks and programs everything INTERFACE.md "Before the guest
 * starts" lists except the training itself: endpoint 106b:1900, root port window/COMMAND, fn0 BAR0 + MEM,
 * BAR0+0x8000 == 0, fn0 MSI (4 vectors), port MSI address/map/enable, RID2SID 0x100-0x102 -> 1-3, the DART for
 * the pool. 0, or negative (printed) with nothing mapped for the guest.
 */
int n1pt_plat_prepare(struct n1pt_plat *p, u64 pool, u64 iova, u64 size);

/* re-programs the bridge, every real function's BAR/MSI/COMMAND, RID2SID and the port MSI after a link cycle;
 * 0 only if the endpoint answers 106b:1900/1901 */
int n1pt_plat_restore(struct n1pt_plat *p, const u16 cmd[3]);

/* port interrupt: unmask (link down) / mask; read-and-clear of the port INTSTAT */
void n1pt_plat_link_irq_enable(struct n1pt_plat *p, int on);
u32 n1pt_plat_link_irq_ack(struct n1pt_plat *p);

/* the service-page commands (struct n1pt_ops.command); sets s->guard/parked/fns_enabled */
int n1pt_plat_command(struct n1pt_plat *p, struct n1pt *s, unsigned cmd);

#endif
