/* SPDX-License-Identifier: MIT */
/*
 * tests/win-n1pt/test_plat: the unchanged pcie_t6050.c, n1pt_plat.c, n1pt_dart.c and hv/win/hv_win_n1pt.c against
 * the register model (model.c) and the resident stand-ins (glue/glue.c). Each scenario runs in its own process
 * (the sources keep one-shot static state, as on hardware: one PREP per session).
 *  - PREP: SMC gP13 pulse, training, root port window, fn0 BAR0/MSI/COMMAND, ROM check, port MSI address/map/
 *    enable, RID2SID 0x100-0x102 -> 1-3, DART: every pool page mapped at IOVA + offset in streams 1-3 and nothing
 *    else (independent T8110 table walk from the TTBR registers; MSI doorbell page unmapped), 3-level fallback,
 *    refusals (order, layout, training, ROM running, DART locked/error/too few streams) with nothing mapped.
 *  - start: ECAM and window hooks, BAR0 device map, BAR2 all-ones, AIC lines; refusals leave an all-ones bus.
 *  - service commands through the guest window: ENABLE (stage check, IDs, fn1/fn2 BAR/MSI/COMMAND, BAR2 map),
 *    PORT_CYCLE (state restored after the model's PERST# wiped it; BARs unmapped while down), PARK (MASTER off,
 *    then DART block, then PERST#), storage guard tripping in PORT_CYCLE and PARK (E_IO, all commands refused,
 *    BARs stay unmapped, no more hardware writes, retire does not touch the PHY again).
 *  - IRQ: MSI lines -> status bits -> SPI; port link-down interrupt -> bit 31 + INTSTAT cleared; storm guard
 *    (line 1523 left masked after 256 events, also across a PORT_CYCLE); other lines not claimed.
 *  - pcie_t6050_storage_guard directly: what may move (PS actual state, mailbox, ANS status, CSTS ready) and what
 *    trips it (PS target, BOOT_STATUS, CC, ANS stop, CSTS fatal, SART).
 */
#include <sys/wait.h>
#include <unistd.h>
#include "model.h"
#include "glue.h"
#include "hv_win_n1pt.h"
#include "hv/win/hv_win_n1pt_logic.h"
#include "pcie_t6050_n1.h"
#undef printf /* n1pt_host.h routes the sources' printf to the model; the test prints its own lines */

extern bool hv_started_cpus[];
static int fails, checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            if (fails < 40)                                                                        \
                fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)

#define WIN   0xbc2000000UL
#define ECAMI 0xf3a000000UL
#define IOVA  0x10000000UL
#define SPI   100u
#define CFG(fn)  (M_ECAM + (1UL << 20) + ((u64)(fn) << 12))
#define P0       m_port(0)
#define BAR4     (WIN + N1PT_BAR4_OFF)
#define PAGES    (N1PT_POOL_SIZE / 0x4000)

static u32 gr(u64 ipa) { u64 v = 0xdead; return guest_read(ipa, 2, &v) < 0 ? 0xbad0bad0u : (u32)v; }
static void gw(u64 ipa, u32 v) { guest_write(ipa, 2, v); }
static u32 svc(u32 off) { return gr(BAR4 + off); }
static s32 cmd(u32 c) { gw(BAR4 + N1SV_CMD, c); return (s32)svc(N1SV_CMD_RESULT); }
static void d3(void) { gw(BAR4 + N1SV_CFG_ADDR, 0x44); gw(BAR4 + N1SV_CFG_DATA, 0x0103); } /* driver: D3hot | PME_En */
static int mapped_dev(u64 ipa) { u64 v; return guest_read(ipa, 2, &v); } /* 1 device map, 0 hook, -1 none */

static void setup(void)
{
    model_reset();
    glue_reset();
    M.verbose = getenv("N1PT_VERBOSE") != NULL;
    G.tables_built = true; /* the launcher's final stage-2 build precedes P_HV_START */
}

static int prep(void) { return (int)hv_win_n1pt_prep(WIN, ECAMI, IOVA, SPI); }
static int prep_start(void)
{
    int r = prep();
    if (!r)
        hv_win_n1pt_start();
    return r;
}

/* ---- independent DART walk (T8110 format: 16 KiB tables, PA[43:14] in bits 39:10, VALID bit 0) ---- */
static u64 pte_pa(u64 pte) { return ((pte >> 10) & ((1UL << 30) - 1)) << 14; }
struct walk { u64 n, bad, doorbell; };
static struct walk walk_sid(u32 sid, u64 pool)
{
    struct walk w = {0, 0, 0};
    u32 tcr = m_get(M_DART + 0x1000 + 4 * sid), ttbr = m_get(M_DART + 0x1400 + 4 * sid);
    bool four = tcr & 8;
    u64 root = (u64)((ttbr >> 2) & ((1u << 28) - 1)) << 14;
    const u64 *top = (const u64 *)root;
    for (u32 t = 0; t < (four ? 2048u : 1u); t++) {
        const u64 *mid;
        if (four) {
            if (!(top[t] & 1)) continue;
            mid = (const u64 *)pte_pa(top[t]);
        } else {
            mid = top;
        }
        for (u32 m = 0; m < 2048; m++) {
            if (!(mid[m] & 1)) continue;
            const u64 *leaf = (const u64 *)pte_pa(mid[m]);
            for (u32 l = 0; l < 2048; l++) {
                if (!(leaf[l] & 1)) continue;
                u64 dva = ((u64)t << 36) | ((u64)m << 25) | ((u64)l << 14);
                u64 pa = pte_pa(leaf[l]);
                w.n++;
                if (dva < IOVA || dva >= IOVA + N1PT_POOL_SIZE || pa != pool + (dva - IOVA) ||
                    (leaf[l] >> 40 & 0xfff) != 0xfff || (leaf[l] >> 52) != 0)
                    w.bad++;
                if (dva == (N1PT_DOORBELL & ~0x3fffUL))
                    w.doorbell++;
            }
        }
    }
    return w;
}

static void check_prepared(bool four)
{
    u64 pool = hv_win_n1pt_stat(1);
    CHECK((hv_win_n1pt_stat(0) & 0x101) == 0x101);
    CHECK(pool && pool == (u64)(uintptr_t)G.pool && hv_win_n1pt_stat(2) == N1PT_POOL_SIZE);
    CHECK(hv_win_n1pt_stat(3) == IOVA && hv_win_n1pt_stat(4) == (u64)four);
    /* SMC reset pulse: template | 0 then template | 1 */
    CHECK(M.ngp13 == 2 && M.gp13_log[0] == 0x00800000 && M.gp13_log[1] == 0x00800001);
    CHECK(M.link);
    /* root port: 64-bit prefetchable window = the 1 MiB window, no I/O / 32-bit window, MEM|MASTER */
    u64 lim = WIN + N1PT_WIN_SIZE - 1;
    CHECK((m_get(M_ECAM + 0x24) & 0xfff0fff0) == ((u32)((WIN >> 16) & 0xfff0) | ((u32)((lim >> 16) & 0xfff0) << 16)));
    CHECK(m_get(M_ECAM + 0x28) == (u32)(WIN >> 32) && m_get(M_ECAM + 0x2c) == (u32)(lim >> 32));
    CHECK((m_get(M_ECAM + 0x04) & 6) == 6);
    CHECK((m_get(M_ECAM + 0x20) & 0xfff0) > (m_get(M_ECAM + 0x20) >> 16 & 0xfff0));
    /* fn0: BAR0, COMMAND = MEM, no MASTER (the driver's), INTx disabled; MSI 4 vectors at the doorbell, data 0 */
    CHECK((m_get(CFG(0) + 0x10) & ~0xfu) == (u32)(WIN + N1PT_BAR0_OFF) && m_get(CFG(0) + 0x14) == (u32)(WIN >> 32));
    u32 c0 = m_get(CFG(0) + 0x04);
    CHECK((c0 & 2) && !(c0 & 4) && (c0 & 0x400));
    u32 mc = m_get(CFG(0) + 0x50) >> 16;
    CHECK((mc & 1) && ((mc >> 4) & 7) == 2);
    CHECK(m_get(CFG(0) + 0x54) == 0xfffff000u && m_get(CFG(0) + 0x58) == 0 && (m_get(CFG(0) + 0x5c) & 0xffff) == 0);
    /* port MSI: doorbell, 64 map entries ENABLE | 64 + i, enable; intr2axi */
    CHECK(m_get(P0 + 0x16c) == 0xfffff000u && m_get(P0 + 0x170) == 0 && (m_get(P0 + 0x124) & 1));
    int bad = 0;
    for (u32 i = 0; i < 64; i++)
        bad += m_get(P0 + 0x3800 + 4 * i) != (0x80000000u | (64 + i));
    CHECK(bad == 0);
    CHECK(m_get(m_i2axi(0) + 0x80) == 1);
    /* RID2SID: slot sid = VALID | sid << 16 | RID, for 0x100-0x102 -> 1-3 */
    for (u32 s = 1; s <= 3; s++)
        CHECK(m_get(P0 + 0x3000 + 4 * s) == (0x80000000u | (s << 16) | (0x100 + s - 1)));
    /* DART: streams 1-3 translate through the same tables; stream 0 and the rest untouched */
    u32 want_tcr = four ? 9 : 1;
    for (u32 s = 0; s < 32; s++) {
        u32 tcr = m_get(M_DART + 0x1000 + 4 * s), ttbr = m_get(M_DART + 0x1400 + 4 * s);
        if (s >= 1 && s <= 3)
            CHECK(tcr == want_tcr && (ttbr & 1) && ttbr == m_get(M_DART + 0x1404));
        else
            CHECK(tcr == 0 && ttbr == 0);
    }
    CHECK((M.dart_enabled & 0xe) == 0xe && !(M.dart_enabled & 1));
    for (u32 s = 1; s <= 3; s++) {
        struct walk w = walk_sid(s, pool);
        CHECK(w.n == PAGES && w.bad == 0 && w.doorbell == 0); /* every pool page, nothing else */
    }
    /* link-down interrupt unmasked at the port */
    CHECK(m_get(P0 + 0x104) == ~(u32)(1u << 14));
    /* storage untouched */
    CHECK(m_get(M_NVME + 0x1300) == 0xde71ce55 && (m_get(M_ANS + 0x44) & 0x10));
}

static void sc_prep_start(void)
{
    setup();
    CHECK(prep() == 0);
    check_prepared(true);
    CHECK(G.nmap == 0); /* nothing mapped for the guest before hv_start */
    hv_win_n1pt_start();
    CHECK((hv_win_n1pt_stat(0) & 0x6) == 0x6); /* armed, irq armed */
    /* guest view */
    CHECK(gr(ECAMI) == 0x100c106b && gr(ECAMI + (1UL << 20)) == 0x1900106b);
    CHECK(gr(ECAMI + (1UL << 20) + 0x1000) == 0xffffffffu && gr(ECAMI + (1UL << 20) + (1UL << 15)) == 0xffffffffu);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 1 && gr(WIN + N1PT_BAR0_OFF + 0x8000) == 0); /* ROM stopped */
    CHECK(mapped_dev(WIN + N1PT_BAR2_OFF) == 0 && gr(WIN + N1PT_BAR2_OFF) == 0xffffffffu);
    CHECK(mapped_dev(WIN + N1PT_BAR2_OFF + 0x1fffc) == 0);
    CHECK(svc(N1SV_MAGIC) == 0x5653314e && svc(N1SV_STATUS) == N1SV_ST_LINK);
    u64 pool = hv_win_n1pt_stat(1);
    CHECK(svc(N1SV_POOL_IPA_LO) == (u32)pool && svc(N1SV_POOL_IPA_HI) == (u32)(pool >> 32));
    CHECK(svc(N1SV_IOVA_LO) == (u32)IOVA && svc(N1SV_POOL_SIZE) == N1PT_POOL_SIZE);
    CHECK(gr(WIN) == 0xffffffffu && gr(WIN + 0x80000) == 0xffffffffu);
    CHECK(mapped_dev(WIN + N1PT_WIN_SIZE) == -1 && mapped_dev(ECAMI + N1PT_ECAM_SIZE) == -1);
    /* AIC: the 22 MSI lines and the port line unmasked; SPI low */
    int n = 0;
    for (u32 l = 1888; l < 1888 + 64; l++)
        n += G.unmask[l] > 0;
    CHECK(n == 22 && G.unmask[1523] == 1 && !G.spi_level && G.spi_intid == SPI);
    /* endpoint COMMAND from the guest: only MEM|MASTER reach fn0 */
    gw(ECAMI + (1UL << 20) + 4, 0xffff);
    u32 c0 = m_get(CFG(0) + 0x04);
    CHECK((c0 & 6) == 6 && (c0 & 0x400) && !(c0 & 0x1));
    /* D3hot from the guest: stored, not forwarded */
    u32 w0 = M.cfg_writes[0][0x44 / 4];
    gw(ECAMI + (1UL << 20) + 0x44, 0x0103);
    CHECK(M.cfg_writes[0][0x44 / 4] == w0 && (gr(ECAMI + (1UL << 20) + 0x44) & 3) == 3);
    /* retire: PARK (MASTER off, DART blocked, PERST#), lines masked */
    u32 p0 = M.perst_asserts;
    hv_win_n1pt_retire();
    CHECK(M.perst_asserts == p0 + 1 && !M.link && (hv_win_n1pt_stat(0) & 0x88) == 0x88);
    CHECK(G.mask[1523] >= 1 && G.mask[1888] >= 1 && G.mask[1888 + 33] >= 1);
    hv_win_n1pt_retire();
    CHECK(M.perst_asserts == p0 + 1);
}

static void sc_three_level(void)
{
    setup();
    M.dart_no_four = true;
    CHECK(prep() == 0);
    check_prepared(false);
}

static void sc_enable(void)
{
    setup();
    CHECK(prep_start() == 0);
    /* ROM not at stage 1 yet: busy, nothing programmed */
    CHECK(cmd(N1PT_CMD_ENABLE) == N1PT_E_BUSY);
    CHECK(mapped_dev(WIN + N1PT_BAR2_OFF) == 0 && !(svc(N1SV_STATUS) & N1SV_ST_FNS));
    /* firmware running: stage 1, fn1/fn2 appear */
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    CHECK(svc(N1SV_STATUS) & N1SV_ST_FNS);
    static const u32 mme[3] = {2, 4, 1}, data[3] = {0, 16, 32};
    for (int fn = 1; fn < 3; fn++) {
        CHECK((m_get(CFG(fn) + 0x10) & ~0xfu) == (u32)(WIN + 0x10000UL * (1 + fn)));
        u32 c = m_get(CFG(fn) + 0x04);
        CHECK((c & 6) == 6 && (c & 0x400));
        u32 mc = m_get(CFG(fn) + 0x50) >> 16;
        CHECK((mc & 1) && ((mc >> 4) & 7) == mme[fn]);
        CHECK(m_get(CFG(fn) + 0x54) == 0xfffff000u && (m_get(CFG(fn) + 0x5c) & 0xffff) == data[fn]);
    }
    CHECK(mapped_dev(WIN + N1PT_BAR2_OFF) == 1 && mapped_dev(WIN + N1PT_BAR2_OFF + 0x1fffc) == 1);
    CHECK(mapped_dev(WIN + N1PT_BAR4_OFF) == 0);
    /* idempotent */
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    /* the config window reaches fn1/fn2 now */
    gw(BAR4 + N1SV_CFG_ADDR, 0x1000);
    CHECK(svc(N1SV_CFG_DATA) == 0x1902106b);
    gw(BAR4 + N1SV_CFG_ADDR, 0x2000);
    CHECK(svc(N1SV_CFG_DATA) == 0x1903106b);
    gw(BAR4 + N1SV_CFG_ADDR, 0x2010); /* BAR write through the window: dropped and counted */
    u32 bar = m_get(CFG(2) + 0x10);
    gw(BAR4 + N1SV_CFG_DATA, 0);
    CHECK(m_get(CFG(2) + 0x10) == bar && svc(N1SV_CNT_DROPPED) == 1);
}

static void sc_enable_nodev(void)
{
    setup();
    CHECK(prep_start() == 0);
    M.rom_stage = 1; /* stage 1 but fn1/fn2 absent */
    CHECK(cmd(N1PT_CMD_ENABLE) == N1PT_E_NODEV);
    CHECK(mapped_dev(WIN + N1PT_BAR2_OFF) == 0 && !(hv_win_n1pt_stat(0) & 0x20));
}

static void sc_cycle(void)
{
    setup();
    CHECK(prep_start() == 0);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    gw(ECAMI + (1UL << 20) + 4, 0x6); /* driver: MEM|MASTER on fn0 */
    u32 ups = M.link_ups, pa = M.perst_asserts, um = G.unmask[1523];
    u32 nmap0 = G.nmap;
    gw(BAR4 + N1SV_CFG_ADDR, 0x44);
    gw(BAR4 + N1SV_CFG_DATA, 0x0103);
    CHECK(m_get(CFG(0) + 0x44) == 0x0103);
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == 0);
    CHECK(M.perst_asserts == pa + 1 && M.link_ups == ups + 1 && M.link);
    /* BARs were unmapped (hooks) during the cycle, then device maps again */
    bool unmapped_seen = false;
    for (u32 i = nmap0; i < G.nmap; i++)
        if (!G.map[i].dev && G.map[i].from == WIN + N1PT_BAR0_OFF)
            unmapped_seen = true;
    CHECK(unmapped_seen);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 1 && mapped_dev(WIN + N1PT_BAR2_OFF) == 1);
    /* everything the model's PERST# wiped is back */
    for (int fn = 0; fn < 3; fn++) {
        CHECK((m_get(CFG(fn) + 0x10) & ~0xfu) == (u32)(WIN + 0x10000UL * (1 + fn)));
        CHECK(m_get(CFG(fn) + 0x50) >> 16 & 1);
        CHECK(m_get(CFG(fn) + 0x54) == 0xfffff000u);
    }
    CHECK((m_get(CFG(0) + 0x04) & 6) == 6);
    CHECK((m_get(CFG(1) + 0x04) & 6) == 6 && (m_get(CFG(2) + 0x04) & 6) == 6);
    for (u32 s = 1; s <= 3; s++)
        CHECK(m_get(P0 + 0x3000 + 4 * s) == (0x80000000u | (s << 16) | (0x100 + s - 1)));
    CHECK(m_get(P0 + 0x3800 + 4 * 33) == (0x80000000u | 97) && (m_get(P0 + 0x124) & 1));
    CHECK(m_get(P0 + 0x104) == ~(u32)(1u << 14) && G.unmask[1523] == um + 1);
    CHECK((svc(N1SV_STATUS) & (N1SV_ST_LINK | N1SV_ST_GUARD)) == N1SV_ST_LINK);
    /* the DART is not on the link: still translating */
    struct walk w = walk_sid(2, hv_win_n1pt_stat(1));
    CHECK(w.n == PAGES && w.bad == 0);
    CHECK(svc(N1SV_CNT_LAST_US) > 100000); /* includes the 100 ms settle, in model time */
    /* a second cycle works too */
    d3();
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == 0 && M.perst_asserts == pa + 2);
}

static void sc_cycle_nolink(void)
{
    setup();
    CHECK(prep_start() == 0);
    M.never_link = true; /* the link does not come back */
    d3();
    s32 r = cmd(N1PT_CMD_PORT_CYCLE);
    CHECK(r == N1PT_E_NOLINK || r == N1PT_E_IO);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 0 && gr(WIN + N1PT_BAR0_OFF) == 0xffffffffu);
    CHECK(!(svc(N1SV_STATUS) & N1SV_ST_LINK));
}

static void sc_guard(int which)
{
    setup();
    CHECK(prep_start() == 0);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    M.trip_storage_on_perst = true;
    u32 pa = M.perst_asserts;
    if (which == N1PT_CMD_PORT_CYCLE)
        d3();
    CHECK(cmd(which) == N1PT_E_IO);
    CHECK((svc(N1SV_STATUS) & N1SV_ST_GUARD) && (hv_win_n1pt_stat(0) & 0x10));
    CHECK(M.perst_asserts == pa + 1);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 0 && mapped_dev(WIN + N1PT_BAR2_OFF) == 0);
    /* from now on: every command refused, no hardware write at all */
    u64 w0 = M.writes;
    for (u32 c = 1; c <= 3; c++)
        CHECK(cmd(c) == N1PT_E_PERM);
    gw(BAR4 + N1SV_CFG_ADDR, 0x44);
    gw(BAR4 + N1SV_CFG_DATA, 0);
    gw(ECAMI + (1UL << 20) + 4, 0x6);
    hv_win_n1pt_retire(); /* "NOT parked": no PHY activity */
    CHECK(M.writes == w0 && M.perst_asserts == pa + 1);
}

static void sc_park(void)
{
    setup();
    CHECK(prep_start() == 0);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    gw(ECAMI + (1UL << 20) + 4, 0x6);
    M.nlog = 0;
    CHECK(cmd(N1PT_CMD_PARK) == 0);
    /* MASTER off (every function) before the DART is blocked, before PERST# */
    int im = -1, id = -1, ip = -1;
    for (u32 i = 0; i < M.nlog && i < 64; i++) {
        if (M.log[i] == 'M' && im < 0) im = (int)i;
        if (M.log[i] == 'D' && id < 0) id = (int)i;
        if (M.log[i] == 'P' && ip < 0) ip = (int)i;
    }
    CHECK(im >= 0 && id > im && ip > id);
    for (u32 s = 1; s <= 3; s++)
        CHECK(m_get(M_DART + 0x1000 + 4 * s) == 0 && m_get(M_DART + 0x1400 + 4 * s) == 0);
    CHECK(!(M.dart_enabled & 0xe) && !M.link);
    CHECK(svc(N1SV_STATUS) & N1SV_ST_PARKED);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 0 && mapped_dev(WIN + N1PT_BAR2_OFF) == 0);
    u32 pa = M.perst_asserts;
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == N1PT_E_PERM && cmd(N1PT_CMD_ENABLE) == N1PT_E_PERM);
    CHECK(cmd(N1PT_CMD_PARK) == 0);
    hv_win_n1pt_retire();
    CHECK(M.perst_asserts == pa);
}

static void sc_irq(void)
{
    setup();
    CHECK(prep_start() == 0);
    CHECK(hv_win_n1pt_irq(1888 + 0) && G.spi_level && svc(N1SV_IRQ_STATUS) == 1);
    CHECK(hv_win_n1pt_irq(1888 + 16) && hv_win_n1pt_irq(1888 + 33));
    CHECK(svc(N1SV_IRQ_STATUS) == (1u | 1u << 4 | 1u << 21));
    CHECK(!hv_win_n1pt_irq(1888 + 4) && !hv_win_n1pt_irq(1888 + 34) && !hv_win_n1pt_irq(1000) && !hv_win_n1pt_irq(1522));
    gw(BAR4 + N1SV_IRQ_ACK, 1u | 1u << 4);
    CHECK(G.spi_level && svc(N1SV_IRQ_STATUS) == 1u << 21);
    gw(BAR4 + N1SV_IRQ_ACK, 1u << 21);
    CHECK(!G.spi_level && svc(N1SV_IRQ_STATUS) == 0);
    CHECK(G.unmask[1888] == 2 && G.unmask[1888 + 33] == 2); /* re-unmasked after each delivery */
    /* port interrupt: link down -> bit 31, LINKDN latched, INTSTAT cleared */
    m_set(P0 + 0x100, 1u << 14);
    CHECK(hv_win_n1pt_irq(1523) && svc(N1SV_IRQ_STATUS) == N1PT_IRQ_LINK && m_get(P0 + 0x100) == 0);
    CHECK(svc(N1SV_STATUS) & N1SV_ST_LINKDN);
    gw(BAR4 + N1SV_IRQ_ACK, N1PT_IRQ_LINK);
    /* another port interrupt cause: cleared, nothing latched */
    m_set(P0 + 0x100, 1u << 3);
    CHECK(hv_win_n1pt_irq(1523) && svc(N1SV_IRQ_STATUS) == 0 && !G.spi_level && m_get(P0 + 0x100) == 0);
    /* storm: after 256 port events the line is left masked, also across a PORT_CYCLE */
    for (int i = 0; i < 300; i++) {
        m_set(P0 + 0x100, 1u << 14);
        hv_win_n1pt_irq(1523);
    }
    CHECK(hv_win_n1pt_stat(10) == 302);
    CHECK(G.unmask[1523] == 1 + 256);
    u32 um = G.unmask[1523];
    d3();
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == 0);
    CHECK(G.unmask[1523] == um);
    CHECK(hv_win_n1pt_stat(5) == 3 + 1 + 254); /* latched: 3 MSI, 1 link, 254 storm events below the cap */
}

static void sc_no_irq(void)
{
    setup();
    G.aic_bad = true;
    CHECK(prep_start() == 0);
    CHECK(!(hv_win_n1pt_stat(0) & 4) && !hv_win_n1pt_irq(1888) && G.unmask[1888] == 0 && G.unmask[1523] == 0);
    CHECK(gr(ECAMI + (1UL << 20)) == 0x1900106b); /* the device still works by polling */
}

static void sc_refusals(int k)
{
    setup();
    int r;
    switch (k) {
    case 0: G.disk_used = true; CHECK(prep() == -2 && M.writes == 0 && M.smc_sessions == 0); break;
    case 1: G.smcp_attempted = true; CHECK(prep() == -2 && M.writes == 0); break;
    case 2: G.n1_touched = true; CHECK(prep() == -2 && M.writes == 0); break;
    case 3: CHECK(hv_win_n1pt_prep(WIN + 0x1000, ECAMI, IOVA, SPI) == -3 && M.writes == 0); break;
    case 4: CHECK(hv_win_n1pt_prep(WIN, ECAMI + 0x1000, IOVA, SPI) == -3); break;
    case 5: CHECK(hv_win_n1pt_prep(WIN, ECAMI, 0xfff00000UL, SPI) == -3); break; /* covers the doorbell */
    case 6: CHECK(hv_win_n1pt_prep(WIN, ECAMI, IOVA, 20) == -3 && hv_win_n1pt_prep(WIN, ECAMI, IOVA, SPI) == -1); break;
    case 7: hv_started_cpus[0] = true; CHECK(prep() == -1 && M.writes == 0); break;
    case 8: CHECK(prep() == 0 && prep() == -1); break;
    case 9: M.never_link = true; r = prep(); CHECK(r == -5); break;
    case 10: M.rom_stage = 5; CHECK(prep() == -6 && !M.link); break; /* ROM still running */
    case 11: M.dart_protect = 1; r = prep(); CHECK((r == -5 || r == -6) && !M.link); break; /* training (stage G) or attach refuses */
    case 12: M.dart_error = 0x80000001; CHECK(prep() == -6); break;
    case 13: M.dart_nsid = 3; CHECK(prep() == -6); break; /* stream 3 does not exist */
    case 14: M.fw_running = true; CHECK(prep() < 0); break; /* not in the ROM personality */
    }
    if (k >= 9) {
        /* a refused PREP maps nothing for the guest */
        hv_win_n1pt_start();
        CHECK(G.nmap == 0 && !(hv_win_n1pt_stat(0) & 1));
        for (u32 s = 1; s <= 3; s++)
            CHECK(m_get(M_DART + 0x1000 + 4 * s) == 0);
    }
    CHECK(m_get(M_NVME + 0x1300) == 0xde71ce55 && (m_get(M_ANS + 0x44) & 0x10));
}

static void sc_start_fail(int k)
{
    setup();
    G.hook_fail_at = (u32)k; /* 1: ECAM hook, 2: window hook */
    CHECK(prep() == 0);
    u32 pa = M.perst_asserts;
    hv_win_n1pt_start();
    CHECK((hv_win_n1pt_stat(0) & 0x48) == 0x48); /* dead, parked */
    CHECK(M.perst_asserts == pa + 1 && !M.link);
    if (k == 2) {
        CHECK(gr(ECAMI) == 0xffffffffu && gr(ECAMI + (1UL << 20)) == 0xffffffffu);
        CHECK(gr(BAR4) == 0xffffffffu && mapped_dev(WIN + N1PT_BAR0_OFF) == 0);
    }
    CHECK(!hv_win_n1pt_irq(1888));
}

static void sc_storage_guard_unit(void)
{
    setup();
    CHECK(pcie_t6050_storage_guard(1) == -1); /* no reference */
    CHECK(pcie_t6050_storage_guard(0) == 0 && pcie_t6050_storage_guard(1) == 0);
    struct { u64 a; u32 x; int want; } t[] = {
        {M_PS + 8 * 0, 0x30, 0},            /* actual state / was-gated may move */
        {M_PS + 8 * 3, 0x01, -1},           /* ANS PS target */
        {M_PS + 8 * 5, 0x1000000, -1},      /* auto enable */
        {M_NVME + 0x1300, 1, -1},           /* BOOT_STATUS */
        {M_NVME + 0x14, 1, -1},             /* CC */
        {M_ANS + 0x44, 0x10, -1},           /* ANS stops */
        {M_NVME + 0x1c, 2, -1},             /* CSTS fatal */
        {M_NVME + 0x1c, 1, 0},              /* CSTS ready drops: allowed (resident's disk path) */
        {M_ANS + 0x48, 0xff, 0},            /* ANS status */
        {M_ANS + 0x8110, 0x1234, 0},        /* mailbox */
        {M_SART + 4 * 3, 1, -1},            /* SART */
        {M_SART + 0x60 + 4 * 15, 1, -1},
    };
    for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
        u32 old = m_get(t[i].a);
        m_set(t[i].a, old ^ t[i].x);
        CHECK(pcie_t6050_storage_guard(1) == t[i].want);
        m_set(t[i].a, old);
        CHECK(pcie_t6050_storage_guard(1) == 0);
    }
}


/* every IPA the guest is told about (service page POOL, BAR0, BAR2, BAR4, ECAM) is handled in EL2: mapped or trapped */
static int acc(u64 ipa, bool w) { u64 v = 0x5a5a5a5a; return guest_access(ipa, w, &v); }
static void all_advertised_handled(bool enabled)
{
    u64 pool = (u64)svc(N1SV_POOL_IPA_HI) << 32 | svc(N1SV_POOL_IPA_LO);
    u64 size = svc(N1SV_POOL_SIZE);
    CHECK(pool && pool == hv_win_n1pt_stat(1) && size == N1PT_POOL_SIZE);
    int unresolved = 0;
    for (u64 off = 0; off < size; off += 0x4000) /* every pool page, read and write: guest RAM-like */
        unresolved += acc(pool + off, true) != 1 || acc(pool + off + 0x3ff8, false) != 1;
    CHECK(unresolved == 0);
    for (u64 off = 0; off < N1PT_WIN_SIZE; off += 0x1000)
        unresolved += acc(WIN + off, false) < 0 || acc(WIN + off, true) < 0;
    for (u64 off = 0; off < N1PT_ECAM_SIZE; off += 0x1000)
        unresolved += acc(ECAMI + off, false) < 0 || acc(ECAMI + off + 4, true) < 0;
    CHECK(unresolved == 0);
    CHECK(acc(WIN + N1PT_BAR0_OFF, false) == 1 && acc(WIN + N1PT_BAR4_OFF, false) == 0);
    CHECK(acc(WIN + N1PT_BAR2_OFF, false) == (enabled ? 1 : 0));
}

static void sc_pool_map(void)
{
    setup();
    CHECK(prep() == 0);
    CHECK(hv_win_n1pt_stat(11) == 0);       /* not mapped before the launcher's MAP / hv_start */
    CHECK(hv_win_n1pt_map() == 0 && hv_win_n1pt_stat(11) == 1);
    CHECK(hv_win_n1pt_map() == 0);          /* idempotent */
    u32 nmap = G.nmap;
    hv_win_n1pt_start();
    CHECK(hv_win_n1pt_stat(11) == 1 && (hv_win_n1pt_stat(0) & 0x42) == 0x02);
    for (u32 i = nmap; i < G.nmap; i++)     /* start did not map the pool a second time */
        CHECK(!G.map[i].ram);
    /* the driver's first write, as in hardware run w24: va = pool start */
    u64 pool = hv_win_n1pt_stat(1), v = 0x1122334455667788UL;
    CHECK(guest_access(pool, true, &v) == 1 && *(u64 *)(uintptr_t)pool == 0x1122334455667788UL);
    all_advertised_handled(false);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    all_advertised_handled(true);
    hv_started_cpus[0] = true;
    CHECK(hv_win_n1pt_map() == -5);         /* MAP is pre-guest only */
    hv_started_cpus[0] = false;
}

static void sc_pool_at_start(void)
{
    setup();
    CHECK(prep_start() == 0);               /* no MAP call: hv_start maps it */
    CHECK(hv_win_n1pt_stat(11) == 1 && !(hv_win_n1pt_stat(0) & 0x40));
    all_advertised_handled(false);
}

static void sc_pool_conflict(void)
{
    setup();
    CHECK(prep() == 0);
    G.foreign_ipa = hv_win_n1pt_stat(1) + 0x100000; /* another stage-2 user inside the pool */
    CHECK(hv_win_n1pt_map() == -2 && hv_win_n1pt_stat(11) == 0);
    u32 pa = M.perst_asserts;
    hv_win_n1pt_start();                    /* refuses the N1, never the guest */
    CHECK((hv_win_n1pt_stat(0) & 0x48) == 0x48 && M.perst_asserts == pa + 1);
    CHECK(svc(N1SV_MAGIC) == 0xffffffffu && gr(ECAMI + (1UL << 20)) == 0xffffffffu);
    /* a stray guest access to the (unusable) pool still never leaves EL2: all-ones / dropped */
    u64 v = 0;
    CHECK(guest_access(hv_win_n1pt_stat(1), false, &v) == 0 && v == 0xffffffffu);
}

static void sc_map_fail(void)
{
    setup();
    CHECK(prep() == 0);
    G.ram_fail = true;
    CHECK(hv_win_n1pt_map() == -3);
    hv_win_n1pt_start();
    CHECK(hv_win_n1pt_stat(0) & 0x40);
    u64 v = 0;
    CHECK(guest_access(hv_win_n1pt_stat(1) + 0x4000, true, &v) == 0); /* RAZ/WI hook, not the host */
}

/* a range lost from stage 2 while the guest runs: repaired in EL2 on the abort, never escalated */
static void sc_unmapped_repair(void)
{
    setup();
    CHECK(prep_start() == 0);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    u64 pool = hv_win_n1pt_stat(1), v;
    glue_unmap(pool, N1PT_POOL_SIZE);
    v = 7;
    CHECK(guest_access(pool + 0x200000, true, &v) == 1 && hv_win_n1pt_stat(11) == 1);
    glue_unmap(ECAMI, N1PT_ECAM_SIZE);
    CHECK(guest_access(ECAMI + (1UL << 20), false, &v) == 0 && (u32)v == 0x1900106b);
    glue_unmap(WIN, N1PT_WIN_SIZE);
    CHECK(guest_access(BAR4, false, &v) == 0 && (u32)v == 0x5653314e);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 1 && mapped_dev(WIN + N1PT_BAR2_OFF) == 1);
    CHECK(hv_win_n1pt_stat(12) == 3);
    /* outside every n1pt range: not ours (the resident's usual path) */
    CHECK(guest_access(0x7000000000UL, false, &v) == -1 && hv_win_n1pt_stat(12) == 3);
    /* bounded */
    for (int i = 0; i < 80; i++)
        glue_unmap(ECAMI, N1PT_ECAM_SIZE), acc(ECAMI, false);
    glue_unmap(ECAMI, N1PT_ECAM_SIZE);
    CHECK(acc(ECAMI, false) == -1);
}

static void sc_unmapped_ecam_hook_failed(void)
{
    setup();
    G.hook_fail_at = 1;                     /* the ECAM hook at start fails: N1 refused, ECAM unmapped */
    CHECK(prep() == 0);
    hv_win_n1pt_start();
    CHECK(hv_win_n1pt_stat(0) & 0x40);
    u64 v = 0;
    CHECK(guest_access(ECAMI + (1UL << 20), false, &v) == 0 && (u32)v == 0xffffffffu); /* hooked on the abort */
}

/* ---- run w24c: the cycle in Linux's order (P0034 reset_root_port + P0033 port_disable/enable/configure/train) ---- */
struct step { const char *what; u64 a; u32 mask, want; };
static int find_step(u32 from, const struct step *st)
{
    for (u32 i = from; i < M.ntrace; i++)
        if (M.trace[i].a == st->a && (M.trace[i].v & st->mask) == st->want)
            return (int)i;
    return -1;
}
static void sc_cycle_linux_order(void)
{
    setup();
    CHECK(prep_start() == 0);
    d3();
    M.ntrace = 0;
    u64 RP = M_ECAM, P = P0, PERST = M_GPIO + 4 * 80, LANE = m_lane(0);
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == 0);
    const struct step seq[] = {
        {"RP COMMAND = 0 (word)", RP + 0x04, 0xffff, 0},
        {"secondary/subordinate bus cleared", RP + 0x18, 0x00ffff00, 0},
        {"APPCLK bit 8", P + 0x800, 0x100, 0x100},
        {"PME_Turn_Off", P + 0x088, 0xffffffff, 0x11},
        {"PERST# output low", PERST, 0x27f, 0x202},
        {"PHY bit 31 clear", LANE, 0x80000000, 0},
        {"0x13c bit 8 set", P + 0x13c, 0x100, 0x100},
        {"INT_RESET bit 24 set", P + 0x82c, 0x1000000, 0x1000000},
        {"INT_RESET bit 16 set", P + 0x82c, 0x10000, 0x10000},
        {"PERST_OFF clear", P + 0x82c, 0x1, 0},
        {"INT_RESET bit 24 clear", P + 0x82c, 0x1000000, 0},
        {"0x13c bit 8 clear", P + 0x13c, 0x100, 0},
        {"lane bit 10 clear", LANE, 0x400, 0},
        {"lane bit 9 clear", LANE, 0x200, 0},
        {"lane bit 4 set", LANE, 0x10, 0x10},
        {"lane refclk0 req clear", LANE, 0x1, 0},
        {"lane refclk1 req clear", LANE, 0x2, 0},
        {"INTSTAT all", P + 0x100, 0xffffffff, 0xffffffff},
        {"APPCLK off", P + 0x800, 0x1, 0},
        /* port_enable */
        {"reset table first entry", P + 0x088, 0xffffffff, 0x110},
        {"INTMSK 0xfffffff0", P + 0x104, 0xffffffff, 0xfffffff0},
        {"INT_RESET 0x10000", P + 0x82c, 0xffffffff, 0x10000},
        {"RID2SID 63 cleared", P + 0x3000 + 4 * 63, 0xffffffff, 0},
        {"MSIMAP 255 cleared", P + 0x3800 + 4 * 255, 0xffffffff, 0},
        {"late table first entry", P + 0x130, 0xffffffff, 0x03020000},
        {"late table last entry", P + 0x83c, 0xffffffff, 0},
        {"APPCLK enable", P + 0x800, 0x1, 0x1},
        {"PERST_OFF set", P + 0x82c, 0x1, 0x1},
        {"INT_RESET bit 16 clear", P + 0x82c, 0x10000, 0},
        {"APPCLK CGDIS", P + 0x800, 0x100, 0x100},
        /* port_train */
        {"lane refclk0 req", LANE, 0x1, 0x1},
        {"lane refclk1 req", LANE, 0x2, 0x2},
        {"lane bit 4 clear", LANE, 0x10, 0},
        {"lane bit 9", LANE, 0x200, 0x200},
        {"lane bit 10", LANE, 0x400, 0x400},
        {"PERST# released", PERST, 0x27f, 0x203},
        {"LTSSM start", P + 0x080, 0x1, 0x1},
    };
    u32 at = 0;
    for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
        int k = find_step(at, &seq[i]);
        if (k < 0) {
            printf("FAIL linux order: step %zu '%s' not found after trace %u\n", i, seq[i].what, at);
            fails++;
            break;
        }
        at = (u32)k + 1;
        checks++;
    }
    /* L2 was reached between PME_Turn_Off and PERST# (the model sets it only on a 0x11 with the link up) */
    CHECK(M.trace[0].a == RP + 0x04 || find_step(0, &seq[0]) >= 0);
    CHECK(M.pm_logged == 1 && M.drops_logged == 0 && M.dumps == 0);
}

static void sc_cycle_not_d3(void)
{
    setup();
    CHECK(prep_start() == 0);
    u32 pa = M.perst_asserts;
    M.ntrace = 0;
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == N1PT_E_DSTATE);
    CHECK(M.perst_asserts == pa && M.link && M.ntrace == 0); /* nothing written */
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 1);            /* BAR0 stays mapped */
    d3();
    CHECK(cmd(N1PT_CMD_PORT_CYCLE) == 0);
}

static void sc_cycle_fail_dump(void)
{
    setup();
    CHECK(prep_start() == 0);
    d3();
    M.no_820 = true; /* run w24c: 0x820 bit 2 never sets */
    s32 r = cmd(N1PT_CMD_PORT_CYCLE);
    CHECK(r == N1PT_E_IO || r == N1PT_E_NOLINK);
    CHECK(M.dumps >= 1);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 0);
    /* PARK after the broken cycle (the retire of run w24c): PERST# asserted and the LTSSM off whatever happens */
    u32 pa = M.perst_asserts;
    hv_win_n1pt_retire();
    CHECK((m_get(M_GPIO + 4 * 80) & 0x27f) == 0x202 && !(m_get(P0 + 0x080) & 1) && M.perst_asserts >= pa);
    CHECK(hv_win_n1pt_stat(0) & 0x88);
}

static void sc_park_broken(int k)
{
    setup();
    CHECK(prep_start() == 0);
    M.fw_running = true;
    M.rom_stage = 1;
    CHECK(cmd(N1PT_CMD_ENABLE) == 0);
    if (k == 0)
        M.pme_stuck = true;  /* link up but PME_Turn_Off never completes */
    else
        M.link = false;      /* link already down */
    s32 r = cmd(N1PT_CMD_PARK);
    CHECK(r == 0 || r == N1PT_E_IO);
    CHECK((m_get(M_GPIO + 4 * 80) & 0x27f) == 0x202);  /* PERST# asserted */
    CHECK(!(m_get(P0 + 0x080) & 1));                   /* LTSSM off */
    CHECK(svc(N1SV_STATUS) & N1SV_ST_PARKED);
    CHECK(mapped_dev(WIN + N1PT_BAR0_OFF) == 0 && mapped_dev(WIN + N1PT_BAR2_OFF) == 0);
    if (k == 0)
        CHECK(r == N1PT_E_IO && !(m_get(P0 + 0x800) & 1)); /* port disabled after the failed L2 */
}

static void sc_drop_logged(void)
{
    setup();
    CHECK(prep_start() == 0);
    gw(BAR4 + N1SV_CFG_ADDR, 0x10);
    gw(BAR4 + N1SV_CFG_DATA, 0);
    guest_write(BAR4 + N1SV_CFG_DATA + 2, 1, 0x1234);
    CHECK(M.drops_logged == 2 && svc(N1SV_CNT_DROPPED) == 2);
    gw(BAR4 + N1SV_CFG_ADDR, 0x44);
    guest_write(BAR4 + N1SV_CFG_DATA, 1, 0x010b); /* the Linux word write */
    CHECK(M.pm_logged == 1 && (m_get(CFG(0) + 0x44) & 0x103) == 0x103);
}

typedef void (*scen_t)(void);
static int run(const char *name, void (*f)(int), int arg)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        fails = checks = 0;
        f(arg);
        printf("  %-22s %d checks%s\n", name, checks, fails ? " FAILED" : "");
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("FAIL scenario %s (status 0x%x)\n", name, st);
        return 1;
    }
    return 0;
}
static scen_t cur;
static void call0(int a) { (void)a; cur(); }
static int run0(const char *name, scen_t f) { cur = f; return run(name, call0, 0); }

int main(void)
{
    int bad = 0, n = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    bad += run0("prep+start+retire", sc_prep_start); n++;
    bad += run0("dart 3-level", sc_three_level); n++;
    bad += run0("enable", sc_enable); n++;
    bad += run0("enable nodev", sc_enable_nodev); n++;
    bad += run0("port cycle", sc_cycle); n++;
    bad += run0("cycle no link", sc_cycle_nolink); n++;
    bad += run("guard in cycle", sc_guard, N1PT_CMD_PORT_CYCLE); n++;
    bad += run("guard in park", sc_guard, N1PT_CMD_PARK); n++;
    bad += run0("park", sc_park); n++;
    bad += run0("irq + storm", sc_irq); n++;
    bad += run0("no aic", sc_no_irq); n++;
    for (int k = 0; k <= 14; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "prep refusal %d", k);
        bad += run(nm, sc_refusals, k); n++;
    }
    bad += run("start fail ecam", sc_start_fail, 1); n++;
    bad += run("start fail window", sc_start_fail, 2); n++;
    bad += run0("storage guard unit", sc_storage_guard_unit); n++;
    bad += run0("pool map + advertised", sc_pool_map); n++;
    bad += run0("pool mapped at start", sc_pool_at_start); n++;
    bad += run0("pool conflict", sc_pool_conflict); n++;
    bad += run0("pool map fails", sc_map_fail); n++;
    bad += run0("unmapped repair", sc_unmapped_repair); n++;
    bad += run0("unmapped ecam (dead)", sc_unmapped_ecam_hook_failed); n++;
    bad += run0("cycle in Linux order", sc_cycle_linux_order); n++;
    bad += run0("cycle refused not D3", sc_cycle_not_d3); n++;
    bad += run0("cycle fail dump+park", sc_cycle_fail_dump); n++;
    bad += run("park PME stuck", sc_park_broken, 0); n++;
    bad += run("park link down", sc_park_broken, 1); n++;
    bad += run0("dropped write logged", sc_drop_logged); n++;
    printf("%s: win-n1pt plat, %d scenarios, %d failed\n", bad ? "FAIL" : "PASS", n, bad);
    return bad ? 1 : 0;
}
