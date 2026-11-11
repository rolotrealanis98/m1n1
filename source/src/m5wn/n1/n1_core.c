/* SPDX-License-Identifier: MIT */
/* N1 bring-up core. Clean-room implementation of the behaviour in LINUX-MAP.md; section numbers
 * (LM x.y) and WRITES.md row ids (W-xx) are cited at each hardware access. No Linux code. */
#include "n1_core.h"

/* ---- small helpers (freestanding) ---- */
static void mzero(void *p, size_t n) { volatile n1_u8 *b = p; while (n--) *b++ = 0; }
static void mcopy(void *d, const void *s, size_t n) { n1_u8 *a = d; const n1_u8 *b = s; while (n--) *a++ = *b++; }
static int mequal(const void *x, const void *y, size_t n)
{
    const n1_u8 *a = x, *b = y;
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}
static n1_u16 le16(const n1_u8 *p) { return (n1_u16)(p[0] | p[1] << 8); }
static n1_u32 le32(const n1_u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (n1_u32)p[3] << 24; }
static void put16(n1_u8 *p, n1_u32 v) { p[0] = (n1_u8)v; p[1] = (n1_u8)(v >> 8); }
static void put32(n1_u8 *p, n1_u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(n1_u8 *p, n1_u64 v) { put32(p, (n1_u32)v); put32(p + 4, (n1_u32)(v >> 32)); }
static n1_u16 vrd16(const n1_u8 *p) { const volatile n1_u8 *q = p; return (n1_u16)(q[0] | q[1] << 8); }
static void vwr16(n1_u8 *p, n1_u16 v) { volatile n1_u8 *q = p; q[0] = (n1_u8)v; q[1] = (n1_u8)(v >> 8); }
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((n1_u64)(a) - 1))

#define FAIL(code, detail) do { n->err = (code); n->err_detail = (n1_u32)(detail); n->stage = N1_ST_FAILED; \
    n->h->log(n->h->ctx, "n1: FAIL %d detail 0x%x at %s:%d\n", (code), (n1_u32)(detail), __func__, __LINE__); \
    return -1; } while (0)

static n1_u64 now_us(struct n1 *n) { return n->h->ticks_us(n->h->ctx); }
static void delay_us(struct n1 *n, n1_u32 us) { n->h->udelay(n->h->ctx, us); }

/* Poll a 32-bit register until pred holds; returns the last value via *out. */
static int poll32(struct n1 *n, n1_u64 pa, n1_u32 mask, n1_u32 want, int equal, n1_u32 timeout_ms, n1_u32 step_us,
                  n1_u32 *out)
{
    n1_u64 end = now_us(n) + (n1_u64)timeout_ms * 1000;
    n1_u32 v;
    for (;;) {
        v = N1_RD(n->h, pa);
        if (((v & mask) == want) == !!equal) { if (out) *out = v; return 0; }
        if (now_us(n) >= end) { if (out) *out = v; return -1; }
        delay_us(n, step_us);
    }
}

/* ---- PCI config through ECAM (bus 0 = root port 00:00.0, bus 1 = the N1; LM 1.1) ---- */
static n1_u64 cfg(struct n1 *n, n1_u32 bus, n1_u32 fn, n1_u32 off)
{
    return n->a->ecam + ((n1_u64)bus << 20) + ((n1_u64)fn << 12) + off;
}
static n1_u32 cfg_rd(struct n1 *n, n1_u32 bus, n1_u32 fn, n1_u32 off) { return N1_RD(n->h, cfg(n, bus, fn, off)); }
static void cfg_wr(struct n1 *n, n1_u32 bus, n1_u32 fn, n1_u32 off, n1_u32 v) { N1_WR(n->h, cfg(n, bus, fn, off), v); }

static int find_cap(struct n1 *n, n1_u32 bus, n1_u32 fn, n1_u32 id)
{
    n1_u32 p = cfg_rd(n, bus, fn, 0x34) & 0xfc;
    for (int i = 0; i < 48 && p >= 0x40; i++) {
        n1_u32 h = cfg_rd(n, bus, fn, p);
        if ((h & 0xff) == id) return (int)p;
        p = (h >> 8) & 0xfc;
    }
    return -1;
}

/* Root port: memory window over what we assigned, decoding and bus mastering (W-RP1/2;
 * standard PCI-to-PCI bridge registers, as any PCI core does; LM 2.2) */
static void rp_setup(struct n1 *n)
{
    n1_u64 base = n->win_pci, limit = n->win_pci + (n->win_next ? n->win_next : 0x100000) - 1;
    cfg_wr(n, 0, 0, 0x20, (n1_u32)((base >> 16) & 0xfff0) | (n1_u32)(((limit >> 16) & 0xfff0) << 16));
    cfg_wr(n, 0, 0, 0x04, (cfg_rd(n, 0, 0, 0x04) & 0xffff) | 0x6);
}

/* BAR0 sizing and assignment for function fn (W-EP1; PCI spec BAR sizing). */
static int fn_bar(struct n1 *n, n1_u32 fn, n1_u32 min_size, int reuse)
{
    n1_u32 lo, mask, size;
    int is64;
    if (!reuse) {
        lo = cfg_rd(n, 1, fn, 0x10);
        if (lo & 1) FAIL(N1E_BAR, fn);           /* an I/O BAR is not the N1 */
        is64 = ((lo >> 1) & 3) == 2;
        cfg_wr(n, 1, fn, 0x10, 0xffffffff);
        mask = cfg_rd(n, 1, fn, 0x10) & ~0xfu;
        size = mask ? (~mask + 1) : 0;
        if (is64) cfg_wr(n, 1, fn, 0x14, 0);
        if (!size || size < min_size || size > (16u << 20)) FAIL(N1E_BAR, size);
        n->win_next = ALIGN_UP(n->win_next, size);
        if (n->win_next + size > n->win_size) FAIL(N1E_BAR, 0x100 | fn);
        n->bar_pci[fn] = n->win_pci + n->win_next;
        n->bar_cpu[fn] = n->win_cpu + n->win_next;
        n->bar_size[fn] = size;
        n->win_next += size;
    }
    cfg_wr(n, 1, fn, 0x10, (n1_u32)n->bar_pci[fn]);
    if (((cfg_rd(n, 1, fn, 0x10) >> 1) & 3) == 2) cfg_wr(n, 1, fn, 0x14, (n1_u32)(n->bar_pci[fn] >> 32));
    rp_setup(n);
    cfg_wr(n, 1, fn, 0x04, (cfg_rd(n, 1, fn, 0x04) & 0xffff) | 0x6); /* memory + bus master */
    return 0;
}

static n1_u64 bar(struct n1 *n, int fn, n1_u32 off) { return n->bar_cpu[fn] + off; }
static n1_u32 brd(struct n1 *n, int fn, n1_u32 off) { return N1_RD(n->h, bar(n, fn, off)); }
static void bwr(struct n1 *n, int fn, n1_u32 off, n1_u32 v) { N1_WR(n->h, bar(n, fn, off), v); }

static int alloc(struct n1 *n, struct n1_buf *b, int sid, size_t size)
{
    size = ALIGN_UP(size, 0x4000);
    b->cpu = n->h->dma_alloc(n->h->ctx, sid, size, 0x4000, &b->iova);
    if (!b->cpu) FAIL(N1E_DMA, size);
    b->size = size;
    mzero(b->cpu, size);
    return 0;
}

int n1_ftab_entry(const n1_u8 *t, size_t size, const char tag[4], n1_u32 *off, n1_u32 *len, int sparse)
{
    n1_u32 count, i, o, l;
    if (size < 0x30 || !mequal(t + 0x24, "ftab", 4)) return -1;
    count = le32(t + 0x28);
    if (!count || count > 256 || 0x30 + (size_t)count * 16 > size) return -1;
    for (i = 0; i < count; i++) {
        const n1_u8 *e = t + 0x30 + 16 * i;
        if (!mequal(e, tag, 4)) continue;
        o = le32(e + 4); l = le32(e + 8);
        if (o < 0x30 + count * 16 || !l || (n1_u64)o + l > (sparse ? (64u << 20) : size)) return -1;
        *off = o; *len = l;
        return 0;
    }
    return -2;
}

void n1_init(struct n1 *n, const struct n1_hal *h, const struct n1_addrs *a, const n1_u8 *fw, size_t fw_size)
{
    mzero(n, sizeof(*n));
    n->h = h; n->a = a; n->fw = fw; n->fw_size = fw_size;
    n->win_cpu = N1_WIN_CPU; n->win_pci = N1_WIN_PCI; n->win_size = N1_WIN_SIZE_DEFAULT;
    mcopy(n->mac, a->mac, 6);
}

/* ---- stage 1: training (resident's pcie_t6050 path, m1n1 mode 4; LM 2.1) ---- */
static int st_train(struct n1 *n)
{
    n1_u32 id;
    if (n->h->train(n->h->ctx)) FAIL(N1E_TRAIN, 0);
    id = cfg_rd(n, 1, 0, 0);
    if (id != 0x1900106b) FAIL(N1E_ENDPOINT, id);      /* never adopt a running N1 (LM 7 "warm N1") */
    if (fn_bar(n, 0, 0x9080, 0)) return -1;            /* BAR0 >= 0x9080 (LM 2.3 probe) */
    if (n->h->route_sid(n->h->ctx, 0x100, 1)) FAIL(N1E_FUNCTIONS, 0x100);
    n->stage = N1_ST_TRAINED;
    return 0;
}

/* ---- stage 2: ROM boot (LM 2.3 phase 0, 9.1) ---- */
static int st_rom(struct n1 *n)
{
    n1_u32 v, platform = n->a->board_id << 16 | (n->a->chip_id & 0xffff);
    if (!n->fw || n->fw_size < 0x30 || n->fw_size > (64u << 20) || !mequal(n->fw + 0x20, "rkosftab", 8))
        FAIL(N1E_FW_FORMAT, n->fw_size);
    if (!le32(n->fw + 0x10) || !le32(n->fw + 0x14) || (n1_u64)le32(n->fw + 0x10) + le32(n->fw + 0x14) > n->fw_size)
        FAIL(N1E_FW_FORMAT, 0x10);                     /* unstitched FTAB: the ROM answers -5 (LM 3) */
    if (brd(n, 0, 0x8000) != 0) FAIL(N1E_ROM_BUSY, brd(n, 0, 0x8000)); /* stopped ROM only */
    if (alloc(n, &n->fwbuf, 1, n->fw_size) || alloc(n, &n->desc, 1, 0x4000)) return -1;
    mcopy(n->fwbuf.cpu, n->fw, n->fw_size);
    put64(n->desc.cpu, n->fwbuf.iova);
    put32(n->desc.cpu + 8, (n1_u32)ALIGN_UP(n->fw_size, 4096));
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x807c, platform);                       /* W-C1 */
    bwr(n, 0, 0x803c, 0);                              /* W-C2 */
    bwr(n, 0, 0x8044, (n1_u32)n->desc.iova);           /* W-C3 */
    bwr(n, 0, 0x8048, (n1_u32)(n->desc.iova >> 32));
    bwr(n, 0, 0x804c, 0x20);                           /* W-C4 */
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x9000, 1);                              /* W-C5 doorbell 0: boot */
    if (poll32(n, bar(n, 0, 0x8040), ~0u, 0, 0, 20000, 20000, &v) || v != 1) FAIL(N1E_ROM_RESPONSE, v);
    if (poll32(n, bar(n, 0, 0x8000), ~0u, 0, 0, 20000, 20000, &v) || (v != 4 && v != 1)) FAIL(N1E_ROM_STAGE, v);
    n->h->log(n->h->ctx, "n1: ROM accepted the firmware, stage %u\n", v);
    n->stage = N1_ST_ROM_DONE;
    return 0;
}

/* ---- stage 3: link cycle after the ROM boot (LM 2.3 phase 1) ---- */
static int st_cycle(struct n1 *n)
{
    n1_u32 id, cmd, pmcsr;
    int pm;
    if (brd(n, 0, 0x8000) == 1) { n->stage = N1_ST_CYCLED; return 0; } /* stage 1: no cycle needed */
    pm = find_cap(n, 1, 0, 0x01);
    if (pm < 0) FAIL(N1E_PM, 0);
    n->pm_cap0 = (n1_u32)pm;
    cmd = cfg_rd(n, 1, 0, 0x04) & 0xffff;
    cfg_wr(n, 1, 0, 0x04, cmd & ~0x4u);                /* W-EP2 stop DMA (bus master off) */
    delay_us(n, 10000);
    pmcsr = cfg_rd(n, 1, 0, pm + 4);
    cfg_wr(n, 1, 0, pm + 4, (pmcsr & ~3u) | 3 | 0x100); /* W-EP3 D3hot + PME_En */
    delay_us(n, 10000);
    if (n->h->port_cycle(n->h->ctx)) FAIL(N1E_CYCLE, 0); /* PME_Turn_Off, PERST#, retrain (resident) */
    id = cfg_rd(n, 1, 0, 0);
    if (id != 0x1900106b && id != 0x1901106b) FAIL(N1E_ENDPOINT, id);
    if (fn_bar(n, 0, 0, 1)) return -1;                 /* BAR0 back at the same place */
    cfg_wr(n, 1, 0, pm + 4, pmcsr & ~3u);              /* W-EP4 back to D0 */
    delay_us(n, 10000);
    cfg_wr(n, 1, 0, 0x04, cmd | 0x6);
    if (n->h->route_sid(n->h->ctx, 0x100, 1)) FAIL(N1E_FUNCTIONS, 0x100); /* port enable cleared RID2SID */
    if (brd(n, 0, 0x8000) != 1) FAIL(N1E_ROM_STAGE, brd(n, 0, 0x8000));
    n->stage = N1_ST_CYCLED;
    return 0;
}

/* ---- ACIPC (LM 4) ---- */
static int ctx_init(struct n1 *n, struct n1_ctx *c, int sid, n1_u16 tr, n1_u16 cr, n1_u64 scratch, n1_u64 scratch_size)
{
    n1_u8 *p;
    if (alloc(n, &c->page, sid, 0x8000) || alloc(n, &c->msgs, sid, 0x4000)) return -1;
    c->tr = tr; c->cr = cr; c->msg_head = 0;
    c->index[0] = 0x78;
    c->index[1] = c->index[0] + 2u * cr;
    c->index[2] = c->index[1] + 2u * tr;
    c->index[3] = c->index[2] + 2u * cr;
    c->mtr = (n1_u32)ALIGN_UP(c->index[3] + 2u * tr, tr == 192 ? 16 : 2);
    c->mcr = (n1_u32)ALIGN_UP(c->mtr + 256, 4);
    p = c->page.cpu;
    put16(p, 768); put16(p + 2, 0x68);
    put64(p + 8, c->page.iova + 0x68);
    for (int i = 0; i < 4; i++) put64(p + 0x10 + 8 * i, c->page.iova + c->index[i]);
    put16(p + 0x30, cr); put16(p + 0x32, tr);
    put64(p + 0x34, c->page.iova + c->mcr);
    put64(p + 0x3c, c->page.iova + c->mtr);
    put16(p + 0x44, 16); put16(p + 0x46, 16);
    put64(p + 0x58, scratch ? scratch : c->page.iova + ALIGN_UP(c->mcr + 256, 8));
    put64(p + 0x60, scratch_size);
    n->h->dma_sync(n->h->ctx);
    return 0;
}
static n1_u16 idx_rd(struct n1_ctx *c, int arr, n1_u32 i) { return vrd16(c->page.cpu + c->index[arr] + 2 * i); }
static void idx_wr(struct n1_ctx *c, int arr, n1_u32 i, n1_u16 v) { vwr16(c->page.cpu + c->index[arr] + 2 * i, v); }

static int handshake(struct n1 *n, int fn, struct n1_ctx *c, n1_u32 status, n1_u32 addr, n1_u32 control)
{
    n1_u32 v;
    bwr(n, fn, control, 1);                                            /* W-C9 / W-A5 */
    if (poll32(n, bar(n, fn, status), ~0u, 1, 1, 2000, 100, &v)) FAIL(N1E_HANDSHAKE, (fn << 16) | v);
    bwr(n, fn, addr, (n1_u32)c->page.iova);
    bwr(n, fn, addr + 4, (n1_u32)(c->page.iova >> 32));
    n->h->dma_sync(n->h->ctx);
    bwr(n, fn, control, 2);
    if (poll32(n, bar(n, fn, status), ~0u, 2, 1, 2000, 100, &v)) FAIL(N1E_HANDSHAKE, (fn << 16) | 0x100 | v);
    return 0;
}

/* Control message ring (ring 0): at most 15 messages per context, never wraps (LM 4.2). */
static int ctl_message(struct n1 *n, const n1_u8 *msg, n1_u32 len)
{
    struct n1_ctx *c = &n->control;
    n1_u16 head = c->msg_head;
    n1_u8 *d;
    n1_u64 end;
    if (head >= 15 || !len || len > 512) FAIL(N1E_MESSAGE, head);
    mcopy(c->msgs.cpu + 512u * (head + 1), msg, len);
    d = c->page.cpu + c->mtr + 16u * head;
    put32(d, 1 | len << 8);
    put64(d + 4, c->msgs.iova + 512u * (head + 1));
    put32(d + 12, head + 1u);
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x9000, head + 1u);                                      /* W-C5 doorbell 0 = head */
    end = now_us(n) + 1200000;
    while (idx_rd(c, 1, 0) != head + 1) {
        if (now_us(n) >= end) FAIL(N1E_MESSAGE, 0x100 | head);
        delay_us(n, 100);
    }
    c->msg_head++;
    return 0;
}

static int ctl_open_transfer(struct n1 *n, n1_u16 id, struct n1_buf *b, n1_u16 footer)
{
    n1_u8 m[0x34];
    n1_u32 words = footer >> 2, shift = 0;
    while (shift < 4 && !(words & (1u << shift))) shift++;
    mzero(m, sizeof(m));
    m[0] = 1;
    m[2] = (n1_u8)(words >> shift);
    m[3] = (n1_u8)(shift << 4);
    put16(m + 4, id); put64(m + 8, b->iova); put64(m + 0x10, ~0ull);
    put16(m + 0x18, 16); put16(m + 0x1a, 0xffff); put16(m + 0x1c, id); put16(m + 0x1e, 0x40);
    return ctl_message(n, m, sizeof(m));
}

static int cchi_init(struct n1 *n)
{
    static const n1_u8 config[4] = {9, 0, 0xff, 0xff};
    n1_u64 end;
    if (alloc(n, &n->cchi_rx, 1, 0x8000) || alloc(n, &n->cchi_tx, 1, 0x8000) || alloc(n, &n->debug_rx, 1, 0x4000))
        return -1;
    if (ctl_message(n, config, 4) || ctl_open_transfer(n, 4, &n->cchi_rx, 1024) ||
        ctl_open_transfer(n, 3, &n->cchi_tx, 1024) || ctl_open_transfer(n, 5, &n->debug_rx, 256))
        return -1;
    for (n1_u32 i = 0; i < 15; i++) {
        put32(n->cchi_rx.cpu + i * 1040, 1 | 1024 << 8); put32(n->cchi_rx.cpu + i * 1040 + 12, i + 1);
        put32(n->debug_rx.cpu + i * 272, 1 | 256 << 8); put32(n->debug_rx.cpu + i * 272 + 12, i + 1);
    }
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x9010, 15);                                             /* W-C7 */
    bwr(n, 0, 0x9014, 15);
    put32(n->cchi_tx.cpu, 1 | 64 << 8); put32(n->cchi_tx.cpu + 12, 1); put16(n->cchi_tx.cpu + 16, 64);
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x900c, 1);                                              /* W-C6 */
    end = now_us(n) + 2000000;
    while (!idx_rd(&n->control, 1, 4)) {
        if (now_us(n) >= end) FAIL(N1E_CCHI, 0);
        delay_us(n, 100);
    }
    if ((le32(n->cchi_rx.cpu) >> 8) < 8 || n->cchi_rx.cpu[18] || n->cchi_rx.cpu[19]) FAIL(N1E_CCHI, le32(n->cchi_rx.cpu));
    return 0;
}

static int memswap(struct n1 *n, int fn, int sid, const char tag[4])
{
    n1_u32 off, len;
    struct n1_buf *w = &n->msw[fn];
    if (n1_ftab_entry(n->secondary.cpu, n->secondary.size, tag, &off, &len, 0)) FAIL(N1E_MEMSWAP, fn);
    if (alloc(n, w, sid, len)) return -1;
    mcopy(w->cpu, n->secondary.cpu + off, len);
    n->h->dma_sync(n->h->ctx);
    cfg_wr(n, 1, fn, 0xf88, (n1_u32)w->iova);                          /* W-EP5 */
    cfg_wr(n, 1, fn, 0xf8c, (n1_u32)(w->iova >> 32));
    cfg_wr(n, 1, fn, 0xf90, len);
    return 0;
}

/* ---- stage 4: IPC (LM 2.3 phase 2) ---- */
static int st_ipc(struct n1 *n)
{
    n1_u32 off, len, count, required, v;
    const n1_u8 *seed;
    for (n1_u32 fn = 1; fn <= 2; fn++) {
        n1_u32 id = cfg_rd(n, 1, fn, 0);
        if (id != (0x1901106bu + (fn << 16))) FAIL(N1E_FUNCTIONS, id);
        if (fn_bar(n, fn, 0x2000, 0)) return -1;
        if (n->h->route_sid(n->h->ctx, 0x100 | fn, 1 + fn)) FAIL(N1E_FUNCTIONS, 0x100 | fn);
    }
    /* secondary FTAB (2ftb): header copied, buffer sized to the largest entry end */
    if (n1_ftab_entry(n->fw, n->fw_size, "2ftb", &off, &len, 0)) FAIL(N1E_SECONDARY, 0);
    seed = n->fw + off;
    if (len < 0x30 || !mequal(seed + 0x24, "ftab", 4)) FAIL(N1E_SECONDARY, 1);
    count = le32(seed + 0x28);
    if (!count || count > 256 || 0x30 + 16 * count != len) FAIL(N1E_SECONDARY, 2);
    required = len;
    for (n1_u32 i = 0; i < count; i++) {
        n1_u64 e = (n1_u64)le32(seed + 0x34 + i * 16) + le32(seed + 0x38 + i * 16);
        if (e > (64u << 20)) FAIL(N1E_SECONDARY, 3);
        if (e > required) required = (n1_u32)e;
    }
    if (alloc(n, &n->secondary, 1, required)) return -1;
    mcopy(n->secondary.cpu, seed, len);
    n->h->dma_sync(n->h->ctx);
    cfg_wr(n, 1, 0, 0xf88, (n1_u32)n->secondary.iova);                 /* W-EP5 */
    cfg_wr(n, 1, 0, 0xf8c, (n1_u32)(n->secondary.iova >> 32));
    cfg_wr(n, 1, 0, 0xf90, required);
    if (ctx_init(n, &n->control, 1, 13, 1, 0, 0)) return -1;
    bwr(n, 0, 0x8044, (n1_u32)n->desc.iova);
    bwr(n, 0, 0x8048, (n1_u32)(n->desc.iova >> 32));
    bwr(n, 0, 0x804c, 0x20);
    bwr(n, 0, 0x8074, 0x62746632);                                     /* W-C8 "btf2" */
    n->h->dma_sync(n->h->ctx);
    bwr(n, 0, 0x9070, 1);                                              /* W-C8 start IPC */
    if (poll32(n, bar(n, 0, 0x8000), ~0u, 2, 1, 3000, 100, &v) &&
        poll32(n, bar(n, 0, 0x8000), ~0u, 3, 1, 1, 100, &v)) FAIL(N1E_IPC_STAGE, v);
    if (v != 2) FAIL(N1E_IPC_STAGE, v);
    if (handshake(n, 0, &n->control, 0x8050, 0x8054, 0x907c)) return -1;
    if (memswap(n, 0, 1, "mswc") || memswap(n, 2, 3, "mswb") || memswap(n, 1, 2, "msww")) return -1;
    if (cchi_init(n)) return -1;
    if (poll32(n, bar(n, 1, 0x8), ~0u, 2, 1, 2000, 100, &v)) FAIL(N1E_ALPHA, v);
    if (alloc(n, &n->window, 2, N1_WIN_SIZE)) return -1;
    if (ctx_init(n, &n->alpha, 2, 192, 234, n->window.iova + (8u << 20), 5u << 20)) return -1;
    bwr(n, 1, 0x18, (n1_u32)n->window.iova);                           /* W-A4 */
    bwr(n, 1, 0x1c, (n1_u32)(n->window.iova >> 32));
    bwr(n, 1, 0x20, N1_WIN_SIZE);
    if (handshake(n, 1, &n->alpha, 0xc, 0x10, 0x1028)) return -1;
    n->stage = N1_ST_IPC;
    n->h->log(n->h->ctx, "n1: IPC up (control + alpha)\n");
    return 0;
}

/* ---- Wi-Fi protocol 27 (LM 4.4, 5) ---- */
#define CR_BASE 0x100000u
#define TR_BASE 0x400000u
#define RING_SPAN 0x20000u
#define CMD_BUF 0x600000u
#define DATA_RX_BUF (16u << 20)
#define DATA_TX_BUF (24u << 20)
static const n1_u32 rx_buffer[5] = {0, 0, 0x620000, 0x680000, 0x700000};
static const n1_u16 ring_size[5] = {0, 64, 192, 96, 192};
#define W(off) (n->window.cpu + (off))
#define WIOVA(off) (n->window.iova + (off))

static void completions(struct n1 *n)
{
    for (n1_u32 v = 0; v <= 4; v++) idx_wr(&n->alpha, 2, v, idx_rd(&n->alpha, 0, v));
    n->h->dma_sync(n->h->ctx);
}

static int alpha_message(struct n1 *n, const n1_u8 *msg, n1_u32 len)
{
    struct n1_ctx *c = &n->alpha;
    n1_u16 head = c->msg_head;
    n1_u8 *d;
    n1_u64 end;
    if (head >= 15 || len > 512) FAIL(N1E_MESSAGE, 0x1000 | head);
    mcopy(c->msgs.cpu + 512u * (head + 1), msg, len);
    d = c->page.cpu + c->mtr + 16u * head;
    put32(d, 1 | len << 8);
    put64(d + 4, c->msgs.iova + 512u * (head + 1));
    put32(d + 12, head + 1u);
    idx_wr(c, 3, 0, head + 1);
    if (head) idx_wr(c, 3, 8, head + 1);
    n->h->dma_sync(n->h->ctx);
    c->msg_head++;
    bwr(n, 1, 0x1004, 1);                                              /* W-A1 */
    end = now_us(n) + 2000000;
    for (;;) {
        if (brd(n, 1, 0x8) != 2) FAIL(N1E_ALPHA, brd(n, 1, 0x8));
        if (idx_rd(c, 1, 0) >= head + 1) { completions(n); return 0; }
        if (now_us(n) >= end) FAIL(N1E_MESSAGE, 0x2000 | head);
        delay_us(n, 100);
    }
}

static int open_pair(struct n1 *n, n1_u16 id)
{
    n1_u8 cr[0x2c], tr[0x34];
    mzero(cr, sizeof(cr)); mzero(tr, sizeof(tr));
    cr[0] = 2; put32(cr + 4, id | (n1_u32)id << 16); put64(cr + 8, WIOVA(CR_BASE + (id - 1u) * RING_SPAN));
    put16(cr + 0x10, 1024); put16(cr + 0x12, 0xffff);
    if (alpha_message(n, cr, sizeof(cr))) return -1;
    tr[0] = 1; put32(tr + 4, id | (n1_u32)id << 16); put64(tr + 8, WIOVA(TR_BASE + (id - 1u) * RING_SPAN));
    put64(tr + 0x10, ~0ull); put16(tr + 0x18, ring_size[id]); put16(tr + 0x1a, id); put16(tr + 0x1c, id);
    put16(tr + 0x1e, 0x40); put16(tr + 0x24, 2);
    return alpha_message(n, tr, sizeof(tr));
}

static void post_receive(struct n1 *n, n1_u32 ring, n1_u16 slot)
{
    n1_u8 *d = W(TR_BASE + (ring - 1) * RING_SPAN + slot * 16u);
    n1_u32 off = rx_buffer[ring] + slot * 2048u;
    mzero(W(off), 2048);
    put32(d, 1 | 2048u << 8); put64(d + 4, WIOVA(off)); put32(d + 12, slot + 1u);
}

static int data_ring_open(struct n1 *n, n1_u32 id)
{
    n1_u8 m[0x34 + 10];
    int transfer = id == 12 || id == 25;
    n1_u32 vector, off, size, doorbell, irq;
    switch (id) {
    case 76: vector = 192; off = 0x200000; size = 2048; doorbell = 18; irq = 3; break;
    case 15: vector = 207; off = 0x240000; size = N1_RX_SLOTS; doorbell = 11; irq = 8; break;
    case 12: vector = 12; off = 0x520000; size = N1_RX_SLOTS; doorbell = 11; irq = 8; break;
    case 25: vector = 25; off = 0x540000; size = N1_TX_SLOTS; doorbell = 18; irq = 3; break;
    default: FAIL(N1E_ARGS, id);
    }
    mzero(W(off), size * 32u);
    idx_wr(&n->alpha, transfer ? 1 : 0, vector, 0);
    idx_wr(&n->alpha, transfer ? 3 : 2, vector, 0);
    mzero(m, sizeof(m));
    m[0] = transfer ? 1 : 2;
    m[2] = (transfer && id == 12) ? 0 : 4;
    put16(m + 4, id); put16(m + 6, vector); put64(m + 8, WIOVA(off));
    if (transfer) {
        put64(m + 0x10, ~0ull); put16(m + 0x18, size); put16(m + 0x1a, id == 12 ? 12 : 76);
        put16(m + 0x1c, doorbell); put16(m + 0x1e, id == 12 ? 0x220 : 0); put32(m + 0x20, 16); put16(m + 0x24, irq);
    } else {
        put16(m + 0x10, size); put16(m + 0x12, id == 15 ? 12 : 0xffff); put16(m + 0x14, doorbell);
        put32(m + 0x18, 1); put16(m + 0x1c, irq);
    }
    if (alpha_message(n, m, (transfer ? 0x34u : 0x2cu) + 10)) return -1;
    if (id == 12) {
        for (n1_u32 s = 0; s < size - 1; s++) {
            n1_u8 *d = W(off + s * 16);
            put32(d, 1 | 4096u << 8); put64(d + 4, WIOVA(DATA_RX_BUF + s * 4096u)); put32(d + 12, s + 1);
        }
        n->h->dma_sync(n->h->ctx);
        idx_wr(&n->alpha, 3, vector, (n1_u16)(size - 1));
        n->h->dma_sync(n->h->ctx);
        bwr(n, 1, 0x1050, 1);                                          /* W-A3 */
    }
    return 0;
}

static void parse_beacon(struct n1 *n, const n1_u8 *payload, n1_u32 size)
{
    const n1_u8 *f, *ie, *end;
    struct n1_bss_entry e;
    n1_u32 i;
    int s;
    if (size < 0x60 + 36) return;
    f = payload + 0x60;
    if (f[0] != 0x80 && f[0] != 0x50) return; /* beacon / probe response */
    end = payload + size;
    mzero(&e, sizeof(e));
    mcopy(e.bssid, f + 16, 6);
    for (ie = f + 36; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
        if (ie[0] == 0 && ie[1] <= 32) { e.ssid_len = ie[1]; mcopy(e.ssid, ie + 2, ie[1]); }
        else if ((ie[0] == 3 || ie[0] == 61) && ie[1]) e.channel = ie[2];
        else if (ie[0] == 48 && ie[1] >= 2) {
            /* version, group(4), pairwise count + suites, AKM count + suites, caps */
            const n1_u8 *r = ie + 2, *re = ie + 2 + ie[1];
            n1_u32 pc, ac;
            e.has_rsn = 1;
            r += 2 + 4;
            if (r + 2 > re) continue;
            pc = le16(r); r += 2 + 4 * pc;
            if (r + 2 > re) continue;
            ac = le16(r); r += 2;
            for (i = 0; i < ac && r + 4 <= re; i++, r += 4)
                if (r[0] == 0x00 && r[1] == 0x0f && r[2] == 0xac && r[3] < 16) e.akm_mask |= (n1_u16)(1u << r[3]);
            if (r + 2 <= re) e.rsn_caps = le16(r);
        }
    }
    if (!e.channel) return;
    e.band = e.channel > 14;
    s = (signed char)payload[0x16] > (signed char)payload[0x17] ? (signed char)payload[0x16] : (signed char)payload[0x17];
    e.rssi = (s >= 0 || s < -127) ? -95 : s;
    n->beacons++;
    for (i = 0; i < n->nscan; i++)
        if (mequal(n->scan[i].bssid, e.bssid, 6)) { n->scan[i] = e; return; }
    if (n->nscan < N1_SCAN_MAX) n->scan[n->nscan++] = e;
}

static void drain_data(struct n1 *n);
static void drain(struct n1 *n)
{
    int posted = 0;
    for (n1_u32 ring = 2; ring <= 4; ring++) {
        for (n1_u32 budget = ring_size[ring]; budget--;) {
            n1_u16 slot = n->rx_next[ring], size, status, seq;
            n1_u32 wire, command;
            n1_u8 *d = W(TR_BASE + (ring - 1) * RING_SPAN + slot * 16u);
            n1_u8 *pk = W(rx_buffer[ring] + slot * 2048u);
            if (!(((volatile n1_u8 *)d)[15] & 4)) break;
            size = le16(pk + 0x10); status = le16(pk + 0xe); seq = le16(pk + 0x12); wire = le32(pk + 0x14);
            command = wire & ~(1u << 30);
            n->rx_next[ring] = (n1_u16)((slot + 1) % ring_size[ring]);
            if (size >= 24 && size <= 2048) {
                if (ring == 4) parse_beacon(n, pk + 24, size - 24u);
                else if (command & 0x80000000u) {
                    n->events++;
                    if (command == 0x80000005u) n->scans_done++;
                    if (command == 0x80000000u && n->connecting) { n->connecting = 2; n->connect_status = status; }
                    if ((command == 0x80000003u || command == 0x80000004u) && n->connected) {
                        n->connected = 0;
                        if (n->sink_link) n->sink_link(n->sink, 0, 0);
                        if (n->sink_event) n->sink_event(n->sink);
                    }
                } else {
                    n->reply_valid = 1; n->reply_seq = seq; n->reply_status = status; n->reply_cmd = wire;
                }
            }
            post_receive(n, ring, n->rx_head[ring]);
            n->rx_head[ring] = (n1_u16)((n->rx_head[ring] + 1) % ring_size[ring]);
            n->h->dma_sync(n->h->ctx);
            idx_wr(&n->alpha, 3, ring, n->rx_head[ring]);
            posted = 1;
        }
    }
    completions(n);
    if (posted) bwr(n, 1, 0x1004, 1);
    if (n->data_open) drain_data(n);
}

static int command(struct n1 *n, n1_u32 id, const n1_u8 *payload, n1_u32 len, n1_u16 *status)
{
    n1_u16 slot = n->cmd_head, seq = ++n->seq;
    n1_u8 *pk, *d;
    n1_u64 end;
    if (n->wifi_failed || len > 2048 - 24) return -1;
    if ((n1_u16)((slot + 1) % ring_size[1]) == idx_rd(&n->alpha, 1, 1)) return -1;
    pk = W(CMD_BUF + slot * 2048u);
    mzero(pk, 24 + len);
    put16(pk + 0x10, 24 + len); put16(pk + 0x12, seq); put32(pk + 0x14, id);
    if (len) mcopy(pk + 24, payload, len);
    d = W(TR_BASE + slot * 16u);
    put32(d, 1 | (24 + len) << 8); put64(d + 4, WIOVA(CMD_BUF + slot * 2048u)); put32(d + 12, slot + 1u);
    n->h->dma_sync(n->h->ctx);
    n->cmd_head = (n1_u16)((slot + 1) % ring_size[1]);
    idx_wr(&n->alpha, 3, 1, n->cmd_head);
    n->reply_valid = 0;
    n->h->dma_sync(n->h->ctx);
    bwr(n, 1, 0x1004, 1);
    end = now_us(n) + 1500000;
    for (;;) {
        drain(n);
        if (brd(n, 1, 0x8) != 2) { n->wifi_failed = 1; return -1; }
        if (n->reply_valid && n->reply_seq == seq && (n->reply_cmd & ~(1u << 30)) == id) {
            if (status) *status = n->reply_status;
            return n->reply_status ? -2 : 0;
        }
        if (now_us(n) >= end) { n->wifi_failed = 1; return -1; }
        delay_us(n, 500);
    }
}

/* ---- stage 5: radio (LM 4.4 radio_start, 5.2 commands 0/1/2, data rings) ---- */
static int st_radio(struct n1 *n)
{
    static const n1_u8 start[5] = {0, 0x0b, 0, 0, 0};
    for (n1_u16 r = 1; r <= 4; r++) if (open_pair(n, r)) return -1;
    for (n1_u32 r = 2; r <= 4; r++) {
        for (n1_u16 s = 0; s < ring_size[r] - 1; s++) post_receive(n, r, s);
        n->rx_head[r] = (n1_u16)(ring_size[r] - 1);
        n->h->dma_sync(n->h->ctx);
        idx_wr(&n->alpha, 3, r, n->rx_head[r]);
    }
    n->h->dma_sync(n->h->ctx);
    bwr(n, 1, 0x1004, 1);
    delay_us(n, 400000);
    if (command(n, 0, 0, 0, 0)) FAIL(N1E_COMMAND, 0);
    if (command(n, 1, n->mac, 6, 0)) FAIL(N1E_COMMAND, 1);
    if (command(n, 2, start, 5, 0)) FAIL(N1E_COMMAND, 2);
    delay_us(n, 1000000);
    if (data_ring_open(n, 76) || data_ring_open(n, 15) || data_ring_open(n, 12) || data_ring_open(n, 25)) return -1;
    n->data_open = 1;
    n->stage = N1_ST_RADIO;
    n->h->log(n->h->ctx, "n1: radio started, station %02x:%02x:%02x:%02x:%02x:%02x\n", n->mac[0], n->mac[1],
              n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    return 0;
}

int n1_bringup(struct n1 *n, enum n1_stage stop)
{
    if (stop < N1_ST_TRAINED || stop > N1_ST_RADIO) { n->err = N1E_ARGS; return -1; }
    if (n->stage == N1_ST_FAILED) return -1;
    if (n->stage < N1_ST_TRAINED && st_train(n)) return -1;
    if (stop >= N1_ST_ROM_DONE && n->stage < N1_ST_ROM_DONE && st_rom(n)) return -1;
    if (stop >= N1_ST_CYCLED && n->stage < N1_ST_CYCLED && st_cycle(n)) return -1;
    if (stop >= N1_ST_IPC && n->stage < N1_ST_IPC && st_ipc(n)) return -1;
    if (stop >= N1_ST_RADIO && n->stage < N1_ST_RADIO && st_radio(n)) return -1;
    return 0;
}

/* ---- data path (LM 5.3) ---- */
static void drain_data(struct n1 *n)
{
    struct n1_ctx *c = &n->alpha;
    n1_u16 head = idx_rd(c, 0, 207), tail = idx_rd(c, 2, 207), post = idx_rd(c, 3, 12);
    n1_u32 done = 0;
    if (head >= N1_RX_SLOTS || tail >= N1_RX_SLOTS || post >= N1_RX_SLOTS) { n->rx_bad++; return; }
    while (tail != head && done < N1_RX_SLOTS - 1) {
        n1_u8 *e = W(0x240000 + tail * 32u);
        n1_u32 hdr = le32(e), info = le32(e + 4), footer = le32(e + 12);
        n1_u16 cookie = (n1_u16)info, length = (n1_u16)(info >> 16), next = (n1_u16)((post + 1) % N1_RX_SLOTS);
        n1_u32 buf, pad, offset, flen;
        n1_u8 *d;
        if ((hdr >> 16) != 12 || !cookie || cookie > N1_RX_SLOTS - 1 || length > 4096 || le32(e + 8)) { n->rx_bad++; break; }
        if (next == idx_rd(c, 1, 12)) break;
        buf = DATA_RX_BUF + (cookie - 1u) * 4096u;
        pad = (footer >> 12) & 15;
        if ((footer & 0x80) && length >= pad + 14) {
            offset = (footer & (1u << 24)) ? le16(W(buf + 2)) : pad;
            flen = length - pad;
            if (offset <= 4096 && flen <= 4096 - offset && flen <= WN_MAX_FRAME) {
                n->rx_frames++;
                if (n->sink_rx) n->sink_rx(n->sink, W(buf + offset), flen);
            } else n->rx_bad++;
        } else n->rx_bad++;
        d = W(0x520000 + post * 16u);
        put32(d, 1 | 4096u << 8); put64(d + 4, WIOVA(buf)); put32(d + 12, cookie);
        tail = (n1_u16)((tail + 1) % N1_RX_SLOTS);
        post = next;
        done++;
    }
    if (done) {
        n->h->dma_sync(n->h->ctx);
        idx_wr(c, 2, 207, tail);
        idx_wr(c, 3, 12, post);
        n->h->dma_sync(n->h->ctx);
        bwr(n, 1, 0x1050, 1);
    }
    /* TX completions (CR 76 / vector 192) */
    head = idx_rd(c, 0, 192); tail = idx_rd(c, 2, 192);
    if (head >= 2048 || tail >= 2048) return;
    done = 0;
    while (tail != head && done++ < 2048) {
        n1_u8 *e = W(0x200000 + tail * 32u);
        n1_u16 ring = (n1_u16)(le32(e) >> 16), cookie = le16(e + 4), wcount = le16(e + 9);
        n1_u8 flags = e[1], st = e[12];
        if (ring == 25) {
            int counted = (flags & 1) || (flags & 0x0e) == 8;
            n1_u16 cnt = counted ? wcount : 1;
            if (!cookie || cookie > N1_TX_SLOTS || counted != !!wcount || wcount >= N1_TX_SLOTS) { n->tx_fail++; break; }
            for (n1_u32 i = 0; i < cnt; i++) {
                n1_u32 s = (cookie + N1_TX_SLOTS - cnt + i) % N1_TX_SLOTS;
                if (n->tx_busy[s]) { n->tx_busy[s] = 0; n->tx_done++; if (st && st != 0x83) n->tx_fail++; }
            }
        }
        tail = (n1_u16)((tail + 1) % 2048);
    }
    idx_wr(c, 2, 192, tail);
}

int n1_wn_tx(void *ctx, const n1_u8 *frame, n1_u32 len)
{
    struct n1 *n = ctx;
    n1_u16 post = n->tx_post, next = (n1_u16)((post + 1) % N1_TX_SLOTS);
    n1_u8 *d;
    if (n->stage != N1_ST_RADIO || !n->connected || len < 14 || len > 2048) return -1;
    if (n->tx_busy[post] || next == idx_rd(&n->alpha, 1, 25)) return -1;
    mcopy(W(DATA_TX_BUF + post * 2048u), frame, len);
    d = W(0x540000 + post * 32u);
    mzero(d, 32);
    put32(d, 3 | len << 8); put64(d + 4, WIOVA(DATA_TX_BUF + post * 2048u)); put16(d + 12, post + 1u);
    put64(d + 16, (6ull << 27) | (4ull << 15)); put64(d + 24, 2000000);
    n->tx_busy[post] = 1;
    n->tx_post = next;
    n->tx_frames++;
    n->h->dma_sync(n->h->ctx);
    idx_wr(&n->alpha, 3, 25, next);
    n->h->dma_sync(n->h->ctx);
    bwr(n, 1, 0x1030, 1);                                              /* W-A2 */
    return 0;
}

/* ---- scan / connect (LM 5.2) ---- */
static const n1_u8 scan_channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 36, 40, 44, 48, 149, 153, 157, 161, 165};

n1_u32 n1_wn_scan(void *ctx)
{
    struct n1 *n = ctx;
    n1_u8 p[14 + 5 + 4 * sizeof(scan_channels) + 9];
    n1_u32 size = 14, cnt = sizeof(scan_channels);
    static const n1_u8 options[5] = {0, 0, 1, 0, 0};
    if (n->stage != N1_ST_RADIO || n->wifi_failed) return WN_NOT_READY;
    if (n->scanning || n->connecting) return WN_BUSY;
    mzero(p, sizeof(p));
    p[0] = (n1_u8)(n->seq & 0x1f); p[3] = 0x80; p[5] = 2; p[7] = 0x10;
    put16(p + 8, 40); put16(p + 10, 40); put16(p + 12, 110);
    put16(p + size, 260); put16(p + size + 2, 1 + 4 * cnt); p[size + 4] = (n1_u8)cnt;
    for (n1_u32 i = 0; i < cnt; i++) { p[size + 5 + 4 * i] = scan_channels[i]; p[size + 6 + 4 * i] = scan_channels[i] > 14; }
    size += 5 + 4 * cnt;
    put16(p + size, 357); put16(p + size + 2, 5); mcopy(p + size + 4, options, 5); size += 9;
    n->nscan = 0;
    n->scan_done_at_start = n->scans_done;
    if (command(n, 0x1000f, p, size, 0)) return WN_IO_ERROR;
    n->scanning = 1;
    n->scan_deadline = now_us(n) + (8000000ull > cnt * 220000ull + 2000000ull ? 8000000ull : cnt * 220000ull + 2000000ull);
    return WN_OK;
}

n1_u32 n1_wn_results(void *ctx, n1_u32 first, wn_bss *out, n1_u32 max, n1_u32 *total)
{
    struct n1 *n = ctx;
    n1_u32 k = 0;
    *total = n->nscan;
    for (; first + k < n->nscan && k < max; k++) {
        struct n1_bss_entry *e = &n->scan[first + k];
        wn_bss *b = &out[k];
        mzero(b, sizeof(*b));
        mcopy(b->ssid, e->ssid, e->ssid_len); b->ssid_len = e->ssid_len;
        mcopy(b->bssid, e->bssid, 6); b->channel = e->channel; b->rssi_dbm = (wn_s16)e->rssi;
        b->security_mask = !e->has_rsn ? (1u << WN_SEC_OPEN)
            : (n1_u16)(((e->akm_mask & (1u << 2)) ? 1u << WN_SEC_WPA2_PSK : 0) | ((e->akm_mask & (1u << 8)) ? 1u << WN_SEC_WPA3_SAE : 0));
    }
    return k;
}

static void tlv(n1_u8 *p, n1_u32 *size, n1_u16 tag, const void *v, n1_u16 len)
{
    put16(p + *size, tag); put16(p + *size + 2, len); mcopy(p + *size + 4, v, len); *size += 4u + len;
}

n1_u32 n1_wn_join(void *ctx, const wn_join *j)
{
    struct n1 *n = ctx;
    static n1_u8 p[1024];
    static const n1_u8 mld[14] = {1, 0, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 3, 0, 0};
    n1_u8 timeouts[20], ssid[35], rsn[20], zero = 0, five = 5, sae[2 + 63];
    n1_u32 size = 130, i;
    struct n1_bss_entry *e = 0;
    static const n1_u8 zero6[6];
    if (n->stage != N1_ST_RADIO || n->wifi_failed) return WN_NOT_READY;
    if (n->scanning || n->connecting) return WN_BUSY;
    for (i = 0; i < n->nscan; i++) {
        struct n1_bss_entry *c = &n->scan[i];
        if (c->ssid_len != j->ssid_len || !mequal(c->ssid, j->ssid, j->ssid_len)) continue;
        if (!mequal(j->bssid, zero6, 6) && !mequal(j->bssid, c->bssid, 6)) continue;
        if (!e || c->rssi > e->rssi) e = c;
    }
    n->last_error = 0;
    if (!e) { n->last_error = 1; if (n->sink_event) n->sink_event(n->sink); return WN_OK; }  /* not in the scan */
    if (j->security == WN_SEC_OPEN || !e->has_rsn) return WN_UNSUPPORTED; /* only PSK/SAE are established (LM 5.5) */
    if ((j->security == WN_SEC_WPA2_PSK && !(e->akm_mask & (1u << 2))) ||
        (j->security == WN_SEC_WPA3_SAE && !(e->akm_mask & (1u << 8)))) {
        n->last_error = 2; if (n->sink_event) n->sink_event(n->sink); return WN_OK;
    }
    mzero(p, sizeof(p));
    p[0] = 1; mcopy(p + 1, e->bssid, 6); p[7] = e->channel; p[8] = e->band;
    p[0x46] = j->security == WN_SEC_WPA3_SAE ? 3 : 0;
    p[0x47] = j->security == WN_SEC_WPA3_SAE ? 8 : 4;
    put16(p + 0x48, 4);
    if (j->security == WN_SEC_WPA2_PSK) { put16(p + 0x4a, 32); mcopy(p + 0x4c, j->key, 32); }
    tlv(p, &size, 304, &five, 1);
    put32(timeouts, 260); put32(timeouts + 4, 6000); put32(timeouts + 8, 2000); put32(timeouts + 12, 15000); put32(timeouts + 16, 300);
    tlv(p, &size, 63763, timeouts, 20);
    ssid[0] = 1; ssid[1] = 4; ssid[2] = (n1_u8)j->ssid_len; mcopy(ssid + 3, j->ssid, j->ssid_len);
    tlv(p, &size, 342, ssid, (n1_u16)(3 + j->ssid_len));
    /* station RSN body: v1, group CCMP, 1 x CCMP, 1 x AKM, the AP's RSN capabilities */
    put16(rsn, 1); rsn[2] = 0; rsn[3] = 0x0f; rsn[4] = 0xac; rsn[5] = 4;
    put16(rsn + 6, 1); rsn[8] = 0; rsn[9] = 0x0f; rsn[10] = 0xac; rsn[11] = 4;
    put16(rsn + 12, 1); rsn[14] = 0; rsn[15] = 0x0f; rsn[16] = 0xac; rsn[17] = j->security == WN_SEC_WPA3_SAE ? 8 : 2;
    put16(rsn + 18, e->rsn_caps);
    tlv(p, &size, 48, rsn, 20);
    if (j->security == WN_SEC_WPA3_SAE) { put16(sae, j->key_len); mcopy(sae + 2, j->key, j->key_len); tlv(p, &size, 313, sae, (n1_u16)(2 + j->key_len)); }
    tlv(p, &size, 316, &zero, 1);
    tlv(p, &size, 376, mld, 14);
    n->cur = *e;
    n->connecting = 1;
    n->connect_status = 0;
    i = (n1_u32)command(n, 0x20000, p, size, 0);
    mzero(p, sizeof(p)); mzero(sae, sizeof(sae));
    if (i) { n->connecting = 0; n->last_error = 3; return WN_IO_ERROR; }
    n->connect_deadline = now_us(n) + 15000000;
    return WN_OK;
}

n1_u32 n1_wn_leave(void *ctx)
{
    struct n1 *n = ctx;
    n1_u8 d[16];
    if (n->stage != N1_ST_RADIO) return WN_NOT_READY;
    mzero(d, 16); put16(d, 3); put16(d + 8, 328); put16(d + 10, 4); put32(d + 12, 10);
    if (n->connected || n->connecting) (void)command(n, 0x20001, d, 16, 0);
    n->connected = n->connecting = 0;
    if (n->sink_link) n->sink_link(n->sink, 0, 0);
    return WN_OK;
}

n1_u32 n1_wn_status(void *ctx, wn_wifi_status *st)
{
    struct n1 *n = ctx;
    st->state = n->stage != N1_ST_RADIO ? WN_WIFI_FAILED : n->connected ? WN_WIFI_CONNECTED
              : n->connecting ? WN_WIFI_JOINING : n->scanning ? WN_WIFI_SCANNING
              : n->last_error ? WN_WIFI_FAILED : WN_WIFI_IDLE;
    st->last_error = n->stage == N1_ST_FAILED ? 0x100u + (n1_u32)n->err : (n1_u32)n->last_error;
    st->scan_done_count = n->scans_done;
    if (n->connected || n->connecting) {
        mcopy(st->current.ssid, n->cur.ssid, n->cur.ssid_len); st->current.ssid_len = n->cur.ssid_len;
        mcopy(st->current.bssid, n->cur.bssid, 6); st->current.channel = n->cur.channel;
        st->current.rssi_dbm = (wn_s16)n->cur.rssi;
    }
    return WN_OK;
}

void n1_poll(struct n1 *n)
{
    if (n->stage != N1_ST_RADIO || n->wifi_failed) return;
    if (brd(n, 1, 0x8) != 2) { n->wifi_failed = 1; if (n->sink_link) n->sink_link(n->sink, 0, 0); return; }
    drain(n);
    if (n->scanning && (n->scans_done != n->scan_done_at_start || now_us(n) >= n->scan_deadline)) {
        n->scanning = 0;
        if (n->sink_event) n->sink_event(n->sink);
    }
    if (n->connecting == 2) {
        n->connecting = 0;
        if (!n->connect_status) {
            n->connected = 1;
            if (n->sink_link) n->sink_link(n->sink, 1, 433);
        } else n->last_error = 0x200 | n->connect_status;
        if (n->sink_event) n->sink_event(n->sink);
    } else if (n->connecting == 1 && now_us(n) >= n->connect_deadline) {
        n->connecting = 0; n->last_error = 4;
        if (n->sink_event) n->sink_event(n->sink);
    }
}
