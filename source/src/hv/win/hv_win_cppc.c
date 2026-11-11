/* SPDX-License-Identifier: MIT */
/*
 * cpuperf-A: EL2 side of the firmware's per-CPU _CPC (see hv_win_cppc_core.h for the page).
 * P_HV_WIN_CPPC_CONFIGURE(base, ncpus) maps one 16 KiB hook page in the synthetic window. The
 * hook only stores the guest's Desired Performance requests and serves CNTPCT as both feedback
 * counters: no SoC register is read or written, and no frequency changes.
 */

#include "hv_win.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#include "../hv.h"

#include "hv_win_cppc_core.h"
#include "hv_win_dvfs_core.h"

static struct cppc_state cppc;

/*
 * cpuperf-B (off unless P_HV_WIN_CPPC_DVFS succeeded): the boot CPU's 1 kHz tick maps the stored
 * Desired requests to cluster p-states, see hv_win_dvfs_core.h and perf/WRITES.md.
 */
static struct dvfs_state dvfs;
static struct dvfs_cfg dvfs_cfg_copy;

static u64 dvfs_rd64(void *ctx, u64 addr)
{
    UNUSED(ctx);
    return read64(addr);
}

static u32 dvfs_rd32(void *ctx, u64 addr)
{
    UNUSED(ctx);
    return read32(addr);
}

static void dvfs_wr64(void *ctx, u64 addr, u64 val)
{
    UNUSED(ctx);
    write64(addr, val);
}

static const struct dvfs_io dvfs_hw = {dvfs_rd64, dvfs_rd32, dvfs_wr64, NULL};

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

int hv_win_cppc_dvfs(u64 cfg_addr, u64 len)
{
    if (!cppc.configured) {
        printf("HV/win cpuperf-B: configure the CPPC page first\n");
        return -1;
    }
    if (dvfs.enabled) {
        printf("HV/win cpuperf-B: already enabled\n");
        return -1;
    }
    if (chip_id != T6050) {
        printf("HV/win cpuperf-B: chip 0x%x is not T6050, refused\n", chip_id);
        return -1;
    }
    if (len != sizeof(dvfs_cfg_copy) || !cfg_addr) {
        printf("HV/win cpuperf-B: config length %ld != %ld\n", len, (long)sizeof(dvfs_cfg_copy));
        return -1;
    }
    memcpy(&dvfs_cfg_copy, (void *)cfg_addr, sizeof(dvfs_cfg_copy));
    int rc = dvfs_check_cfg(&dvfs_cfg_copy, cppc.ncpus);
    if (rc < 0) {
        printf("HV/win cpuperf-B: config refused (%d)\n", rc);
        return -1;
    }
    /* read-only probe of CMD/STATUS on each cluster; any problem leaves the feature off */
    rc = dvfs_probe(&dvfs, &dvfs_cfg_copy, &dvfs_hw);
    for (u32 i = 0; i < dvfs_cfg_copy.nclusters; i++) {
        struct dvfs_cluster *c = &dvfs.c[i];
        printf("HV/win cpuperf-B: cluster %d @0x%lx probe: cmd 0x%lx status 0x%x (current p%d, "
               "target p%d), clamp p%d..p%d\n",
               i, c->cfg.base, c->boot_cmd, c->boot_status, DVFS_STATUS_CUR(c->boot_status),
               c->boot_status & 0x1f, c->cfg.pmin, c->cfg.pmax);
    }
    if (rc < 0) {
        dvfs.enabled = false;
        printf("HV/win cpuperf-B: probe refused (%d): no DVFS write will be made\n", rc);
        return -1;
    }
    printf("HV/win cpuperf-B: ON, rate limit %ld ns per cluster, timeout %ld ns\n",
           dvfs.min_interval_ns, (long)DVFS_TIMEOUT_NS);
    return 0;
}

/* hv_tick() (boot CPU, big HV lock held): a no-op unless cpuperf-B is on. */
void hv_win_cppc_tick(void)
{
    static u32 desired[DVFS_MAX_CPUS];
    static bool seen[DVFS_MAX_CPUS];

    if (!dvfs.enabled)
        return;
    for (u32 i = 0; i < dvfs.ncpus; i++) {
        desired[i] = cppc.slot[i].desired;
        seen[i] = cppc.slot[i].desired_writes != 0;
    }
    dvfs_service(&dvfs, &dvfs_hw, desired, seen, mrs(CNTPCT_EL0) * 1000000000UL / mrs(CNTFRQ_EL0));
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
    if (!dvfs.enabled)
        return 0;
    for (u32 i = 0; i < dvfs.nclusters; i++) {
        struct dvfs_cluster *c = &dvfs.c[i];
        u32 st = read32(c->cfg.base + DVFS_STATUS); /* read only */
        printf("HV/win cpuperf-B: cluster %d status 0x%x (current p%d, target p%d) expected p%d want p%d "
               "writes %ld (p%d..p%d) busy %ld ratelim %ld fail %d cmd 0x%lx\n",
               i, st, DVFS_STATUS_CUR(st), st & 0x1f, c->expected, c->target, c->writes,
               c->writes ? c->min_written : 0, c->max_written, c->busy_ticks, c->rate_limited, c->fail,
               c->fail_cmd);
    }
    return 0;
}
