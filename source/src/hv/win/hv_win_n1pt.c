/* SPDX-License-Identifier: MIT */
/*
 * n1pt (wireless/n1-native-20261111): the N1 Wi-Fi/BT chip passed through to the Windows guest by PCI ID.
 * Contract: wireless/n1-native-20261111/INTERFACE.md s1-s2. Register inventory and safety argument:
 * wireless/n1-native-20261111/platform/REGISTERS.md.
 *
 *  P_HV_WIN_N1PT_PREP (win, ecam_ipa, iova, gsiv)  pre-guest, from the launcher, once per session, before any
 *      disk path and before the SMC client (SMCP) starts: m1n1 mode-4 training (gP13 pulse, storage snapshot and
 *      NVMe block compare: pcie_t6050_n1_train_ans), then n1pt_plat_prepare (bridge, fn0 BAR0/MSI, ROM check,
 *      port MSI, RID2SID, the 128 MiB pool off the top of RAM and its DART tables). Any failure parks the port
 *      and returns < 0: the launcher then boots Windows without the N1.
 *  P_HV_WIN_N1PT_STAT (index)                       state words for the launcher (pool address, flags, counters)
 *  P_HV_WIN_N1PT_MAP ()                             after the launcher's final stage-2 build, before the guest: identity
 *      map of the DMA pool for the guest (normal memory, the guest's stage-1 attributes, RW); 0 only if every pool
 *      page then resolves to itself. The pool is outside Windows' memory map (m5w,memory) but the driver writes it.
 *  hv_win_n1pt_start()  in hv_start(): ECAM and window hooks, BAR0 Device-nGnRnE, AIC lines; never refuses the
 *      guest (a failure leaves an all-ones bus and parks the N1).
 *  hv_win_n1pt_irq()    AIC events of the N1 MSI lines and the port interrupt -> IRQ_STATUS -> one level SPI.
 *  hv_win_n1pt_unmapped() a guest abort on an unmapped IPA inside an n1pt range (ECAM, window, pool): EL2 restores
 *      the hook/map and retries; never escalated to the host (a session must not depend on the host for the N1).
 *  hv_win_n1pt_retire() guest exit (SYSTEM_OFF/RESET, host stop, dead-man) and resident retire: PARK.
 */

#include "types.h"
#include "hv_win.h"
#include "hv_win_n1pt.h"
#include "hv_win_n1pt_logic.h"
#include "hv_win_storage.h"
#include "../hv.h"
#include "aic.h"
#include "hv_win_n1.h"
#include "hv_win_smcp.h"
#include "n1pt_plat.h"
#include "pcie_t6050.h"
#include "pcie_t6050_n1.h"
#include "string.h"
#include "utils.h"

static struct n1pt st;
static struct n1pt_plat plat;
static u64 ecam_ipa, pool;
static u32 gsiv;
static bool attempted, prepped, armed, irq_armed, retired;
static u32 remaps; /* hv_win_n1pt_unmapped() repairs, bounded */
static u32 link_events;
static DECLARE_SPINLOCK(irq_lock);

extern bool hv_started_cpus[];

static bool guest_started(void)
{
    for (int i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return true;
    return false;
}

static void s2_flush(void)
{
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
}

/* ---- ops for the logic and the commands ---- */
static u32 o_cfg_read(void *c, unsigned fn, unsigned off) { (void)c; return n1pt_plat_cfg_read(&plat, fn, off); }
static void o_cfg_write(void *c, unsigned fn, unsigned off, u32 v) { (void)c; n1pt_plat_cfg_write(&plat, fn, off, v); }
static bool o_link_up(void *c) { (void)c; return n1pt_plat_link_up(&plat); }
static int o_command(struct n1pt *s, unsigned cmd, u32 arg) { (void)arg; return n1pt_plat_command(&plat, s, cmd); }
static u64 o_now_us(void *c) { (void)c; return ticks_to_usecs(get_ticks()); }
static void o_spi(void *c, bool level) { (void)c; hv_vgic3_set_spi_level(gsiv, level); }
static const struct n1pt_ops ops = {0, o_cfg_read, o_cfg_write, o_link_up, o_command, o_now_us, o_spi};

static bool win_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

static int cb_map_bars(void *c, int bar0, int bar2)
{
    (void)c;
    u64 b0 = plat.win + N1PT_BAR0_OFF, b2 = plat.win + N1PT_BAR2_OFF;
    int r0 = bar0 ? hv_map_hw_dev(b0, b0, N1PT_BAR0_SIZE) : hv_map_hook(b0, win_hook, N1PT_BAR0_SIZE);
    int r2 = bar2 ? hv_map_hw_dev(b2, b2, N1PT_BAR2_SIZE) : hv_map_hook(b2, win_hook, N1PT_BAR2_SIZE);
    s2_flush();
    return (r0 < 0 || r2 < 0) ? -1 : 0;
}

static void cb_link_irq(void *c, int on)
{
    (void)c;
    if (!irq_armed)
        return;
    if (on && link_events <= 256)
        aic_unmask(N1PT_LINK_AIC);
    else
        aic_mask(N1PT_LINK_AIC);
}
static const struct n1pt_plat_cb plat_cb = {0, cb_map_bars, cb_link_irq};

/* ---- the DMA pool in the guest's stage 2 ---- */
#define N1PT_S2AP_RW    (3UL << 6)
#define N1PT_MEMATTR_S1 (0xfUL << 2) /* stage-2 MemAttr 1111: the guest's stage-1 type (as guest RAM) */

/* every pool page resolves to itself, RW, normal memory with the guest's attributes */
static bool pool_ok(void)
{
    if (!prepped || !pool)
        return false;
    for (u64 off = 0; off < N1PT_POOL_SIZE;) {
        struct hv_s2_leaf l;
        u64 ipa = pool + off;
        if (!hv_pt_resolve_leaf(ipa, &l) || l.pa_base + (ipa - l.ipa_base) != ipa ||
            (l.descriptor & N1PT_S2AP_RW) != N1PT_S2AP_RW || (l.descriptor & N1PT_MEMATTR_S1) != N1PT_MEMATTR_S1)
            return false;
        off += l.size - (ipa - l.ipa_base);
    }
    return true;
}

/* 0 mapped (or already), < 0 refused: something else holds a pool page, or the map did not take */
static int pool_map(void)
{
    if (!prepped || !pool)
        return -1;
    if (pool_ok())
        return 0;
    for (u64 off = 0; off < N1PT_POOL_SIZE; off += 0x4000) {
        if (hv_pt_walk(pool + off)) {
            printf("n1pt: pool page 0x%lx already has a stage-2 entry: not mapping the pool\n", pool + off);
            return -2;
        }
    }
    if (hv_map_hw(pool, pool, N1PT_POOL_SIZE) < 0)
        return -3;
    s2_flush();
    if (!pool_ok())
        return -4;
    printf("n1pt: pool 0x%lx+0x%lx identity-mapped for the guest (normal, RW; not in Windows' memory map)\n", pool,
           (u64)N1PT_POOL_SIZE);
    return 0;
}

s64 hv_win_n1pt_map(void)
{
    if (guest_started()) {
        printf("n1pt: MAP refused: guest running\n");
        return -5;
    }
    return pool_map();
}

/* ---- guest traps (under the big HV lock) ---- */
static bool ecam_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - ecam_ipa;
    if (write)
        n1pt_ecam_write(&st, off, (u32)*val, width);
    else
        *val = width > 2 ? ~0UL : n1pt_ecam_read(&st, off, width);
    return true;
}

static bool win_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - plat.win;
    if (write && width == 2 && off == N1PT_BAR4_OFF + N1SV_CMD) {
        n1pt_window_write(&st, off, (u32)*val, width); /* the command runs without the IRQ lock */
        return true;
    }
    u32 d0 = st.n_dropped, w0 = st.n_cfgw, h0 = st.n_held;
    spin_lock(&irq_lock);
    if (write)
        n1pt_window_write(&st, off, (u32)*val, width);
    else
        *val = width > 2 ? ~0UL : n1pt_window_read(&st, off, width);
    spin_unlock(&irq_lock);
    if (st.n_dropped != d0) /* every dropped config-window write, with its function/offset/value (4 bytes) */
        printf("n1pt: cfg write DROPPED fn%u +0x%03x byte-enables 0x%x (size %u) value 0x%08x (%s)\n",
               st.drop_addr >> 12, st.drop_addr & 0xffc, st.drop_be, (u32)__builtin_popcount(st.drop_be), st.drop_val,
               (st.drop_addr >> 12) > 2 ? "no such function" : n1pt_cfg_write_allowed(st.drop_addr & 0xffc) ?
               "no link / parked / guard" : "offset not allowed");
    else if (st.n_held != h0)
        printf("n1pt: cfg write fn%u COMMAND held (link down): MEM|MASTER 0x%x applied after the next restore\n",
               (st.cfg_addr >> 12) & 7, st.held_cmd[((st.cfg_addr >> 12) & 7) % 3]);
    else if (st.n_cfgw != w0 && (st.cfgw_addr & 0xffc) == 0x44)
        printf("n1pt: cfg write fn%u PM_CTRL <- 0x%08x (dword written)\n", st.cfgw_addr >> 12, st.cfgw_val);
    return true;
}

/* ---- pre-guest ---- */
s64 hv_win_n1pt_prep(u64 win, u64 ecam, u64 iova, u64 spi)
{
    if (guest_started() || attempted) {
        printf("n1pt: PREP refused (guest running or already tried)\n");
        return -1;
    }
    attempted = true;
    if (hv_win_disk_path_used() || hv_win_smcp_attempted() || hv_win_n1_touched()) {
        printf("n1pt: PREP refused: a disk path, the SMC client or the old N1 path ran first (order: N1 first)\n");
        return -2;
    }
    if (!win || (win & (N1PT_WIN_SIZE - 1)) || !ecam || (ecam & (N1PT_ECAM_SIZE - 1)) || (iova & 0x3fff) ||
        iova + N1PT_POOL_SIZE > (1UL << 36) || (iova <= N1PT_DOORBELL && N1PT_DOORBELL < iova + N1PT_POOL_SIZE) ||
        spi < 32 || spi > 1019) {
        printf("n1pt: PREP refused: layout win 0x%lx ecam 0x%lx iova 0x%lx spi %lu\n", win, ecam, iova, spi);
        return -3;
    }
    if (pcie_t6050_n1_addrs(&plat.a))
        return -4;
    printf("n1pt: training apcie0 port 0 (gP13 pulse + m1n1 mode 4 with the storage snapshot/NVMe compare)\n");
    if (pcie_t6050_n1_train_ans(N1_TRAIN_SMC | N1_TRAIN_LINUX)) {
        pcie_t6050_n1_park(); /* only a live link is touched: the GP side may never have powered up */
        printf("n1pt: N1 REFUSED: training failed (port parked); Windows boots without it\n");
        return -5;
    }
    pool = top_of_memory_alloc(N1PT_POOL_SIZE);
    memset((void *)pool, 0, N1PT_POOL_SIZE);
    plat.win = win;
    plat.cb = &plat_cb;
    if (n1pt_plat_prepare(&plat, pool, iova, N1PT_POOL_SIZE)) {
        pcie_t6050_n1_park_any();
        printf("n1pt: N1 REFUSED: preparation failed (port parked); pool 0x%lx stays out of guest RAM\n", pool);
        return -6;
    }
    ecam_ipa = ecam;
    gsiv = (u32)spi;
    n1pt_init(&st, &ops, win, pool, iova, N1PT_POOL_SIZE);
    prepped = true;
    printf("n1pt: PREP done: ECAM IPA 0x%lx, window 0x%lx, pool 0x%lx+0x%lx IOVA 0x%lx, SPI %u\n", ecam, win, pool,
           (u64)N1PT_POOL_SIZE, iova, gsiv);
    return 0;
}

u64 hv_win_n1pt_stat(u64 index)
{
    switch (index) {
    case 0:
        return (u64)prepped | (u64)armed << 1 | (u64)irq_armed << 2 | (u64)st.parked << 3 | (u64)st.guard << 4 |
               (u64)st.fns_enabled << 5 | (u64)st.dead << 6 | (u64)retired << 7 | (u64)attempted << 8;
    case 1: return pool;
    case 2: return prepped ? N1PT_POOL_SIZE : 0;
    case 3: return st.pool_iova;
    case 4: return plat.four_level;
    case 5: return st.n_irq;
    case 6: return st.n_cmds;
    case 7: return st.n_dropped;
    case 8: return st.n_bar_moves;
    case 9: return (u64)(s64)st.cmd_result;
    case 10: return link_events;
    case 11: return pool_ok();
    case 12: return remaps;
    default: return 0;
    }
}

/* ---- guest start (hv_start), never refuses the guest ---- */
bool hv_win_n1pt_start(void)
{
    if (!prepped)
        return true;
    if (hv_map_hook(ecam_ipa, ecam_hook, N1PT_ECAM_SIZE) < 0) {
        printf("n1pt: N1 REFUSED at start: ECAM hook failed (Windows boots without it)\n");
        st.dead = true;
        st.ops->command(&st, N1PT_CMD_PARK, 0);
        return true;
    }
    if (hv_map_hook(plat.win, win_hook, N1PT_WIN_SIZE) < 0 || !n1pt_plat_link_up(&plat) ||
        cb_map_bars(0, 1, 0)) {
        printf("n1pt: N1 REFUSED at start: window map failed or link down (bus reads all-ones)\n");
        st.dead = true;
        hv_map_hook(plat.win, win_hook, N1PT_WIN_SIZE);
        s2_flush();
        st.ops->command(&st, N1PT_CMD_PARK, 0);
        return true;
    }
    s2_flush();
    if (pool_map()) {
        printf("n1pt: N1 REFUSED at start: the DMA pool is not mapped for the guest (bus reads all-ones)\n");
        st.dead = true;
        cb_map_bars(0, 0, 0);
        st.ops->command(&st, N1PT_CMD_PARK, 0);
        return true;
    }
    if (!hv_win_storage_claims_spi(gsiv) && hv_vgic3_spi_unassigned(gsiv) && hv_win_aic_claim_t6050()) {
        for (u32 d = 0; d < 64; d++)
            if (n1pt_msi_bit(d) >= 0)
                aic_unmask(N1PT_MSI_AIC_BASE + d);
        aic_unmask(N1PT_LINK_AIC);
        irq_armed = true;
    } else {
        printf("n1pt: interrupts NOT armed (AIC layout or SPI %u taken): the driver must poll\n", gsiv);
    }
    hv_vgic3_set_spi_level(gsiv, false);
    armed = true;
    printf("n1pt: armed: ECAM 0x%lx (bus 0 root port 106b:100c, bus 1 106b:1900), BAR0 0x%lx Device-nGnRnE, BAR2 "
           "0x%lx (all-ones until ENABLE_FUNCTIONS), service page 0x%lx, SPI %u%s\n",
           ecam_ipa, (u64)(plat.win + N1PT_BAR0_OFF), (u64)(plat.win + N1PT_BAR2_OFF), (u64)(plat.win + N1PT_BAR4_OFF), gsiv,
           irq_armed ? " (MSI AIC 1888+, link AIC 1523)" : "");
    return true;
}

/* hv_handle_dabort(): the guest touched an unmapped IPA. Inside an n1pt range EL2 puts the hook/map back and the access
 * is retried (true); outside, false (not ours). Bounded: after 64 repairs the range stays as it is and false is
 * returned, so a broken stage 2 cannot loop forever. */
static bool razwi_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    UNUSED(addr);
    if (!write)
        *val = width > 2 ? ~0UL : (u64)n1pt_width_mask(width);
    return true;
}

bool hv_win_n1pt_unmapped(u64 ipa)
{
    if (!prepped)
        return false;
    bool ecam = ipa >= ecam_ipa && ipa < ecam_ipa + N1PT_ECAM_SIZE;
    bool win = ipa >= plat.win && ipa < plat.win + N1PT_WIN_SIZE;
    bool inpool = ipa >= pool && ipa < pool + N1PT_POOL_SIZE;
    if (!ecam && !win && !inpool)
        return false;
    if (remaps >= 64) {
        if (remaps++ == 64)
            printf("n1pt: unmapped IPA 0x%lx: repair limit reached\n", ipa);
        return false;
    }
    remaps++;
    int r;
    if (ecam)
        r = hv_map_hook(ecam_ipa, ecam_hook, N1PT_ECAM_SIZE);
    else if (win)
        r = hv_map_hook(plat.win, win_hook, N1PT_WIN_SIZE); /* BARs back to all-ones: the driver sees a dead device */
    else if (st.dead || (r = pool_map()) < 0)
        r = hv_map_hook(pool, razwi_hook, N1PT_POOL_SIZE); /* no usable pool: reads all-ones, writes dropped */
    s2_flush();
    printf("n1pt: unmapped IPA 0x%lx (%s) repaired in EL2: %d\n", ipa, ecam ? "ECAM" : win ? "window" : "pool", r);
    if (r < 0)
        return false;
    if (win && armed && !st.dead && !st.parked && !st.guard)
        cb_map_bars(0, n1pt_plat_link_up(&plat), st.fns_enabled && n1pt_plat_link_up(&plat));
    return true;
}

/* AIC event (hv_win_handle_irq): true if it was an N1 line */
bool hv_win_n1pt_irq(u32 line)
{
    if (!irq_armed)
        return false;
    int bit = n1pt_aic_bit(line);
    if (bit < 0)
        return false;
    u32 bits = BIT(bit);
    if (bit == 31) {
        u32 v = n1pt_plat_link_irq_ack(&plat);
        bits = (v & BIT(14)) ? N1PT_IRQ_LINK : 0;
        if (++link_events > 256) {
            if (link_events == 257)
                printf("n1pt: port interrupt storm (INTSTAT 0x%x): line 1523 left masked\n", v);
            return true;
        }
    }
    spin_lock(&irq_lock);
    n1pt_irq_latch(&st, bits);
    spin_unlock(&irq_lock);
    aic_unmask(line);
    return true;
}

/* guest exit and resident retire: PARK (LINUX-MAP 7: a live link at restart hangs the next boot) */
void hv_win_n1pt_retire(void)
{
    if (!prepped || retired)
        return;
    retired = true;
    if (irq_armed) {
        for (u32 d = 0; d < 64; d++)
            if (n1pt_msi_bit(d) >= 0)
                aic_mask(N1PT_MSI_AIC_BASE + d);
        aic_mask(N1PT_LINK_AIC);
    }
    int r = 0;
    if (st.guard)
        printf("n1pt: retire: NOT parked: the storage guard tripped earlier (no more PHY activity)\n");
    else if (!st.parked)
        r = n1pt_plat_command(&plat, &st, N1PT_CMD_PARK);
    printf("n1pt: retire: park %d; irqs %u, commands %u (last %d, %u us), dropped cfg writes %u, BAR moves %u, "
           "link events %u, guard %d\n",
           r, st.n_irq, st.n_cmds, st.cmd_result, st.last_us, st.n_dropped, st.n_bar_moves, link_events, st.guard);
}
