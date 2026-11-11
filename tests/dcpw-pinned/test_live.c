/* SPDX-License-Identifier: GPL-2.0-only */
/* test_live.c -- the live DCP session against fake_live: attach (boot conversation, memory callbacks
 * through real dart4 tables), stage gating, client open + power, swaps, mode set, brightness. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_live.h"
#include "fake_dcp.h"
#include "../service/dcpw_static.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* fake physical memory: [MEM_BASE, MEM_BASE + MEM_SIZE) */
#define MEM_BASE 0x10000000000ull
#define MEM_SIZE (96ull << 20)
static uint8_t *mem;
static uint64_t bump;
static void *ptr(void *c, uint64_t pa) { (void)c; return pa >= MEM_BASE && pa < MEM_BASE + MEM_SIZE ? mem + (pa - MEM_BASE) : 0; }
static uint64_t rd64(void *c, uint64_t pa) { uint64_t v = 0; uint8_t *p = ptr(c, pa); if (p) memcpy(&v, p, 8); return v; }
static void wr64(void *c, uint64_t pa, uint64_t v) { uint8_t *p = ptr(c, pa); if (p) memcpy(p, &v, 8); }
static uint64_t alloc(void *c, uint64_t len) { (void)c; uint64_t a = bump; len = (len + 0x3fff) & ~0x3fffull; if (bump + len > MEM_BASE + MEM_SIZE) return 0; bump += len; memset(ptr(0, a), 0, len); return a; }
static uint64_t alloc_table(void *c) { return alloc(c, 0x4000); }
static void barrier(void *c) { (void)c; }

/* three fake DARTs: registers per instance; firmware tables built like the hardware's */
struct fdart { uint64_t base; uint32_t regs[0x2000 / 4]; };
static struct fdart darts[3] = { { 0x48a340000ull, {0} }, { 0x48a300000ull, {0} }, { 0x48a300000ull + 0x100000, {0} } };
static uint32_t mrd(void *c, uint64_t pa)
{
	(void)c;
	for (int i = 0; i < 3; i++) if (pa >= darts[i].base && pa < darts[i].base + 0x2000) return darts[i].regs[(pa - darts[i].base) / 4];
	return 0;
}
static int tlb_writes;
static void mwr(void *c, uint64_t pa, uint32_t v) { (void)c; (void)pa; (void)v; tlb_writes++; }
static struct dart4_io IO = { 0, rd64, wr64, mrd, mwr, alloc_table, barrier };

static uint64_t pte(uint64_t pa) { return ((pa >> 14) << 10) | 1; }
static uint64_t make_fw_tree(struct fdart *d, uint32_t sid)
{
	uint64_t root = alloc(0, 0x4000), mid = alloc(0, 0x4000);
	wr64(0, root + 8 * 16, pte(mid));       /* firmware holds root slot 16 (DVA 0x100_0000_0000) */
	d->regs[0x08 / 4] = 0x0202;             /* PARAMS3 version */
	d->regs[0x200 / 4] = 1;
	d->regs[(0x1000 + 4 * sid) / 4] = 0x9;
	d->regs[(0x1400 + 4 * sid) / 4] = (uint32_t)(((root >> 14) << 2) | 1);
	return root;
}

static uint64_t now_v;
static uint64_t now(void *c) { (void)c; return now_v += 1000000; }
static void lg(void *c, const char *s) { (void)c; printf("  live: %s\n", s); }

static int irq_level;
static void env_irq(void *c, int l) { (void)c; irq_level = l; }
static int env_gpa_ok(void *c, uint64_t gpa, uint64_t len) { (void)c; return gpa >= MEM_BASE && gpa + len <= MEM_BASE + MEM_SIZE; }

struct world { struct dart4 dcp, pio, disp; struct live l; struct fake_live f; uint8_t blob[16384]; };

static int setup(struct world *w, enum live_stage st)
{
	bump = MEM_BASE;
	memset(mem, 0, MEM_SIZE);
	memset(darts, 0, sizeof(darts));
	darts[0].base = 0x48a340000ull; darts[1].base = 0x48a300000ull; darts[2].base = 0x48a300000ull + 0x100000;
	uint64_t r0 = make_fw_tree(&darts[0], 23), r1 = make_fw_tree(&darts[1], 16), r2 = make_fw_tree(&darts[2], 0);
	uint64_t vb = 0x10000000000ull, vs = 1ull << 40;
	if (dart4_init(&w->dcp, &IO, darts[0].base, 23, r0, vb, vs, 2) ||
	    dart4_init(&w->pio, &IO, darts[1].base, 16, r1, vb, vs, 1) ||
	    dart4_init(&w->disp, &IO, darts[2].base, 0, r2, vb, vs, 1))
		return -1;
	uint32_t n = j716s_timing_blob(w->blob, sizeof(w->blob));
	struct live_io io = { &w->f, 0, 0, now, alloc, ptr, lg };
	fake_live_init(&w->f, &w->l, &w->dcp, &w->pio, &w->disp, ptr, 0, w->blob, n);
	fake_live_transport(&w->f, &io.send, &io.pump);
	io.ctx = &w->f;
	live_init(&w->l, &io, st, &w->dcp, &w->pio, &w->disp);
	return 0;
}

static int has_cmd(struct fake_live *f, const char *t)
{
	for (int i = 0; i < f->ncmds; i++) if (!strcmp(f->cmdlog[i], t)) return 1;
	return 0;
}

int main(void)
{
	mem = calloc(1, MEM_SIZE);
	static struct world w;

	printf("== stage gate: LIVE_OFF never attaches\n");
	CHECK(setup(&w, LIVE_OFF) == 0);
	CHECK(live_attach(&w.l) == -1 && w.f.ncmds == 0 && w.f.shm == 0);

	printf("== LIVE_ATTACH: boot conversation\n");
	CHECK(setup(&w, LIVE_ATTACH) == 0);
	int r = live_attach(&w.l);
	CHECK(r == 0 && w.l.state == LS_STARTED && w.f.booted);
	CHECK(w.f.errors == 0);
	CHECK(w.l.ntimings == 6 && w.l.cur_timing == 2 && w.l.brightness_scale == 0x10000 && w.l.bl_nits == 300);
	CHECK(w.l.dfb_w == 3456 && w.l.dfb_h == 2234 && w.l.dfb_format == 0x62336138);
	CHECK(w.l.cb_unknown == 1 && !strcmp(w.l.last_unknown, "D999"));
	static const char *boot[] = { "A451", "A391", "A136", "A137", "A035", "A390", "A504", "A037", "A036", "A103", "A503",
	                              "A104", "A105", "A027", "A529", "A035", "A000", "A385", "A526", "A534" };
	int order_ok = w.f.ncmds >= (int)(sizeof(boot) / sizeof(boot[0]));
	for (unsigned i = 0; order_ok && i < sizeof(boot) / sizeof(boot[0]); i++)
		order_ok = !strcmp(w.f.cmdlog[i], boot[i]);
	CHECK(order_ok);
	if (!order_ok) for (int i = 0; i < w.f.ncmds; i++) printf("   cmd %d %s\n", i, w.f.cmdlog[i]);
	CHECK(!has_cmd(&w.f, "A463") && !has_cmd(&w.f, "A456") && !has_cmd(&w.f, "A535") && !has_cmd(&w.f, "A461"));
	CHECK(w.f.nids == 3 && w.f.replies > 30);
	CHECK(live_open(&w.l) == -1);                        /* stage forbids it */
	struct dcpw_backend be;
	live_backend(&w.l, &be);
	struct dcpw_mode modes[16];
	uint32_t cur = 9;
	CHECK(be.caps == 0 && be.get_modes(be.ctx, modes, 16, &cur) == 6 && cur == 0 && modes[0].refresh_mhz == 120000);
	struct dcpw_present pr = { MEM_BASE + (64ull << 20), 13824, 3456, 2234, DCPW_FMT_B8G8R8A8, 0, 0, 3456, 2234 };
	CHECK(be.present(be.ctx, &pr) == -1 && !has_cmd(&w.f, "A456"));   /* no flips in the attach stage */
	printf("   attach: %u callbacks, %u AP commands, %u DART flushes\n", w.l.cb_count, w.l.cmd_count, (unsigned)tlb_writes);

	printf("== LIVE_FLIP: client open, power on OOBCMD, flips at the active mode\n");
	CHECK(setup(&w, LIVE_FLIP) == 0);
	CHECK(live_attach(&w.l) == 0);
	CHECK(live_open(&w.l) == 0 && w.l.state == LS_READY && w.l.main_display == 1);
	CHECK(has_cmd(&w.f, "A516") && has_cmd(&w.f, "A535") && has_cmd(&w.f, "A033") && w.f.errors == 0);
	static struct dcpw_dev dev;
	struct dcpw_env env = { 0, env_gpa_ok, env_irq };
	live_backend(&w.l, &be);
	CHECK(dcpw_configure(&dev, &be, &env, 600000, 300000, 0) == 0);
	w.l.dev = &dev;
	CHECK(dev.caps == (DCPW_CAP_FLIP | DCPW_CAP_HW_VBLANK) && dev.nmodes == 6);
	uint32_t a[12] = { (uint32_t)pr.gpa, (uint32_t)(pr.gpa >> 32), 13824, 3456, 2234, DCPW_FMT_B8G8R8A8, 0, 0, 3456, 2234 };
	for (int i = 0; i < 12; i++) dcpw_write32(&dev, DCPW_REQ_OFF + 8 + 4 * i, a[i < 10 ? i : 0] * (i < 10), 0);
	dcpw_write32(&dev, DCPW_REQ_OFF, 1, 0); dcpw_write32(&dev, DCPW_REQ_OFF + 4, DCPW_OP_PRESENT, 0);
	dcpw_write32(&dev, DCPW_REG_DOORBELL, 1, 0);
	CHECK(dcpw_read32(&dev, DCPW_RSP_OFF + 4) == DCPW_OK);
	CHECK(w.f.last_swap == 1 && w.f.last_swap_pa == pr.gpa && w.f.last_bl == 0 && !has_cmd(&w.f, "A463"));
	for (int i = 0; i < 100 && (w.f.qn || w.f.complete_pending); i++) w.l.io.pump(w.l.io.ctx);
	CHECK(w.l.swaps_done == 1 && dcpw_read32(&dev, DCPW_REG_VBL_LO) == 1 && (dcpw_read32(&dev, DCPW_REG_IRQ_STATUS) & DCPW_IRQ_PRESENT) && irq_level == 0);   /* IRQ not enabled */
	a[0] = 1;
	dcpw_write32(&dev, DCPW_REQ_OFF, 2, 0); dcpw_write32(&dev, DCPW_REQ_OFF + 4, DCPW_OP_SET_MODE, 0);
	dcpw_write32(&dev, DCPW_REQ_OFF + 8, 1, 0); dcpw_write32(&dev, DCPW_REG_DOORBELL, 2, 0);
	CHECK(dcpw_read32(&dev, DCPW_RSP_OFF + 4) == DCPW_E_UNSUP && !has_cmd(&w.f, "A463"));   /* no mode set in LIVE_FLIP */
	CHECK(w.f.errors == 0);

	printf("== LIVE_FULL: mode set (A500 then A463), brightness in the swap\n");
	CHECK(setup(&w, LIVE_FULL) == 0);
	CHECK(live_attach(&w.l) == 0 && live_open(&w.l) == 0);
	live_backend(&w.l, &be);
	static struct dcpw_dev d2;
	CHECK(dcpw_configure(&d2, &be, &env, 600000, 300000, 0) == 0);
	w.l.dev = &d2;
	CHECK(be.set_mode(be.ctx, &d2.modes[1], 0) == 0 && w.f.cur_timing == 3);
	CHECK(be.set_mode(be.ctx, &d2.modes[0], 1) == 0 && w.f.cur_timing == 2 && w.f.adaptive == 0x180000);
	CHECK(be.present(be.ctx, &pr) == 0);
	for (int i = 0; i < 100 && (w.f.qn || w.f.complete_pending); i++) w.l.io.pump(w.l.io.ctx);
	CHECK(be.set_brightness(be.ctx, 450000) == 0 && w.f.last_bl == 1 && w.l.bl_nits == 450);
	CHECK(w.f.errors == 0);

	printf("== failure: DCP stops answering\n");
	CHECK(setup(&w, LIVE_ATTACH) == 0);
	w.l.timeout_polls = 50;
	w.f.blob_len = 0;   /* a broken TimingElements: started without modes */
	CHECK(live_attach(&w.l) == -1 && w.l.state == LS_FAILED);
	printf("   fail reason: %s\n", w.l.fail_why);

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	free(mem);
	return fails != 0;
}
