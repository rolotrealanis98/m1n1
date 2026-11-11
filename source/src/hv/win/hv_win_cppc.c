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

/* cpuperf-G inputs */
extern u64 hv_win_wfi_idle_tk[MAX_CPUS];
extern u64 hv_win_wfi_since_tk[MAX_CPUS];
extern bool hv_started_cpus[MAX_CPUS];

/*
 * CPU temperature in milli-degC for the governor's thermal guard, DVFS_TEMP_UNKNOWN when this build has
 * no SMC temperature reads. A build that runs the SMC power reads (--smc-power) provides the strong
 * definition (TCMb, the key Linux's macsmc_soc_die and its draft thermal zone use); see perf/GOVERNOR.md.
 */
int __attribute__((weak)) hv_win_cpu_temp_mc(void)
{
    return DVFS_TEMP_UNKNOWN;
}

/* timeline of governor-driven p-state changes (bounded) and residency per cluster/p-state */
#define GOV_LOG_MAX 300
static u32 gov_logged;
static u32 gov_prev_expected[DVFS_MAX_CLUSTERS];
static u64 gov_res_ns[DVFS_MAX_CLUSTERS][DVFS_PSTATES];
static u64 gov_res_last, gov_t0;

/*
 * n7cpu Stage C: scaled Delivered counters (hv_win_cppc_core.h struct cppc_dl), one per cluster. On only
 * with the CPPC page and cpuperf-B, when the launcher appended the per-cluster Reference Performance
 * (the _CPC Nominal it published) to the DVFS config. Rate = perf(STATUS current p-state) / reference,
 * re-read and rebased at every boot-CPU tick.
 */
#define DL_TAIL_SIZE (DVFS_MAX_CLUSTERS * sizeof(u32))
static bool dl_on;
static u32 dl_ref[DVFS_MAX_CLUSTERS];
static struct cppc_dl dl[DVFS_MAX_CLUSTERS];
static u32 dl_cur[DVFS_MAX_CLUSTERS];
static u64 dl_start_t[DVFS_MAX_CLUSTERS], dl_start_d[DVFS_MAX_CLUSTERS];
static u64 dl_status_odd[DVFS_MAX_CLUSTERS];

static bool cppc_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 now = mrs(CNTPCT_EL0), off = addr - cppc.base, delivered = now;
    u64 cpu = off / CPPC_SLOT;
    if (dl_on && cpu < dvfs.ncpus && dvfs.cluster_of[cpu] < dvfs.nclusters)
        delivered = cppc_dl_value(&dl[dvfs.cluster_of[cpu]], now);
    return cppc_access2(&cppc, off, val, write, width, now, delivered);
}

/* The cluster's current p-state for the Delivered rate: STATUS bits 9:5 when that is an operating point of
 * the table, else the state EL2 last wrote or inherited. */
static u32 dl_pstate(u32 i)
{
    struct dvfs_cluster *c = &dvfs.c[i];
    u32 cur = DVFS_STATUS_CUR(read32(c->cfg.base + DVFS_STATUS)); /* read only, as Linux get() */
    if (cur < DVFS_PSTATES && c->cfg.perf[cur])
        return cur;
    dl_status_odd[i]++;
    return c->expected;
}

static void dl_update(u64 t)
{
    for (u32 i = 0; i < dvfs.nclusters; i++) {
        u32 p = dl_pstate(i);
        dl_cur[i] = p;
        cppc_dl_rebase(&dl[i], t, dvfs.c[i].cfg.perf[p], dl_ref[i]);
    }
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
    if (dvfs.enabled) {
        printf("HV/win cpuperf-B: already enabled\n");
        return -1;
    }
    if (chip_id != T6050) {
        printf("HV/win cpuperf-B: chip 0x%x is not T6050, refused\n", chip_id);
        return -1;
    }
    /* n7cpu: v2 + a 16-byte tail = u32 Reference Performance per cluster for the scaled Delivered counter */
    bool tail = len == sizeof(dvfs_cfg_copy) + DL_TAIL_SIZE;
    if ((len != sizeof(dvfs_cfg_copy) && len != DVFS_CFG_V1_SIZE && !tail) || !cfg_addr) {
        printf("HV/win cpuperf-B: config length %ld is neither %ld (v2), %ld (v2 + reference perf) nor %d (v1)\n",
               len, (long)sizeof(dvfs_cfg_copy), (long)(sizeof(dvfs_cfg_copy) + DL_TAIL_SIZE), DVFS_CFG_V1_SIZE);
        return -1;
    }
    memset(&dvfs_cfg_copy, 0, sizeof(dvfs_cfg_copy));
    memset(dl_ref, 0, sizeof(dl_ref));
    memcpy(&dvfs_cfg_copy, (void *)cfg_addr, tail ? sizeof(dvfs_cfg_copy) : len);
    if (tail) {
        memcpy(dl_ref, (u8 *)cfg_addr + sizeof(dvfs_cfg_copy), DL_TAIL_SIZE);
        if (dvfs_cfg_copy.version != DVFS_CFG_VERSION || !cppc.configured) {
            printf("HV/win cpuperf-C: reference perf needs a v%d config and the CPPC page\n", DVFS_CFG_VERSION);
            return -1;
        }
        for (u32 i = 0; i < DVFS_MAX_CLUSTERS; i++) {
            if (dl_ref[i] > 0xffff || (i >= dvfs_cfg_copy.nclusters && dl_ref[i])) {
                printf("HV/win cpuperf-C: reference perf %d for cluster %d refused\n", dl_ref[i], i);
                return -1;
            }
        }
    }
    if (len == DVFS_CFG_V1_SIZE && dvfs_cfg_copy.version != 1) {
        printf("HV/win cpuperf-B: a %d-byte config must be version 1\n", DVFS_CFG_V1_SIZE);
        return -1;
    }
    /* Without the CPPC page (no _CPC: --cpu-dvfs fixed/governor with uniform/adt classes) only fixed or
     * governed clusters make sense */
    if (!cppc.configured && !dvfs_cfg_copy.gov.enabled) {
        for (u32 i = 0; i < dvfs_cfg_copy.nclusters && i < DVFS_MAX_CLUSTERS; i++) {
            if (dvfs_cfg_copy.c[i].pmin != dvfs_cfg_copy.c[i].pmax) {
                printf("HV/win cpuperf-B: cluster %d follows Desired but there is no CPPC page\n", i);
                return -1;
            }
        }
    }
    int rc = dvfs_check_cfg(&dvfs_cfg_copy, cppc.configured ? cppc.ncpus : dvfs_cfg_copy.ncpus);
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
    u32 nfixed = 0;
    for (u32 i = 0; i < dvfs.nclusters; i++)
        nfixed += dvfs.c[i].cfg.pmin == dvfs.c[i].cfg.pmax;
    bool scaled = false;
    for (u32 i = 0; i < dvfs.nclusters; i++)
        scaled |= dl_ref[i] != 0;
    if (scaled) {
        u64 t = mrs(CNTPCT_EL0);
        for (u32 i = 0; i < dvfs.nclusters; i++) {
            cppc_dl_init(&dl[i], t);
            dl_start_t[i] = dl_start_d[i] = t;
        }
        dl_update(t);
        dl_on = true; /* the hook uses dl[] only from here on */
        for (u32 i = 0; i < dvfs.nclusters; i++)
            printf("HV/win cpuperf-C: cluster %d delivered counter scaled: p%d perf %d / reference %d\n", i,
                   dl_cur[i], dvfs.c[i].cfg.perf[dl_cur[i]], dl_ref[i]);
    }
    printf("HV/win cpuperf-B: ON, rate limit %ld ns per cluster, timeout %ld ns, %d of %d cluster(s) fixed, "
           "CPPC page %s\n",
           dvfs.min_interval_ns, (long)DVFS_TIMEOUT_NS, nfixed, dvfs.nclusters, cppc.configured ? "yes" : "no");
    if (dvfs.gov.enabled) {
        int t = hv_win_cpu_temp_mc();
        printf("HV/win cpuperf-G: governor period %d us up %d%%x%d mid x%d down <%d%%x%d; temperature %s; "
               "hot %d crit %d release %d mC\n",
               dvfs.gov.period_us, dvfs.gov.up_pct, dvfs.gov.up_periods, dvfs.gov.mid_periods,
               dvfs.gov.down_pct, dvfs.gov.down_periods,
               t == DVFS_TEMP_UNKNOWN ? "none (ceiling capped at safe_ceiling)" : "SMC", dvfs.gov.temp_hot_mc,
               dvfs.gov.temp_crit_mc, dvfs.gov.temp_release_mc);
        for (u32 i = 0; i < dvfs.nclusters; i++)
            printf("HV/win cpuperf-G: cluster %d floor p%d mid p%d ceiling p%d safe ceiling p%d\n", i,
                   dvfs.c[i].cfg.pmin, dvfs.gov.mid[i], dvfs.c[i].cfg.pmax, dvfs.gov.safe_ceiling[i]);
    }
    return 0;
}

/* hv_tick() (boot CPU, big HV lock held): a no-op unless cpuperf-B is on. */
void hv_win_cppc_tick(void)
{
    static u32 desired[DVFS_MAX_CPUS];
    static bool seen[DVFS_MAX_CPUS];

    static u64 idle_ns[DVFS_MAX_CPUS];
    static bool online[DVFS_MAX_CPUS];

    if (!dvfs.enabled)
        return;
    u64 tk = mrs(CNTPCT_EL0), frq = mrs(CNTFRQ_EL0);
    u64 now = tk / frq * 1000000000UL + (tk % frq) * 1000000000UL / frq;
    for (u32 i = 0; i < dvfs.ncpus; i++) {
        desired[i] = cppc.configured ? cppc.slot[i].desired : 0;
        seen[i] = cppc.configured && cppc.slot[i].desired_writes != 0;
    }
    if (dvfs.gov.enabled) {
        for (u32 i = 0; i < dvfs.ncpus; i++) {
            int cpu = hv_win.vcpu_cpu[i];
            if (cpu < 0 || cpu >= MAX_CPUS) {
                online[i] = false;
                idle_ns[i] = 0;
                continue;
            }
            u64 since = __atomic_load_n(&hv_win_wfi_since_tk[cpu], __ATOMIC_ACQUIRE);
            u64 idle = __atomic_load_n(&hv_win_wfi_idle_tk[cpu], __ATOMIC_RELAXED);
            if (since && tk > since)
                idle += tk - since; /* a WFI in progress counts up to now */
            idle_ns[i] = idle / frq * 1000000000UL + (idle % frq) * 1000000000UL / frq;
            online[i] = i == 0 || hv_started_cpus[cpu];
        }
        dvfs_governor(&dvfs, idle_ns, online, hv_win_cpu_temp_mc(), now);
    }
    dvfs_service(&dvfs, &dvfs_hw, desired, seen, now);
    if (dl_on)
        dl_update(tk);

    /* residency (by the state we last wrote / inherited) and a bounded change timeline */
    if (!gov_res_last) {
        gov_res_last = gov_t0 = now;
        for (u32 i = 0; i < dvfs.nclusters; i++)
            gov_prev_expected[i] = dvfs.c[i].expected;
        return;
    }
    for (u32 i = 0; i < dvfs.nclusters; i++) {
        struct dvfs_cluster *c = &dvfs.c[i];
        if (gov_prev_expected[i] < DVFS_PSTATES)
            gov_res_ns[i][gov_prev_expected[i]] += now - gov_res_last;
        if (c->expected != gov_prev_expected[i]) {
            if (gov_logged < GOV_LOG_MAX) {
                gov_logged++;
                printf("HV/win cpuperf-G: +%ld ms cluster %d p%d -> p%d util %d%% temp %d mC\n",
                       (long)((now - gov_t0) / 1000000), i, gov_prev_expected[i], c->expected, c->util,
                       dvfs.last_temp == DVFS_TEMP_UNKNOWN ? -1 : dvfs.last_temp);
            }
            gov_prev_expected[i] = c->expected;
        }
    }
    gov_res_last = now;
}

/* Printed from the SMP dump and on P_HV_WIN_CPPC_STAT: one line per CPU that saw any access. */
int hv_win_cppc_dump(void)
{
    if (!cppc.configured && !dvfs.enabled)
        return -1;
    for (u32 i = 0; cppc.configured && i < cppc.ncpus; i++) {
        struct cppc_slot *s = &cppc.slot[i];
        if (!s->desired_writes && !s->counter_reads)
            continue;
        printf("HV/win cpuperf: CPU %d desired last %d min %d max %d writes %ld; counter reads %ld\n",
               i, s->desired, s->desired_writes ? s->desired_min : 0, s->desired_max,
               s->desired_writes, s->counter_reads);
    }
    if (cppc.configured)
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
        if (dl_on) {
            u64 t = mrs(CNTPCT_EL0);
            u64 dr = t - dl_start_t[i], dd = cppc_dl_value(&dl[i], t) - dl_start_d[i];
            printf("HV/win cpuperf-C: cluster %d delivered/reference avg %ld.%02ld%% (p%d now, perf %d / ref %d), "
                   "status not an OPP %ld\n",
                   i, dr ? (long)(dd * 100 / dr) : 0, dr ? (long)((dd * 10000 / dr) % 100) : 0, dl_cur[i],
                   c->cfg.perf[dl_cur[i]], dl_ref[i], dl_status_odd[i]);
        }
        if (dvfs.gov.enabled) {
            char buf[256];
            int n = 0;
            for (u32 p = 0; p < DVFS_PSTATES && n < 230; p++)
                if (gov_res_ns[i][p] >= 1000000)
                    n += snprintf(buf + n, sizeof(buf) - n, " p%d:%ldms", p, (long)(gov_res_ns[i][p] / 1000000));
            buf[n] = 0;
            printf("HV/win cpuperf-G: cluster %d level %d util %d%% changes %ld evals %ld thermal %s%s temp %d mC;"
                   " residency%s\n",
                   i, c->level, c->util, c->gov_changes, dvfs.gov_evals,
                   dvfs.thermal_crit ? "CRIT" : dvfs.thermal_hot ? "HOT" : "ok",
                   dvfs.last_temp == DVFS_TEMP_UNKNOWN ? " (no source: safe ceiling)" : "",
                   dvfs.last_temp == DVFS_TEMP_UNKNOWN ? -1 : dvfs.last_temp, buf);
        }
    }
    return 0;
}
