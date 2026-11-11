/* SPDX-License-Identifier: MIT */
/* wireless/n1-20261111: resident side of the N1 (Wi-Fi) work.
 *   P_HV_WIN_N1_PROBE (flags)                 read-only probe of apcie0 port 0 (n1_probe.c)
 *   P_HV_WIN_N1_STAT  (index)                 probe/core result words
 *   P_HV_WIN_N1_START (fw, fw_len, stage, f)  bring-up up to `stage` (1 train .. 5 radio), pre-guest
 *   P_HV_WIN_N1_CTL   (op, arg)               1 = scan and wait (prints the list), 2 = join (arg = wn_join)
 * Safety (WRITES.md "shared PHY"): START refuses once any vCPU started, and in any session where the
 * disk path is or was enabled (native storage tried/selected, or S1 storage prepared): port 0
 * training shares the GP PHY/power closure with the SSD controller; the training itself keeps
 * m1n1's storage snapshot/compare around every shared stage and (with ANS started) a GPT block
 * read before/after, resetting through the watchdog if storage is in doubt. */
#include "hv_win_n1.h"
#include "m5wn/n1/n1_probe.h"
#include "m5wn/n1/n1_core.h"
#include "pcie_t6050.h"
#include "pcie_t6050_n1.h"
#include "hv/hv.h"
#include "hv/win/hv_win.h"
#include "hv/win/hv_win_storage.h"
#include "dart.h"
#include "malloc.h"
#include "pmgr.h"
#include "smc.h"
#include "rtkit.h"
#include "smp.h"
#include "string.h"
#include "utils.h"
#include "vsprintf.h"
#include <stdarg.h>

#define N1_POOL_SIZE (128ul << 20)
#define N1_IOVA_BASE 0x10000000ul

static struct n1_addrs addrs;
static bool have_addrs;
static n1_u64 probe_out[N1P_COUNT];
static struct n1 core;
static bool started;
static u64 pool, pool_used, iova_next[4];
static dart_dev_t *darts[4];

static n1_u32 h_rd(void *c, n1_u64 pa) { (void)c; return read32(pa); }
static void h_wr(void *c, n1_u64 pa, n1_u32 v) { (void)c; write32(pa, v); }
static n1_u32 h_ps(void *c, enum n1_ps ps)
{
    static const char names[N1_PS_COUNT][16] = {"GPIO", "APCIE_GP", "APCIE_SYS_GP", "APCIE_PHY_SW", "APHY_GP_AUS5_A",
                                                "APHY_GP_AUS5", "FAB6_SOC", "ANS", "APCIE_ST0", "APCIE_SYS_ST0"};
    (void)c;
    uintptr_t a = pmgr_psreg_by_name(0, names[ps]);
    return a ? read32(a) : 0;
}
/* Same SMC path m1n1 uses for gP13 (pcie_t6050.c n1_smc_release): one RTKit session per call. */
static int h_smc_read(void *c, n1_u32 key, n1_u32 *v)
{
    (void)c;
    rtkit_set_boot_timeout(10 * USEC_PER_SEC);
    smc_dev_t *smc = smc_init();
    rtkit_set_boot_timeout(0);
    if (!smc) return -1;
    int rc = smc_read_u32(smc, key, v);
    smc_shutdown(smc);
    return rc;
}
static void h_udelay(void *c, n1_u32 us) { (void)c; udelay(us); }
static n1_u64 h_ticks(void *c) { (void)c; return get_ticks() * 1000000ull / mrs(CNTFRQ_EL0); }
static void *h_dma_alloc(void *c, int sid, size_t size, size_t align, n1_u64 *iova)
{
    (void)c;
    if (sid < 1 || sid > 3 || !pool || !darts[sid]) return NULL;
    size = ALIGN_UP(size, SZ_16K);
    pool_used = ALIGN_UP(pool_used, align < SZ_16K ? SZ_16K : align);
    if (pool_used + size > N1_POOL_SIZE) return NULL;
    void *cpu = (void *)(pool + pool_used);
    u64 va = iova_next[sid] ? iova_next[sid] : N1_IOVA_BASE;
    if (va + size >= 0xf0000000ul) return NULL; /* keep clear of the MSI doorbell 0xfffff000 */
    if (dart_map(darts[sid], va, cpu, size)) return NULL;
    pool_used += size;
    iova_next[sid] = va + size + SZ_16K;           /* guard page between buffers */
    *iova = va;
    return cpu;
}
static void h_sync(void *c) { (void)c; dma_wmb(); }
static int h_train(void *c) { (void)c; return pcie_t6050_n1_train_ans(N1_TRAIN_SMC); }
static int h_cycle(void *c) { (void)c; return pcie_t6050_n1_cycle(); }
static int h_route(void *c, n1_u32 rid, n1_u32 sid) { (void)c; return pcie_t6050_n1_route(rid, sid); }
static void h_log(void *c, const char *fmt, ...)
{
    va_list ap;
    char buf[160];
    (void)c;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
}
static const struct n1_hal hal = {
    0, h_rd, h_wr, h_ps, h_smc_read, NULL, h_udelay, h_ticks, h_dma_alloc, h_sync, h_train, h_cycle, h_route, h_log,
};

static bool get_addrs(void)
{
    if (!have_addrs) have_addrs = !pcie_t6050_n1_addrs(&addrs);
    return have_addrs;
}

extern bool hv_started_cpus[];
static bool guest_started(void)
{
    for (unsigned i = 0; i < MAX_CPUS; i++) if (hv_started_cpus[i]) return true;
    return false;
}

s64 hv_win_n1_probe(u64 flags)
{
    if (guest_started() || !get_addrs()) return -1;
    if (n1_probe(&hal, &addrs, (unsigned)flags, probe_out)) return -2;
    printf("N1PROBE: verdict 0x%lx rc 0x%lx/0x%lx port0 status 0x%lx linksts 0x%lx ltssm 0x%lx perst 0x%lx ep 0x%lx smc 0x%lx\n",
           probe_out[N1P_VERDICT], probe_out[N1P_RC_CTL], probe_out[N1P_RC_STAT], probe_out[N1P_P0_STATUS],
           probe_out[N1P_P0_LINKSTS], probe_out[N1P_P0_LTSSM], probe_out[N1P_PERST_PAD], probe_out[N1P_EP_ID],
           probe_out[N1P_SMC_VALUE]);
    return 0;
}

u64 hv_win_n1_stat(u64 index)
{
    if (index < N1P_COUNT) return probe_out[index];
    switch (index) {
    case 100: return core.stage;
    case 101: return (u64)core.err | (u64)core.err_detail << 32;
    case 102: return pool;
    case 103: return pool_used;
    case 104: return core.nscan;
    case 105: return core.rx_frames | (u64)core.tx_frames << 32;
    case 106: return core.connected | (u64)core.last_error << 8;
    default: return 0;
    }
}

s64 hv_win_n1_start(u64 fw, u64 fw_len, u64 stage, u64 flags)
{
    (void)flags;
    if (guest_started()) return -1;
    if (hv_win_storage_native_stat(0) & 6) { printf("N1: refused: native (disk) storage tried/selected this session\n"); return -2; }
    bool attempted = false, ready = false;
    hv_win_storage_model(&attempted, &ready);
    if (attempted) { printf("N1: refused: S1 storage prepared this session\n"); return -2; }
    if (stage < N1_ST_TRAINED || stage > N1_ST_RADIO || (stage >= N1_ST_ROM_DONE && (!fw || !fw_len || fw_len > (64ul << 20))))
        return -3;
    if (!get_addrs()) return -4;
    if (!started) {
        started = true;
        n1_init(&core, &hal, &addrs, (const n1_u8 *)fw, fw_len);
    } else if (fw) {
        core.fw = (const n1_u8 *)fw; core.fw_size = fw_len;
    }
    if (stage >= N1_ST_ROM_DONE && !pool) {
        /* One pool above guest RAM (boot_args mem_size shrinks; the launcher's plan excludes it). */
        pool = top_of_memory_alloc(N1_POOL_SIZE);
        pool_used = 0;
        memset((void *)pool, 0, SZ_16K);
    }
    /* DARTs only once the link is up: a DART whose port is down takes an SError (kboot_n1.c) */
    if (core.stage < N1_ST_TRAINED && n1_bringup(&core, N1_ST_TRAINED)) return -10 - core.err;
    if (stage >= N1_ST_ROM_DONE)
        for (int sid = 1; sid <= 3; sid++)
            if (!darts[sid] && !(darts[sid] = dart_init(addrs.dart, sid, false, DART_T8110))) return -5;
    if (n1_bringup(&core, (enum n1_stage)stage)) return -10 - core.err;
    printf("N1: stage %u reached (pool 0x%lx used 0x%lx)\n", core.stage, pool, pool_used);
    return 0;
}

static void print_scan(void)
{
    for (u32 i = 0; i < core.nscan; i++) {
        struct n1_bss_entry *e = &core.scan[i];
        char ssid[33];
        memcpy(ssid, e->ssid, e->ssid_len); ssid[e->ssid_len] = 0;
        for (u32 k = 0; k < e->ssid_len; k++) if (ssid[k] < 32 || ssid[k] > 126) ssid[k] = '?';
        printf("N1SCAN: %u ch %u rssi %d rsn %u akm 0x%x %02x:%02x:%02x:%02x:%02x:%02x \"%s\"\n", i, e->channel, e->rssi,
               e->has_rsn, e->akm_mask, e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5], ssid);
    }
}

s64 hv_win_n1_ctl(u64 op, u64 arg)
{
    if (guest_started() || core.stage != N1_ST_RADIO) return -1;
    if (op == 1) {
        if (n1_wn_scan(&core) != WN_OK) return -2;
        for (int i = 0; i < 1200 && core.scanning; i++) { n1_poll(&core); udelay(10000); }
        print_scan();
        return core.nscan;
    }
    if (op == 2 && arg) {
        wn_join j;
        memcpy(&j, (const void *)arg, sizeof(j));
        if (j.ssid_len > 32 || j.key_len > 64) return -3;
        if (n1_wn_join(&core, &j) != WN_OK) { memset(&j, 0, sizeof(j)); return -4; }
        memset(&j, 0, sizeof(j));
        for (int i = 0; i < 1600 && core.connecting; i++) { n1_poll(&core); udelay(10000); }
        printf("N1JOIN: connected %d last_error 0x%x\n", core.connected, core.last_error);
        return core.connected ? 0 : -5;
    }
    return -6;
}

/* Park before any reset: bus mastering off on every function, then PME_Turn_Off, PERST#, port off.
 * A restart with the link alive does not come back (0085); PERST# also ends all N1 DMA into the pool. */
s64 hv_win_n1_park(void)
{
    if (core.stage == N1_ST_NONE || !have_addrs) return 0;
    for (int fn = 0; fn < 3; fn++) {
        u64 c = addrs.ecam + (1ul << 20) + ((u64)fn << 12) + 0x04;
        u32 v = read32(c);
        if ((v & 0xffff) != 0xffff) write32(c, (v & 0xffff) & ~0x4u);
    }
    core.stage = N1_ST_NONE;
    return pcie_t6050_n1_park();
}

void hv_win_n1_retire(void) { hv_win_n1_park(); }

void *hv_win_n1_core(void) { return core.stage == N1_ST_RADIO ? &core : NULL; }

void hv_win_n1_tick(void)
{
    if (core.stage != N1_ST_RADIO || smp_id() != boot_cpu_idx) return;
    n1_poll(&core);
}
