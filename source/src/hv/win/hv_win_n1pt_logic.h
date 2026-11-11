/* SPDX-License-Identifier: MIT */
/*
 * n1pt (wireless/n1-native-20261111): pure logic of the N1 Wi-Fi/BT PCIe passthrough, shared with the host
 * test (tests/win-n1pt). No hardware access here: real config space, the port commands, the clock and the
 * guest SPI are reached through struct n1pt_ops. Contract: wireless/n1-native-20261111/INTERFACE.md s1, s2.
 *
 *  - ECAM window (2 MiB, guest IPA chosen by the launcher, trapped): bus 0 dev 0 fn 0 = emulated root port
 *    (106b:100c, type 1, buses 0/1/1, prefetchable window = the 1 MiB N1 window, no ASPM); bus 1 dev 0 fn 0 =
 *    emulated N1 header, always 106b:1900, class ff00, single function, INTA, PM + Express capabilities, no
 *    MSI capability. Everything else reads all-ones; writes there are dropped. Only COMMAND MEM|MASTER of the
 *    endpoint reach the real function 0; D-state (PMCSR) writes are stored and never forwarded.
 *  - Window (1 MiB): BAR0 at +0x10000 (64 KiB, real fn0 BAR0, stage-2 device map, not trapped while mapped),
 *    BAR2 at +0x20000 (128 KiB: real fn1 BAR0 then fn2 BAR0; all-ones until ENABLE_FUNCTIONS maps it), BAR4 at
 *    +0x40000 (16 KiB service page, trapped). The rest of the window reads all-ones.
 *  - Service page registers (INTERFACE.md s2) and the IRQ latch (one level SPI held while any status bit is set).
 */
#ifndef HV_WIN_N1PT_LOGIC_H
#define HV_WIN_N1PT_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define N1PT_ECAM_SIZE ((uint64_t)0x200000)
#define N1PT_WIN_SIZE ((uint64_t)0x100000)
#define N1PT_BAR0_OFF ((uint64_t)0x10000)
#define N1PT_BAR0_SIZE ((uint64_t)0x10000)
#define N1PT_BAR2_OFF ((uint64_t)0x20000)
#define N1PT_BAR2_SIZE ((uint64_t)0x20000)
#define N1PT_BAR4_OFF ((uint64_t)0x40000)
#define N1PT_BAR4_SIZE ((uint64_t)0x4000)
#define N1PT_POOL_SIZE  ((uint64_t)128 << 20)
#define N1PT_DOORBELL ((uint64_t)0xfffff000)
#define N1PT_IOVA_LIMIT ((uint64_t)1 << 40)

/* service page (BAR4) */
#define N1SV_MAGIC       0x000
#define N1SV_VERSION     0x004
#define N1SV_STATUS      0x008
#define N1SV_POOL_IPA_LO 0x010
#define N1SV_POOL_IPA_HI 0x014
#define N1SV_IOVA_LO     0x018
#define N1SV_IOVA_HI     0x01c
#define N1SV_POOL_SIZE   0x020
#define N1SV_CMD         0x030
#define N1SV_CMD_ARG     0x034
#define N1SV_CMD_RESULT  0x038
#define N1SV_CFG_ADDR    0x040
#define N1SV_CFG_DATA    0x044
#define N1SV_IRQ_STATUS  0x050
#define N1SV_IRQ_ACK     0x054
#define N1SV_CNT_IRQ     0x060
#define N1SV_CNT_DROPPED 0x064
#define N1SV_CNT_CMDS    0x068
#define N1SV_CNT_LAST_US 0x06c

#define N1SV_MAGIC_VALUE 0x5653314eu /* "N1SV" */
#define N1SV_VERSION_1   1u

#define N1SV_ST_LINK    (1u << 0)
#define N1SV_ST_FNS     (1u << 1)
#define N1SV_ST_LINKDN  (1u << 2) /* link-down latched since the last STATUS read */
#define N1SV_ST_PARKED  (1u << 3)
#define N1SV_ST_GUARD   (1u << 4) /* storage guard tripped: every N1 command refused from then on */

#define N1PT_CMD_PORT_CYCLE 1u
#define N1PT_CMD_ENABLE     2u
#define N1PT_CMD_PARK       3u

/* CMD_RESULT: 0 or a negative errno-style code */
#define N1PT_E_PERM   (-1)
#define N1PT_E_IO     (-5)
#define N1PT_E_BUSY   (-16)
#define N1PT_E_NODEV  (-19)
#define N1PT_E_INVAL  (-22)
#define N1PT_E_NOLINK (-67)

/* IRQ_STATUS bits */
#define N1PT_IRQ_LINK (1u << 31)

/* MSI data values EL2 programs: fn0 0..3, fn1 16..31, fn2 32..33; AIC line = N1PT_MSI_AIC_BASE + data
 * (MSIMAP[data] = ENABLE | (64 + data), AIC msi-vector-offset 1824 + 64 = 1888; LINUX-MAP 1.4) */
#define N1PT_MSI_AIC_BASE 1888u
#define N1PT_MSI_DATA_FN0 0u
#define N1PT_MSI_DATA_FN1 16u
#define N1PT_MSI_DATA_FN2 32u
#define N1PT_LINK_AIC     1523u /* apcie0 port0 interrupt (LINUX-MAP 1.1) */

/* IRQ_STATUS bit for an MSI data value, -1 if EL2 never programs it */
static inline int n1pt_msi_bit(uint32_t data)
{
    if (data < 4)
        return (int)data;
    if (data >= 16 && data < 32)
        return 4 + (int)(data - 16);
    if (data >= 32 && data < 34)
        return 20 + (int)(data - 32);
    return -1;
}

/* IRQ_STATUS bit for an AIC line, -1 if not an N1 line */
static inline int n1pt_aic_bit(uint32_t line)
{
    if (line == N1PT_LINK_AIC)
        return 31;
    if (line < N1PT_MSI_AIC_BASE || line >= N1PT_MSI_AIC_BASE + 64)
        return -1;
    return n1pt_msi_bit(line - N1PT_MSI_AIC_BASE);
}

/* config window filter: may the guest write this dword of a real function? (INTERFACE.md s2 CFG_DATA) */
static inline bool n1pt_cfg_write_allowed(uint32_t off)
{
    return off == 0xf88 || off == 0xf8c || off == 0xf90 || off == 0x44 || off == 0x04;
}

struct n1pt;

struct n1pt_ops {
    void *ctx;
    uint32_t (*cfg_read)(void *ctx, unsigned fn, unsigned off); /* real config dword of bus 1 fn 0..2 */
    void (*cfg_write)(void *ctx, unsigned fn, unsigned off, uint32_t v);
    bool (*link_up)(void *ctx);
    int (*command)(struct n1pt *s, unsigned cmd, uint32_t arg); /* PORT_CYCLE / ENABLE / PARK on the hardware */
    uint64_t (*now_us)(void *ctx);
    void (*spi)(void *ctx, bool level); /* the guest's INTA SPI line level */
};

struct n1pt_hdr {
    uint16_t cmd, bridge_ctl, pmcsr, devctl, lnkctl;
    uint8_t int_line;
};

struct n1pt {
    const struct n1pt_ops *ops;
    uint64_t win, pool_ipa, pool_iova, pool_size;
    struct n1pt_hdr rp, ep;
    uint32_t bar[6];   /* the endpoint's emulated BAR registers as last written */
    bool fns_enabled, parked, guard, dead; /* dead: N1 refused at start, everything reads all-ones */
    bool link_seen;
    uint32_t latched;  /* N1SV_ST_LINKDN */
    uint32_t cmd_arg, cfg_addr;
    int32_t cmd_result;
    uint32_t irq_status;
    uint32_t n_irq, n_dropped, n_cmds, last_us, n_bar_moves;
};

/* ---- emulated headers ------------------------------------------------------------------------- */
#define N1PT_RP_ID 0x100c106bu
#define N1PT_EP_ID 0x1900106bu
#define N1PT_CMD_MASK 0x0547u /* IO, MEM, MASTER, PERR, SERR, INTx disable: storage bits of COMMAND */
#define N1PT_CMD_FWD  0x0006u /* MEM | MASTER: the only endpoint COMMAND bits forwarded to the real fn0 */

static inline void n1pt_init(struct n1pt *s, const struct n1pt_ops *ops, uint64_t win, uint64_t pool_ipa,
                             uint64_t pool_iova, uint64_t pool_size)
{
    *s = (struct n1pt){0};
    s->ops = ops;
    s->win = win;
    s->pool_ipa = pool_ipa;
    s->pool_iova = pool_iova;
    s->pool_size = pool_size;
    s->rp.cmd = 0x0006;      /* firmware leaves the bridge decoding and forwarding */
    s->ep.cmd = 0x0002;      /* EL2 enabled MEM on the real fn0 to check the ROM; MASTER is the driver's */
    uint64_t b[3] = {win + N1PT_BAR0_OFF, win + N1PT_BAR2_OFF, win + N1PT_BAR4_OFF};
    for (int i = 0; i < 3; i++) {
        s->bar[2 * i] = (uint32_t)b[i] | 0xc;
        s->bar[2 * i + 1] = (uint32_t)(b[i] >> 32);
    }
}

/* the real functions may be touched: not refused at start, not parked, storage guard not tripped, link up */
static inline bool n1pt_live(const struct n1pt *s)
{
    return !s->dead && !s->parked && !s->guard && s->ops->link_up(s->ops->ctx);
}

static inline uint32_t n1pt_rp_read(const struct n1pt *s, unsigned reg)
{
    uint64_t lim = s->win + N1PT_WIN_SIZE - 1;
    switch (reg) {
    case 0x00: return N1PT_RP_ID;
    case 0x04: return s->rp.cmd | (0x0010u << 16); /* status: capabilities list */
    case 0x08: return 0x06040000u;                  /* PCI-to-PCI bridge */
    case 0x0c: return 0x00010000u;                  /* header type 1, single function */
    case 0x18: return 0x00010100u;                  /* primary 0, secondary 1, subordinate 1 */
    case 0x1c: return 0x000000f0u;                  /* no I/O window */
    case 0x20: return 0x0000fff0u;                  /* no non-prefetchable window */
    case 0x24: return (((uint32_t)(s->win >> 16) & 0xfff0u) | 1u) | ((((uint32_t)(lim >> 16) & 0xfff0u) | 1u) << 16);
    case 0x28: return (uint32_t)(s->win >> 32);
    case 0x2c: return (uint32_t)(lim >> 32);
    case 0x34: return 0x40u;
    case 0x3c: return s->rp.int_line | ((uint32_t)s->rp.bridge_ctl << 16); /* interrupt pin 0 */
    case 0x40: return 0x00035001u;                  /* PM v3, next 0x50, no PME, no D1/D2 */
    case 0x44: return (s->rp.pmcsr & 0x0103u) | 0x8u; /* NoSoftReset */
    case 0x50: return 0x00420010u;                  /* Express v2, root port, last capability */
    case 0x54: return 0x00008000u;                  /* DevCap: role-based error reporting */
    case 0x58: return s->rp.devctl;
    case 0x5c: return 0x00000013u;                  /* LnkCap: port 0, 8 GT/s, x1, no ASPM */
    case 0x60: return s->rp.lnkctl | (0x0013u << 16);
    default: return 0; /* BARs, ROM, the rest of the capabilities, extended space: 0 */
    }
}

static inline uint32_t n1pt_ep_read(const struct n1pt *s, unsigned reg)
{
    static const uint32_t size[3] = {(uint32_t)N1PT_BAR0_SIZE, (uint32_t)N1PT_BAR2_SIZE, (uint32_t)N1PT_BAR4_SIZE};
    if (reg >= 0x10 && reg < 0x28) {
        unsigned i = (reg - 0x10) / 4;
        if (i & 1)
            return s->bar[i]; /* upper half: every bit decodes (the BAR is below 4 GiB in size) */
        return (s->bar[i] & ~(size[i / 2] - 1)) | 0xc; /* 64-bit, prefetchable memory */
    }
    switch (reg) {
    case 0x00: return N1PT_EP_ID;
    case 0x04: return s->ep.cmd | (0x0010u << 16);
    case 0x08: return 0xff000001u;                  /* class ff00, revision 1 */
    case 0x0c: return 0x00000000u;                  /* header type 0, single function */
    case 0x2c: return N1PT_EP_ID;                   /* subsystem 106b:1900 */
    case 0x34: return 0x40u;
    case 0x3c: return s->ep.int_line | (1u << 8);   /* INTA */
    case 0x40: return 0x00037001u;                  /* PM v3, next 0x70 (no MSI capability) */
    case 0x44: return (s->ep.pmcsr & 0x0103u) | 0x8u;
    case 0x70: return 0x00020010u;                  /* Express v2, endpoint, last capability */
    case 0x74: return 0x00008000u;                  /* DevCap: RBE, MPS 128, no FLR */
    case 0x78: return s->ep.devctl;
    case 0x7c: return 0x00000013u;                  /* LnkCap: 8 GT/s x1, no ASPM */
    case 0x80: return s->ep.lnkctl | (0x0013u << 16);
    default: return 0;
    }
}

/* full-dword write after byte merge; `be` = byte enables of the guest access */
static inline void n1pt_rp_write(struct n1pt *s, unsigned reg, uint32_t v, uint32_t be)
{
    switch (reg) {
    case 0x04: if (be & 0xffff) s->rp.cmd = v & N1PT_CMD_MASK; break;
    case 0x3c:
        if (be & 0xff) s->rp.int_line = (uint8_t)v;
        if (be & 0xffff0000u) s->rp.bridge_ctl = (uint16_t)(v >> 16); /* incl. secondary reset: never forwarded */
        break;
    case 0x44: if (be & 0xffff) s->rp.pmcsr = (uint16_t)(v & 0x0103u); break;  /* D-state: stored only */
    case 0x58: if (be & 0xffff) s->rp.devctl = (uint16_t)(v & 0x7fffu); break;
    case 0x60: if (be & 0xffff) s->rp.lnkctl = (uint16_t)(v & ~0x0023u); break; /* no ASPM, no retrain */
    default: break; /* read-only emulation: bus numbers, windows, BARs, IDs */
    }
}

static inline void n1pt_ep_write(struct n1pt *s, unsigned reg, uint32_t v, uint32_t be)
{
    if (reg >= 0x10 && reg < 0x28) {
        unsigned i = (reg - 0x10) / 4;
        uint32_t old = s->bar[i];
        s->bar[i] = v;
        /* a non-probe write that moves a BAR: the real BARs and stage-2 maps never move (_DSM 5 = 0) */
        uint64_t want = s->win + (i < 2 ? N1PT_BAR0_OFF : i < 4 ? N1PT_BAR2_OFF : N1PT_BAR4_OFF);
        uint32_t w = (i & 1) ? (uint32_t)(want >> 32) : ((uint32_t)want | 0xc);
        if (v != 0xffffffffu && (v & ~0xfu) != (w & ~0xfu) && old != v)
            s->n_bar_moves++;
        return;
    }
    switch (reg) {
    case 0x04:
        if (!(be & 0xffff))
            break;
        s->ep.cmd = v & N1PT_CMD_MASK;
        if (n1pt_live(s)) {
            uint32_t real = s->ops->cfg_read(s->ops->ctx, 0, 0x04) & 0xffffu;
            s->ops->cfg_write(s->ops->ctx, 0, 0x04, (real & ~N1PT_CMD_FWD) | (v & N1PT_CMD_FWD));
        }
        break;
    case 0x3c: if (be & 0xff) s->ep.int_line = (uint8_t)v; break;
    case 0x44: if (be & 0xffff) s->ep.pmcsr = (uint16_t)(v & 0x0103u); break; /* swallowed */
    case 0x78: if (be & 0xffff) s->ep.devctl = (uint16_t)(v & 0x7fffu); break; /* no FLR */
    case 0x80: if (be & 0xffff) s->ep.lnkctl = (uint16_t)(v & ~0x0023u); break;
    default: break;
    }
}

/* which emulated function an ECAM offset names: 0 root port, 1 endpoint, -1 none (all-ones) */
static inline int n1pt_ecam_target(uint64_t off)
{
    if (off >= N1PT_ECAM_SIZE)
        return -1;
    unsigned bus = (unsigned)(off >> 20), devfn = (unsigned)(off >> 12) & 0xff;
    if (devfn)
        return -1;
    return bus == 0 ? 0 : 1;
}

static inline uint32_t n1pt_width_mask(int width)
{
    return width >= 2 ? 0xffffffffu : width == 1 ? 0xffffu : 0xffu;
}

/* width: log2 of the access size (0, 1, 2) */
static inline uint32_t n1pt_ecam_read(const struct n1pt *s, uint64_t off, int width)
{
    int t = s->dead ? -1 : n1pt_ecam_target(off);
    unsigned reg = (unsigned)off & 0xfff, sh = (reg & 3) * 8;
    uint32_t m = n1pt_width_mask(width);
    if (width < 0 || width > 2 || (reg & ((1u << width) - 1)))
        return m; /* unaligned or 64-bit: not a config access */
    if (t < 0)
        return m;
    uint32_t d = t == 0 ? n1pt_rp_read(s, reg & ~3u) : n1pt_ep_read(s, reg & ~3u);
    return (d >> sh) & m;
}

static inline void n1pt_ecam_write(struct n1pt *s, uint64_t off, uint32_t val, int width)
{
    int t = s->dead ? -1 : n1pt_ecam_target(off);
    unsigned reg = (unsigned)off & 0xfff, sh = (reg & 3) * 8;
    if (t < 0 || width < 0 || width > 2 || (reg & ((1u << width) - 1)))
        return;
    uint32_t be = n1pt_width_mask(width) << sh;
    unsigned dw = reg & ~3u;
    /* merge into the register's current value; read-only bits are ignored by the write handlers */
    uint32_t cur;
    if (t == 0) {
        cur = n1pt_rp_read(s, dw);
        if (dw == 0x3c)
            cur = s->rp.int_line | ((uint32_t)s->rp.bridge_ctl << 16);
    } else {
        cur = (dw >= 0x10 && dw < 0x28) ? s->bar[(dw - 0x10) / 4] : n1pt_ep_read(s, dw);
        if (dw == 0x3c)
            cur = s->ep.int_line;
    }
    uint32_t v = (cur & ~be) | ((val << sh) & be);
    if (t == 0)
        n1pt_rp_write(s, dw, v, be);
    else
        n1pt_ep_write(s, dw, v, be);
}

/* ---- IRQ latch ------------------------------------------------------------------------------- */
static inline void n1pt_irq_latch(struct n1pt *s, uint32_t bits)
{
    if (!bits)
        return;
    if (bits & N1PT_IRQ_LINK)
        s->latched |= N1SV_ST_LINKDN;
    s->irq_status |= bits;
    s->n_irq++;
    s->ops->spi(s->ops->ctx, true);
}

static inline void n1pt_irq_ack(struct n1pt *s, uint32_t bits)
{
    s->irq_status &= ~bits;
    s->ops->spi(s->ops->ctx, s->irq_status != 0);
}

/* ---- service page ---------------------------------------------------------------------------- */
static inline uint32_t n1pt_status(struct n1pt *s)
{
    bool link = n1pt_live(s);
    if (link)
        s->link_seen = true;
    else if (s->link_seen && !s->parked)
        s->latched |= N1SV_ST_LINKDN;
    uint32_t v = (link ? N1SV_ST_LINK : 0) | (s->fns_enabled ? N1SV_ST_FNS : 0) | s->latched |
                 (s->parked ? N1SV_ST_PARKED : 0) | (s->guard ? N1SV_ST_GUARD : 0);
    s->latched = 0;
    if (!link && s->link_seen)
        s->link_seen = false; /* report one transition per loss */
    return v;
}

static inline uint32_t n1pt_cfg_data_read(struct n1pt *s)
{
    unsigned fn = (s->cfg_addr >> 12) & 7, off = s->cfg_addr & 0xffc;
    if (fn > 2 || !n1pt_live(s))
        return 0xffffffffu;
    return s->ops->cfg_read(s->ops->ctx, fn, off);
}

static inline void n1pt_cfg_data_write(struct n1pt *s, uint32_t v)
{
    unsigned fn = (s->cfg_addr >> 12) & 7, off = s->cfg_addr & 0xffc;
    if (fn > 2 || !n1pt_cfg_write_allowed(off) || !n1pt_live(s)) {
        s->n_dropped++;
        return;
    }
    if (off == 0x04) {
        uint32_t real = s->ops->cfg_read(s->ops->ctx, fn, 0x04) & 0xffffu;
        v = (real & ~N1PT_CMD_FWD) | (v & N1PT_CMD_FWD);
    }
    s->ops->cfg_write(s->ops->ctx, fn, off, v);
}

static inline void n1pt_run_command(struct n1pt *s, uint32_t cmd)
{
    uint64_t t0 = s->ops->now_us(s->ops->ctx);
    int r;
    s->n_cmds++;
    if (cmd < N1PT_CMD_PORT_CYCLE || cmd > N1PT_CMD_PARK)
        r = N1PT_E_INVAL;
    else if (s->guard || s->dead)
        r = N1PT_E_PERM;
    else if (s->parked)
        r = cmd == N1PT_CMD_PARK ? 0 : N1PT_E_PERM;
    else
        r = s->ops->command(s, cmd, s->cmd_arg);
    s->cmd_result = r;
    s->last_us = (uint32_t)(s->ops->now_us(s->ops->ctx) - t0);
}

static inline uint32_t n1pt_svc_read(struct n1pt *s, uint32_t off)
{
    switch (off & ~3u) {
    case N1SV_MAGIC: return N1SV_MAGIC_VALUE;
    case N1SV_VERSION: return N1SV_VERSION_1;
    case N1SV_STATUS: return n1pt_status(s);
    case N1SV_POOL_IPA_LO: return (uint32_t)s->pool_ipa;
    case N1SV_POOL_IPA_HI: return (uint32_t)(s->pool_ipa >> 32);
    case N1SV_IOVA_LO: return (uint32_t)s->pool_iova;
    case N1SV_IOVA_HI: return (uint32_t)(s->pool_iova >> 32);
    case N1SV_POOL_SIZE: return (uint32_t)s->pool_size;
    case N1SV_CMD_ARG: return s->cmd_arg;
    case N1SV_CMD_RESULT: return (uint32_t)s->cmd_result;
    case N1SV_CFG_ADDR: return s->cfg_addr;
    case N1SV_CFG_DATA: return n1pt_cfg_data_read(s);
    case N1SV_IRQ_STATUS: return s->irq_status;
    case N1SV_CNT_IRQ: return s->n_irq;
    case N1SV_CNT_DROPPED: return s->n_dropped;
    case N1SV_CNT_CMDS: return s->n_cmds;
    case N1SV_CNT_LAST_US: return s->last_us;
    default: return 0;
    }
}

static inline void n1pt_svc_write(struct n1pt *s, uint32_t off, uint32_t v)
{
    switch (off & ~3u) {
    case N1SV_CMD: n1pt_run_command(s, v); break;
    case N1SV_CMD_ARG: s->cmd_arg = v; break;
    case N1SV_CFG_ADDR: s->cfg_addr = v & 0x7ffcu; break;
    case N1SV_CFG_DATA: n1pt_cfg_data_write(s, v); break;
    case N1SV_IRQ_ACK: n1pt_irq_ack(s, v); break;
    default: break; /* read-only */
    }
}

/* the trapped part of the 1 MiB window: the service page; anything else (BAR2 before ENABLE_FUNCTIONS,
 * BAR0/BAR2 while unmapped for a cycle or after PARK, the unassigned rest) reads all-ones, writes dropped */
static inline uint32_t n1pt_window_read(struct n1pt *s, uint64_t off, int width)
{
    if (!s->dead && off >= N1PT_BAR4_OFF && off < N1PT_BAR4_OFF + N1PT_BAR4_SIZE) {
        uint32_t v = n1pt_svc_read(s, (uint32_t)(off - N1PT_BAR4_OFF));
        return width >= 2 ? v : (v >> ((off & 3) * 8)) & n1pt_width_mask(width);
    }
    return n1pt_width_mask(width);
}

static inline void n1pt_window_write(struct n1pt *s, uint64_t off, uint32_t v, int width)
{
    if (!s->dead && width == 2 && !(off & 3) && off >= N1PT_BAR4_OFF && off < N1PT_BAR4_OFF + N1PT_BAR4_SIZE)
        n1pt_svc_write(s, (uint32_t)(off - N1PT_BAR4_OFF), v);
}

#endif
