/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fake_live.c -- a scripted DCP for iomfb_live: speaks IOMFB on a shared memory it finds THROUGH the
 * DART tables the live session wrote (so the DART mapping is tested too), replays the macOS 27.3 boot
 * conversation shape (DEC §2.2/§4, CONV), checks every AP command's tag and sizes against the 27.3
 * table (V2) and every callback reply against what Linux/macOS answer. Our code, test only.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fake_live.h"

static uint32_t g32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t g64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void p64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

#define BAD(...) do { f->errors++; if (f->errors < 20) { printf("  fake: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void q(struct fake_live *f, uint64_t m)
{
	if (f->qn >= FL_QMAX) { BAD("queue overflow"); return; }
	f->qm[f->qn++] = m;
}

/* 27.3 AP method sizes (V2 table; sizes REF/iomfb*.h and DEC) */
static const struct { const char *t; uint32_t in, out; } meth[] = {
	{ "A000", 4, 4 }, { "A027", 0, 0 }, { "A033", 0x6c, 0 }, { "A035", 0, 4 }, { "A036", 0x2c, 0x2c },
	{ "A037", 8, 8 }, { "A103", 0xc, 8 }, { "A104", 8, 0 }, { "A105", 0, 0 }, { "A136", 0, 4 },
	{ "A137", 0, 4 }, { "A385", 0, 4 }, { "A390", 0, 0 }, { "A391", 0, 4 }, { "A451", 0, 4 },
	{ "A456", 0x10, 8 }, { "A458", 0x1144, 0xc }, { "A461", 4, 4 }, { "A462", 0, 4 }, { "A463", 8, 4 },
	{ "A464", 8, 8 }, { "A465", 0xc, 0xc }, { "A477", 8, 8 }, { "A500", 0x28, 4 }, { "A503", 4, 8 },
	{ "A504", 0, 4 }, { "A508", 4, 4 }, { "A516", 0, 0 }, { "A526", 0, 4 }, { "A529", 4, 0 },
	{ "A534", 0x230, 4 }, { "A535", 0x10, 8 }, { "A536", 0, 4 }, { "A538", 4, 0 }, { "A540", 0, 4 },
};

static uint8_t *dva_ptr(struct fake_live *f, struct dart4 *d, uint64_t dva, uint64_t len)
{
	uint64_t pa = dart4_translate(d, dva), pa2 = dart4_translate(d, dva + len - 1);
	if (!pa || pa2 != pa + len - 1) return 0;   /* contiguous in our fake RAM */
	return f->ptr(f->ctx, pa);
}

/* send a DCP->AP callback on context ctx; nested ones stack after the record below them */
static void callback(struct fake_live *f, unsigned ctx, const char *tag, const void *in, uint32_t in_len, uint32_t out_len)
{
	int base = iomfb_channel_offset(ctx);
	uint32_t off = ctx == IOMFB_CTX_CB ? 0x100 : 0;   /* the DCP chooses; a non-zero start exercises placement */
	for (int i = 0; i < f->cbn; i++)
		if (f->cbs[i].ctx == ctx) off = f->cbs[i].end;
	size_t used = iomfb_pkt_write(f->shm + base, IOMFB_CHAN_SIZE, off, tag, in, in_len, out_len);
	memset(f->shm + base + off + 12 + in_len, 0xa5, out_len);   /* the AP must zero it */
	struct fl_cb *c = &f->cbs[f->cbn++];
	memcpy(c->tag, tag, 5);
	c->ctx = ctx; c->off = off; c->in_len = in_len; c->out_len = out_len;
	c->end = (uint32_t)((off + used + 63) & ~63u);
	q(f, iomfb_msg_rpc(ctx, (uint32_t)used, (uint16_t)off));
}

static const uint8_t *cb_reply(struct fake_live *f, struct fl_cb *c)
{
	return f->shm + iomfb_channel_offset(c->ctx) + c->off + 12 + c->in_len;
}

static void key(uint8_t *k, const char *s) { memset(k, 0, 0x40); strncpy((char *)k, s, 0x3f); }

/* ---------------------------------------------------------------- the boot script */
enum { S_D603, S_D107, S_D111, S_D414A, S_D414B, S_D575, S_D300, S_D999, S_D100, S_D206, S_D207, S_D000, S_D120,
       S_D113, S_D006, S_D586, S_P0, S_P1, S_P2, S_P3, S_P4, S_P5, S_P6, S_P7, S_D122, S_END };

static void script_next(struct fake_live *f);

static void dcpav(struct fake_live *f, const char *k, const uint8_t *blob, uint32_t len, int phase)
{
	uint8_t b[0x1008];
	if (phase == 0) { p32(b, len); callback(f, IOMFB_CTX_CB, "D126", b, 4, 4); return; }
	if (phase == 1) {
		uint32_t off = f->chunk_off, n = len - off > 0x1000 ? 0x1000 : len - off;
		memset(b, 0, sizeof(b)); memcpy(b, blob + off, n); p32(b + 0x1000, off); p32(b + 0x1004, n);
		f->chunk_off += n;
		callback(f, IOMFB_CTX_CB, "D127", b, 0x1008, 4);
		return;
	}
	key(b, k);
	callback(f, IOMFB_CTX_CB, "D128", b, 0x40, 4);
}

static void script_send(struct fake_live *f)
{
	uint8_t b[0x1044];
	memset(b, 0, sizeof(b));
	switch (f->step) {
	case S_D603: callback(f, IOMFB_CTX_CB, "D603", 0, 0, 0); break;
	case S_D107: callback(f, IOMFB_CTX_CB, "D107", 0, 0, 4); break;
	case S_D111: callback(f, IOMFB_CTX_CB, "D111", 0, 0, 4); break;
	case S_D414A: memcpy(b, "FMOI", 4); key(b + 4, "DPTimingModeId"); p64(b + 0x44, 2); callback(f, IOMFB_CTX_CB, "D414", b, 80, 4); break;
	case S_D414B: memcpy(b, "FMOI", 4); key(b + 4, "Brightness_Scale"); p64(b + 0x44, 0x10000); callback(f, IOMFB_CTX_CB, "D414", b, 80, 4); break;
	case S_D575: p64(b, 1); callback(f, IOMFB_CTX_CB, "D575", b, 88, 76); break;
	case S_D300: p32(b, 20); p32(b + 4, 0x012c0000); callback(f, IOMFB_CTX_CB, "D300", b, 16, 0); break;
	case S_D999: callback(f, IOMFB_CTX_CB, "D999", b, 8, 4); break;   /* unknown: zeroed reply + ack */
	case S_D100: callback(f, IOMFB_CTX_CB, "D100", 0, 0, 0); break;
	case S_D206: callback(f, IOMFB_CTX_CB, "D206", 0, 0, 4); break;
	case S_D207: callback(f, IOMFB_CTX_CB, "D207", 0, 0, 4); break;
	case S_D000: callback(f, IOMFB_CTX_CB, "D000", 0, 0, 4); break;
	case S_D120: callback(f, IOMFB_CTX_CB, "D120", 0, 0, 4); break;
	case S_D113: key(b, "com.apple.dcp.test"); for (int i = 0; i < 0x1000; i++) b[0x40 + i] = (uint8_t)(i * 7 + 1);
		callback(f, IOMFB_CTX_CB, "D113", b, 0x1044, 0x1004); break;
	case S_D006: for (int i = 0; i < 84; i++) b[i] = (uint8_t)i; callback(f, IOMFB_CTX_ASYNC, "D006", b, 84, 80); break;
	case S_D586: p32(b, 3456); p32(b + 4, 2234); callback(f, IOMFB_CTX_CB, "D586", b, 8, 4); break;
	case S_P0: f->chunk_off = 0; dcpav(f, "TimingElements", f->blob, f->blob_len, 0); break;
	case S_P1: case S_P2: case S_P3: dcpav(f, "TimingElements", f->blob, f->blob_len, 1); break;
	case S_P4: dcpav(f, "TimingElements", f->blob, f->blob_len, 2); break;
	case S_P5: p32(b, 16); callback(f, IOMFB_CTX_CB, "D126", b, 4, 4); break;      /* ColorElements: not parsed */
	case S_P6: memset(b, 0, 0x1008); p32(b + 0x1004, 16); callback(f, IOMFB_CTX_CB, "D127", b, 0x1008, 4); break;
	case S_P7: key(b, "ColorElements"); callback(f, IOMFB_CTX_CB, "D128", b, 0x40, 4); break;
	case S_D122: callback(f, IOMFB_CTX_CB, "D122", 0, 0, 4); break;
	case S_END: /* the boot is over: ack A451 */
		f->booted = 1;
		q(f, iomfb_msg_ack(IOMFB_CTX_CMD));
		f->a451_open = 0;
		break;
	}
}

static void check_reply(struct fake_live *f, struct fl_cb *c)
{
	const uint8_t *o = cb_reply(f, c);
	const char *t = c->tag;
	f->replies++;
	if (!strcmp(t, "D107") || !strcmp(t, "D111") || !strcmp(t, "D000") || !strcmp(t, "D206") || !strcmp(t, "D207") ||
	    !strcmp(t, "D120") || !strcmp(t, "D586") || !strcmp(t, "D414")) {
		if (o[0] != 1 || o[1] || o[2] || o[3]) BAD("%s reply %02x %02x %02x %02x, want 01 00 00 00", t, o[0], o[1], o[2], o[3]);
	}
	if (!strcmp(t, "D999") && g32(o) != 0) BAD("unknown callback reply not zeroed");
	if (!strcmp(t, "D122") && o[0] != 0) BAD("D122 should be false");
	if (!strcmp(t, "D575")) { for (int i = 0; i < 76; i++) if (o[i]) { BAD("D575 reply not zero"); break; } }
	if (!strcmp(t, "D113")) {
		const uint8_t *in = f->shm + iomfb_channel_offset(c->ctx) + c->off + 12;
		if (memcmp(o, in + 0x40, 0x1000) || o[0x1000]) BAD("D113 analytics value not echoed");
	}
	if (!strcmp(t, "D006")) {
		if (g32(o + 0x24) != 0x10000 || g32(o + 0x28) != 0x10000 || g32(o + 0x2c) != 0x10000 || o[0] != 0 || o[1] != 1)
			BAD("D006 frame sync reply wrong");
	}
	if (!strcmp(t, "D003")) {
		if (g64(o) != 0x286720000ull || g64(o + 8) || g32(o + 16)) BAD("D003 bandwidth reply %llx", (unsigned long long)g64(o));
	}
	if (!strcmp(t, "D126") || !strcmp(t, "D127") || !strcmp(t, "D128")) {
		if (o[0] != 1) BAD("%s refused", t);
	}
	if (!strcmp(t, "D451")) {
		uint64_t dva = g64(o), cap = g64(o + 8);
		uint32_t id = g32(o + 16);
		uint8_t *p = dva_ptr(f, f->dart_dcp, dva, cap);
		if (!dva || !p || cap < f->last_alloc || !id) BAD("D451 buffer not mapped in dart-dcp (dva %llx cap %llx id %u)",
		                                                (unsigned long long)dva, (unsigned long long)cap, id);
		else { memset(p, 0x5a, (size_t)f->last_alloc); f->ids[f->nids] = id; f->dvas[f->nids++] = dva; }
	}
	if (!strcmp(t, "D201")) {
		uint64_t pd = g64(o + 4);
		uint32_t id = g32(o);
		uint64_t pa1 = dart4_translate(f->dart_piodma, pd), pa2 = 0;
		for (int i = 0; i < f->nids; i++) if (f->ids[i] == id) pa2 = dart4_translate(f->dart_dcp, f->dvas[i]);
		if (!pd || !pa1 || pa1 != pa2) BAD("D201 piodma DVA does not reach the same buffer");
	}
}

static void on_cb_ack(struct fake_live *f, unsigned ctx)
{
	int i;
	for (i = f->cbn - 1; i >= 0 && f->cbs[i].ctx != ctx; i--) ;
	if (i < 0) { BAD("ack on ctx %u with no callback outstanding", ctx); return; }
	if (i != f->cbn - 1 && f->cbs[f->cbn - 1].ctx == ctx) BAD("ack order");
	struct fl_cb c = f->cbs[i];
	memmove(&f->cbs[i], &f->cbs[i + 1], sizeof(f->cbs[0]) * (size_t)(f->cbn - i - 1));
	f->cbn--;
	check_reply(f, &c);
	if (c.nested_script) { f->nest_step++; return; }       /* inside A000: next nested callback */
	if (f->a451_open && !c.inner) script_next(f);
}

/* ---------------------------------------------------------------- A000: nested memory callbacks */
static void nest_send(struct fake_live *f)
{
	uint8_t b[32];
	memset(b, 0, sizeof(b));
	switch (f->nest_step) {
	case 0: callback(f, IOMFB_CTX_CB, "D003", b, 4, 20); break;
	case 1: case 3: case 5: {
		uint64_t sz = f->nest_step == 5 ? 0x1000 : 0x40000;
		f->last_alloc = sz;
		p32(b, 0x703); p64(b + 4, sz); p32(b + 12, 0x1000);
		callback(f, IOMFB_CTX_CB, "D451", b, 20, 20);
		break;
	}
	case 2: case 4: case 6: p64(b, f->ids[f->nids - 1]); b[8] = 1; callback(f, IOMFB_CTX_CB, "D201", b, 12, 16); break;
	default: q(f, iomfb_msg_ack(IOMFB_CTX_CB)); f->a000_open = 0; return;    /* A000 done (it was nested on CB) */
	}
	f->cbs[f->cbn - 1].nested_script = 1;
}

/* ---------------------------------------------------------------- AP commands */
static void on_cmd(struct fake_live *f, const struct iomfb_msg *m)
{
	int oob = m->ctx == IOMFB_CTX_OOBCMD || m->ctx == IOMFB_CTX_OOBCB;
	uint8_t *area = f->shm + (oob ? 0x8000 : 0);
	struct iomfb_pkt p;
	int known = 0;
	if (iomfb_pkt_read(area, IOMFB_CHAN_SIZE, m->offset, m->length, &p)) { BAD("bad command packet"); return; }
	for (unsigned i = 0; i < sizeof(meth) / sizeof(meth[0]); i++)
		if (!memcmp(p.tag, meth[i].t, 4)) {
			known = 1;
			if (p.in_len != meth[i].in || p.out_len != meth[i].out) BAD("%s sizes %u/%u", p.tag, p.in_len, p.out_len);
		}
	if (!known) { BAD("unknown AP tag %s: the firmware would panic", p.tag); return; }
	if (f->ncmds < FL_LOG) memcpy(f->cmdlog[f->ncmds++], p.tag, 5);
	if (!strcmp(p.tag, "A535") && m->ctx != IOMFB_CTX_OOBCMD) BAD("A535 must go on OOBCMD");
	if (!strcmp(p.tag, "A461") && f->a461++) BAD("A461 sent twice");
	/* nesting: commands issued inside a deferred callback must use ctx CB */
	if (f->cbn && m->ctx != IOMFB_CTX_CB && !oob) BAD("%s nested in a callback but sent on ctx %u", p.tag, m->ctx);
	/* Linux places a nested command at the end of the innermost record of its channel (iomfb.c:785,803) */
	if (m->ctx == IOMFB_CTX_CB) {
		uint32_t want = 0;
		for (int i = 0; i < f->cbn; i++) if (f->cbs[i].ctx == IOMFB_CTX_CB) want = f->cbs[i].end;
		if (m->offset != want) BAD("%s nested at offset %#x, want %#x", p.tag, m->offset, want);
	}
	memset(p.out, 0, p.out_len);
	if (!strcmp(p.tag, "A451")) { f->a451_open = 1; f->step = 0; script_send(f); return; }
	if (!strcmp(p.tag, "A504")) p.out[0] = 1;
	if (!strcmp(p.tag, "A036")) p32(p.out, 0x62336138);   /* b3a8 */
	if (!strcmp(p.tag, "A037")) { p32(p.out, 3); p.out[4] = 1; }
	if (!strcmp(p.tag, "A503")) { p32(p.out, 3456); p32(p.out + 4, 2234); }
	if (!strcmp(p.tag, "A104") && (g32(p.in) != 3456 || g32(p.in + 4) != 2234)) BAD("A104 dims");
	if (!strcmp(p.tag, "A000")) {
		if (g32(p.in) != 1) BAD("A000 arg");
		f->a000_open = 1; f->nest_step = 0;
		/* the nested callbacks are sent while A000 is outstanding (CONV seq 522-531) */
		f->inner_cb_base = f->cbn;
		return;
	}
	if (!strcmp(p.tag, "A534")) {
		if (p.in[2] != 1 || g32(p.in + 0x0b) != 0x62336138 || g32(p.in + 0x21) != 3456 || g32(p.in + 0x25) != 2234 ||
		    g32(p.in + 0x29) != 0x025ec000u) BAD("A534 surface fields");
		p32(p.out, 1);                                     /* macOS: A534 returned 1 */
	}
	if (!strcmp(p.tag, "A462")) p.out[0] = 1;
	if (!strcmp(p.tag, "A456")) { p32(p.out, ++f->swap_id); p32(p.out + 4, 0); }
	if (!strcmp(p.tag, "A458")) {
		uint64_t iova = g64(p.in + IOMFB_SW_SURF_IOVA);
		uint64_t pa = dart4_translate(f->dart_disp, iova);
		f->last_swap = g32(p.in + IOMFB_SW_SWAP_ID);
		f->last_swap_pa = pa;
		f->last_bl = p.in[IOMFB_SW_BL_UPDATE];
		if (!pa) BAD("swap surface not mapped in dart-disp0");
		if (g64(p.in) != 120) BAD("swap timestamp is not the constant deadline");
		f->complete_pending = f->last_swap;
	}
	if (!strcmp(p.tag, "A463")) f->cur_timing = g32(p.in + 4);
	if (!strcmp(p.tag, "A500")) f->adaptive = g32(p.in + 4);
	q(f, iomfb_msg_ack(m->ctx));
}

static void script_next(struct fake_live *f)
{
	f->step++;
	script_send(f);
}

/* ---------------------------------------------------------------- transport */
static void fsend(void *ctx, uint8_t ep, uint64_t w)
{
	struct fake_live *f = ctx;
	struct iomfb_msg m;
	if (ep != IOMFB_EP || iomfb_msg_decode(w, &m)) { BAD("bad message"); return; }
	if (m.type == IOMFB_T_SET_SHMEM) {
		f->shm = dva_ptr(f, f->dart_dcp, m.dva, IOMFB_SHMEM_SIZE);
		if (!f->shm) { BAD("SET_SHMEM DVA %llx not mapped in dart-dcp", (unsigned long long)m.dva); return; }
		f->shm_dva = m.dva;
		q(f, 0x0040227b39590101ull);   /* INITIALIZED, the word macOS saw (DEC; DCP-UNDER-HV.md:69-79) */
		return;
	}
	if (m.type != IOMFB_T_MSG) { BAD("unexpected type"); return; }
	if (m.ack) { on_cb_ack(f, m.ctx); return; }
	on_cmd(f, &m);
}

static int fpump(void *ctx)
{
	struct fake_live *f = ctx;
	if (f->a000_open && f->qn == 0 && f->cbn == f->inner_cb_base) { nest_send(f); }
	if (f->qn == 0) {
		if (f->complete_pending && f->auto_complete) {
			uint8_t b[0x730]; memset(b, 0, sizeof(b)); p32(b, f->complete_pending);
			f->complete_pending = 0;
			callback(f, IOMFB_CTX_CB, "D594", b, 0x730, 0);
			f->cbs[f->cbn - 1].inner = 1;
		}
		if (!f->qn) return 0;
	}
	uint64_t m = f->qm[0];
	memmove(f->qm, f->qm + 1, sizeof(f->qm[0]) * (size_t)(--f->qn));
	live_rx(f->live, IOMFB_EP, m);
	return 1;
}

void fake_live_init(struct fake_live *f, struct live *l, struct dart4 *dcp, struct dart4 *piodma, struct dart4 *disp,
                    void *(*ptr)(void *, uint64_t), void *ctx, const uint8_t *blob, uint32_t blob_len)
{
	memset(f, 0, sizeof(*f));
	f->live = l; f->dart_dcp = dcp; f->dart_piodma = piodma; f->dart_disp = disp;
	f->ptr = ptr; f->ctx = ctx; f->blob = blob; f->blob_len = blob_len; f->auto_complete = 1;
}
void fake_live_transport(struct fake_live *f, void (**send)(void *, uint8_t, uint64_t), int (**pump)(void *))
{
	(void)f;
	*send = fsend;
	*pump = fpump;
}
