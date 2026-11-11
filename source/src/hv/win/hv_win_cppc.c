/* SPDX-License-Identifier: MIT */
/*
 * cpuperf-A: EL2 side of the firmware's per-CPU _CPC (see hv_win_cppc_core.h for the page).
 * P_HV_WIN_CPPC_CONFIGURE(base, ncpus) maps one 16 KiB hook page in the synthetic window. The
 * hook only stores the guest's Desired Performance requests and serves CNTPCT as both feedback
 * counters: no SoC register is read or written, and no frequency changes.
 */

#include "hv_win.h"
#include "utils.h"

#include "../hv.h"

#include "hv_win_cppc_core.h"

static struct cppc_state cppc;

static bool cppc_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    return cppc_access(&cppc, addr - cppc.base, val, write, width, mrs(CNTPCT_EL0));
}

int hv_win_cppc_configure(u64 base, u64 ncpus)
{
    if (cppc_check_config(base, ncpus) < 0) {
        printf("HV/win cpuperf: CPPC page 0x%lx / %ld CPUs refused (16 KiB aligned, 1..%ld CPUs)\n",
               base, ncpus, (long)CPPC_MAX_CPUS);
        return -1;
    }
    if (cppc.configured) {
        printf("HV/win cpuperf: CPPC page already at 0x%lx\n", cppc.base);
        return cppc.base == base && cppc.ncpus == ncpus ? 0 : -1;
    }
    if (hv_pt_is_ram(base)) {
        printf("HV/win cpuperf: CPPC page 0x%lx is guest RAM\n", base);
        return -1;
    }

    cppc_reset(&cppc, base, (u32)ncpus);
    if (hv_map_hook(base, cppc_hook, CPPC_PAGE_SIZE) < 0) {
        printf("HV/win cpuperf: CPPC hook at 0x%lx failed\n", base);
        cppc.configured = false;
        return -1;
    }

    printf("HV/win cpuperf: CPPC page at 0x%lx for %ld CPUs (desired stored only; counters = "
           "CNTPCT; no DVFS access)\n",
           base, ncpus);
    return 0;
}

/* Printed from the SMP dump and on P_HV_WIN_CPPC_STAT: one line per CPU that saw any access. */
int hv_win_cppc_dump(void)
{
    if (!cppc.configured)
        return -1;
    for (u32 i = 0; i < cppc.ncpus; i++) {
        struct cppc_slot *s = &cppc.slot[i];
        if (!s->desired_writes && !s->counter_reads)
            continue;
        printf("HV/win cpuperf: CPU %d desired last %d min %d max %d writes %ld; counter reads %ld\n",
               i, s->desired, s->desired_writes ? s->desired_min : 0, s->desired_max,
               s->desired_writes, s->counter_reads);
    }
    printf("HV/win cpuperf: odd accesses %ld\n", cppc.odd_accesses);
    return 0;
}
