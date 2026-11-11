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
typedef int32_t s32;

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

/* ---- cpuperf-G governor: synthetic load traces on the mock ---------------------------------- */
static u64 g_idle[DVFS_MAX_CPUS];
static bool g_online[DVFS_MAX_CPUS];
static s32 g_temp = DVFS_TEMP_UNKNOWN;
static u64 g_writes_per_cluster[3];
static u64 g_last_write_t[3];
static bool g_bad_spacing, g_bad_value, g_out_of_clamp;

static struct dvfs_cfg gov_cfg(void)
{
    struct dvfs_cfg c = base_cfg();
    for (int k = 0; k < 3; k++) {
        c.c[k].pmin = 2;
        c.c[k].pmax = k < 2 ? 15 : 17;
    }
    c.gov.enabled = 1;
    c.gov.period_us = 10000;
    c.gov.up_pct = 60;
    c.gov.down_pct = 25;
    c.gov.up_periods = 2;
    c.gov.mid_periods = 5;
    c.gov.down_periods = 20;
    c.gov.mid[0] = c.gov.mid[1] = c.gov.mid[2] = 6;
    c.gov.safe_ceiling[0] = c.gov.safe_ceiling[1] = 12;
    c.gov.safe_ceiling[2] = 14;
    c.gov.temp_hot_mc = 95000;
    c.gov.temp_crit_mc = 100000;
    c.gov.temp_release_mc = 90000;
    return c;
}

static void gov_start(void)
{
    struct dvfs_cfg c = gov_cfg();
    CHECK(dvfs_check_cfg(&c, 18) == 0);
    mock_boot();
    memset(desired, 0, sizeof(desired));
    memset(seen, 0, sizeof(seen));
    memset(g_idle, 0, sizeof(g_idle));
    memset(g_writes_per_cluster, 0, sizeof(g_writes_per_cluster));
    memset(g_last_write_t, 0, sizeof(g_last_write_t));
    for (int i = 0; i < 18; i++)
        g_online[i] = true;
    g_temp = DVFS_TEMP_UNKNOWN;
    g_bad_spacing = g_bad_value = g_out_of_clamp = false;
    now = 1000000000UL;
    CHECK(dvfs_probe(&st, &c, &io) == 0);
}

/* busy(cpu, ms) -> 0..100 % busy during that millisecond */
typedef int (*load_fn)(int cpu, u64 ms);

static void gov_run(load_fn busy, u64 ms0, u64 ms1)
{
    for (u64 ms = ms0; ms < ms1; ms++) {
        for (int i = 0; i < 18; i++)
            g_idle[i] += (u64)(100 - busy(i, ms)) * 10000; /* ns of WFI in this 1 ms */
        now += 1000000;
        m_tick();
        u64 w0 = n_wr64;
        dvfs_governor(&st, g_idle, g_online, g_temp, now);
        dvfs_service(&st, &io, desired, seen, now);
        if (n_wr64 != w0) {
            u64 off;
            struct mock_cluster *mcl = find(last_wr_addr, &off);
            int k = (int)(mcl - mc);
            u32 p = (u32)(last_wr_val & 0x1f);
            g_writes_per_cluster[k]++;
            if (g_last_write_t[k] && now - g_last_write_t[k] < DVFS_MIN_INTERVAL_NS)
                g_bad_spacing = true;
            g_last_write_t[k] = now;
            if ((last_wr_val & 0xffffffffffbffee0UL & ~DVFS_CMD_SET) != 0)
                g_bad_value = true;
            u32 ceil = g_temp == DVFS_TEMP_UNKNOWN ? st.gov.safe_ceiling[k] : st.c[k].cfg.pmax;
            if (p < st.c[k].cfg.pmin || p > ceil)
                g_out_of_clamp = true;
        }
    }
}

static int l_idle(int cpu, u64 ms) { (void)cpu; (void)ms; return 0; }
static int l_one13(int cpu, u64 ms) { return (cpu == 13 && ms < 1000) ? 100 : 0; }
static int l_all(int cpu, u64 ms) { (void)cpu; (void)ms; return 100; }
static int l_burst0(int cpu, u64 ms) { return (cpu == 0 && ms % 100 < 30) ? 100 : 0; }
static int l_flip0(int cpu, u64 ms) { return (cpu == 0 && (ms / 10) % 2 == 0) ? 100 : 0; }
static int l_slow0(int cpu, u64 ms) { return (cpu == 0 && (ms / 300) % 2 == 0) ? 100 : 0; }
static int l_half(int cpu, u64 ms) { (void)ms; return cpu == 7 ? 40 : 0; }

static void governor_tests(void)
{
    struct dvfs_cfg b;
    /* config refusals */
#define GBAD(stmt)                                                                                 \
    do {                                                                                           \
        b = gov_cfg();                                                                             \
        stmt;                                                                                      \
        CHECK(dvfs_check_cfg(&b, 18) < 0);                                                         \
    } while (0)
    b = gov_cfg();
    CHECK(dvfs_check_cfg(&b, 18) == 0);
    GBAD(b.gov.enabled = 2);
    GBAD(b.gov.period_us = 999);
    GBAD(b.gov.down_pct = 60);
    GBAD(b.gov.up_pct = 101);
    GBAD(b.gov.up_periods = 0);
    GBAD(b.gov.down_periods = 1001);
    GBAD(b.gov.mid[2] = 1);
    GBAD(b.gov.mid[0] = 16);
    GBAD(b.gov.safe_ceiling[0] = 5);
    GBAD(b.gov.safe_ceiling[2] = 18);
    GBAD(b.c[1].pmin = b.c[1].pmax = 6);  /* a governed cluster needs a range */
    GBAD(b.gov.temp_release_mc = 95000);
    GBAD(b.gov.temp_crit_mc = 120000);

    /* 1. idle: only mc0 comes down from its inherited p6, once, after ~200 ms; nothing goes up */
    gov_start();
    gov_run(l_idle, 0, 3000);
    CHECK(g_writes_per_cluster[0] == 1 && g_writes_per_cluster[1] == 0 && g_writes_per_cluster[2] == 0);
    CHECK(mc[0].cur == 2 && mc[1].cur == 2 && mc[2].cur == 2);
    CHECK(g_last_write_t[0] - 1000000000UL >= 190000000UL && g_last_write_t[0] - 1000000000UL <= 260000000UL);

    /* 2. one busy core in pc: up fast (<= 40 ms), the other clusters untouched; then down slowly */
    gov_start();
    gov_run(l_one13, 0, 40);
    CHECK(mc[2].cur == 14);                       /* safe ceiling without a temperature source */
    CHECK(g_writes_per_cluster[1] == 0);
    gov_run(l_one13, 40, 1000);
    CHECK(g_writes_per_cluster[2] == 1 && mc[2].cur == 14);   /* stays while busy: no oscillation */
    gov_run(l_one13, 1000, 1150);
    CHECK(mc[2].cur == 14);                       /* not yet down after 150 ms */
    gov_run(l_one13, 1150, 1300);
    CHECK(mc[2].cur == 6);                        /* ceiling -> mid after ~200 ms */
    gov_run(l_one13, 1300, 1700);
    CHECK(mc[2].cur == 2 && g_writes_per_cluster[2] == 3);   /* mid -> floor after another ~200 ms */
    CHECK(!g_bad_spacing && !g_bad_value && !g_out_of_clamp && !wrote_while_busy && bad_access == 0);

    /* 3. all busy: every cluster at its ceiling within 40 ms, then no further writes for 3 s */
    gov_start();
    gov_run(l_all, 0, 40);
    CHECK(mc[0].cur == 12 && mc[1].cur == 12 && mc[2].cur == 14);
    u64 w = n_wr64;
    gov_run(l_all, 40, 3040);
    CHECK(n_wr64 == w);

    /* 4. bursty 30 ms every 100 ms on CPU 0: up once, never down (idle gaps < 200 ms) */
    gov_start();
    gov_run(l_burst0, 0, 5000);
    CHECK(mc[0].cur == 12 && g_writes_per_cluster[0] == 1);

    /* 5. 10 ms on / 10 ms off: neither up (needs 2 busy periods in a row) nor down: hold, no write */
    gov_start();
    gov_run(l_flip0, 0, 3000);
    CHECK(g_writes_per_cluster[0] == 0 && mc[0].cur == 6);

    /* 6. 300 ms on / 300 ms off: bounded writes (<= 2 per 600 ms cycle), never more than 4/s */
    gov_start();
    gov_run(l_slow0, 0, 6000);
    CHECK(g_writes_per_cluster[0] >= 10 && g_writes_per_cluster[0] <= 21);
    CHECK(!g_bad_spacing && !g_out_of_clamp);

    /* 7. moderate 40 % on CPU 7: floor -> mid after the mid count, never ceiling */
    gov_start();
    gov_run(l_idle, 0, 600);                      /* mc1 at floor (p2) */
    gov_run(l_half, 600, 1200);
    CHECK(mc[1].cur == 6 && g_writes_per_cluster[1] == 1);

    /* 8. thermal guard with a temperature: hot -> mid at once, crit -> floor, release -> ceiling */
    gov_start();
    g_temp = 60000;
    gov_run(l_all, 0, 50);
    CHECK(mc[0].cur == 15 && mc[2].cur == 17);   /* with a temperature the full clamp is allowed */
    g_temp = 96000;
    gov_run(l_all, 50, 70);
    CHECK(mc[0].cur == 6 && mc[2].cur == 6 && st.thermal_hot);
    g_temp = 101000;
    gov_run(l_all, 70, 90);
    CHECK(mc[0].cur == 2 && mc[2].cur == 2 && st.thermal_crit);
    g_temp = 93000;                               /* between release and hot: stays capped */
    gov_run(l_all, 90, 200);
    CHECK(mc[0].cur == 2);
    g_temp = 85000;
    gov_run(l_all, 200, 240);
    CHECK(mc[0].cur == 15 && mc[2].cur == 17 && !st.thermal_hot);
    CHECK(!g_bad_spacing && !g_out_of_clamp);

    /* 9. latch: another owner moves pc while governed -> no further pc writes */
    gov_start();
    gov_run(l_all, 0, 40);
    mc[2].cmd = (mc[2].cmd & ~0x1fUL) | 9;
    u64 wpc = g_writes_per_cluster[2];
    gov_run(l_idle, 40, 1000);
    CHECK(st.c[2].fail == DVFS_FAIL_FOREIGN && g_writes_per_cluster[2] == wpc);
    CHECK(mc[0].cur == 2);                        /* the M clusters keep being governed */

    /* 10. offline vCPUs count as idle; online CPU 1 busy drives mc0 */
    gov_start();
    for (int i = 0; i < 18; i++)
        g_online[i] = i < 2;
    gov_run(l_all, 0, 40);
    CHECK(mc[0].cur == 12 && mc[1].cur == 2 && mc[2].cur == 2);

    /* 11. an idle delta larger than the period (a WFI in progress counted up to "now", then the counter
     * catching up) is clamped: it reads as fully idle, never as busy */
    gov_start();
    gov_run(l_idle, 0, 50);
    for (int i = 0; i < 18; i++)
        g_idle[i] += 50000000; /* 50 ms extra idle reported inside one 10 ms period */
    {
        u64 e0 = st.gov_evals, ms = 50;
        while (st.gov_evals == e0 && ms < 80) { /* up to the first evaluation that sees the jump */
            gov_run(l_idle, ms, ms + 1);
            ms++;
        }
        CHECK(st.gov_evals == e0 + 1);
        CHECK(st.c[0].util == 0 && st.c[1].util == 0 && st.c[2].util == 0);
        gov_run(l_idle, ms, 600);
    }
    CHECK(g_writes_per_cluster[1] == 0 && g_writes_per_cluster[2] == 0 && mc[0].cur == 2);
}

int main(void)
{
    /* layout the launcher packs (run_windows.DVFS_CFG_FMT) */
    CHECK(sizeof(struct dvfs_cfg) == 712); /* cpuperf-G: + struct dvfs_gov_cfg (76 bytes, padded to 8) */
    CHECK(offsetof(struct dvfs_cfg, gov) == 632);
    CHECK(sizeof(struct dvfs_gov_cfg) == 76);
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
    BAD(b.version = 3);
    BAD(b.version = 1; b.gov.enabled = 1);
    b = base_cfg();
    b.version = 1; /* the 632-byte cpuperf-B layout (gov block zero-filled by the glue) stays accepted */
    CHECK(dvfs_check_cfg(&b, 18) == 0);
    CHECK(DVFS_CFG_V1_SIZE == offsetof(struct dvfs_cfg, gov));
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

    /* ---- fixed mode (pmin == pmax): driven without any Desired request ---- */
    {
        struct dvfs_cfg fc = base_cfg();
        for (int k = 0; k < 3; k++)
            fc.c[k].pmin = fc.c[k].pmax = 6;
        CHECK(dvfs_check_cfg(&fc, 18) == 0);
        mock_boot();
        memset(desired, 0, sizeof(desired));
        memset(seen, 0, sizeof(seen));
        now = 1000000000UL;
        CHECK(dvfs_probe(&st, &fc, &io) == 0);
        run(1, 1000000);
        CHECK(n_wr64 == 2);                                  /* mc0 is already p6: only mc1 and pc */
        CHECK(st.c[0].writes == 0 && st.c[1].writes == 1 && st.c[2].writes == 1);
        run(1000, 1000000);
        CHECK(n_wr64 == 2 && mc[1].cur == 6 && mc[2].cur == 6 && bad_access == 0);
        request(12, 957);                                    /* Desired is ignored by a fixed cluster */
        run(10, 1000000);
        CHECK(n_wr64 == 2 && (mc[2].cmd & 0x1f) == 6);
        mc[1].cmd = (mc[1].cmd & ~0x1fUL) | 9;               /* another owner: latch, no fight */
        run(5, 1000000);
        CHECK(st.c[1].fail == DVFS_FAIL_FOREIGN && n_wr64 == 2);
        CHECK(((last_wr_val & 0xffffffffffbffee0UL) & ~DVFS_CMD_SET) == 0);
    }
    {
        /* mixed: P fixed at p10, M clusters follow Desired (none seen -> untouched) */
        struct dvfs_cfg fc = base_cfg();
        fc.c[2].pmin = fc.c[2].pmax = 10;
        CHECK(dvfs_check_cfg(&fc, 18) == 0);
        mock_boot();
        memset(desired, 0, sizeof(desired));
        memset(seen, 0, sizeof(seen));
        CHECK(dvfs_probe(&st, &fc, &io) == 0);
        run(50, 1000000);
        CHECK(n_wr64 == 1 && last_wr_addr == 0x212e20020UL && last_wr_val == 0x240010aUL);
        CHECK(st.c[0].writes == 0 && st.c[1].writes == 0);
    }
    fresh();

    governor_tests();

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
