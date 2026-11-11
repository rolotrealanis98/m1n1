/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iomfb_live.c -- see iomfb_live.h. Behaviour ported (not pasted) from drm/apple (iomfb.c,
 * iomfb_template.c, iomfb_v27.c: GPL-2.0-only [OR MIT]) and the owner's 27.3 patches; values the
 * macOS 27.3 capture shows are used where Linux differs (marked "macOS").
 * Citation keys (full paths in LINUX-MAP.md / CALLBACKS.md):
 *   tpl = REF/iomfb_template.c, v27 = REF/iomfb_v27.c, iomfb = REF/iomfb.c,
 *   DEC = m5pro-linux contrib/pending/hv/captures/DECODE-DCP-RPC-20261109.md,
 *   CONV = .../dcp-rpc-decode-20261109-conversation.txt, P9 = kernel-dcp/0009, V2 = dcp-27.3/0002.
 */
#include "iomfb_live.h"

static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void cpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static uint32_t g32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t *p) { return g32(p) | ((uint64_t)g32(p + 4) << 32); }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
static int teq(const char *a, const char *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }
static int keq(const uint8_t *k, uint32_t max, const char *s)
{
	uint32_t i = 0;
	for (; s[i]; i++)
		if (i >= max || k[i] != (uint8_t)s[i])
			return 0;
	return i < max && k[i] == 0;
}

static void say(struct live *l, const char *s) { if (l->io.log) l->io.log(l->io.ctx, s); }

static void fail(struct live *l, const char *why)
{
	if (l->state == LS_FAILED)
		return;
	l->state = LS_FAILED;
	for (int i = 0; i < 47 && why[i]; i++)
		l->fail_why[i] = why[i], l->fail_why[i + 1] = 0;
	say(l, why);
	if (l->dev)
		dcpw_backend_failed(l->dev);
}

void live_init(struct live *l, const struct live_io *io, enum live_stage stage, struct dart4 *dart_dcp,
               struct dart4 *dart_piodma, struct dart4 *dart_disp)
{
	zero(l, sizeof(*l));
	l->io = *io;
	l->stage = stage;
	l->dart_dcp = dart_dcp;
	l->dart_piodma = dart_piodma;
	l->dart_disp = dart_disp;
	l->bw_scratch = 0x286720000ull;     /* D003: PMC 0x286700000 + 0x20000 (BW-SCRATCH.md; macOS CONV seq 522) */
	l->clock0_hz = 810000000ull;        /* D408 PROV 0: macOS answer (DEC stage C) */
	l->cur_timing = 2;
	l->bl_nits = 300;
	l->timeout_polls = 200000;
}

/* ------------------------------------------------------------------ AP commands */
/* stack 0: tx area 0x0, contexts CMD (top level) and CB (nested in a CB callback); stack 1: 0x8000 OOB */
static int issue(struct live *l, int oob, const char tag[4], const void *in, uint32_t in_len, uint32_t out_len,
                 void (*done)(struct live *, struct live_cmd *, const uint8_t *), uint32_t cookie)
{
	int s = oob ? 1 : 0;
	uint8_t *area = l->shmem + (oob ? 0x8000 : 0);
	uint32_t off = l->txn[s] ? l->tx[s][l->txn[s] - 1].end : 0;
	size_t used;
	unsigned ctx;
	struct live_cmd *c;
	if (l->state == LS_FAILED || l->txn[s] >= LIVE_TXDEPTH)
		return -1;
	used = iomfb_pkt_write(area, IOMFB_CHAN_SIZE, off, tag, in, in_len, out_len);
	if (!used) {
		fail(l, "live: command does not fit its channel");
		return -1;
	}
	c = &l->tx[s][l->txn[s]++];
	zero(c, sizeof(*c));
	cpy(c->tag, tag, 4);
	c->off = off;
	c->end = (uint32_t)((off + used + IOMFB_PKT_ALIGN - 1) & ~(size_t)(IOMFB_PKT_ALIGN - 1));
	c->in_len = in_len;
	c->out_len = out_len;
	c->done = done;
	c->cookie = cookie;
	/* nested in a deferred callback: the callback's context (iomfb.c dcp_call_context) */
	if (oob)
		ctx = l->ncb && l->cbs[l->ncb - 1].ctx == IOMFB_CTX_OOBCB ? IOMFB_CTX_OOBCB : IOMFB_CTX_OOBCMD;
	else
		ctx = l->ncb ? IOMFB_CTX_CB : IOMFB_CTX_CMD;
	l->cmd_count++;
	l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_rpc(ctx, (uint32_t)used, (uint16_t)off));
	return 0;
}

static void on_ack(struct live *l, unsigned ctx)
{
	int s = (ctx == IOMFB_CTX_OOBCMD || ctx == IOMFB_CTX_OOBCB) ? 1 : 0;
	struct live_cmd c;
	if (!l->txn[s]) {
		fail(l, "live: ack with no command outstanding");
		return;
	}
	c = l->tx[s][--l->txn[s]];
	if (c.done)
		c.done(l, &c, l->shmem + (s ? 0x8000 : 0) + c.off + IOMFB_HDR_SIZE + c.in_len);
}

/* ------------------------------------------------------------------ deferred callbacks */
static uint8_t *cb_out(struct live *l, struct live_cb *cb)
{
	return l->shmem + iomfb_channel_offset(cb->ctx) + cb->off + IOMFB_HDR_SIZE + cb->in_len;
}

static void cb_finish(struct live *l, int ok_byte)
{
	struct live_cb cb;
	if (!l->ncb)
		return;
	cb = l->cbs[--l->ncb];
	if (cb.out_len >= 1 && ok_byte >= 0)
		cb_out(l, &cb)[0] = (uint8_t)ok_byte;
	l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_ack(cb.ctx));
}

/* D000 did_boot_signal: nested A035 dfb_query, then out u8 1 (tpl.c:1664-1672) */
static void d000_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; cb_finish(l, 1); }
/* D100 match_pmu_service: nested A391 vi_set_temperature_hint, then ack, no out (tpl.c:531-542) */
static void d100_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; cb_finish(l, -1); }
/* D206 / D207: nested A136 / A137, then out u8 1 (tpl.c:688-722) */
static void d20x_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; cb_finish(l, 1); }
/* D586: nested A534 register_dfb_surface, ack true. Linux acks true only if A534 returned 0, but macOS
 * acked true with A534 = 1 (CONV seq 662-665; DEC §6 "Creating default FB surface failed"). */
static void d586_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; cb_finish(l, 1); }

/* D120 start_hardware_boot: the boot chain (tpl.c:1478-1580 fw27/H17 branch; tags V2) */
enum { B_A390, B_A504, B_A037, B_A036, B_A103, B_A503, B_A104, B_A105, B_A027, B_A529, B_A035, B_A000, B_A385, B_A526, B_DONE };
static void boot_step(struct live *l, uint32_t step);
static void boot_done_cmd(struct live *l, struct live_cmd *c, const uint8_t *o)
{
	uint32_t next = c->cookie + 1;
	if (c->cookie == B_A504 && !(o[0] & 1))
		next = B_A103;                                    /* no default fb: skip to the layout (tpl boot_1_5) */
	if (c->cookie == B_A036)
		l->dfb_format = g32(o);
	if (c->cookie == B_A503) {
		l->dfb_w = g32(o);
		l->dfb_h = g32(o + 4);
		if (l->dfb_w > 16384 || l->dfb_h > 16384) {      /* Linux fails the boot ack */
			cb_finish(l, 0);
			return;
		}
	}
	boot_step(l, next);
}
static void boot_step(struct live *l, uint32_t step)
{
	uint8_t z[0x2c];
	zero(z, sizeof(z));
	switch (step) {
	case B_A390: issue(l, 0, "A390", 0, 0, 0, boot_done_cmd, step); break;
	case B_A504: issue(l, 0, "A504", 0, 0, 4, boot_done_cmd, step); break;
	case B_A037: issue(l, 0, "A037", z, 8, 8, boot_done_cmd, step); break;
	case B_A036: issue(l, 0, "A036", z, 0x2c, 0x2c, boot_done_cmd, step); break;
	case B_A103: issue(l, 0, "A103", z, 0xc, 8, boot_done_cmd, step); break;
	case B_A503: issue(l, 0, "A503", z, 4, 8, boot_done_cmd, step); break;
	case B_A104: p32(z, l->dfb_w); p32(z + 4, l->dfb_h); issue(l, 0, "A104", z, 8, 0, boot_done_cmd, step); break;
	case B_A105: issue(l, 0, "A105", 0, 0, 0, boot_done_cmd, step); break;
	case B_A027: issue(l, 0, "A027", 0, 0, 0, boot_done_cmd, step); break;
	case B_A529: z[0] = 1; issue(l, 0, "A529", z, 4, 0, boot_done_cmd, step); break;
	case B_A035: issue(l, 0, "A035", 0, 0, 4, boot_done_cmd, step); break;
	/* A000 late_init_signal: macOS sends 1 (CONV seq 521); Linux sends profile==H17G */
	case B_A000: p32(z, 1); issue(l, 0, "A000", z, 4, 4, boot_done_cmd, step); break;
	case B_A385: issue(l, 0, "A385", 0, 0, 4, boot_done_cmd, step); break;
	case B_A526: issue(l, 0, "A526", 0, 0, 4, boot_done_cmd, step); break;
	default: cb_finish(l, 1); break;                      /* boot_done: out u8 1 */
	}
}

/* ------------------------------------------------------------------ immediate callbacks */
enum cbk { K_NOP, K_BOOL1, K_BOOL0, K_ZERO, K_SPECIAL, K_DEFER };
struct cbent { const char *tag; uint32_t in, out; uint8_t kind; };
/* Minimum sizes as Linux's callback_sizes[] (v27.c:434-515); kinds as v27.c cb_handlers[] (:348-432)
 * and the specials below. "seen" = in the macOS 27.3 capture (DEC §2.2). */
static const struct cbent cbtab[] = {
	{ "D000", 0, 4, K_DEFER },      /* seen */
	{ "D001", 0, 4, K_BOOL1 },      /* seen */
	{ "D002", 0, 0, K_NOP },
	{ "D003", 4, 20, K_SPECIAL },   /* seen: {0x286720000, 0, 0} */
	{ "D006", 84, 80, K_SPECIAL },  /* seen (ASYNC) */
	{ "D100", 0, 0, K_DEFER },      /* seen */
	{ "D101", 0, 4, K_ZERO },       /* seen */
	{ "D102", 68, 0, K_NOP },       /* seen */
	{ "D104", 68, 0, K_NOP },       /* seen */
	{ "D106", 64, 0, K_NOP },
	{ "D107", 0, 4, K_BOOL1 },      /* seen */
	{ "D108", 0, 4, K_BOOL1 },      /* seen */
	{ "D109", 0, 4, K_BOOL1 },      /* seen; macOS answered false (DEC §4.3), Linux true: Linux kept */
	{ "D110", 0, 4, K_BOOL1 },      /* seen */
	{ "D111", 0, 4, K_BOOL1 },      /* seen: internal panel -> true (tpl.c:2658-2671) */
	{ "D112", 0, 4, K_BOOL1 },      /* seen */
	{ "D113", 0x1044, 0x1004, K_SPECIAL }, /* CoreAnalytics: echo, never zeros (P3, P9) */
	{ "D114", 16, 8, K_SPECIAL },
	{ "D115", 16, 4, K_ZERO },
	{ "D116", 12, 0, K_NOP },
	{ "D117", 20, 4, K_ZERO },
	{ "D118", 0x3e88, 4, K_ZERO },
	{ "D119", 4, 4, K_ZERO },
	{ "D120", 0, 4, K_DEFER },      /* seen */
	{ "D121", 0, 4, K_BOOL0 },
	{ "D122", 0, 4, K_BOOL0 },      /* seen */
	{ "D123", 0, 4, K_ZERO },
	{ "D124", 100, 36, K_SPECIAL }, /* seen */
	{ "D126", 4, 4, K_SPECIAL },    /* seen */
	{ "D127", 4104, 4, K_SPECIAL }, /* seen */
	{ "D128", 64, 4, K_SPECIAL },   /* seen */
	{ "D129", 0x1c, 0x14, K_SPECIAL },
	{ "D130", 8, 0, K_NOP },
	{ "D131", 16, 12, K_SPECIAL },
	{ "D132", 4, 0, K_NOP },
	{ "D133", 0, 4, K_ZERO },       /* Linux defers with A375 (tag unverified on 27.3): we answer 0. INFERENCE */
	{ "D201", 12, 16, K_SPECIAL },  /* seen */
	{ "D202", 0x18, 0, K_SPECIAL },
	{ "D204", 0, 4, K_SPECIAL },    /* seen (ASYNC): 2 = P3 */
	{ "D206", 0, 4, K_DEFER },      /* seen */
	{ "D207", 0, 4, K_DEFER },      /* seen */
	{ "D208", 4, 0, K_NOP },
	{ "D209", 0, 8, K_SPECIAL },    /* seen */
	{ "D300", 16, 0, K_SPECIAL },   /* seen x8745 */
	{ "D400", 76, 3076, K_ZERO },   /* seen: macOS replied length 0 */
	{ "D401", 80, 12, K_ZERO },     /* seen: value 0 ret 0 */
	{ "D404", 0x48, 0, K_NOP },
	{ "D406", 72, 0, K_NOP },       /* seen */
	{ "D408", 8, 8, K_SPECIAL },    /* seen: 810 MHz */
	{ "D411", 16, 28, K_SPECIAL },
	{ "D413", 4168, 4, K_BOOL1 },   /* seen */
	{ "D414", 80, 4, K_SPECIAL },   /* seen */
	{ "D415", 76, 4, K_BOOL1 },     /* seen */
	{ "D451", 20, 20, K_SPECIAL },  /* seen x29 */
	{ "D452", 8, 4, K_SPECIAL },
	{ "D453", 8, 4, K_SPECIAL },
	{ "D454", 4, 4, K_SPECIAL },
	{ "D552", 4164, 4, K_BOOL1 },   /* seen */
	{ "D561", 4164, 4, K_BOOL1 },   /* seen */
	{ "D563", 76, 4, K_BOOL1 },     /* seen */
	{ "D565", 72, 4, K_BOOL1 },     /* seen */
	{ "D567", 0x80, 4, K_BOOL1 },
	{ "D569", 0x44, 4, K_SPECIAL },
	{ "D572", 4, 4, K_ZERO },
	{ "D574", 4, 8, K_SPECIAL },
	{ "D575", 88, 76, K_SPECIAL },  /* seen */
	{ "D576", 4, 0, K_NOP },
	{ "D586", 8, 4, K_DEFER },      /* seen */
	{ "D587", 0, 0, K_NOP },
	{ "D588", 0x18, 0, K_NOP },
	{ "D589", 0xe4, 0xe0, K_SPECIAL },
	{ "D592", 0, 0, K_NOP },
	{ "D594", 0x730, 0, K_SPECIAL },
	{ "D596", 0x14, 0, K_NOP },
	{ "D597", 4, 0, K_NOP },
	{ "D598", 4, 0, K_SPECIAL },
	{ "D599", 4, 0, K_NOP },
	{ "D601", 0, 4, K_BOOL0 },
	{ "D602", 0, 4, K_BOOL0 },
	{ "D603", 0, 0, K_NOP },        /* seen */
};
#define NCB (sizeof(cbtab) / sizeof(cbtab[0]))

static const struct cbent *cb_lookup(const char *tag)
{
	for (unsigned i = 0; i < NCB; i++)
		if (teq(cbtab[i].tag, tag))
			return &cbtab[i];
	return 0;
}

static struct live_memdesc *md_get(struct live *l, uint32_t id)
{
	return id < LIVE_MAX_MEMDESC && l->md[id].used ? &l->md[id] : 0;
}

/* returns 0 = acked now, 1 = deferred */
static int handle_special(struct live *l, const struct iomfb_pkt *p, struct live_cb *pend)
{
	const uint8_t *in = p->in;
	uint8_t *out = p->out;
	const char *t = p->tag;
	(void)pend;
	if (teq(t, "D003")) {                 /* rt_bandwidth_setup_ap (v27.c:110-121; dcp-link.h:786-797) */
		if (!(g32(in) & 1)) { p64(out, l->bw_scratch); p64(out + 8, 0); p32(out + 16, 0); }
	} else if (teq(t, "D006")) {          /* set_frame_sync_props: echo, then 0x10000 at 0x24/0x28/0x2c (tpl.c:1742-1760; macOS) */
		cpy(out, in, 80);
		p32(out + 0x24, 0x10000); p32(out + 0x28, 0x10000); p32(out + 0x2c, 0x10000);
	} else if (teq(t, "D113")) {          /* CoreAnalytics: value echoed unless null, ret 0 (P9) */
		if (!(in[0x1040] & 1))
			cpy(out, in + 0x40, 0x1000);
		out[0x1000] = 0;
	} else if (teq(t, "D114")) {          /* get_tiling_state: value = req.value, ret 0 (v27.c:310-332) */
		p32(out, g32(in + 8)); p32(out + 4, 0);
	} else if (teq(t, "D124")) {          /* read_edt_data: value[0] = req.value[0], ret 0 (tpl.c:1212-1226) */
		p32(out, g32(in + 0x44)); out[0x20] = 0;
	} else if (teq(t, "D126")) {
		uint32_t len = g32(in);
		int ok = !l->prop_active && len > 0 && len <= IOMFB_PROP_MAX;
		l->prop_active = ok; l->prop_len = ok ? len : 0; l->prop_recv = 0; l->prop_contig = 1;
		if (ok) zero(l->prop, len);
		out[0] = (uint8_t)ok;
	} else if (teq(t, "D127")) {
		uint32_t off = g32(in + 0x1000), len = g32(in + 0x1004);
		int ok = l->prop_active && off <= l->prop_len && len <= 0x1000 && len <= l->prop_len - off;
		if (ok) {
			if (off != l->prop_recv) l->prop_contig = 0; else l->prop_recv += len;
			cpy(l->prop + off, in, len);
		} else {
			l->prop_contig = 0;
		}
		out[0] = (uint8_t)ok;
	} else if (teq(t, "D128")) {
		int ok = l->prop_active && l->prop_contig && l->prop_recv == l->prop_len;
		if (ok && keq(in, 0x40, "TimingElements")) {
			struct iomfb_timing tm[IOMFB_MAX_TIMINGS];
			int n = iomfb_parse_timings(l->prop, l->prop_len, tm, IOMFB_MAX_TIMINGS);
			if (n > 0) { cpy(l->timings, tm, sizeof(tm[0]) * (uint32_t)n); l->ntimings = n; } else ok = 0;
		}
		l->prop_active = 0;
		out[0] = (uint8_t)ok;
	} else if (teq(t, "D129")) {          /* allocate_bandwidth: {req.unk1, req.unk2, 1} */
		p64(out, g64(in)); p64(out + 8, g64(in + 8)); p32(out + 16, 1);
	} else if (teq(t, "D131")) {          /* get_external_counts: internal panel -> zeros */
		zero(out, 12);
	} else if (teq(t, "D201")) {          /* map_piodma: map the memdesc into dart-disp0 SID 16 (tpl.c:868-933; macOS own DVA space) */
		uint32_t id = (uint32_t)g64(in);
		struct live_memdesc *m = md_get(l, id);
		p32(out, id);
		if (m && !m->piodma_dva && l->dart_piodma)
			m->piodma_dva = dart4_map(l->dart_piodma, m->pa, m->len);
		p64(out + 4, m ? m->piodma_dva : 0);
		p32(out + 12, 0);
	} else if (teq(t, "D202")) {          /* unmap_piodma */
		uint32_t id = (uint32_t)g64(in);
		struct live_memdesc *m = md_get(l, id);
		if (m && m->piodma_dva && l->dart_piodma) { dart4_unmap(l->dart_piodma, m->piodma_dva, m->len); m->piodma_dva = 0; }
	} else if (teq(t, "D204")) {          /* get_ideal_screen_space: P3 -> 2 (v27.c:293-308; macOS 2) */
		p32(out, 2);
	} else if (teq(t, "D209")) {          /* calendar ms since the epoch (resident clock if it has one, else 0) */
		p64(out, l->io.now_ns ? l->io.now_ns(l->io.ctx) / 1000000ull : 0);
	} else if (teq(t, "D300")) {          /* pr_publish: id 20 = panel level 16.16 nits (DEC:225) */
		if (g32(in) == 20 && g32(in + 4))
			l->bl_nits = (g32(in + 4) + 0x8000) >> 16;
	} else if (teq(t, "D408")) {          /* getClockFrequency: PROV clock 0 = 810 MHz (macOS), others 0 */
		p64(out, g32(in + 4) == 0 ? l->clock0_hz : 0);
	} else if (teq(t, "D411")) {          /* map_reg: dva = addr = disp window, length, ret (tpl.c:1164-1210) */
		uint32_t idx = g32(in + 4);
		if (idx < l->ndisp_regs) {
			p64(out, l->disp_regs[idx]); p64(out + 8, l->disp_regs[idx]); p64(out + 16, l->disp_reg_len[idx]); p32(out + 24, 0);
		} else {
			p32(out + 24, 1);
		}
	} else if (teq(t, "D414")) {          /* sr_setProperty_int: Brightness_Scale; DPTimingModeId (fw27 current timing, DEC:221) */
		if (!in[0x4c]) {
			if (keq(in + 4, 0x40, "Brightness_Scale")) l->brightness_scale = (uint32_t)g64(in + 0x44);
			if (keq(in + 4, 0x40, "DPTimingModeId")) l->cur_timing = (uint32_t)g64(in + 0x44);
		}
		out[0] = 1;
	} else if (teq(t, "D451")) {          /* allocate_buffer (v27.c:248-271; tpl.c:947-1001) */
		uint64_t size = g64(in + 4);
		uint32_t align = g32(in + 12), id;
		uint64_t len = (size + 0x3fff) & ~0x3fffull;
		if (!size || size > (1ull << 30) || align > 0x4000 || (align && (align & (align - 1))))
			return 0;                     /* zeros, acked */
		for (id = 1; id < LIVE_MAX_MEMDESC && l->md[id].used; id++)
			;                             /* ids from 1, as macOS (CONV seq 524) */
		if (id >= LIVE_MAX_MEMDESC)
			return 0;
		{
			uint64_t pa = l->io.alloc(l->io.ctx, len);
			uint64_t dva = pa ? dart4_map(l->dart_dcp, pa, len) : 0;
			if (!dva)
				return 0;
			l->md[id].pa = pa; l->md[id].len = len; l->md[id].dva = dva; l->md[id].used = 1;
			if (!in[16]) p64(out, dva);
			if (!in[17]) p64(out + 8, len);
			p32(out + 16, id);
		}
	} else if (teq(t, "D452") || teq(t, "D453")) {
		p32(out, md_get(l, g32(in)) ? 0 : 14);
	} else if (teq(t, "D454")) {          /* release_mem_desc */
		struct live_memdesc *m = md_get(l, g32(in));
		if (m) {
			if (m->piodma_dva && l->dart_piodma) dart4_unmap(l->dart_piodma, m->piodma_dva, m->len);
			dart4_unmap(l->dart_dcp, m->dva, m->len);
			zero(m, sizeof(*m));          /* pages stay with the resident pool (not reused while the DCP lives) */
			out[0] = 1;
		}
	} else if (teq(t, "D569")) {
		p32(out, 1);
	} else if (teq(t, "D574")) {          /* video_range {0x435b0000, 0} (v27.c:177) */
		p32(out, 0x435b0000); p32(out + 4, 0);
	} else if (teq(t, "D575")) {          /* hotplug: echo tiled data if present, else zeros */
		if (!(in[0x54] & 1)) cpy(out, in + 8, 0x4c);
	} else if (teq(t, "D589")) {          /* swap_info echo */
		if (!(in[0xe0] & 1)) cpy(out, in, 0xe0);
	} else if (teq(t, "D594")) {          /* swap_complete */
		uint32_t id = g32(in);
		if (id && id == l->swap_inflight) {
			l->swap_inflight = 0;
			l->swaps_done++;
			if (l->dev) {
				dcpw_backend_vblank(l->dev, l->io.now_ns ? l->io.now_ns(l->io.ctx) : 0);
				dcpw_backend_present_done(l->dev);
			}
		}
	} else if (teq(t, "D598")) {          /* enable_backlight_message: resend the level with the next swap */
		l->bl_pending = 1;
	}
	return 0;
}

static int handle_defer(struct live *l, const struct iomfb_pkt *p, struct live_cb *cb)
{
	const char *t = p->tag;
	if (l->ncb >= LIVE_TXDEPTH)
		return -1;
	l->cbs[l->ncb++] = *cb;
	if (teq(t, "D000")) return issue(l, 0, "A035", 0, 0, 4, d000_done, 0);
	if (teq(t, "D100")) return issue(l, 0, "A391", 0, 0, 4, d100_done, 0);
	if (teq(t, "D206")) return issue(l, 0, "A136", 0, 0, 4, d20x_done, 0);
	if (teq(t, "D207")) return issue(l, 0, "A137", 0, 0, 4, d20x_done, 0);
	if (teq(t, "D120")) { boot_step(l, B_A390); return 0; }
	if (teq(t, "D586")) {                 /* register the default fb surface (tpl.c:640-672 fw27 branch) */
		uint8_t s[0x230];
		uint32_t w = g32(p->in), h = g32(p->in + 4);
		zero(s, sizeof(s));
		s[IOMFB_SF_PREMULT] = 1;
		p32(s + IOMFB_SF_FORMAT, l->dfb_format);
		s[IOMFB_SF_PIX_SIZE] = 1; s[IOMFB_SF_PEL_W] = 1; s[IOMFB_SF_PEL_H] = 1;
		p32(s + IOMFB_SF_WIDTH, w); p32(s + IOMFB_SF_HEIGHT, h);
		/* buf_size: b3a8 two-plane layout (iomfb-dfb.h); 3456x2234 -> 0x025ec000 (DEC:272) */
		p32(s + IOMFB_SF_BUF_SIZE, (w == 3456 && h == 2234) ? 0x025ec000u : ((w * 4 + w) * h + 0x3fff) & ~0x3fffu);
		p32(s + 0x35, 5);                 /* surface_id (Linux 5; macOS 3) */
		p64(s + 0x51, 1);                 /* has_comp */
		p64(s + 0x149, 1);                /* has_planes */
		return issue(l, 0, "A534", s, 0x230, 4, d586_done, 0);
	}
	l->ncb--;
	return -1;
}

static void on_callback(struct live *l, const struct iomfb_msg *m)
{
	int base = iomfb_channel_offset(m->ctx);
	struct iomfb_pkt p;
	const struct cbent *e;
	if (base < 0 || iomfb_pkt_read(l->shmem + base, IOMFB_CHAN_SIZE, m->offset, m->length, &p)) {
		fail(l, "live: malformed callback record");
		return;
	}
	l->cb_count++;
	for (uint32_t i = 0; i < p.out_len; i++)
		p.out[i] = 0;                     /* iomfb.c:1480 */
	e = p.tag[0] == 'D' ? cb_lookup(p.tag) : 0;
	if (!e) {                             /* unknown: zeroed out + ack, as Linux (iomfb.c:1461-1480); counted */
		l->cb_unknown++;
		cpy(l->last_unknown, p.tag, 5);
		l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_ack(m->ctx));
		return;
	}
	if (p.in_len < e->in || p.out_len < e->out) {
		/* Linux marks the DCP crashed (iomfb.c:1428-1440); we answer zeros so the DCP is not wedged */
		l->cb_unknown++;
		cpy(l->last_unknown, p.tag, 5);
		l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_ack(m->ctx));
		return;
	}
	switch (e->kind) {
	case K_BOOL1: if (p.out_len) p.out[0] = 1; break;
	case K_BOOL0: case K_ZERO: case K_NOP: break;
	case K_SPECIAL: handle_special(l, &p, 0); break;
	case K_DEFER: {
		struct live_cb cb;
		zero(&cb, sizeof(cb));
		cb.ctx = m->ctx; cb.off = m->offset; cb.in_len = p.in_len; cb.out_len = p.out_len;
		cpy(cb.tag, p.tag, 5);
		if (handle_defer(l, &p, &cb) == 0)
			return;                       /* acked later by cb_finish */
		fail(l, "live: could not defer a callback");
		return;
	}
	}
	l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_ack(m->ctx));
}

void live_rx(struct live *l, uint8_t ep, uint64_t w)
{
	struct iomfb_msg m;
	if (ep != IOMFB_EP) {
		say(l, "live: message on another endpoint ignored");
		return;
	}
	if (iomfb_msg_decode(w, &m)) {
		fail(l, "live: undecodable IOMFB message");
		return;
	}
	if (m.type == IOMFB_T_INITIALIZED) {
		if (l->state == LS_WAIT_INIT)
			l->state = LS_STARTING;
		return;
	}
	if (m.type != IOMFB_T_MSG)
		return;
	if (m.ack) {
		on_ack(l, m.ctx);
		return;
	}
	if (m.ctx == IOMFB_CTX_CB || m.ctx == IOMFB_CTX_ASYNC || m.ctx == IOMFB_CTX_OOBCB || m.ctx == IOMFB_CTX_OOBASYNC)
		on_callback(l, &m);
	else
		fail(l, "live: DCP message on an AP context");
}

int live_wait_idle(struct live *l, uint32_t polls)
{
	for (uint32_t i = 0; i < polls && l->state != LS_FAILED; i++) {
		if (!l->txn[0] && !l->txn[1] && !l->ncb)
			return 0;
		l->io.pump(l->io.ctx);
	}
	if (l->state != LS_FAILED) {
		l->timeouts++;
		fail(l, "live: DCP did not answer in time");
	}
	return -1;
}

/* ------------------------------------------------------------------ attach */
static void a451_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; if (l->state == LS_STARTING) l->state = LS_STARTED; }

int live_attach(struct live *l)
{
	if (l->stage == LIVE_OFF || !l->dart_dcp)
		return -1;
	/* endpoints 0x2d (remote alloc) then 0x37 are started by the caller (m1n1 rtkit_start_ep), REF/iomfb.c:3110-3132 */
	l->shmem_pa = l->io.alloc(l->io.ctx, IOMFB_SHMEM_SIZE);
	l->shmem = l->shmem_pa ? l->io.ptr(l->io.ctx, l->shmem_pa) : 0;
	l->shmem_dva = l->shmem ? dart4_map(l->dart_dcp, l->shmem_pa, IOMFB_SHMEM_SIZE) : 0;
	if (!l->shmem_dva) {
		fail(l, "live: no shared memory");
		return -1;
	}
	l->state = LS_WAIT_INIT;
	l->io.send(l->io.ctx, IOMFB_EP, iomfb_msg_set_shmem(l->shmem_dva));
	for (uint32_t i = 0; i < l->timeout_polls && l->state == LS_WAIT_INIT; i++)
		l->io.pump(l->io.ctx);
	if (l->state != LS_STARTING) {
		fail(l, "live: no INITIALIZED after SET_SHMEM");
		return -1;
	}
	/* A451 start_signal on CMD; it is acked only after the whole boot (macOS 4.9 s) */
	if (issue(l, 0, "A451", 0, 0, 4, a451_done, 0))
		return -1;
	if (live_wait_idle(l, l->timeout_polls))
		return -1;
	return l->state == LS_STARTED && l->ntimings > 0 ? 0 : (fail(l, "live: started without modes"), -1);
}

/* ------------------------------------------------------------------ client open + power (LIVE_FLIP) */
static void open_step(struct live *l, struct live_cmd *c, const uint8_t *o);
static void open_issue(struct live *l, uint32_t step)
{
	uint8_t z[0x6c];
	/* dcp_started + dcp_client_open_run (tpl.c:3664-3796), 27.3 tags (V2); A033 sets cumulative */
	static const uint8_t sets[9][8] = { {0}, {6}, {6, 7}, {6, 7, 8}, {6, 7, 8, 10}, {6, 7, 8, 10, 11},
		{6, 7, 8, 10, 11, 12}, {6, 7, 8, 10, 11, 12, 25}, {6, 7, 8, 10, 11, 12, 25, 26} };
	zero(z, sizeof(z));
	l->open_step = step;
	switch (step) {
	case 0: p32(z, 6); issue(l, 0, "A477", z, 8, 8, open_step, step); break;   /* get_color_remap_mode {6} */
	case 1: issue(l, 0, "A503", z, 4, 8, open_step, step); break;
	case 2: issue(l, 0, "A461", z, 4, 4, open_step, step); break;              /* set_display_device 0, once */
	case 3: issue(l, 0, "A503", z, 4, 8, open_step, step); break;
	case 4: issue(l, 0, "A465", z, 0xc, 0xc, open_step, step); break;
	case 5: issue(l, 0, "A508", z, 4, 4, open_step, step); break;
	default:
		if (step >= 6 && step < 15) {                                          /* A033 x9 */
			for (int i = 0; i < 8 && sets[step - 6][i]; i++)
				p32(z + 4 * (uint32_t)i, sets[step - 6][i]);
			issue(l, 0, "A033", z, 0x6c, 0, open_step, step);
		} else switch (step) {
		case 15: issue(l, 0, "A464", z, 8, 8, open_step, step); break;
		case 16: issue(l, 0, "A540", 0, 0, 4, open_step, step); break;
		case 17: issue(l, 0, "A516", 0, 0, 0, open_step, step); break;     /* first_client_open */
		case 18: issue(l, 0, "A536", 0, 0, 4, open_step, step); break;
		case 19: issue(l, 0, "A540", 0, 0, 4, open_step, step); break;
		case 20: p32(z, 1); issue(l, 0, "A538", z, 4, 0, open_step, step); break;
		case 21: issue(l, 0, "A540", 0, 0, 4, open_step, step); break;
		case 22: issue(l, 0, "A462", 0, 0, 4, open_step, step); break;     /* is_main_display */
		case 23:                                                            /* A535 set_power_state on OOBCMD {1,0,0} */
			p64(z, 1);
			issue(l, 1, "A535", z, 0x10, 8, open_step, step);
			break;
		default: l->state = LS_READY; break;
		}
	}
}
static void open_step(struct live *l, struct live_cmd *c, const uint8_t *o)
{
	if (c->cookie == 22)
		l->main_display = o[0] & 1;
	open_issue(l, c->cookie + 1);
}

int live_open(struct live *l)
{
	if (l->stage < LIVE_FLIP || l->state != LS_STARTED)
		return -1;
	l->state = LS_OPENING;
	open_issue(l, 0);
	if (live_wait_idle(l, l->timeout_polls))
		return -1;
	return l->state == LS_READY ? 0 : -1;
}

/* ------------------------------------------------------------------ DCPW backend */
static int lb_get_modes(void *ctx, struct dcpw_mode *out, uint32_t max, uint32_t *cur)
{
	struct live *l = ctx;
	return l->ntimings > 0 ? iomfb_timings_to_modes(l->timings, l->ntimings, out, max, l->cur_timing, cur) : -1;
}

static void cmd_done_store(struct live *l, struct live_cmd *c, const uint8_t *o)
{
	(void)l;
	c->cookie = 0;
	(void)o;
}

static uint8_t g_lastout[16];
static void cmd_done_copy(struct live *l, struct live_cmd *c, const uint8_t *o)
{
	(void)l;
	cpy(g_lastout, o, c->out_len < 16 ? c->out_len : 16);
}

static int lb_set_mode(void *ctx, const struct dcpw_mode *m, int vrr_on)
{
	struct live *l = ctx;
	uint8_t req[0x28];
	if (l->stage < LIVE_FULL || l->state != LS_READY)
		return -1;
	if (m->flags & DCPW_MODE_VRR) {      /* A500 param 15 first, then A463 (vrr/0002:228-235) */
		iomfb_build_adaptive_sync(req, vrr_on ? (uint32_t)(((uint64_t)m->vrr_min_mhz << 16) / 1000) : 0);
		zero(g_lastout, sizeof(g_lastout));
		if (issue(l, 0, "A500", req, 0x28, 4, cmd_done_copy, 0) || live_wait_idle(l, l->timeout_polls) || g32(g_lastout))
			return -1;
	} else if (vrr_on) {
		return -1;
	}
	iomfb_build_digital_out(req, 1, m->timing_id);
	zero(g_lastout, sizeof(g_lastout));
	if (issue(l, 0, "A463", req, 8, 4, cmd_done_copy, 0) || live_wait_idle(l, l->timeout_polls) || g32(g_lastout))
		return -1;
	l->cur_timing = m->timing_id;
	return 0;
}

static int lb_swap(struct live *l, const struct dcpw_present *p, int with_bl)
{
	static uint8_t wire[IOMFB_SWAP_SUBMIT_LEN];
	uint8_t sin[0x10];
	uint64_t len = ((uint64_t)p->stride * p->height + 0x3fff) & ~0x3fffull, iova;
	uint32_t id;
	if (l->stage < LIVE_FLIP || l->state != LS_READY || l->swap_inflight || !l->dart_disp)
		return -1;
	/* map the guest surface into dart-disp0 SID 0 once; remap when it moves */
	if (!l->have_last || l->last.gpa != p->gpa || l->last.stride != p->stride || l->last.height != p->height) {
		iova = dart4_map(l->dart_disp, p->gpa, len);
		if (!iova)
			return -1;
		l->md[0].pa = p->gpa; l->md[0].dva = iova; l->md[0].len = len;   /* slot 0 = scanout surface */
	} else {
		iova = l->md[0].dva;
	}
	zero(sin, sizeof(sin));
	zero(g_lastout, sizeof(g_lastout));
	if (issue(l, 0, "A456", sin, 0x10, 8, cmd_done_copy, 0) || live_wait_idle(l, l->timeout_polls))
		return -1;
	id = g32(g_lastout);
	if (!id || g32(g_lastout + 4))
		return -1;
	{
		struct iomfb_swap_desc d = { id, iova, p->width, p->height, p->stride, with_bl, l->bl_nits };
		if (iomfb_build_swap_submit(wire, &d))
			return -1;
	}
	l->swap_inflight = id;
	zero(g_lastout, sizeof(g_lastout));
	if (issue(l, 0, "A458", wire, IOMFB_SWAP_SUBMIT_LEN, 0xc, cmd_done_copy, 0) || live_wait_idle(l, l->timeout_polls) ||
	    g32(g_lastout + 5)) {
		l->swap_inflight = 0;
		return -1;
	}
	l->last = *p;
	l->have_last = 1;
	(void)cmd_done_store;
	return 0;
}

static int lb_present(void *ctx, const struct dcpw_present *p)
{
	struct live *l = ctx;
	int bl = l->bl_pending && l->stage >= LIVE_FULL;
	if (lb_swap(l, p, bl))
		return -1;
	if (bl)
		l->bl_pending = 0;
	return 0;
}

static int lb_set_brightness(void *ctx, uint32_t mnits)
{
	struct live *l = ctx;
	if (l->stage < LIVE_FULL)
		return -1;
	l->bl_nits = (mnits + 500) / 1000;
	l->bl_pending = 1;
	if (!l->have_last || l->swap_inflight)
		return 0;
	{
		struct dcpw_present p = l->last;
		if (lb_swap(l, &p, 1))
			return -1;
		l->bl_pending = 0;
	}
	return 0;
}

void live_backend(struct live *l, struct dcpw_backend *be)
{
	be->ctx = l;
	be->caps = 0;
	if (l->stage >= LIVE_FLIP)
		be->caps |= DCPW_CAP_FLIP | DCPW_CAP_HW_VBLANK;
	if (l->stage >= LIVE_FULL)
		be->caps |= DCPW_CAP_MODESET | DCPW_CAP_VRR | DCPW_CAP_BRIGHTNESS;
	be->get_modes = lb_get_modes;
	be->set_mode = lb_set_mode;
	be->set_brightness = lb_set_brightness;
	be->present = lb_present;
}
