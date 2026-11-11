/* SPDX-License-Identifier: GPL-2.0-only */
/* fake_dcp.c -- scripted IOMFB endpoint. Sizes follow the 27.3 table (dcp-27.3/patches-kernel/0002). */
#include <string.h>
#include <stdio.h>
#include "fake_dcp.h"

static void push(struct fake_dcp *f, uint64_t m)
{
	if ((f->qt + 1) % 64 == f->qh) { f->unexpected++; return; }
	f->q[f->qt] = m;
	f->qt = (f->qt + 1) % 64;
}

static uint32_t g32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static const struct { const char *tag; uint32_t in, out; } table[] = {
	{ "A456", 0x10, 0x8 }, { "A458", 0x1144, 0xc }, { "A463", 8, 4 }, { "A500", 0x28, 4 }, { "A526", 0, 4 },
};

static void on_cmd(struct fake_dcp *f, const struct iomfb_msg *m)
{
	struct iomfb_pkt p;
	uint8_t *chan = f->shmem + iomfb_tx_offset(IOMFB_CTX_CMD);
	if (iomfb_pkt_read(chan, IOMFB_CHAN_SIZE, m->offset, m->length, &p)) { f->unexpected++; return; }
	int known = 0;
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (!memcmp(p.tag, table[i].tag, 4)) {
			known = 1;
			if (p.in_len != table[i].in || p.out_len != table[i].out) f->unexpected++;
		}
	if (!known) { f->unexpected++; return; } /* the real firmware panics: "link_rpc_lookup callback not found" */
	for (uint32_t i = 0; i < p.out_len; i++)
		if (p.out[i] != 0xff) f->unexpected++; /* AP must pre-fill outputs with 0xff */
	if (f->ncalls < FAKE_MAX_CALLS) {
		struct fake_call *c = &f->calls[f->ncalls++];
		memcpy(c->tag, p.tag, 5);
		c->in_len = p.in_len; c->out_len = p.out_len;
		memcpy(c->in, p.in, p.in_len < sizeof(c->in) ? p.in_len : sizeof(c->in));
	}
	memset(p.out, 0, p.out_len);
	if (!memcmp(p.tag, "A456", 4)) {
		p32(p.out, ++f->next_swap_id);
		p32(p.out + 4, f->fault == FF_SWAP_START_RET ? 1 : 0);
	} else if (!memcmp(p.tag, "A458", 4)) {
		f->last_submitted = g32(p.in + IOMFB_SW_SWAP_ID);
		p32(p.out + 5, f->fault == FF_SWAP_RET ? 0xffffffffu : 0);
	} else if (!memcmp(p.tag, "A463", 4)) {
		if (f->fault != FF_DIGITAL_RET) f->cur_timing = g32(p.in + 4);
		p32(p.out, f->fault == FF_DIGITAL_RET ? 1 : 0);
	} else if (!memcmp(p.tag, "A500", 4)) {
		if (g32(p.in) == 15) f->adaptive_q16 = g32(p.in + 4);
	}
	if (f->fault != FF_DROP_ACK)
		push(f, iomfb_msg_ack(IOMFB_CTX_CMD));
}

static void fsend(void *ctx, uint8_t ep, uint64_t w)
{
	struct fake_dcp *f = ctx;
	struct iomfb_msg m;
	if (ep != IOMFB_EP || iomfb_msg_decode(w, &m) || m.type != IOMFB_T_MSG) { f->unexpected++; return; }
	if (m.ack) {
		if (m.ctx == IOMFB_CTX_CB) f->cb_acked = 1; else f->unexpected++;
		return;
	}
	if (m.ctx == IOMFB_CTX_CMD) on_cmd(f, &m); else f->unexpected++;
}

static int fpump(void *ctx)
{
	struct fake_dcp *f = ctx;
	if (f->qh == f->qt) return 0;
	uint64_t m = f->q[f->qh];
	f->qh = (f->qh + 1) % 64;
	iomfb_sess_rx(f->sess, IOMFB_EP, m);
	return 1;
}

static uint64_t fnow(void *ctx) { return ((struct fake_dcp *)ctx)->now; }

static uint64_t fiova(void *ctx, uint64_t gpa, uint64_t len)
{
	struct fake_dcp *f = ctx;
	if (gpa < f->gpa_base || len > f->gpa_size || gpa - f->gpa_base > f->gpa_size - len) return 0;
	return f->iova_base + (gpa - f->gpa_base);
}

void fake_init(struct fake_dcp *f, uint8_t *shmem, struct iomfb_sess *s)
{
	memset(f, 0, sizeof(*f));
	f->shmem = shmem;
	f->sess = s;
	f->cur_timing = 2;
	f->gpa_base = 0x80000000ull;
	f->gpa_size = 0x40000000ull;
	f->iova_base = 0xf00000000ull;
}

struct iomfb_transport fake_transport(struct fake_dcp *f)
{
	struct iomfb_transport t = { f, fsend, fpump, fnow, fiova };
	return t;
}

void fake_reset_calls(struct fake_dcp *f) { f->ncalls = 0; }

int fake_callback(struct fake_dcp *f, const char tag[4], const void *in, uint32_t in_len, uint32_t out_len, uint8_t *out_copy)
{
	uint8_t *chan = f->shmem + iomfb_channel_offset(IOMFB_CTX_CB);
	size_t used = iomfb_pkt_write(chan, IOMFB_CHAN_SIZE, 0, tag, in, in_len, out_len);
	if (!used) return 0;
	f->cb_acked = 0;
	push(f, iomfb_msg_rpc(IOMFB_CTX_CB, (uint32_t)used, 0));
	while (fpump(f)) ;
	if (out_copy && out_len) memcpy(out_copy, chan + IOMFB_HDR_SIZE + in_len, out_len);
	return f->cb_acked;
}

int fake_deliver_prop(struct fake_dcp *f, const char *key, const uint8_t *blob, uint32_t len)
{
	uint8_t out[4], start[4], chunk[0x1008], end[0x40];
	p32(start, len);
	if (!fake_callback(f, "D126", start, 4, 4, out) || out[0] != 1) return -1;
	for (uint32_t off = 0; off < len; off += 0x1000) {
		uint32_t n = len - off > 0x1000 ? 0x1000 : len - off;
		memset(chunk, 0, sizeof(chunk));
		memcpy(chunk, blob + off, n);
		p32(chunk + 0x1000, off);
		p32(chunk + 0x1004, n);
		if (!fake_callback(f, "D127", chunk, 0x1008, 4, out) || out[0] != 1) return -2;
	}
	memset(end, 0, sizeof(end));
	snprintf((char *)end, sizeof(end), "%s", key);
	if (!fake_callback(f, "D128", end, 0x40, 4, out)) return -3;
	return out[0] == 1 ? 0 : -4;
}

int fake_complete_swap(struct fake_dcp *f, uint32_t id)
{
	uint8_t in[0x730];
	memset(in, 0, sizeof(in));
	p32(in, id);
	return fake_callback(f, "D594", in, 0x730, 0, NULL);
}

/* ---- serializer ---- */
static void sput(struct ser *s, const void *p, uint32_t n)
{
	if (s->pos + n > s->cap) { s->err = 1; return; }
	memcpy(s->b + s->pos, p, n);
	s->pos += n;
}
static void salign(struct ser *s) { uint8_t z = 0; while (s->pos & 3) sput(s, &z, 1); }
void ser_header(struct ser *s) { uint32_t h = 0xd3; sput(s, &h, 4); }
void ser_tag(struct ser *s, uint32_t size, uint32_t type, int last)
{
	salign(s);
	uint32_t t = (size & 0xffffff) | (type << 24) | ((uint32_t)!!last << 31);
	sput(s, &t, 4);
}
void ser_int(struct ser *s, int64_t v, int last) { ser_tag(s, 8, IOMFB_SER_INT, last); sput(s, &v, 8); }
void ser_str(struct ser *s, const char *str, int last) { uint32_t n = (uint32_t)strlen(str) + 1; ser_tag(s, n, IOMFB_SER_STRING, last); sput(s, str, n); }
void ser_bool(struct ser *s, int v, int last) { ser_tag(s, !!v, IOMFB_SER_BOOL, last); }

static void attrs(struct ser *s, uint32_t active, uint32_t total, int64_t rate, int last)
{
	ser_tag(s, rate >= 0 ? 5 : 4, IOMFB_SER_DICT, last);
	ser_str(s, "Active", 0); ser_int(s, active, 0);
	ser_str(s, "Total", 0); ser_int(s, total, 0);
	ser_str(s, "FrontPorch", 0); ser_int(s, 8, 0);
	if (rate >= 0) { ser_str(s, "SyncWidth", 0); ser_int(s, 4, 0); ser_str(s, "PreciseSyncRate", 0); ser_int(s, rate, 1); }
	else { ser_str(s, "SyncWidth", 0); ser_int(s, 4, 1); }
}

/* The six J716s timings (firmware-timings.txt), plus keys the parser must skip. */
uint32_t j716s_timing_blob(uint8_t *buf, uint32_t cap)
{
	static const struct { uint32_t id, vt; int64_t rate; int pref; } T[] = {
		{ 2, 2456, 120 << 16, 1 }, { 3, 2456, 60 << 16, 0 }, { 4, 2456, 48 << 16, 0 },
		{ 5, 2456, 3928227 /* 59.940 Hz */, 0 }, { 6, 2456, 3142582 /* 47.952 Hz */, 0 }, { 7, 2946, 50 << 16, 0 },
	};
	struct ser s = { buf, cap, 0, 0 };
	ser_header(&s);
	ser_tag(&s, 6, IOMFB_SER_ARRAY, 1);
	for (int i = 0; i < 6; i++) {
		ser_tag(&s, 11, IOMFB_SER_DICT, i == 5);
		ser_str(&s, "ID", 0); ser_int(&s, T[i].id, 0);
		ser_str(&s, "HorizontalAttributes", 0); attrs(&s, 3456, 3536, -1, 0);
		ser_str(&s, "VerticalAttributes", 0); attrs(&s, 2234, T[i].vt, T[i].rate, 0);
		ser_str(&s, "Score", 0); ser_int(&s, 15000 + i, 0);
		ser_str(&s, "IsPreferred", 0); ser_bool(&s, T[i].pref, 0);
		ser_str(&s, "IsPromoted", 0); ser_bool(&s, !T[i].pref, 0);
		ser_str(&s, "IsVirtual", 0); ser_bool(&s, 0, 0);
		ser_str(&s, "MinimumVariableRefreshRate", 0); ser_int(&s, 0, 0);
		ser_str(&s, "MaximumVariableRefreshRate", 0); ser_int(&s, 0, 0);
		ser_str(&s, "DiscreteMediaRefreshRates", 0); ser_tag(&s, 0, IOMFB_SER_ARRAY, 0);
		ser_str(&s, "ColorModes", 1);
		ser_tag(&s, 1, IOMFB_SER_ARRAY, 1);
		ser_tag(&s, 2, IOMFB_SER_DICT, 1);
		ser_str(&s, "ID", 0); ser_int(&s, 1, 0);
		ser_str(&s, "Blob", 0); ser_tag(&s, 3, IOMFB_SER_BLOB, 1); { uint8_t b[3] = {1, 2, 3}; sput(&s, b, 3); }
	}
	return s.err ? 0 : s.pos;
}
