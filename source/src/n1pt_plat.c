/* SPDX-License-Identifier: MIT */
/*
 * n1pt (wireless/n1-native-20261111): hardware half of the N1 passthrough (see n1pt_plat.h). Register values
 * follow LINUX-MAP.md 1.4/1.5/2.2/2.3 (behaviour description of the Linux series 0031/0033/0034; no Linux code)
 * and m1n1's own pcie_t6050.c, which does the training, the link cycle and the park. Every write is in
 * wireless/n1-native-20261111/platform/REGISTERS.md.
 */

#include "types.h"
#include "utils.h"
#include "n1pt_plat.h"
#include "hv/win/hv_win_n1pt_logic.h"
#include "pcie_t6050_n1.h"

/* port block (apcie0 reg[16]) */
#define P_INTSTAT       0x100
#define P_INTMSK        0x104
#define P_INT_LINK_DOWN BIT(14) /* upstream pcie-apple PORT_INT_LINK_DOWN; T6050 layout [inference] */
#define P_MSICFG        0x124
#define P_MSICFG_EN     BIT(0)
#define P_MSIADDR       0x16c /* T602x layout (PORT_T602X_MSIADDR), used by the T6050 series */
#define P_MSIADDR_HI    0x170
#define P_LINKSTS       0x208
#define P_MSIMAP        0x3800
#define P_MSIMAP_EN     BIT(31)
#define P_MSI_TARGET0   64 /* apple,msi-vector-base */
#define I2AXI_CTRL      0x80
#define I2AXI_DONE      0x84

#define CFG_CMD      0x04
#define CMD_MEM      BIT(1)
#define CMD_MASTER   BIT(2)
#define CMD_INTX_DIS BIT(10)

#define ID_N1_ROM   0x1900106bu
#define ID_N1_RUN   0x1901106bu
#define ID_N1_ALPHA 0x1902106bu
#define ID_N1_BETA  0x1903106bu

static const char pfx[] = "n1pt: ";
#define N1FAIL(...)                                                                                \
    do {                                                                                           \
        printf("%s", pfx);                                                                         \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

int n1pt_plat_link_up(struct n1pt_plat *p)
{
    return (read32(p->a.port[0] + P_LINKSTS) & 5) == 1; /* UP, not BUSY */
}

u32 n1pt_plat_cfg_read(struct n1pt_plat *p, unsigned fn, unsigned off)
{
    if (fn > 2 || (off & 3) || off > 0xffc || !n1pt_plat_link_up(p))
        return 0xffffffff;
    return read32(N1PT_FN_CFG(p, fn) + off);
}

void n1pt_plat_cfg_write(struct n1pt_plat *p, unsigned fn, unsigned off, u32 v)
{
    if (fn > 2 || (off & 3) || off > 0xffc || !n1pt_plat_link_up(p))
        return;
    write32(N1PT_FN_CFG(p, fn) + off, v);
}

u32 n1pt_plat_rom_stage(struct n1pt_plat *p)
{
    if (!n1pt_plat_link_up(p))
        return 0xffffffff;
    return read32(N1PT_FN_BAR(p, 0) + 0x8000);
}

static int find_cap(u64 cfg, u32 id)
{
    u32 cap = read32(cfg + 0x34) & 0xfc;
    for (int i = 0; i < 48 && cap >= 0x40 && !(cap & 3); i++) {
        u32 hdr = read32(cfg + cap);
        if ((hdr & 0xff) == id)
            return (int)cap;
        cap = (hdr >> 8) & 0xfc;
    }
    return -1;
}

/* root port: the 64-bit prefetchable window = the 1 MiB N1 window; no I/O or 32-bit window; MEM | MASTER */
static int bridge_program(struct n1pt_plat *p)
{
    u64 rp = N1PT_RP_CFG(p), lim = p->win + N1PT_WIN_SIZE - 1;
    u32 pref = ((u32)(p->win >> 16) & 0xfff0) | (((u32)(lim >> 16) & 0xfff0) << 16);

    write32(rp + 0x1c, 0x000000f0); /* I/O base f0 > limit 0; secondary status (W1C) written 0 */
    write32(rp + 0x30, 0);
    write32(rp + 0x20, 0x0000fff0);
    write32(rp + 0x24, pref);
    write32(rp + 0x28, (u32)(p->win >> 32));
    write32(rp + 0x2c, (u32)(lim >> 32));
    write32(rp + CFG_CMD, (read32(rp + CFG_CMD) & 0xffff) | CMD_MEM | CMD_MASTER);
    if ((read32(rp + 0x24) & 0xfff0fff0) != pref || (read32(rp + 0x24) & 0xf) != 1 ||
        read32(rp + 0x28) != (u32)(p->win >> 32) || read32(rp + 0x2c) != (u32)(lim >> 32) ||
        (read32(rp + CFG_CMD) & 6) != 6)
        N1FAIL("root port window/command did not take (0x%x 0x%x 0x%x)\n", read32(rp + 0x24),
               read32(rp + 0x28), read32(rp + CFG_CMD));
    return 0;
}

static int bar_program(struct n1pt_plat *p, unsigned fn)
{
    u64 c = N1PT_FN_CFG(p, fn), bar = N1PT_FN_BAR(p, fn);
    if ((read32(c + 0x10) & 7) != 4)
        N1FAIL("fn%u BAR0 is not 64-bit memory (0x%x)\n", fn, read32(c + 0x10));
    write32(c + 0x10, (u32)bar);
    write32(c + 0x14, (u32)(bar >> 32));
    if ((read32(c + 0x10) & ~0xfu) != (u32)bar || read32(c + 0x14) != (u32)(bar >> 32))
        N1FAIL("fn%u BAR0 0x%lx did not take (0x%x)\n", fn, bar, read32(c + 0x10));
    return 0;
}

/* MSI capability: 2^log2n vectors at doorbell 0xfffff000, data base `data` */
static int msi_program(struct n1pt_plat *p, unsigned fn, u32 log2n, u32 data)
{
    u64 c = N1PT_FN_CFG(p, fn);
    int cap = find_cap(c, 0x05);
    if (cap < 0)
        N1FAIL("fn%u has no MSI capability\n", fn);
    u32 hdr = read32(c + cap), ctrl = hdr >> 16;
    if (((ctrl >> 1) & 7) < log2n)
        N1FAIL("fn%u MSI offers 2^%u vectors\n", fn, (ctrl >> 1) & 7);
    ctrl &= ~(BIT(0) | (7u << 4));
    write32(c + cap, (hdr & 0xffff) | (ctrl << 16)); /* disabled while programmed */
    write32(c + cap + 4, (u32)N1PT_DOORBELL);
    u32 doff = 8;
    if (ctrl & BIT(7)) {
        write32(c + cap + 8, 0);
        doff = 12;
    }
    write32(c + cap + doff, data);
    ctrl |= (log2n << 4) | BIT(0);
    write32(c + cap, (hdr & 0xffff) | (ctrl << 16));
    u32 rb = read32(c + cap) >> 16;
    if ((rb & 0x71) != ((log2n << 4) | 1) || read32(c + cap + 4) != (u32)N1PT_DOORBELL ||
        (read32(c + cap + doff) & 0xffff) != data)
        N1FAIL("fn%u MSI did not take (ctrl 0x%x)\n", fn, rb);
    return 0;
}

static int cmd_set(struct n1pt_plat *p, unsigned fn, u32 bits)
{
    u64 c = N1PT_FN_CFG(p, fn) + CFG_CMD;
    u32 v = (read32(c) & 0xffff & ~(CMD_MEM | CMD_MASTER)) | bits;
    write32(c, v);
    if ((read32(c) & 0xffff) != v)
        N1FAIL("fn%u COMMAND 0x%x did not take\n", fn, v);
    return 0;
}

/* port MSI: doorbell address, the 64 map entries ENABLE | (64 + i), intr2axi, MSICFG enable */
static int port_msi_program(struct n1pt_plat *p)
{
    u64 port = p->a.port[0];
    write32(port + P_MSIADDR, (u32)N1PT_DOORBELL);
    write32(port + P_MSIADDR_HI, 0);
    for (u32 i = 0; i < 64; i++)
        write32(port + P_MSIMAP + 4 * i, P_MSIMAP_EN | (P_MSI_TARGET0 + i));
    write32(p->a.i2axi + I2AXI_CTRL, 1);
    (void)read32(p->a.i2axi + I2AXI_DONE);
    write32(port + P_MSICFG, P_MSICFG_EN);
    if (read32(port + P_MSIADDR) != (u32)N1PT_DOORBELL || read32(port + P_MSIADDR_HI) ||
        read32(port + P_MSIMAP + 4 * 33) != (P_MSIMAP_EN | (P_MSI_TARGET0 + 33)) ||
        !(read32(port + P_MSICFG) & P_MSICFG_EN))
        N1FAIL("port MSI did not take (addr 0x%x cfg 0x%x)\n", read32(port + P_MSIADDR),
               read32(port + P_MSICFG));
    return 0;
}

static int routes_program(struct n1pt_plat *p)
{
    (void)p;
    for (u32 fn = 0; fn < 3; fn++)
        if (pcie_t6050_n1_route(0x100 + fn, 1 + fn))
            N1FAIL("RID2SID 0x%x -> %u did not take\n", 0x100 + fn, 1 + fn);
    return 0;
}

static int fn_program(struct n1pt_plat *p, unsigned fn, u16 cmd)
{
    static const u32 log2n[3] = {2, 4, 1}, data[3] = {N1PT_MSI_DATA_FN0, N1PT_MSI_DATA_FN1, N1PT_MSI_DATA_FN2};
    if (bar_program(p, fn) || msi_program(p, fn, log2n[fn], data[fn]))
        return -1;
    u64 c = N1PT_FN_CFG(p, fn) + CFG_CMD;
    write32(c, (read32(c) & 0xffff) | CMD_INTX_DIS); /* INTx never routed: MSI only */
    return cmd_set(p, fn, cmd & (CMD_MEM | CMD_MASTER));
}

void n1pt_plat_link_irq_enable(struct n1pt_plat *p, int on)
{
    write32(p->a.port[0] + P_INTSTAT, 0xffffffff);
    write32(p->a.port[0] + P_INTMSK, on ? ~P_INT_LINK_DOWN : 0xffffffff);
}

u32 n1pt_plat_link_irq_ack(struct n1pt_plat *p)
{
    u32 v = read32(p->a.port[0] + P_INTSTAT);
    if (v)
        write32(p->a.port[0] + P_INTSTAT, v);
    return v;
}

int n1pt_plat_prepare(struct n1pt_plat *p, u64 pool, u64 iova, u64 size)
{
    if ((p->win & (N1PT_WIN_SIZE - 1)) || !p->a.port[0] || !p->a.ecam || !p->a.dart)
        N1FAIL("bad layout\n");
    if (!n1pt_plat_link_up(p))
        N1FAIL("no link\n");
    u32 id = read32(N1PT_FN_CFG(p, 0));
    if (id != ID_N1_ROM)
        N1FAIL("endpoint is 0x%08x, not the N1 ROM\n", id);
    if (bridge_program(p) || bar_program(p, 0))
        return -1;
    u64 c0 = N1PT_FN_CFG(p, 0) + CFG_CMD;
    write32(c0, (read32(c0) & 0xffff) | CMD_INTX_DIS);
    if (cmd_set(p, 0, CMD_MEM))
        return -1;
    u32 stage = n1pt_plat_rom_stage(p);
    if (stage != 0)
        N1FAIL("fn0 BAR0+0x8000 = 0x%x: the ROM is not stopped\n", stage);
    if (msi_program(p, 0, 2, N1PT_MSI_DATA_FN0) || port_msi_program(p) || routes_program(p))
        return -1;
    if (n1pt_dart_build(&p->dart, iova, pool, size))
        return -1;
    if (n1pt_dart_attach(p->a.dart, &p->dart, 0xe, &p->four_level)) {
        n1pt_dart_free(&p->dart);
        return -1;
    }
    n1pt_plat_link_irq_enable(p, 1);
    printf("%sready: fn0 BAR0 0x%lx, MSI 4 at data 0, RID 0x100-0x102 -> SID 1-3, DART %u-level 0x%lx+0x%lx -> "
           "IOVA 0x%lx\n", pfx, N1PT_FN_BAR(p, 0), p->four_level ? 4 : 3, pool, size, iova);
    return 0;
}

int n1pt_plat_restore(struct n1pt_plat *p, const u16 cmd[3])
{
    if (!n1pt_plat_link_up(p))
        N1FAIL("restore: no link\n");
    u32 id = read32(N1PT_FN_CFG(p, 0));
    if (id != ID_N1_ROM && id != ID_N1_RUN)
        N1FAIL("restore: endpoint 0x%08x\n", id);
    if (bridge_program(p) || fn_program(p, 0, cmd[0]))
        return -1;
    if (p->fns_enabled && (fn_program(p, 1, cmd[1]) || fn_program(p, 2, cmd[2])))
        return -1;
    return 0; /* RID2SID and the port MSI were restored before training (cycle_pre_train) */
}

/* Linux reset_root_port's restore point (capture cap2): RID2SID, MSI address, MSI map, intr2axi, MSICFG, before the
 * link trains. A failure is remembered and fails the command after the cycle. */
static void cycle_pre_train(void *ctx)
{
    struct n1pt_plat *p = ctx;
    if (routes_program(p) || port_msi_program(p))
        p->pre_train_failed = 1;
}

static int cmd_cycle(struct n1pt_plat *p, struct n1pt *s)
{
    u16 cmd[3] = {0, 0, 0};

    if (!n1pt_plat_link_up(p))
        return N1PT_E_NOLINK;
    u32 pmcsr = n1pt_plat_cfg_read(p, 0, 0x44) & 0xffff;
    if ((pmcsr & 3) != 3) { /* Linux: D3hot | PME_En before PME_Turn_Off, or the port never quiesces (run w24c) */
        printf("%sPORT_CYCLE refused: fn0 PMCSR 0x%x is not D3hot (write D3hot | PME_En through the config window "
               "first); nothing touched\n", pfx, pmcsr);
        return N1PT_E_DSTATE;
    }
    for (unsigned fn = 0; fn < 3; fn++)
        if (!fn || p->fns_enabled)
            cmd[fn] = read32(N1PT_FN_CFG(p, fn) + CFG_CMD) & (CMD_MEM | CMD_MASTER);
    p->cb->map_bars(p->cb->ctx, 0, 0); /* no guest access to a BAR while the link is down */
    p->cb->link_irq(p->cb->ctx, 0);
    n1pt_plat_link_irq_enable(p, 0);
    /* Linux centauri_port_cycle: D3hot | PME_En (the driver, through the config window), then 10 ms, then the reset */
    printf("%sPORT_CYCLE: fn0 PM_CTRL 0x%x COMMAND 0x%x\n", pfx, n1pt_plat_cfg_read(p, 0, 0x44) & 0xffff,
           n1pt_plat_cfg_read(p, 0, CFG_CMD) & 0xffff);
    mdelay(11); /* capture cap2: 11 ms from the driver's PMCSR write to reset_root_port */
    pcie_t6050_storage_guard(0);
    p->pre_train_failed = 0;
    int rc = pcie_t6050_n1_cycle_cb(cycle_pre_train, p);
    if (!rc && p->pre_train_failed)
        rc = -1;
    if (!rc)
        rc = n1pt_plat_restore(p, cmd);
    if (pcie_t6050_storage_guard(1)) {
        s->guard = true;
        printf("%sSTORAGE GUARD tripped by PORT_CYCLE: N1 commands refused, BARs stay unmapped\n", pfx);
        return N1PT_E_IO;
    }
    if (rc)
        return n1pt_plat_link_up(p) ? N1PT_E_IO : N1PT_E_NOLINK;
    p->cb->map_bars(p->cb->ctx, 1, p->fns_enabled);
    n1pt_plat_link_irq_enable(p, 1);
    p->cb->link_irq(p->cb->ctx, 1);
    return 0;
}

static int cmd_enable(struct n1pt_plat *p, struct n1pt *s)
{
    if (p->fns_enabled) {
        s->fns_enabled = true;
        return 0;
    }
    if (!n1pt_plat_link_up(p))
        return N1PT_E_NOLINK;
    u32 stage = n1pt_plat_rom_stage(p);
    if (stage != 1) {
        printf("%sENABLE_FUNCTIONS: fn0 stage %u, not 1\n", pfx, stage);
        return N1PT_E_BUSY;
    }
    u32 id1 = read32(N1PT_FN_CFG(p, 1)), id2 = read32(N1PT_FN_CFG(p, 2));
    if (id1 != ID_N1_ALPHA || id2 != ID_N1_BETA) {
        printf("%sENABLE_FUNCTIONS: fn1 0x%08x fn2 0x%08x\n", pfx, id1, id2);
        return N1PT_E_NODEV;
    }
    if (fn_program(p, 1, CMD_MEM | CMD_MASTER) || fn_program(p, 2, CMD_MEM | CMD_MASTER) || routes_program(p))
        return N1PT_E_IO;
    p->fns_enabled = 1;
    s->fns_enabled = true;
    if (p->cb->map_bars(p->cb->ctx, 1, 1))
        return N1PT_E_IO;
    printf("%sfunctions 1/2 enabled: BAR2 0x%lx (fn1 MSI 16 at data 16, fn2 MSI 2 at data 32)\n", pfx,
           N1PT_FN_BAR(p, 1));
    return 0;
}

static int cmd_park(struct n1pt_plat *p, struct n1pt *s)
{
    p->cb->map_bars(p->cb->ctx, 0, 0);
    p->cb->link_irq(p->cb->ctx, 0);
    n1pt_plat_link_irq_enable(p, 0);
    pcie_t6050_storage_guard(0);
    if (n1pt_plat_link_up(p)) {
        for (unsigned fn = 0; fn < 3; fn++) /* DMA off first (MASTER), then the DART, while the link is up */
            if (!fn || p->fns_enabled)
                write32(N1PT_FN_CFG(p, fn) + CFG_CMD, read32(N1PT_FN_CFG(p, fn) + CFG_CMD) & 0xffff & ~CMD_MASTER);
        if (p->dart.root4)
            n1pt_dart_block(p->a.dart, 0xe);
    }
    int rc = pcie_t6050_n1_park_any(); /* live, broken or down link: PERST# and LTSSM off in every case */
    s->parked = true;
    if (pcie_t6050_storage_guard(1)) {
        s->guard = true;
        printf("%sSTORAGE GUARD tripped by PARK\n", pfx);
        return N1PT_E_IO;
    }
    return rc ? N1PT_E_IO : 0;
}

int n1pt_plat_command(struct n1pt_plat *p, struct n1pt *s, unsigned cmd)
{
    switch (cmd) {
    case N1PT_CMD_PORT_CYCLE: return cmd_cycle(p, s);
    case N1PT_CMD_ENABLE: return cmd_enable(p, s);
    case N1PT_CMD_PARK: return cmd_park(p, s);
    default: return N1PT_E_INVAL;
    }
}
