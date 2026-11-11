/* SPDX-License-Identifier: MIT */
/*
 * Host unit test for hv_win_dvfs_core.h (cpuperf-B) against a mock of the T6050 cluster DVFS block.
 * The mock models what the owner's Linux driver relies on (linux-cs wt-power apple-soc-cpufreq.c):
 * CMD +0x20 reads BUSY (bit 31) while a transition runs and PS1 (bits 4:0) = the last request; a
 * write with SET (bit 25) starts a transition; STATUS +0x50 has the current state in bits 9:5 and the
 * target in bits 4:0 and may stay below the request (a refused boost state). Every access is logged and
 * audited: only CMD (read/write) and STATUS (read) may ever be touched.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#include "hv_win_dvfs_core.h"

static int checks, fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
        }                                                                                          \
    } while (0)

/* ---- mock DVFS block ---------------------------------------------------------------------------- */
struct mock_cluster {
    u64 base;
    u64 cmd;
    u32 cur;
    int busy_left;      /* ticks until the running transition ends; -1 = never (stuck) */
    int busy_ticks;     /* duration given to each new transition */
    u32 refuse_above;   /* STATUS current state never exceeds this (boost refused) */
};
static struct mock_cluster mc[3];
static u64 n_rd64, n_rd32, n_wr64, bad_access;
static u64 last_wr_addr, last_wr_val;
static bool wrote_while_busy;

static struct mock_cluster *find(u64 addr, u64 *off)
{
    for (int i = 0; i < 3; i++)
        if (addr >= mc[i].base && addr < mc[i].base + 0x1000) {
            *off = addr - mc[i].base;
            return &mc[i];
        }
    return NULL;
}

static u64 m_rd64(void *ctx, u64 addr)
{
    (void)ctx;
    u64 off;
    struct mock_cluster *c = find(addr, &off);
    n_rd64++;
    if (!c || off != DVFS_CMD) {
        bad_access++;
        return 0;
    }
    return c->cmd;
}

static u32 m_rd32(void *ctx, u64 addr)
{
    (void)ctx;
    u64 off;
    struct mock_cluster *c = find(addr, &off);
    n_rd32++;
    if (!c || off != DVFS_STATUS) {
        bad_access++;
        return 0;
    }
    return (c->cur << 5) | (u32)(c->cmd & 0x1f);
}

static void m_wr64(void *ctx, u64 addr, u64 val)
{
    (void)ctx;
    u64 off;
    struct mock_cluster *c = find(addr, &off);
    n_wr64++;
    last_wr_addr = addr;
    last_wr_val = val;
    if (!c || off != DVFS_CMD || !(val & DVFS_CMD_SET)) {
        bad_access++;
        return;
    }
    if (c->cmd & DVFS_CMD_BUSY)
        wrote_while_busy = true;
    c->cmd = (val & ~DVFS_CMD_SET) | DVFS_CMD_BUSY;
    c->busy_left = c->busy_ticks;
}

/* hardware time passes between EL2 ticks */
static void m_tick(void)
{
    for (int i = 0; i < 3; i++) {
        struct mock_cluster *c = &mc[i];
        if (!(c->cmd & DVFS_CMD_BUSY) || c->busy_left < 0)
            continue;
        if (c->busy_left > 0)
            c->busy_left--;
        if (c->busy_left == 0) {
            c->cmd &= ~DVFS_CMD_BUSY;
            u32 want = (u32)(c->cmd & 0x1f);
            c->cur = want > c->refuse_above ? c->refuse_above : want;
        }
    }
}

static const struct dvfs_io io = {m_rd64, m_rd32, m_wr64, NULL};

/* ---- the T6050 configuration the launcher builds (perf = MHz * capacity / 4608) --------------- */
static const u32 m_mhz[17] = {0, 300, 1344, 1644, 1992, 2304, 2652, 2964, 3240, 3504, 3696, 3876, 4044, 4176, 4284, 4308, 4380};
static const u32 p_mhz[22] = {0, 300, 1308, 1620, 1980, 2292, 2580, 2880, 3180, 3432, 3648, 3828, 3984, 4104, 4188, 4236, 4284, 4308, 4332, 4428, 4512, 4608};

static struct dvfs_cfg base_cfg(void)
{
    struct dvfs_cfg c;
    memset(&c, 0, sizeof(c));
    c.magic = DVFS_CFG_MAGIC;
    c.version = DVFS_CFG_VERSION;
    c.nclusters = 3;
    c.ncpus = 18;
    c.min_interval_ns = DVFS_MIN_INTERVAL_NS;
    for (int i = 0; i < 18; i++)
        c.cluster_of[i] = (u8)(i / 6);
    for (int k = 0; k < 2; k++) {
        c.c[k].base = dvfs_t6050_bases[k];
        c.c[k].pmin = 1;
        c.c[k].pmax = 15;
        for (int p = 1; p <= 16; p++)
            c.c[k].perf[p] = (m_mhz[p] * 851 + 2304) / 4608;
    }
    c.c[2].base = dvfs_t6050_bases[2];
    c.c[2].pmin = 1;
    c.c[2].pmax = 17;
    for (int p = 1; p <= 21; p++)
        c.c[2].perf[p] = (p_mhz[p] * 1024 + 2304) / 4608;
    return c;
}

static void mock_boot(void)
{
    /* the values iBoot leaves (runs/20261110-145557-t1b-gpu/t1-evidence.json; Linux dmesg) */
    static const u64 cmd[3] = {0x400106, 0x400102, 0x400102};
    for (int i = 0; i < 3; i++) {
        mc[i].base = dvfs_t6050_bases[i];
        mc[i].cmd = cmd[i];
        mc[i].cur = (u32)(cmd[i] & 0x1f);
        mc[i].busy_left = 0;
        mc[i].busy_ticks = 1;
        mc[i].refuse_above = 31;
    }
    n_rd64 = n_rd32 = n_wr64 = bad_access = 0;
    wrote_while_busy = false;
}

static u32 desired[DVFS_MAX_CPUS];
static bool seen[DVFS_MAX_CPUS];
static struct dvfs_state st;
static u64 now;

static void run(int ticks, u64 step_ns)
{
    for (int i = 0; i < ticks; i++) {
        m_tick();
        now += step_ns;
        dvfs_service(&st, &io, desired, seen, now);
    }
}

static void fresh(void)
{
    struct dvfs_cfg c = base_cfg();
    mock_boot();
    memset(desired, 0, sizeof(desired));
    memset(seen, 0, sizeof(seen));
    now = 1000000000UL;
    CHECK(dvfs_probe(&st, &c, &io) == 0);
}

static void request(int cpu, u32 v)
{
    desired[cpu] = v;
    seen[cpu] = true;
}

int main(void)
{
    /* layout the launcher packs (run_windows.DVFS_CFG_FMT) */
    CHECK(sizeof(struct dvfs_cfg) == 632);
    CHECK(offsetof(struct dvfs_cfg, min_interval_ns) == 16);
    CHECK(offsetof(struct dvfs_cfg, cluster_of) == 24);
    CHECK(offsetof(struct dvfs_cfg, c) == 56);
    CHECK(sizeof(struct dvfs_cfg_cluster) == 144);

    /* ---- configuration checks ---- */
    struct dvfs_cfg c = base_cfg(), b;
    CHECK(dvfs_check_cfg(&c, 18) == 0);
    CHECK(dvfs_check_cfg(&c, 17) < 0);
#define BAD(stmt)                                                                                  \
    do {                                                                                           \
        b = base_cfg();                                                                            \
        stmt;                                                                                      \
        CHECK(dvfs_check_cfg(&b, 18) < 0);                                                         \
    } while (0)
    BAD(b.magic ^= 1);
    BAD(b.version = 2);
    BAD(b.nclusters = 0);
    BAD(b.nclusters = 5);
    BAD(b.ncpus = 0);
    BAD(b.min_interval_ns = 999999);
    BAD(b.min_interval_ns = 2000000000UL);
    BAD(b.c[1].base = 0x210e200c0UL);
    BAD(b.c[1].base = 0x210e60020UL); /* cpm-impl-reg + 0x20020: hangs the bus on T6050 */
    BAD(b.c[1].base = b.c[0].base);
    BAD(b.c[0].pmin = 0);
    BAD(b.c[2].pmax = 32);
    BAD(b.c[0].pmin = 9; b.c[0].pmax = 8);
    BAD(b.c[0].perf[15] = 0);           /* pmax not an operating point */
    BAD(b.c[0].perf[0] = 1);            /* p0 never */
    BAD(b.c[0].perf[7] = b.c[0].perf[6]); /* not increasing */
    BAD(b.c[2].perf[21] = 0x10000);
    BAD(b.cluster_of[17] = 3);
    BAD(for (int i = 12; i < 18; i++) b.cluster_of[i] = 1); /* cluster 2 unused */

    /* ---- probe: read-only, Linux init rules ---- */
    mock_boot();
    CHECK(dvfs_probe(&st, &c, &io) == 0);
    CHECK(st.enabled && n_wr64 == 0 && n_rd64 == 3 && n_rd32 == 3 && bad_access == 0);
    CHECK(st.c[0].expected == 6 && st.c[1].expected == 2 && st.c[2].expected == 2);
    CHECK(st.c[0].boot_cmd == 0x400106 && DVFS_STATUS_CUR(st.c[0].boot_status) == 6);
    mock_boot();
    mc[1].cmd |= DVFS_CMD_BUSY;
    CHECK(dvfs_probe(&st, &c, &io) < 0 && !st.enabled && n_wr64 == 0);
    mock_boot();
    mc[2].cmd = 0x400100; /* p0: not an operating point */
    CHECK(dvfs_probe(&st, &c, &io) < 0 && !st.enabled);
    /* disabled state never touches hardware */
    n_rd64 = n_rd32 = n_wr64 = 0;
    request(12, 900);
    dvfs_service(&st, &io, desired, seen, 5);
    CHECK(n_rd64 == 0 && n_wr64 == 0);

    /* ---- p-state mapping ---- */
    const struct dvfs_cfg_cluster *pc = &c.c[2], *m0 = &c.c[0];
    CHECK(dvfs_pstate_for(pc, 0) == 1);
    CHECK(dvfs_pstate_for(pc, 1) == 1);
    CHECK(dvfs_pstate_for(pc, pc->perf[1] + 1) == 2);
    CHECK(dvfs_pstate_for(pc, pc->perf[17]) == 17);
    CHECK(dvfs_pstate_for(pc, pc->perf[17] + 1) == 17);  /* clamp: no p18-p21 */
    CHECK(dvfs_pstate_for(pc, 0xffffffffU) == 17);
    CHECK(dvfs_pstate_for(m0, 0xffffffffU) == 15);        /* M p16 (refused boost) excluded */
    for (u32 p = 1; p <= 15; p++)
        CHECK(dvfs_pstate_for(m0, m0->perf[p]) == p);

    /* ---- no Desired seen: no write, ever ---- */
    fresh();
    n_rd64 = n_rd32 = n_wr64 = 0;
    run(1000, 1000000);
    CHECK(n_wr64 == 0 && bad_access == 0);
    CHECK(n_rd64 == 0); /* clusters without a request are not even read */

    /* ---- one request: the exact Linux write ---- */
    fresh();
    request(12, 957);
    run(1, 1000000);
    CHECK(n_wr64 == 1);
    CHECK(last_wr_addr == 0x212e20020UL);
    CHECK(last_wr_val == 0x2400111UL); /* (0x400102 & ~0x1f) | 17 | SET; bits 22 and 8 kept */
    CHECK(st.c[2].expected == 17 && st.c[2].in_flight);
    CHECK(st.c[0].writes == 0 && st.c[1].writes == 0);
    run(3, 1000000);
    CHECK(n_wr64 == 1 && !st.c[2].in_flight && st.c[2].fail == DVFS_OK);
    CHECK(mc[2].cur == 17 && (mc[2].cmd & 0x1f) == 17);

    /* back down: Windows lowers Desired */
    request(12, 100);
    run(3, 1000000);
    CHECK(n_wr64 == 2 && (last_wr_val & 0x1f) == dvfs_pstate_for(pc, 100) && mc[2].cur == dvfs_pstate_for(pc, 100));

    /* ---- aggregation: max over the cluster ---- */
    fresh();
    request(0, 100);
    request(3, 700);
    request(5, 300);
    run(3, 1000000);
    CHECK(st.c[0].expected == dvfs_pstate_for(m0, 700));
    CHECK(st.c[1].writes == 0 && st.c[2].writes == 0); /* CPUs 6-17 have not written */
    request(3, 0);
    run(3, 1000000);
    CHECK(st.c[0].expected == dvfs_pstate_for(m0, 300));

    /* ---- busy: no write while the transition runs ---- */
    fresh();
    mc[2].busy_ticks = 5; /* 5 x 250 us = 1.25 ms, inside the 2 ms timeout */
    request(13, 957);
    run(1, 250000);
    request(13, 400);
    run(3, 250000);
    CHECK(n_wr64 == 1 && !wrote_while_busy && st.c[2].busy_ticks >= 3);
    run(10, 250000);
    CHECK(n_wr64 == 2 && !wrote_while_busy && st.c[2].fail == DVFS_OK);

    /* ---- rate limit: requests every 250 us, at most one write per 1 ms per cluster ---- */
    fresh();
    u64 t0 = now;
    for (int i = 0; i < 400; i++) {
        request(14, (i & 1) ? 957 : 100);
        run(1, 250000);
    }
    u64 span_ms = (now - t0) / 1000000;
    CHECK(st.c[2].writes <= span_ms + 1);
    CHECK(st.c[2].writes >= span_ms / 2);
    CHECK(st.c[2].rate_limited > 0 && !wrote_while_busy);

    /* ---- busy timeout: the cluster never finishes -> latched, no further write ---- */
    fresh();
    mc[2].busy_ticks = -1;
    request(12, 957);
    run(1, 1000000);
    run(2, 1000000);
    CHECK(st.c[2].fail == DVFS_OK);
    run(2, 1000000); /* > 2 ms after the write */
    CHECK(st.c[2].fail == DVFS_FAIL_BUSY_TIMEOUT);
    u64 w = n_wr64;
    mc[2].busy_ticks = 1;
    mc[2].busy_left = 0;
    mc[2].cmd &= ~DVFS_CMD_BUSY;
    request(12, 100);
    run(100, 1000000);
    CHECK(n_wr64 == w);
    /* other clusters keep working */
    request(0, 700);
    run(3, 1000000);
    CHECK(st.c[0].writes == 1 && st.c[0].fail == DVFS_OK);

    /* ---- busy without our write (another owner mid-transition) gets one timeout, then latches ---- */
    fresh();
    mc[1].cmd |= DVFS_CMD_BUSY;
    mc[1].busy_left = -1;
    request(7, 500);
    run(4, 1000000);
    CHECK(st.c[1].fail == DVFS_FAIL_BUSY_TIMEOUT && st.c[1].writes == 0);

    /* ---- foreign owner changes PS1 -> latched ---- */
    fresh();
    request(12, 957);
    run(3, 1000000);
    mc[2].cmd = (mc[2].cmd & ~0x1fUL) | 9;
    request(12, 500);
    run(3, 1000000);
    CHECK(st.c[2].fail == DVFS_FAIL_FOREIGN && (st.c[2].fail_cmd & 0x1f) == 9);
    w = n_wr64;
    run(50, 1000000);
    CHECK(n_wr64 == w);

    /* ---- refused boost: STATUS stays lower, CMD readback is what is verified (Linux) ---- */
    fresh();
    mc[2].refuse_above = 15;
    request(12, 957);
    run(5, 1000000);
    CHECK(st.c[2].fail == DVFS_OK && mc[2].cur == 15 && (mc[2].cmd & 0x1f) == 17);

    /* ---- random soak with invariants ---- */
    fresh();
    srand(12345);
    u64 last_w[3] = {0, 0, 0};
    bool bad_spacing = false, bad_value = false;
    for (int i = 0; i < 200000; i++) {
        if (rand() % 7 == 0) {
            int cpu = rand() % 18;
            request(cpu, (u32)(rand() % 1200));
        }
        for (int k = 0; k < 3; k++)
            mc[k].busy_ticks = 1 + rand() % 3;
        u64 before = n_wr64;
        m_tick();
        now += 100000 + (u64)(rand() % 900000);
        dvfs_service(&st, &io, desired, seen, now);
        if (n_wr64 != before) {
            u64 off;
            struct mock_cluster *mcl = find(last_wr_addr, &off);
            int k = (int)(mcl - mc);
            u32 p = (u32)(last_wr_val & 0x1f);
            const struct dvfs_cfg_cluster *cc = &st.c[k].cfg;
            if (p < cc->pmin || p > cc->pmax || !cc->perf[p] || !(last_wr_val & DVFS_CMD_SET) ||
                (last_wr_val & 0xffffffffffbffee0UL & ~DVFS_CMD_SET & ~DVFS_CMD_BUSY))
                bad_value = true;
            if (last_w[k] && now - last_w[k] < DVFS_MIN_INTERVAL_NS)
                bad_spacing = true;
            last_w[k] = now;
        }
    }
    CHECK(!bad_value && !bad_spacing && !wrote_while_busy && bad_access == 0);
    CHECK(st.c[0].fail == DVFS_OK && st.c[1].fail == DVFS_OK && st.c[2].fail == DVFS_OK);
    CHECK(st.c[0].writes > 100 && st.c[2].writes > 100);

    printf("%s: win-dvfs %d checks, %d failures (soak writes %lu/%lu/%lu)\n", fails ? "FAIL" : "PASS",
           checks, fails, (unsigned long)st.c[0].writes, (unsigned long)st.c[1].writes,
           (unsigned long)st.c[2].writes);
    return fails != 0;
}
