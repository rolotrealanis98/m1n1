/* SPDX-License-Identifier: MIT */
/*
 * tests/win-n1pt/test_logic: hv_win_n1pt_logic.h against stub ops. Expectations come from the contract
 * (wireless/n1-native-20261111/INTERFACE.md s1-s2), not from the code under test:
 *  - ECAM: every dword offset of both emulated headers read at every width (consistent), every other
 *    bus/device/function and everything past the window all-ones; header properties (IDs, class, header type,
 *    bus numbers, window covering the BARs, capability lists: ep PM + Express and no MSI, rp PM + Express root
 *    port, no ASPM anywhere, INTA); random writes to every offset change only the writable registers; BAR
 *    probing returns the sizes and a moved BAR is counted, never forwarded; COMMAND forwards only MEM|MASTER to
 *    the real fn0; D-state (PMCSR) writes are stored and never forwarded; secondary reset never forwarded.
 *  - service page: identity/pool registers, commands (result, refusal after guard/park, invalid), config window
 *    filter (reads fn0-2 only, writes only 0xf88/0xf8c/0xf90/PM_CTRL/COMMAND MEM|MASTER, the rest dropped and
 *    counted, nothing without link), STATUS bits incl. the one-shot link-down latch, counters.
 *  - IRQ latch: status bits per MSI data value and AIC line, SPI level held while any bit is set, partial acks.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hv/win/hv_win_n1pt_logic.h"

static int fails, checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            if (fails < 40)                                                                        \
                printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                \
        }                                                                                          \
    } while (0)

/* ---- stub hardware ---- */
static struct {
    bool link;
    uint32_t cfg[3][1024];
    unsigned nreads, nwrites, ncmds, last_cmd;
    struct { unsigned fn, off; uint32_t v; } w[64];
    uint64_t now;
    bool spi;
    unsigned spi_calls;
    int cmd_ret;
} H;

static uint32_t s_cfg_read(void *c, unsigned fn, unsigned off) { (void)c; H.nreads++; return H.cfg[fn][off / 4]; }
static void s_cfg_write(void *c, unsigned fn, unsigned off, uint32_t v)
{
    (void)c;
    if (H.nwrites < 64) {
        H.w[H.nwrites].fn = fn; H.w[H.nwrites].off = off; H.w[H.nwrites].v = v;
    }
    H.nwrites++;
    H.cfg[fn][off / 4] = v;
}
static bool s_link(void *c) { (void)c; return H.link; }
static int s_command(struct n1pt *s, unsigned cmd, uint32_t arg)
{
    (void)arg;
    H.ncmds++;
    H.last_cmd = cmd;
    H.now += 1234;
    if (cmd == N1PT_CMD_ENABLE && !H.cmd_ret)
        s->fns_enabled = true;
    if (cmd == N1PT_CMD_PARK)
        s->parked = true;
    return H.cmd_ret;
}
static uint64_t s_now(void *c) { (void)c; return H.now; }
static void s_spi(void *c, bool l) { (void)c; H.spi = l; H.spi_calls++; }
static const struct n1pt_ops OPS = {0, s_cfg_read, s_cfg_write, s_link, s_command, s_now, s_spi};

#define WIN  0xbc2000000ULL
#define POOL 0x9f8000000ULL
#define IOVA 0x10000000ULL

static struct n1pt S;
static void fresh(void)
{
    memset(&H, 0, sizeof(H));
    H.link = true;
    H.cfg[0][0] = 0x1900106b;
    H.cfg[0][1] = 0x00100002;
    H.cfg[1][0] = 0x1902106b;
    H.cfg[2][0] = 0x1903106b;
    n1pt_init(&S, &OPS, WIN, POOL, IOVA, N1PT_POOL_SIZE);
}

static uint32_t er(unsigned bus, unsigned dev, unsigned fn, unsigned reg, int w)
{
    return n1pt_ecam_read(&S, ((uint64_t)bus << 20) | (dev << 15) | (fn << 12) | reg, w);
}
static void ew(unsigned bus, unsigned dev, unsigned fn, unsigned reg, uint32_t v, int w)
{
    n1pt_ecam_write(&S, ((uint64_t)bus << 20) | (dev << 15) | (fn << 12) | reg, v, w);
}
static uint32_t sr(uint32_t off) { return n1pt_window_read(&S, N1PT_BAR4_OFF + off, 2); }
static void sw(uint32_t off, uint32_t v) { n1pt_window_write(&S, N1PT_BAR4_OFF + off, v, 2); }

/* walk a capability list: returns the offset of `id`, -1 if absent; *n = entries, loops are failures */
static int cap_find(unsigned bus, uint8_t id, int *n)
{
    unsigned p = er(bus, 0, 0, 0x34, 0) & 0xfc;
    int found = -1, k = 0;
    while (p && k < 48) {
        uint32_t h = er(bus, 0, 0, p, 2);
        if ((h & 0xff) == id && found < 0)
            found = (int)p;
        p = (h >> 8) & 0xfc;
        k++;
    }
    *n = k;
    return k >= 48 ? -2 : found;
}

static void test_widths_and_allones(void)
{
    fresh();
    /* every dword of both headers: byte and halfword reads equal the dword's lanes; unaligned = all-ones */
    for (unsigned bus = 0; bus < 2; bus++)
        for (unsigned reg = 0; reg < 0x1000; reg += 4) {
            uint32_t d = er(bus, 0, 0, reg, 2);
            for (unsigned b = 0; b < 4; b++)
                CHECK(er(bus, 0, 0, reg + b, 0) == ((d >> (8 * b)) & 0xff));
            CHECK(er(bus, 0, 0, reg, 1) == (d & 0xffff));
            CHECK(er(bus, 0, 0, reg + 2, 1) == (d >> 16));
            CHECK(er(bus, 0, 0, reg + 1, 1) == 0xffff);
            CHECK(er(bus, 0, 0, reg + 2, 2) == 0xffffffffu);
        }
    /* every other device/function of buses 0 and 1, every dword: all-ones */
    unsigned bad = 0;
    for (uint64_t off = 0; off < N1PT_ECAM_SIZE; off += 4) {
        if (((off >> 12) & 0xff) == 0)
            continue;
        bad += n1pt_ecam_read(&S, off, 2) != 0xffffffffu;
        bad += n1pt_ecam_read(&S, off, 0) != 0xff;
    }
    CHECK(bad == 0);
    /* buses past 1 (outside the 2 MiB window) and 64-bit accesses */
    CHECK(n1pt_ecam_read(&S, N1PT_ECAM_SIZE, 2) == 0xffffffffu);
    CHECK(n1pt_ecam_read(&S, 2ULL << 20, 2) == 0xffffffffu);
    CHECK(n1pt_ecam_read(&S, 0, 3) == 0xffffffffu);
    /* the endpoint is single-function: fn 1-7 of 01:00 all-ones (the real fn1/fn2 never show) */
    for (unsigned fn = 1; fn < 8; fn++)
        CHECK(er(1, 0, fn, 0, 2) == 0xffffffffu);
    CHECK(H.nreads == 0 && H.nwrites == 0); /* emulation never reads the real config space */
}

static void test_header_properties(void)
{
    fresh();
    int n;
    /* root port */
    CHECK(er(0, 0, 0, 0x00, 2) == 0x100c106b);
    CHECK((er(0, 0, 0, 0x0c, 2) >> 16 & 0x7f) == 1);             /* type 1 */
    CHECK(!(er(0, 0, 0, 0x0c, 2) >> 16 & 0x80));                 /* single function */
    CHECK(er(0, 0, 0, 0x08, 2) >> 8 == 0x060400);                /* PCI-to-PCI bridge */
    CHECK((er(0, 0, 0, 0x18, 2) & 0xffffff) == 0x010100);        /* primary 0, secondary 1, subordinate 1 */
    CHECK(er(0, 0, 0, 0x06, 1) & 0x10);                          /* capability list */
    uint32_t pb = er(0, 0, 0, 0x24, 2);
    uint64_t base = ((uint64_t)er(0, 0, 0, 0x28, 2) << 32) | ((uint64_t)(pb & 0xfff0) << 16);
    uint64_t lim = ((uint64_t)er(0, 0, 0, 0x2c, 2) << 32) | ((uint64_t)(pb >> 16 & 0xfff0) << 16) | 0xfffff;
    CHECK((pb & 0xf) == 1 && (pb >> 16 & 0xf) == 1);             /* 64-bit prefetchable window */
    CHECK(base <= WIN + N1PT_BAR0_OFF && lim >= WIN + N1PT_BAR4_OFF + N1PT_BAR4_SIZE - 1); /* covers the BARs */
    CHECK(base == WIN && lim == WIN + N1PT_WIN_SIZE - 1);
    CHECK((er(0, 0, 0, 0x1c, 2) & 0xf0) > (er(0, 0, 0, 0x1c, 2) >> 8 & 0xf0) || (er(0, 0, 0, 0x1c, 2) & 0xffff) == 0xf0); /* no I/O */
    CHECK((er(0, 0, 0, 0x20, 2) & 0xfff0) > (er(0, 0, 0, 0x20, 2) >> 16 & 0xfff0)); /* no 32-bit window */
    CHECK(er(0, 0, 0, 0x10, 2) == 0 && er(0, 0, 0, 0x14, 2) == 0); /* bridge BARs unimplemented */
    CHECK((er(0, 0, 0, 0x3c, 2) >> 8 & 0xff) == 0);              /* no interrupt pin */
    int rpm = cap_find(0, 0x01, &n), rex = cap_find(0, 0x10, &n);
    CHECK(rpm >= 0x40 && rex >= 0x40 && n == 2);
    CHECK(cap_find(0, 0x05, &n) == -1);
    if (rex > 0) {
        CHECK((er(0, 0, 0, rex + 2, 1) >> 4 & 0xf) == 4);       /* root port */
        CHECK((er(0, 0, 0, rex + 0x0c, 2) >> 10 & 3) == 0);      /* LnkCap: no ASPM support */
        CHECK((er(0, 0, 0, rex + 0x0c, 2) >> 4 & 0x3f) == 1);    /* x1 */
    }
    if (rpm > 0)
        CHECK((er(0, 0, 0, rpm + 2, 1) >> 9 & 3) == 0);         /* no D1/D2 */
    /* endpoint */
    CHECK(er(1, 0, 0, 0x00, 2) == 0x1900106b);                   /* always 106b:1900 */
    CHECK(er(1, 0, 0, 0x08, 2) >> 8 == 0xff0000);                /* class ff00 */
    CHECK((er(1, 0, 0, 0x0c, 2) >> 16 & 0xff) == 0);             /* type 0, single function */
    CHECK((er(1, 0, 0, 0x3c, 2) >> 8 & 0xff) == 1);              /* Interrupt Pin = INTA */
    int epm = cap_find(1, 0x01, &n), eex = cap_find(1, 0x10, &n);
    CHECK(epm >= 0x40 && eex >= 0x40 && n == 2);
    CHECK(cap_find(1, 0x05, &n) == -1 && cap_find(1, 0x11, &n) == -1); /* no MSI, no MSI-X */
    if (eex > 0) {
        CHECK((er(1, 0, 0, eex + 2, 1) >> 4 & 0xf) == 0);       /* PCIe endpoint */
        CHECK((er(1, 0, 0, eex + 0x0c, 2) >> 10 & 3) == 0);      /* no ASPM */
        CHECK(!(er(1, 0, 0, eex + 4, 2) & (1u << 28)));          /* no FLR */
    }
    /* BARs: 64-bit prefetchable at the contract's addresses */
    uint64_t want[3] = {WIN + N1PT_BAR0_OFF, WIN + N1PT_BAR2_OFF, WIN + N1PT_BAR4_OFF};
    for (int i = 0; i < 3; i++) {
        uint32_t lo = er(1, 0, 0, 0x10 + 8 * i, 2), hi = er(1, 0, 0, 0x14 + 8 * i, 2);
        CHECK((lo & 0xf) == 0xc);
        CHECK((((uint64_t)hi << 32) | (lo & ~0xfu)) == want[i]);
    }
    CHECK(er(1, 0, 0, 0x30, 2) == 0); /* no expansion ROM */
}

/* the registers a guest may change, per header (dword offset -> writable mask of the read value) */
static uint32_t writable(unsigned bus, unsigned reg)
{
    if (bus == 0) {
        switch (reg) {
        case 0x04: return 0x0547;
        case 0x3c: return 0xffff00ff;
        case 0x44: return 0x0103;
        case 0x58: return 0x7fff;
        case 0x60: return 0xffdc;
        default: return 0;
        }
    }
    switch (reg) {
    case 0x04: return 0x0547;
    case 0x3c: return 0xff;
    case 0x44: return 0x0103;
    case 0x78: return 0x7fff;
    case 0x80: return 0xffdc;
    default: return 0;
    }
}

static void test_random_writes(void)
{
    srand(7);
    for (unsigned bus = 0; bus < 2; bus++)
        for (unsigned reg = 0; reg < 0x1000; reg += 4) {
            if (bus == 1 && reg >= 0x10 && reg < 0x28)
                continue; /* BARs: test_bars */
            fresh();
            uint32_t before[1024];
            for (unsigned r = 0; r < 0x1000; r += 4)
                before[r / 4] = er(bus, 0, 0, r, 2);
            uint32_t v = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
            ew(bus, 0, 0, reg, v, 2);
            unsigned changed_elsewhere = 0;
            for (unsigned r = 0; r < 0x1000; r += 4)
                if (r != reg && er(bus, 0, 0, r, 2) != before[r / 4])
                    changed_elsewhere++;
            CHECK(changed_elsewhere == 0);
            uint32_t m = writable(bus, reg), now = er(bus, 0, 0, reg, 2);
            CHECK((now & ~m) == (before[reg / 4] & ~m)); /* read-only bits unchanged */
            if (reg == 0x44)
                CHECK((now & 0x0103) == (v & 0x0103));   /* PMCSR power state + PME_En stored */
            if (bus == 0 || reg != 0x04)
                CHECK(H.nwrites == 0);                   /* only the endpoint's COMMAND reaches hardware */
            /* byte writes: the same rule per lane */
            fresh();
            for (unsigned b = 0; b < 4; b++)
                ew(bus, 0, 0, reg + b, 0xff, 0);
            now = er(bus, 0, 0, reg, 2);
            CHECK((now & ~m) == (before[reg / 4] & ~m));
            if (bus == 0 || reg != 0x04)
                CHECK(H.nwrites == 0);
        }
    /* writes to every other device/function are dropped and reach nothing */
    fresh();
    struct n1pt snap = S;
    for (uint64_t off = 0x1000; off < N1PT_ECAM_SIZE; off += 0x1000)
        if (((off >> 12) & 0xff) != 0)
            for (unsigned r = 0; r < 0x100; r += 4)
                n1pt_ecam_write(&S, off + r, 0xffffffffu, 2);
    n1pt_ecam_write(&S, N1PT_ECAM_SIZE + 4, 0x6, 2);
    CHECK(memcmp(&snap, &S, sizeof(S)) == 0 && H.nwrites == 0 && H.nreads == 0);
}

static void test_command_and_dstate(void)
{
    fresh();
    H.cfg[0][1] = 0x00100140; /* real fn0: SERR + INTx disable set by EL2, MEM/MASTER off */
    ew(1, 0, 0, 0x04, 0xffff, 1);
    CHECK(H.nwrites == 1 && H.w[0].fn == 0 && H.w[0].off == 0x04);
    CHECK(H.w[0].v == (0x0140 | 0x6)); /* MEM | MASTER forwarded, everything else the real value */
    CHECK((er(1, 0, 0, 0x04, 1)) == 0x0547);
    ew(1, 0, 0, 0x04, 0x0000, 1);
    CHECK(H.nwrites == 2 && H.w[1].v == 0x0140);
    /* status half written (W1C on real hardware): COMMAND untouched */
    ew(1, 0, 0, 0x06, 0xffff, 1);
    CHECK(H.nwrites == 2);
    /* link down: stored, not forwarded */
    H.link = false;
    ew(1, 0, 0, 0x04, 0x6, 1);
    CHECK(H.nwrites == 2 && (er(1, 0, 0, 0x04, 1) & 6) == 6);
    H.link = true;
    S.parked = true;
    ew(1, 0, 0, 0x04, 0x6, 1);
    CHECK(H.nwrites == 2);
    S.parked = false;
    /* D-state: D3hot written to both PMCSRs, read back, never forwarded */
    unsigned w0 = H.nwrites;
    ew(1, 0, 0, 0x44, 0x0103, 1);
    ew(0, 0, 0, 0x44, 0x0003, 1);
    CHECK((er(1, 0, 0, 0x44, 1) & 3) == 3 && (er(0, 0, 0, 0x44, 1) & 3) == 3);
    CHECK(H.nwrites == w0);
    ew(1, 0, 0, 0x44, 0x0000, 2);
    CHECK((er(1, 0, 0, 0x44, 1) & 3) == 0 && H.nwrites == w0);
    /* bridge control with secondary bus reset: stored only */
    ew(0, 0, 0, 0x3e, 0x0040, 1);
    CHECK((er(0, 0, 0, 0x3e, 1) & 0x40) && H.nwrites == w0);
    /* ASPM control and retrain in LnkCtl are dropped */
    int n, rex = cap_find(0, 0x10, &n), eex = cap_find(1, 0x10, &n);
    ew(0, 0, 0, rex + 0x10, 0x0023, 1);
    ew(1, 0, 0, eex + 0x10, 0x0023, 1);
    CHECK((er(0, 0, 0, rex + 0x10, 1) & 0x23) == 0 && (er(1, 0, 0, eex + 0x10, 1) & 0x23) == 0);
    /* FLR bit in DevCtl dropped */
    ew(1, 0, 0, eex + 0x08, 0x8000, 1);
    CHECK(!(er(1, 0, 0, eex + 0x08, 1) & 0x8000) && H.nwrites == w0);
}

static void test_bars(void)
{
    fresh();
    static const uint32_t size[3] = {0x10000, 0x20000, 0x4000};
    for (int i = 0; i < 3; i++) {
        unsigned lo = 0x10 + 8 * i, hi = lo + 4;
        uint32_t olo = er(1, 0, 0, lo, 2), ohi = er(1, 0, 0, hi, 2);
        ew(1, 0, 0, lo, 0xffffffffu, 2);
        ew(1, 0, 0, hi, 0xffffffffu, 2);
        CHECK(er(1, 0, 0, lo, 2) == ((~(size[i] - 1)) | 0xc)); /* size probe */
        CHECK(er(1, 0, 0, hi, 2) == 0xffffffffu);
        ew(1, 0, 0, lo, olo, 2);
        ew(1, 0, 0, hi, ohi, 2);
        CHECK(er(1, 0, 0, lo, 2) == olo && er(1, 0, 0, hi, 2) == ohi);
    }
    CHECK(S.n_bar_moves == 0 && H.nwrites == 0);
    ew(1, 0, 0, 0x10, 0x80000000u, 2); /* a move: counted, the real BAR and the map do not move */
    CHECK(S.n_bar_moves == 1 && H.nwrites == 0);
    /* BAR1/3/5 empty registers after BAR4 (0x28..0x2c) are not BARs */
    CHECK(er(1, 0, 0, 0x28, 2) == 0);
}

static void test_service_page(void)
{
    fresh();
    CHECK(sr(N1SV_MAGIC) == 0x5653314e && sr(N1SV_VERSION) == 1);
    CHECK(n1pt_window_read(&S, N1PT_BAR4_OFF + 1, 0) == 0x31); /* byte lanes of MAGIC */
    CHECK(sr(N1SV_POOL_IPA_LO) == (uint32_t)POOL && sr(N1SV_POOL_IPA_HI) == (uint32_t)(POOL >> 32));
    CHECK(sr(N1SV_IOVA_LO) == (uint32_t)IOVA && sr(N1SV_IOVA_HI) == 0);
    CHECK(sr(N1SV_POOL_SIZE) == 128u << 20);
    CHECK(((uint64_t)sr(N1SV_IOVA_HI) << 32 | sr(N1SV_IOVA_LO)) + sr(N1SV_POOL_SIZE) < (1ULL << 40));
    /* read-only registers ignore writes */
    sw(N1SV_MAGIC, 0); sw(N1SV_POOL_IPA_LO, 0); sw(N1SV_POOL_SIZE, 0); sw(N1SV_IRQ_STATUS, 0xffffffffu);
    CHECK(sr(N1SV_MAGIC) == 0x5653314e && sr(N1SV_POOL_IPA_LO) == (uint32_t)POOL && sr(N1SV_POOL_SIZE) == 128u << 20);
    /* rest of the window: all-ones, writes dropped */
    for (uint64_t off = 0; off < N1PT_WIN_SIZE; off += 0x800)
        if (off < N1PT_BAR4_OFF || off >= N1PT_BAR4_OFF + N1PT_BAR4_SIZE) {
            CHECK(n1pt_window_read(&S, off, 2) == 0xffffffffu);
            n1pt_window_write(&S, off, N1PT_CMD_PARK, 2);
        }
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CMD, N1PT_CMD_PARK, 1); /* sub-dword write: dropped */
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CMD + 1, N1PT_CMD_PARK, 2);
    CHECK(H.ncmds == 0 && !S.parked);

    /* commands */
    sw(N1SV_CMD_ARG, 77);
    CHECK(sr(N1SV_CMD_ARG) == 77);
    sw(N1SV_CMD, N1PT_CMD_ENABLE);
    CHECK(H.ncmds == 1 && H.last_cmd == N1PT_CMD_ENABLE && sr(N1SV_CMD_RESULT) == 0);
    CHECK(sr(N1SV_CNT_CMDS) == 1 && sr(N1SV_CNT_LAST_US) == 1234);
    CHECK(sr(N1SV_STATUS) & N1SV_ST_FNS);
    sw(N1SV_CMD, 0); sw(N1SV_CMD, 4); sw(N1SV_CMD, 0xffffffffu);
    CHECK(H.ncmds == 1 && (int32_t)sr(N1SV_CMD_RESULT) == N1PT_E_INVAL && sr(N1SV_CNT_CMDS) == 4);
    H.cmd_ret = N1PT_E_NOLINK;
    sw(N1SV_CMD, N1PT_CMD_PORT_CYCLE);
    CHECK((int32_t)sr(N1SV_CMD_RESULT) == N1PT_E_NOLINK);
    H.cmd_ret = 0;
    /* guard: every command refused from then on, without reaching the hardware */
    S.guard = true;
    unsigned c0 = H.ncmds;
    for (uint32_t c = 1; c <= 3; c++) {
        sw(N1SV_CMD, c);
        CHECK((int32_t)sr(N1SV_CMD_RESULT) == N1PT_E_PERM);
    }
    CHECK(H.ncmds == c0 && (sr(N1SV_STATUS) & N1SV_ST_GUARD));
    /* guard: no config access either (window or forwarded COMMAND), no link reported */
    unsigned w1 = H.nwrites, r1 = H.nreads;
    sw(N1SV_CFG_ADDR, 0x44);
    sw(N1SV_CFG_DATA, 3);
    CHECK(sr(N1SV_CFG_DATA) == 0xffffffffu);
    ew(1, 0, 0, 0x04, 6, 2);
    CHECK(H.nwrites == w1 && H.nreads == r1 && !(sr(N1SV_STATUS) & N1SV_ST_LINK));
    S.guard = false;
    /* park: then only PARK answers 0 (no hardware access), the rest E_PERM */
    sw(N1SV_CMD, N1PT_CMD_PARK);
    CHECK(S.parked && sr(N1SV_CMD_RESULT) == 0 && (sr(N1SV_STATUS) & N1SV_ST_PARKED));
    c0 = H.ncmds;
    sw(N1SV_CMD, N1PT_CMD_PORT_CYCLE);
    CHECK((int32_t)sr(N1SV_CMD_RESULT) == N1PT_E_PERM);
    sw(N1SV_CMD, N1PT_CMD_ENABLE);
    CHECK((int32_t)sr(N1SV_CMD_RESULT) == N1PT_E_PERM);
    sw(N1SV_CMD, N1PT_CMD_PARK);
    CHECK(sr(N1SV_CMD_RESULT) == 0 && H.ncmds == c0);
    CHECK(!(sr(N1SV_STATUS) & N1SV_ST_LINK)); /* parked: no link reported */
}

static void test_cfg_window(void)
{
    fresh();
    for (unsigned fn = 0; fn < 3; fn++)
        for (unsigned off = 0; off < 0x1000; off += 4)
            H.cfg[fn][off / 4] = (fn << 24) | off;
    H.cfg[0][0] = 0x1901106b;
    /* reads: anywhere in fn 0-2 */
    for (unsigned fn = 0; fn < 8; fn++)
        for (unsigned off = 0; off < 0x1000; off += 0x44) {
            sw(N1SV_CFG_ADDR, (fn << 12) | off);
            uint32_t v = sr(N1SV_CFG_DATA);
            CHECK(fn > 2 ? v == 0xffffffffu : v == H.cfg[fn][off / 4]);
        }
    sw(N1SV_CFG_ADDR, 0x1003); /* low bits ignored */
    CHECK(sr(N1SV_CFG_ADDR) == 0x1000);
    /* writes: every dword offset of every function */
    for (unsigned fn = 0; fn < 4; fn++)
        for (unsigned off = 0; off < 0x1000; off += 4) {
            unsigned w0 = H.nwrites, d0 = sr(N1SV_CNT_DROPPED);
            uint32_t real = H.cfg[fn < 3 ? fn : 0][off / 4];
            sw(N1SV_CFG_ADDR, (fn << 12) | off);
            sw(N1SV_CFG_DATA, 0xa5a5ffffu);
            bool allowed = fn < 3 && (off == 0xf88 || off == 0xf8c || off == 0xf90 || off == 0x44 || off == 0x04);
            if (allowed) {
                CHECK(H.nwrites == w0 + 1 && sr(N1SV_CNT_DROPPED) == d0);
                if (off == 0x04)
                    CHECK(H.w[(w0) % 64].v == ((real & 0xffff & ~6u) | 6u)); /* only MEM|MASTER change */
                else if (off == 0x44) /* PMCSR: PowerState, PME_En, PME_Status only; the rest the real value */
                    CHECK(H.cfg[fn][off / 4] == (((real & ~0x8000u) & ~0x8103u) | (0xa5a5ffffu & 0x8103u)));
                else
                    CHECK(H.cfg[fn][off / 4] == 0xa5a5ffffu);
            } else {
                CHECK(H.nwrites == w0 && sr(N1SV_CNT_DROPPED) == d0 + 1);
            }
        }
    /* 1/2-byte writes with byte enables (Linux writes PMCSR as a word: 0x010b = D3hot | PME_En | NoSoftReset) */
    fresh();
    H.cfg[0][0x44 / 4] = 0x00c08008u;  /* BSE/Data in the upper half, NoSoftReset, PME_Status pending */
    sw(N1SV_CFG_ADDR, 0x44);
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA, 0x010b, 1);
    CHECK(H.nwrites == 1 && H.cfg[0][0x44 / 4] == 0x00c0010bu); /* D3hot | PME_En; PME_Status not cleared */
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA, 0x0008, 1);
    CHECK(H.cfg[0][0x44 / 4] == 0x00c00008u);                  /* back to D0 */
    H.cfg[0][0x44 / 4] = 0x00c08008u;
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA + 1, 0x80, 0); /* byte lane 1: PME_Status W1C */
    CHECK(H.w[H.nwrites - 1].v == 0x00c08008u && sr(N1SV_CNT_DROPPED) == 0);
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA + 2, 0xffff, 1); /* upper half: nothing writable */
    CHECK((H.w[H.nwrites - 1].v >> 16) == 0x00c0);
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA + 1, 0x80, 1); /* unaligned halfword: dropped silently */
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA, 0x3, 0);      /* byte write of the D-state */
    CHECK((H.cfg[0][0x44 / 4] & 3) == 3);
    H.cfg[0][1] = 0xffff0140u;          /* STATUS error bits set: never written back */
    sw(N1SV_CFG_ADDR, 0x04);
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA, 0x0006, 1);
    CHECK(H.w[H.nwrites - 1].v == 0x00000146u);
    unsigned d1 = sr(N1SV_CNT_DROPPED);
    sw(N1SV_CFG_ADDR, 0x10);            /* not allowed: dropped, recorded with its byte enables */
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CFG_DATA + 2, 0x1234, 1);
    CHECK(sr(N1SV_CNT_DROPPED) == d1 + 1 && S.drop_be == 0xc && S.drop_val == 0x12340000u && S.drop_addr == 0x10);
    /* no link: reads all-ones, writes dropped */
    H.link = false;
    sw(N1SV_CFG_ADDR, 0x44);
    CHECK(sr(N1SV_CFG_DATA) == 0xffffffffu);
    unsigned w0 = H.nwrites;
    sw(N1SV_CFG_DATA, 3);
    CHECK(H.nwrites == w0);
}

static void test_status(void)
{
    fresh();
    CHECK(sr(N1SV_STATUS) == N1SV_ST_LINK);
    H.link = false;
    CHECK(sr(N1SV_STATUS) == N1SV_ST_LINKDN); /* the loss, once */
    CHECK(sr(N1SV_STATUS) == 0);
    H.link = true;
    CHECK(sr(N1SV_STATUS) == N1SV_ST_LINK);
    /* a link event interrupt also latches LINKDN until the next STATUS read */
    n1pt_irq_latch(&S, N1PT_IRQ_LINK);
    CHECK(sr(N1SV_STATUS) == (N1SV_ST_LINK | N1SV_ST_LINKDN));
    CHECK(sr(N1SV_STATUS) == N1SV_ST_LINK);
    /* dead: the whole bus and window read all-ones */
    S.dead = true;
    CHECK(n1pt_window_read(&S, N1PT_BAR4_OFF, 2) == 0xffffffffu);
    CHECK(er(0, 0, 0, 0, 2) == 0xffffffffu && er(1, 0, 0, 0, 2) == 0xffffffffu);
    unsigned c0 = H.ncmds;
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_CMD, N1PT_CMD_ENABLE, 2);
    ew(1, 0, 0, 0x04, 6, 2);
    CHECK(H.ncmds == c0 && H.nwrites == 0);
}

static void test_irq(void)
{
    /* MSI data -> status bit: fn0 0-3 -> 0-3, fn1 16-31 -> 4-19, fn2 32-33 -> 20-21, nothing else */
    uint32_t seen = 0;
    for (uint32_t d = 0; d < 256; d++) {
        int b = n1pt_msi_bit(d);
        bool want = d < 4 || (d >= 16 && d < 34);
        CHECK((b >= 0) == want);
        if (b >= 0) {
            CHECK(b < 22 && !(seen & (1u << b)));
            seen |= 1u << b;
            CHECK(b == (d < 4 ? (int)d : d < 32 ? (int)(4 + d - 16) : (int)(20 + d - 32)));
        }
    }
    CHECK(seen == 0x3fffff);
    /* AIC line -> bit: 1888 + data and 1523 only */
    unsigned n = 0;
    for (uint32_t l = 0; l < 8192; l++) {
        int b = n1pt_aic_bit(l);
        if (b < 0)
            continue;
        n++;
        CHECK(l == 1523 ? b == 31 : (l >= 1888 && n1pt_msi_bit(l - 1888) == b));
    }
    CHECK(n == 23);
    /* latch / ack: level held while any bit is set */
    fresh();
    n1pt_irq_latch(&S, 0);
    CHECK(!H.spi && sr(N1SV_IRQ_STATUS) == 0 && sr(N1SV_CNT_IRQ) == 0);
    n1pt_irq_latch(&S, 1u << 0);
    n1pt_irq_latch(&S, 1u << 5);
    CHECK(H.spi && sr(N1SV_IRQ_STATUS) == 0x21 && sr(N1SV_CNT_IRQ) == 2);
    sw(N1SV_IRQ_ACK, 1u << 0);
    CHECK(H.spi && sr(N1SV_IRQ_STATUS) == 0x20);
    sw(N1SV_IRQ_ACK, 1u << 7); /* acking a clear bit changes nothing */
    CHECK(H.spi && sr(N1SV_IRQ_STATUS) == 0x20);
    sw(N1SV_IRQ_ACK, 0xffffffffu);
    CHECK(!H.spi && sr(N1SV_IRQ_STATUS) == 0);
    n1pt_irq_latch(&S, N1PT_IRQ_LINK);
    CHECK(H.spi && sr(N1SV_IRQ_STATUS) == N1PT_IRQ_LINK);
    n1pt_window_write(&S, N1PT_BAR4_OFF + N1SV_IRQ_ACK, 0xffffffffu, 0); /* byte write: dropped */
    CHECK(H.spi);
    sw(N1SV_IRQ_ACK, N1PT_IRQ_LINK);
    CHECK(!H.spi);
}

int main(void)
{
    test_widths_and_allones();
    test_header_properties();
    test_random_writes();
    test_command_and_dstate();
    test_bars();
    test_service_page();
    test_cfg_window();
    test_status();
    test_irq();
    printf("%s: win-n1pt logic, %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
