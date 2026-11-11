/* SPDX-License-Identifier: MIT */
/*
 * cpuperf-B: Windows' CPPC Desired Performance (hv_win_cppc_core.h page) -> T6050 cluster p-state.
 * Pure logic over an I/O vtable: hv_win_cppc.c plugs in read64/write64/read32 at EL2, the host test
 * (tests/win-dvfs) plugs in a mock cluster register file that logs every access.
 *
 * Register use is exactly that of the owner's Linux driver for this SoC, drivers/cpufreq/apple-soc-cpufreq.c
 * (linux-cs wt-power 0fdf38de, T6050 profile = patches 809d23c8 + bb1cac58; and the T8112 fallback that
 * has driven this machine, wt-cpufreq c92c7c3b). Every access is listed with its citation in
 * tooling/smp-20261110/perf/WRITES.md:
 *   CMD    base + 0x20, 64-bit: read (BUSY bit 31, PS1 bits 4:0) and the one write
 *          (CMD & ~PS1) | pstate | SET (bit 25), every other bit preserved.
 *   STATUS base + 0x50, 32-bit: read only (current p-state bits 9:5), for the probe and the dump.
 * Nothing else in the block is touched (never +0x38, +0xc0, +0xc8).
 *
 * Model (Linux lazy_verify, wt-power apple-soc-cpufreq.c:539-700), driven by the 1 kHz EL2 tick instead
 * of an hrtimer:
 *   - probe: CMD must be idle and its PS1 a p-state of the table; it becomes the expected state
 *     (Linux init :1432-1454). Probe failure refuses the whole feature (no write ever).
 *   - each tick, per cluster: target = table p-state for the max Desired of the cluster's CPUs, clamped
 *     to [pmin, pmax]. CMD busy: wait, or fail once the timeout since our write has passed. CMD idle:
 *     PS1 must equal the expected state (else fail: another owner); write only if target differs and
 *     the per-cluster minimum interval has passed (rate limit; later requests coalesce).
 *   - a failure latches for that cluster: no further write (Linux transition_failed).
 *   - fixed mode (cpuperf-B2, --cpu-dvfs fixed): a cluster configured with pmin == pmax is driven to that one
 *     p-state without any Desired request (one write, then nothing unless another owner moves it = latch).
 */
#ifndef HV_WIN_DVFS_CORE_H
#define HV_WIN_DVFS_CORE_H

#define DVFS_CMD          0x20
#define DVFS_STATUS       0x50
#define DVFS_CMD_BUSY     (1UL << 31)
#define DVFS_CMD_SET      (1UL << 25)
#define DVFS_CMD_PS1      0x1fUL
#define DVFS_STATUS_CUR(v) (((v) >> 5) & 0x1f)
#define DVFS_PSTATES      32
#define DVFS_MAX_CLUSTERS 4
#define DVFS_MAX_CPUS     32
#define DVFS_TIMEOUT_NS   2000000UL /* soc_t6050_info.transition_timeout_us = 2000 */
#define DVFS_MIN_INTERVAL_NS 1000000UL /* default rate limit: one write per cluster per ms */

#define DVFS_CFG_MAGIC   0x53465644U /* "DVFS" */
#define DVFS_CFG_VERSION 1

/* Launcher -> EL2 configuration (P_HV_WIN_CPPC_DVFS), little endian, packed by construction. */
struct dvfs_cfg_cluster {
    u64 base;
    u32 pmin, pmax;
    u32 perf[DVFS_PSTATES]; /* CPPC perf of p-state i; 0 = not an operating point */
};

struct dvfs_cfg {
    u32 magic, version, nclusters, ncpus;
    u64 min_interval_ns;
    u8 cluster_of[DVFS_MAX_CPUS]; /* vCPU i (= ACPI UID i = CPPC slot i) -> cluster index */
    struct dvfs_cfg_cluster c[DVFS_MAX_CLUSTERS];
};

struct dvfs_io {
    u64 (*rd64)(void *ctx, u64 addr);
    u32 (*rd32)(void *ctx, u64 addr);
    void (*wr64)(void *ctx, u64 addr, u64 val);
    void *ctx;
};

enum dvfs_fail {
    DVFS_OK = 0,
    DVFS_FAIL_BUSY_TIMEOUT = 1, /* CMD still busy DVFS_TIMEOUT_NS after our write */
    DVFS_FAIL_FOREIGN = 2,      /* idle CMD.PS1 != the state we last wrote: another owner */
};

struct dvfs_cluster {
    struct dvfs_cfg_cluster cfg;
    u64 boot_cmd;
    u32 boot_status;
    u32 expected;
    u32 target;
    bool in_flight;
    u64 deadline;
    u64 last_write;
    bool written_once;
    enum dvfs_fail fail;
    u64 fail_cmd;
    u64 writes, busy_ticks, rate_limited, no_change;
    u32 min_written, max_written;
};

struct dvfs_state {
    bool enabled;
    u32 nclusters, ncpus;
    u64 min_interval_ns;
    u8 cluster_of[DVFS_MAX_CPUS];
    struct dvfs_cluster c[DVFS_MAX_CLUSTERS];
};

/* Bases EL2 accepts (T6050 die 0: cpufreq_mc0/mc1/pc, linux-cs wt-power t6050-dieX.dtsi:17-36). */
static const u64 dvfs_t6050_bases[3] = {0x210e20000UL, 0x211e20000UL, 0x212e20000UL};

static inline int dvfs_check_cfg(const struct dvfs_cfg *cfg, u32 ncpus)
{
    if (cfg->magic != DVFS_CFG_MAGIC || cfg->version != DVFS_CFG_VERSION)
        return -1;
    if (!cfg->nclusters || cfg->nclusters > DVFS_MAX_CLUSTERS || cfg->ncpus != ncpus ||
        !ncpus || ncpus > DVFS_MAX_CPUS)
        return -2;
    if (cfg->min_interval_ns < DVFS_MIN_INTERVAL_NS || cfg->min_interval_ns > 1000000000UL)
        return -3;
    for (u32 i = 0; i < cfg->nclusters; i++) {
        const struct dvfs_cfg_cluster *c = &cfg->c[i];
        bool known = false;
        for (u32 b = 0; b < 3; b++)
            known |= c->base == dvfs_t6050_bases[b];
        if (!known)
            return -4;
        for (u32 j = 0; j < i; j++)
            if (cfg->c[j].base == c->base)
                return -4;
        /* soc_t6050_info: min_pstate 1, max_pstate 31 */
        if (c->pmin < 1 || c->pmax > 31 || c->pmin > c->pmax || !c->perf[c->pmin] || !c->perf[c->pmax])
            return -5;
        u32 last = 0;
        for (u32 p = 0; p < DVFS_PSTATES; p++) {
            if (!c->perf[p])
                continue;
            if (p == 0 || c->perf[p] <= last || c->perf[p] > 0xffff)
                return -6; /* strictly increasing, p0 never an operating point */
            last = c->perf[p];
        }
    }
    for (u32 i = 0; i < ncpus; i++)
        if (cfg->cluster_of[i] >= cfg->nclusters)
            return -7;
    for (u32 i = 0; i < cfg->nclusters; i++) {
        bool used = false;
        for (u32 k = 0; k < ncpus; k++)
            used |= cfg->cluster_of[k] == i;
        if (!used)
            return -7;
    }
    return 0;
}

/* Smallest operating point in [pmin, pmax] whose perf covers the request; 0 = lowest allowed. */
static inline u32 dvfs_pstate_for(const struct dvfs_cfg_cluster *c, u32 desired)
{
    u32 best = 0;
    for (u32 p = c->pmin; p <= c->pmax; p++) {
        if (!c->perf[p])
            continue;
        best = p;
        if (c->perf[p] >= desired)
            return p;
    }
    return best;
}

/*
 * Read-only probe (Linux init): CMD idle and PS1 in the table. Fills state; returns 0 or <0 with
 * state->enabled false. Only CMD and STATUS are read.
 */
static inline int dvfs_probe(struct dvfs_state *s, const struct dvfs_cfg *cfg, const struct dvfs_io *io)
{
    u8 *p = (u8 *)s;
    for (unsigned long i = 0; i < sizeof(*s); i++)
        p[i] = 0;
    s->nclusters = cfg->nclusters;
    s->ncpus = cfg->ncpus;
    s->min_interval_ns = cfg->min_interval_ns;
    for (u32 i = 0; i < DVFS_MAX_CPUS; i++)
        s->cluster_of[i] = cfg->cluster_of[i];
    for (u32 i = 0; i < cfg->nclusters; i++) {
        struct dvfs_cluster *c = &s->c[i];
        c->cfg = cfg->c[i];
        c->boot_cmd = io->rd64(io->ctx, c->cfg.base + DVFS_CMD);
        c->boot_status = io->rd32(io->ctx, c->cfg.base + DVFS_STATUS);
        if (c->boot_cmd & DVFS_CMD_BUSY)
            return -10 - (int)i; /* "DVFS busy at init" */
        c->expected = (u32)(c->boot_cmd & DVFS_CMD_PS1);
        if (!c->cfg.perf[c->expected])
            return -20 - (int)i; /* "current P-state has no OPP" */
        c->target = c->expected;
        c->min_written = 0xffffffffU;
    }
    s->enabled = true;
    return 0;
}

static inline void dvfs_fail_cluster(struct dvfs_cluster *c, enum dvfs_fail why, u64 cmd)
{
    c->fail = why;
    c->fail_cmd = cmd;
    c->in_flight = false;
}

/*
 * One EL2 tick. desired[i] = vCPU i's last Desired Performance, seen[i] = vCPU i has written it at
 * least once. A cluster none of whose CPUs has written Desired is left alone (no request = no write,
 * so a Windows that never engages CPPC keeps the boot p-states). now in ns.
 */
static inline void dvfs_service(struct dvfs_state *s, const struct dvfs_io *io, const u32 *desired,
                                const bool *seen, u64 now)
{
    if (!s->enabled)
        return;
    for (u32 i = 0; i < s->nclusters; i++) {
        struct dvfs_cluster *c = &s->c[i];
        if (c->fail)
            continue;

        u32 want = 0;
        bool any = false;
        for (u32 k = 0; k < s->ncpus; k++) {
            if (s->cluster_of[k] != i || !seen[k])
                continue;
            any = true;
            if (desired[k] > want)
                want = desired[k];
        }
        if (!any && c->cfg.pmin != c->cfg.pmax)
            continue; /* follow-Desired cluster without a request: untouched; fixed cluster: always */
        c->target = dvfs_pstate_for(&c->cfg, want);

        u64 cmd = io->rd64(io->ctx, c->cfg.base + DVFS_CMD);
        if (cmd & DVFS_CMD_BUSY) {
            c->busy_ticks++;
            if (!c->in_flight) {
                /* busy without a write of ours in flight: give it one timeout, then stop */
                c->in_flight = true;
                c->deadline = now + DVFS_TIMEOUT_NS;
            } else if (now > c->deadline) {
                dvfs_fail_cluster(c, DVFS_FAIL_BUSY_TIMEOUT, cmd);
            }
            continue;
        }
        c->in_flight = false;
        if ((u32)(cmd & DVFS_CMD_PS1) != c->expected) {
            dvfs_fail_cluster(c, DVFS_FAIL_FOREIGN, cmd);
            continue;
        }
        if (c->target == c->expected) {
            c->no_change++;
            continue;
        }
        if (c->written_once && now - c->last_write < s->min_interval_ns) {
            c->rate_limited++;
            continue;
        }
        u64 v = (cmd & ~DVFS_CMD_PS1) | c->target | DVFS_CMD_SET;
        io->wr64(io->ctx, c->cfg.base + DVFS_CMD, v);
        c->expected = c->target;
        c->in_flight = true;
        c->deadline = now + DVFS_TIMEOUT_NS;
        c->last_write = now;
        c->written_once = true;
        c->writes++;
        if (c->target < c->min_written)
            c->min_written = c->target;
        if (c->target > c->max_written)
            c->max_written = c->target;
    }
}

#endif
