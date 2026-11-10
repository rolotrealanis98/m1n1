/* SPDX-License-Identifier: MIT */
/*
 * Host build of src/hv/win/hv_vgic3.c (tests/win-vgic3): everything the file takes from m1n1 at EL2
 * -- system registers, the AIC, SMP/IPIs, the stage-2 hook API, spinlocks, printf -- is provided by
 * the mocks in test_vgic3.c.
 */

#ifndef VGIC3_HOST_H
#define VGIC3_HOST_H

#include <stdio.h>
#include <string.h>

#include "exception.h"
#include "smp_asm.h"
#include "types.h"

#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))

/* src/aic.h, src/aic_regs.h */
#define AIC_MAX_DIES   4
#define AIC_MAX_HW_NUM (0x80 * 32)
void aic_mask(u32 irq);
void aic_unmask(u32 irq);

/* src/utils.h */
typedef struct {
    int depth;
    int owner;
} spinlock_t;
#define SPINLOCK_INIT {0, -1}
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* smp2f: the generic counter, read by the diagnostics (time stamps, dump ages) */
#define CNTPCT_EL0 sys_reg(3, 3, 14, 0, 1)
#define CNTFRQ_EL0 sys_reg(3, 3, 14, 0, 0)

u64 host_mrs(u32 reg);
void host_msr(u32 reg, u64 val);
#define mrs(reg)      host_mrs(reg)
#define msr(reg, val) host_msr(reg, val)
#define sysop(op)                                                                                  \
    do {                                                                                           \
    } while (0)

int host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifndef VGIC3_HOST_KEEP_PRINTF
#define printf host_printf
#endif

/* src/smp.h */
extern int host_cpu;
static inline int smp_id(void)
{
    return host_cpu;
}
u64 smp_get_mpidr(int cpu);
void smp_send_ipi(int cpu);

/* src/hv/hv.h */
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
u64 hv_pt_walk(u64 addr);

#endif
