/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hv_win_dcpw.c -- dcpw-live-1: the live DCP session from the m1n1 proxy, no guest (PLAN D3 steps 1-3,
 * display/dcp-20261111/LIVE-PLAN.md). Proxy opcode P_DCPW_LIVE(stage, nflips, report_pa, flags).
 * Default off: nothing here runs unless the host sends that opcode.
 *
 *   stage 1 (attach): adopt the running DCP over RTKit (m1n1 rtkit_boot: IOP_PWR 0x220, HELLO, EPMAP,
 *           system endpoints), start EP 0x2d and 0x37, map 1 MiB shmem into dart-dcp SID 23 with dart4,
 *           SET_SHMEM, answer every callback, A451 start chain incl. D120; read the modes. No mode set,
 *           no swap, no power call.
 *   stage 2 (flip): + client open, A535 power (OOBCMD), nflips swaps of a resident test pattern at the
 *           already active timing.
 *   stage 3 (full): + one A463 to 60 Hz and back, brightness 150 then 300 nits in swaps.
 * Every register/memory write and mailbox message is listed in display/dcp-20261111/WRITES.md.
 */
#include "hv_win_dcpw.h"
#include "adt.h"
#include "asc.h"
#include "heapblock.h"
#include "malloc.h"
#include "memory.h"
#include "rtkit.h"
#include "string.h"
#include "utils.h"
#include "dcpw/service/dart4.h"
#include "dcpw/service/iomfb_live.h"

#define DCP_ASC_BASE     0x48ae00000ull
#define DART_DCP_BASE    0x48a340000ull
#define DART_DISP0_BASE  0x48a300000ull
#define DCPW_VM_BASE     0x10000000000ull
#define DCPW_VM_SIZE     (3ull << 40)

static void *phys(u64 pa) { return (void *)(uintptr_t)pa; }

/* ---- dart4 accessors: tables are ordinary cached RAM; clean each entry so the DART sees it ---- */
static uint64_t d_rd64(void *c, uint64_t pa) { (void)c; return *(volatile u64 *)phys(pa); }
static void d_wr64(void *c, uint64_t pa, uint64_t v)
{
	(void)c;
	*(volatile u64 *)phys(pa) = v;
	dc_civac_range(phys(pa), 8);
}
static uint32_t d_mrd(void *c, uint64_t pa) { (void)c; return read32(pa); }
static void d_mwr(void *c, uint64_t pa, uint32_t v) { (void)c; write32(pa, v); }
static uint64_t d_alloc(void *c)
{
	(void)c;
	void *p = memalign(SZ_16K, SZ_16K);
	if (!p)
		return 0;
	memset(p, 0, SZ_16K);
	dc_civac_range(p, SZ_16K);
	return (u64)(uintptr_t)p;
}
static void d_barrier(void *c) { (void)c; dma_wmb(); }
static const struct dart4_io dio = { 0, d_rd64, d_wr64, d_mrd, d_mwr, d_alloc, d_barrier };

/* ---- live io over m1n1 rtkit ---- */
static rtkit_dev_t *g_rtk;
static struct live g_live;
static struct dart4 g_ddcp, g_dpio, g_ddisp;
static u32 g_ep2d_msgs, g_other_msgs;

static void l_send(void *c, uint8_t ep, uint64_t msg)
{
	(void)c;
	struct rtkit_message m = { ep, msg };
	if (!rtkit_send(g_rtk, &m))
		printf("dcpw: rtkit_send ep %x failed\n", ep);
}
static int l_pump(void *c)
{
	(void)c;
	struct rtkit_message m;
	int r = rtkit_recv(g_rtk, &m);
	if (r < 0) {
		printf("dcpw: rtkit_recv %d (crashed?)\n", r);
		return 0;
	}
	if (r == 0) {
		udelay(50);
		return 0;
	}
	if (m.ep == IOMFB_EP)
		live_rx(&g_live, m.ep, m.msg);
	else if (m.ep == IOMFB_EP_REMOTE_ALLOC) {
		if (g_ep2d_msgs++ < 4)
			printf("dcpw: EP 0x2d message %016lx (not answered; Linux's remote-alloc handling not ported)\n", m.msg);
	} else if (g_other_msgs++ < 4)
		printf("dcpw: EP %x message %016lx ignored\n", m.ep, m.msg);
	return 1;
}
static uint64_t l_now(void *c) { (void)c; return ticks_to_usecs(get_ticks()) * 1000ull; }
static uint64_t l_alloc(void *c, uint64_t len)
{
	(void)c;
	void *p = memalign(SZ_16K, ALIGN_UP(len, SZ_16K));
	if (!p)
		return 0;
	memset(p, 0, ALIGN_UP(len, SZ_16K));
	dc_civac_range(p, ALIGN_UP(len, SZ_16K));
	return (u64)(uintptr_t)p;
}
static void *l_ptr(void *c, uint64_t pa) { (void)c; return phys(pa); }
static void l_log(void *c, const char *s) { (void)c; printf("dcpw: %s\n", s); }

/* rtkit buffer requests through dart4 (syslog/crashlog pre-allocated by iBoot are translated) */
static u64 r_map(void *c, void *p, size_t sz) { (void)c; return dart4_map(&g_ddcp, (u64)(uintptr_t)p, ALIGN_UP(sz, SZ_16K)); }
static void *r_xlate(void *c, u64 dva)
{
	(void)c;
	u64 pa = dart4_translate(&g_ddcp, dva);
	return pa ? phys(pa) : NULL;
}

static int adt_reg(const char *path, int idx, u64 *base, u64 *size)
{
	int p[8];
	int node = adt_path_offset_trace(adt, path, p);
	return node < 0 ? -1 : adt_get_reg(adt, p, "reg", idx, base, size);
}
static int adt_l1tt(const char *path, int sid, u64 *root)
{
	char name[16];
	u64 v[2];
	int node = adt_path_offset(adt, path);
	snprintf(name, sizeof(name), "l1-tt-%d", sid);
	if (node < 0 || ADT_GETPROP_ARRAY(adt, node, name, v) < 0)
		return -1;
	*root = v[0];
	return 0;
}

static void fill_pattern(u8 *s, u32 w, u32 h, u32 stride, u32 frame)
{
	for (u32 y = 0; y < h; y++) {
		u32 *row = (u32 *)(s + (u64)y * stride);
		for (u32 x = 0; x < w; x++) {
			u32 band = ((x + frame * 24) / 96) & 1;
			row[x] = 0xff000000u | ((x * 255 / w) << 16) | ((y * 255 / h) << 8) | (band ? 0xff : 0x40);
		}
	}
	dc_civac_range(s, (u64)stride * h);
}

int dcpw_live_run(u64 stage, u64 nflips, u64 report_pa, u64 flags)
{
	struct dcpw_live_report *rep = report_pa ? phys(report_pa) : NULL;
	u64 b, root0, root16, root23;
	int ret = -1;
	(void)flags;
	if (rep) {
		memset(rep, 0, sizeof(*rep));
		rep->magic = DCPW_LIVE_REPORT_MAGIC;
		rep->stage = (u32)stage;
	}
	if (stage < LIVE_ATTACH || stage > LIVE_FULL) {
		printf("dcpw: stage %lu refused\n", stage);
		return -1;
	}
#define STEP(n) do { if (rep) rep->step = (n); } while (0)
	STEP(1);
	/* addresses must be the ones the Linux DT and the probe verified */
	if (adt_reg("/arm-io/dcp", 0, &b, NULL) || b != DCP_ASC_BASE || adt_reg("/arm-io/dart-dcp", 0, &b, NULL) ||
	    b != DART_DCP_BASE || adt_reg("/arm-io/dart-disp0", 0, &b, NULL) || b != DART_DISP0_BASE) {
		printf("dcpw: ADT addresses are not the expected ones; nothing touched\n");
		return -2;
	}
	if (adt_l1tt("/arm-io/dart-dcp", 23, &root23) || adt_l1tt("/arm-io/dart-disp0", 0, &root0) ||
	    adt_l1tt("/arm-io/dart-disp0", 16, &root16)) {
		printf("dcpw: ADT l1-tt roots missing\n");
		return -3;
	}
	if (!(read32(DCP_ASC_BASE + 0x44) & 0x10)) {          /* CPU_CONTROL RUN (read only) */
		printf("dcpw: DCP is not running; nothing touched\n");
		return -4;
	}
	STEP(2);
	if (dart4_init(&g_ddcp, &dio, DART_DCP_BASE, 23, 0x0202, root23, DCPW_VM_BASE, DCPW_VM_SIZE, 2) ||
	    dart4_init(&g_dpio, &dio, DART_DISP0_BASE, 16, 0x0202, root16, DCPW_VM_BASE, DCPW_VM_SIZE, 1) ||
	    dart4_init(&g_ddisp, &dio, DART_DISP0_BASE, 0, 0x0202, root0, DCPW_VM_BASE, DCPW_VM_SIZE, 1)) {
		printf("dcpw: dart4 init refused (dcp %d pio %d disp %d); nothing written\n", g_ddcp.failed,
		       g_dpio.failed, g_ddisp.failed);
		return -5;
	}
	printf("dcpw: dart-dcp L1 slots %u/%u, piodma %u, scanout %u\n", g_ddcp.slot[0], g_ddcp.slot[1], g_dpio.slot[0],
	       g_ddisp.slot[0]);
	STEP(3);
	asc_dev_t *asc = asc_init("/arm-io/dcp");
	if (!asc)
		return -6;
	g_rtk = rtkit_init("dcp", asc, NULL, NULL, NULL, false);
	if (!g_rtk)
		return -7;
	rtkit_set_hooks(g_rtk, r_map, r_xlate, NULL);
	rtkit_set_boot_timeout(2000000);
	if (!rtkit_boot(g_rtk)) {
		printf("dcpw: rtkit_boot failed\n");
		ret = -8;
		goto out;
	}
	STEP(4);
	if (!rtkit_start_ep(g_rtk, IOMFB_EP_REMOTE_ALLOC) || !rtkit_start_ep(g_rtk, IOMFB_EP)) {
		printf("dcpw: start endpoints failed\n");
		ret = -9;
		goto out;
	}
	STEP(5);
	struct live_io io = { NULL, l_send, l_pump, l_now, l_alloc, l_ptr, l_log };
	live_init(&g_live, &io, (enum live_stage)stage, &g_ddcp, &g_dpio, &g_ddisp);
	g_live.timeout_polls = 400000;                        /* about 20 s with the 50 us idle delay */
	{
		u64 base, size;
		for (int i = 0; i < LIVE_MAX_REGS && !adt_reg("/arm-io/disp0", i, &base, &size); i++) {
			g_live.disp_regs[i] = base;
			g_live.disp_reg_len[i] = size;
			g_live.ndisp_regs = i + 1;
		}
	}
	ret = live_attach(&g_live);
	STEP(6);
	printf("dcpw: attach %d state %d: %u callbacks (%u unknown, last %s), %u commands, %d timings, current %u, "
	       "dfb %ux%u fmt %08x, brightness %u nits scale %x, ep2d %u\n",
	       ret, g_live.state, g_live.cb_count, g_live.cb_unknown, g_live.last_unknown, g_live.cmd_count,
	       g_live.ntimings, g_live.cur_timing, g_live.dfb_w, g_live.dfb_h, g_live.dfb_format, g_live.bl_nits,
	       g_live.brightness_scale, g_ep2d_msgs);
	for (int i = 0; i < g_live.ntimings; i++)
		printf("dcpw:   timing %u: %ux%u total %ux%u %u.%03u Hz vrr %x-%x%s\n", g_live.timings[i].id,
		       g_live.timings[i].hactive, g_live.timings[i].vactive, g_live.timings[i].htotal,
		       g_live.timings[i].vtotal, g_live.timings[i].rate_q16 >> 16,
		       ((g_live.timings[i].rate_q16 & 0xffff) * 1000) >> 16, g_live.timings[i].vrr_min_q16,
		       g_live.timings[i].vrr_max_q16, g_live.timings[i].preferred ? " preferred" : "");
	if (ret || stage < LIVE_FLIP)
		goto out;

	STEP(7);
	ret = live_open(&g_live);
	printf("dcpw: client open + power %d (main display %d)\n", ret, g_live.main_display);
	if (ret)
		goto out;
	STEP(8);
	{
		struct dcpw_backend be;
		struct dcpw_mode modes[16];
		u32 cur = 0;
		live_backend(&g_live, &be);
		int nm = be.get_modes(be.ctx, modes, 16, &cur);
		if (nm <= 0) {
			ret = -10;
			goto out;
		}
		u32 w = modes[cur].width, h = modes[cur].height, stride = ALIGN_UP(w * 4, 64);
		u8 *surf = heapblock_alloc_aligned((u64)stride * h, SZ_16K);
		if (!surf) {
			ret = -11;
			goto out;
		}
		struct dcpw_present pr = { (u64)(uintptr_t)surf, stride, w, h, DCPW_FMT_B8G8R8A8, 0, 0, w, h };
		u64 t0 = get_ticks(), tlast = t0;
		u32 done = 0;
		for (u32 i = 0; i < nflips; i++) {
			fill_pattern(surf, w, h, stride, i);
			if (be.present(be.ctx, &pr)) {
				printf("dcpw: swap %u failed\n", i);
				ret = -12;
				break;
			}
			for (u32 k = 0; k < 4000 && g_live.swap_inflight; k++)
				l_pump(NULL);                            /* wait for D594 (<= 200 ms) */
			if (g_live.swap_inflight) {
				printf("dcpw: swap %u never completed\n", i);
				ret = -13;
				break;
			}
			done++;
			tlast = get_ticks();
		}
		u64 us = ticks_to_usecs(tlast - t0);
		printf("dcpw: %u/%lu swaps completed in %lu us (%lu us each)\n", done, nflips, us, done ? us / done : 0);
		if (rep) {
			rep->swaps = done;
			rep->swap_us = (u32)(done ? us / done : 0);
		}
		if (!ret && stage >= LIVE_FULL) {
			STEP(9);
			int a = be.set_mode(be.ctx, &modes[1], 0);   /* 60 Hz */
			int b2 = be.present(be.ctx, &pr);
			mdelay(2000);
			int c = be.set_mode(be.ctx, &modes[cur], 0); /* back */
			int d = be.set_brightness(be.ctx, 150000);
			mdelay(2000);
			int e = be.set_brightness(be.ctx, 300000);
			printf("dcpw: full: mode 60 Hz %d, swap %d, back %d, brightness 150 %d, 300 %d\n", a, b2, c, d, e);
			if (rep) rep->full_rc = (u32)((a != 0) | (b2 != 0) << 1 | (c != 0) << 2 | (d != 0) << 3 | (e != 0) << 4);
			ret = (a || b2 || c || d || e) ? -14 : 0;
		}
	}
out:
	if (rep) {
		rep->result = (u32)ret;
		rep->state = g_live.state;
		rep->callbacks = g_live.cb_count;
		rep->unknown = g_live.cb_unknown;
		rep->commands = g_live.cmd_count;
		rep->ntimings = (u32)g_live.ntimings;
		rep->cur_timing = g_live.cur_timing;
		for (int i = 0; i < g_live.ntimings && i < 8; i++) {
			rep->timing_id[i] = g_live.timings[i].id;
			rep->timing_rate_q16[i] = g_live.timings[i].rate_q16;
			rep->timing_wh[i] = (g_live.timings[i].hactive << 16) | g_live.timings[i].vactive;
		}
		rep->dfb_w = g_live.dfb_w;
		rep->dfb_h = g_live.dfb_h;
		rep->bl_nits = g_live.bl_nits;
		rep->ep2d = g_ep2d_msgs;
		rep->dart_tables = g_ddcp.stat_tables + g_dpio.stat_tables + g_ddisp.stat_tables;
		rep->dart_flushes = g_ddcp.stat_flushes + g_dpio.stat_flushes + g_ddisp.stat_flushes;
		memcpy(rep->fail, g_live.fail_why, sizeof(rep->fail));
		memcpy(rep->last_unknown, g_live.last_unknown, 4);
		dc_civac_range(rep, sizeof(*rep));
	}
	printf("dcpw: result %d (step %u)\n", ret, rep ? rep->step : 0);
	return ret;
}
