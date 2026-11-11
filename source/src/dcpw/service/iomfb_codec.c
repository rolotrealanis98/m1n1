/* SPDX-License-Identifier: GPL-2.0-only */
/* iomfb_codec.c -- see iomfb_codec.h. Freestanding: no libc beyond what is defined here. */
#include "iomfb_codec.h"

static void cpy(uint8_t *d, const uint8_t *s, size_t n) { while (n--) *d++ = *s++; }
static void zero(uint8_t *d, size_t n) { while (n--) *d++ = 0; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* ---------------- mailbox words ---------------- */
uint64_t iomfb_msg_set_shmem(uint64_t dva)
{
	return (uint64_t)IOMFB_T_SET_SHMEM | (4ull << 4) | ((dva & 0xffffffffffffull) << 16);
}

uint64_t iomfb_msg_rpc(unsigned ctx, uint32_t length, uint16_t offset)
{
	return (uint64_t)IOMFB_T_MSG | ((uint64_t)(ctx & 0xf) << 8) | ((uint64_t)offset << 16) |
	       ((uint64_t)length << 32);
}

uint64_t iomfb_msg_ack(unsigned ctx) { return iomfb_msg_rpc(ctx, 0, 0) | (1ull << 6); }

int iomfb_msg_decode(uint64_t w, struct iomfb_msg *m)
{
	zero((uint8_t *)m, sizeof(*m));
	m->type = w & 0xf;
	switch (m->type) {
	case IOMFB_T_SET_SHMEM:
		if (((w >> 4) & 0xf) != 4)
			return -1;
		m->dva = w >> 16;
		return 0;
	case IOMFB_T_INITIALIZED:
		return 0; /* payload bits unexplained (DCP-UNDER-HV.md:69-79) */
	case IOMFB_T_MSG:
		m->ctx = (w >> 8) & 0xf;
		m->ack = (w >> 6) & 1;
		m->offset = (w >> 16) & 0xffff;
		m->length = (uint32_t)(w >> 32);
		return m->ctx < IOMFB_CTX_NUM ? 0 : -1;
	default:
		return -1;
	}
}

int iomfb_tx_offset(unsigned ctx)
{
	switch (ctx) {
	case IOMFB_CTX_CB: case IOMFB_CTX_CMD: return 0x00000;
	case IOMFB_CTX_OOBCB: case IOMFB_CTX_OOBCMD: return 0x08000;
	case IOMFB_CTX_ASYNC_CB: return 0x20000;
	case IOMFB_CTX_OOBASYNC_CB: return 0x28000;
	default: return -1;
	}
}

int iomfb_channel_offset(unsigned ctx)
{
	switch (ctx) {
	case IOMFB_CTX_ASYNC_CB: case IOMFB_CTX_ASYNC: return 0x40000;
	case IOMFB_CTX_OOBASYNC_CB: case IOMFB_CTX_OOBASYNC: return 0x48000;
	case IOMFB_CTX_CB: return 0x60000;
	case IOMFB_CTX_OOBCB: return 0x68000;
	default: return iomfb_tx_offset(ctx);
	}
}

/* ---------------- packets ---------------- */
size_t iomfb_pkt_write(uint8_t *chan, size_t chan_size, size_t off, const char tag[4],
                       const void *in, uint32_t in_len, uint32_t out_len)
{
	uint64_t need = (uint64_t)IOMFB_HDR_SIZE + in_len + out_len;
	if (off % IOMFB_PKT_ALIGN || off > chan_size || need > chan_size - off)
		return 0;
	uint8_t *p = chan + off;
	p[0] = tag[3]; p[1] = tag[2]; p[2] = tag[1]; p[3] = tag[0];
	put32(p + 4, in_len);
	put32(p + 8, out_len);
	if (in_len)
		cpy(p + IOMFB_HDR_SIZE, in, in_len);
	/* An unwritten status must not look like success (REF/iomfb.c:792-794). */
	for (uint32_t i = 0; i < out_len; i++)
		p[IOMFB_HDR_SIZE + in_len + i] = 0xff;
	return (size_t)need;
}

int iomfb_pkt_read(uint8_t *chan, size_t chan_size, size_t off, uint32_t msg_len, struct iomfb_pkt *pk)
{
	if (off > chan_size || chan_size - off < IOMFB_HDR_SIZE)
		return -1;
	uint8_t *p = chan + off;
	pk->tag[0] = p[3]; pk->tag[1] = p[2]; pk->tag[2] = p[1]; pk->tag[3] = p[0]; pk->tag[4] = 0;
	for (int i = 0; i < 4; i++) {
		char c = pk->tag[i];
		int ok = (i == 0) ? (c == 'A' || c == 'D') : (c >= '0' && c <= '9');
		if (!ok)
			return -1;
	}
	pk->in_len = get32(p + 4);
	pk->out_len = get32(p + 8);
	uint64_t total = (uint64_t)IOMFB_HDR_SIZE + pk->in_len + pk->out_len;
	if (total > chan_size - off || (msg_len && total != msg_len))
		return -1;
	pk->in = p + IOMFB_HDR_SIZE;
	pk->out = p + IOMFB_HDR_SIZE + pk->in_len;
	return 0;
}

/* ---------------- builders ---------------- */
void iomfb_build_digital_out(uint8_t out[8], uint32_t color_id, uint32_t timing_id)
{
	put32(out, color_id);  /* struct dcp_set_digital_out_mode_req (REF/iomfb.h:224-227) */
	put32(out + 4, timing_id);
}

void iomfb_build_adaptive_sync(uint8_t out[0x28], uint32_t min_rr_q16)
{
	zero(out, 0x28);                    /* struct dcp_set_parameter_dcp {param; value[8]; count;} */
	put32(out, IOMFB_PARAM_ADAPTIVE_SYNC_FW27);
	put32(out + 4, min_rr_q16);         /* minRR */
	put32(out + 8, 0);                  /* mediaTargetRate */
	put32(out + 12, 0);                 /* fractional */
	put32(out + 0x24, 3);               /* count */
}

uint64_t iomfb_u32_to_f64_bits(uint32_t v)
{
	if (!v)
		return 0;
	int e = 31;
	while (!(v & (1u << e)))
		e--;
	uint64_t mant = ((uint64_t)v << (52 - e)) & ((1ull << 52) - 1); /* exact: e <= 31 < 52 */
	return ((uint64_t)(1023 + e) << 52) | mant;
}

int iomfb_build_swap_submit(uint8_t out[IOMFB_SWAP_SUBMIT_LEN], const struct iomfb_swap_desc *d)
{
	if (!d->width || !d->height || d->width > 8192 || d->height > 8192)
		return -1;
	if ((uint64_t)d->stride < (uint64_t)d->width * 4 || d->stride % 64)
		return -1;
	uint64_t buf = (uint64_t)d->stride * d->height;
	if (buf > 0xffffffffull || !d->iova)
		return -1;

	zero(out, IOMFB_SWAP_SUBMIT_LEN);
	put64(out + IOMFB_SW_TIMESTAMP0, IOMFB_FW27_DEADLINE);
	put64(out + IOMFB_SW_TIMESTAMP1, IOMFB_FW27_DEADLINE);
	put64(out + IOMFB_SW_TIMESTAMP6, IOMFB_FW27_DEADLINE);
	put32(out + IOMFB_SW_SWAP_ID, d->swap_id);
	put32(out + IOMFB_SW_SURF_IDS, 0);
	/* src/dst rect layer 0: {x, y, w, h} */
	put32(out + IOMFB_SW_SRC_RECT + 8, d->width);
	put32(out + IOMFB_SW_SRC_RECT + 12, d->height);
	put32(out + IOMFB_SW_DST_RECT + 8, d->width);
	put32(out + IOMFB_SW_DST_RECT + 12, d->height);
	put32(out + IOMFB_SW_SWAP_ENABLED, 1u); /* BIT(layer 0) */
	put32(out + IOMFB_SW_BG_COLOR, 0);
	if (d->with_brightness) {
		out[IOMFB_SW_BL_UPDATE] = 1;
		out[IOMFB_SW_BL_FLAGS1] = 1;
		put64(out + IOMFB_SW_BL_NITS, iomfb_u32_to_f64_bits(d->bl_nits));
	}

	uint8_t *s = out + IOMFB_SW_SURF0;
	s[IOMFB_SF_PREMULT] = 1;            /* XRGB on the bottom plane: premultiplied over black (REF/plane.c:425-432) */
	put32(s + IOMFB_SF_PLANE_CNT, 1);
	put32(s + IOMFB_SF_PLANE_CNT2, 1);
	put32(s + IOMFB_SF_FORMAT, IOMFB_FMT_BGRA);
	s[IOMFB_SF_XFER] = IOMFB_XFER_SDR;
	s[IOMFB_SF_COLORSPACE] = IOMFB_CS_NATIVE; /* internal panel: native (REF/iomfb_template.c eDP rule) */
	put32(s + IOMFB_SF_STRIDE, d->stride);
	s[IOMFB_SF_PIX_SIZE] = 4;           /* bytes per element on fw 27 (kernel-dcp/0007) */
	s[IOMFB_SF_PEL_W] = 1;
	s[IOMFB_SF_PEL_H] = 1;
	put32(s + IOMFB_SF_WIDTH, d->width);
	put32(s + IOMFB_SF_HEIGHT, d->height);
	put32(s + IOMFB_SF_BUF_SIZE, (uint32_t)buf);
	/* planes[] / comp_types[] are left for M3: must be filled as plane.c does before hardware. */

	put64(out + IOMFB_SW_SURF_IOVA, d->iova);
	out[IOMFB_SW_SWAP_NULL] = 0;
	out[IOMFB_SW_SURF_NULL + 0] = 0;
	out[IOMFB_SW_SURF_NULL + 1] = 1;
	out[IOMFB_SW_SURF_NULL + 2] = 1;
	out[IOMFB_SW_SURF_NULL + 3] = 1;
	out[IOMFB_SW_UNK_PTR_NULL] = 1;
	out[IOMFB_SW_UNK_OUT_NULL] = 1;
	return 0;
}

/* ---------------- serialized dictionaries ---------------- */
struct rd { const uint8_t *b; size_t len, pos; };

static int tag(struct rd *r, uint32_t *size, uint32_t *type)
{
	r->pos = (r->pos + 3) & ~(size_t)3;
	if (r->pos > r->len || r->len - r->pos < 4)
		return -1;
	uint32_t t = get32(r->b + r->pos);
	r->pos += 4;
	if ((t >> 29) & 3) /* padding bits must be zero (REF/parser.c:69) */
		return -1;
	*size = t & 0xffffff;
	*type = (t >> 24) & 0x1f;
	return 0;
}

static int skip(struct rd *r, int depth);

static int take(struct rd *r, size_t n, const uint8_t **p)
{
	if (r->pos > r->len || n > r->len - r->pos)
		return -1;
	if (p)
		*p = r->b + r->pos;
	r->pos += n;
	return 0;
}

static int skip_body(struct rd *r, uint32_t size, uint32_t type, int depth)
{
	if (depth > 32)
		return -1;
	switch (type) {
	case IOMFB_SER_DICT:
	case IOMFB_SER_ARRAY: {
		uint64_t n = type == IOMFB_SER_DICT ? 2ull * size : size;
		if (n > r->len) /* each value needs >= 4 bytes; cheap bound against huge counts */
			return -1;
		for (uint64_t i = 0; i < n; i++)
			if (skip(r, depth + 1))
				return -1;
		return 0;
	}
	case IOMFB_SER_INT: return take(r, 8, 0);
	case IOMFB_SER_STRING:
	case IOMFB_SER_BLOB: return take(r, size, 0);
	case IOMFB_SER_BOOL: return 0;
	default: return -1;
	}
}

static int skip(struct rd *r, int depth)
{
	uint32_t s, t;
	if (tag(r, &s, &t))
		return -1;
	return skip_body(r, s, t, depth);
}

static int key_is(const uint8_t *k, uint32_t klen, const char *want)
{
	uint32_t i = 0;
	/* firmware strings may or may not carry a trailing NUL inside size */
	while (want[i]) {
		if (i >= klen || k[i] != (uint8_t)want[i])
			return 0;
		i++;
	}
	return i == klen || (i + 1 == klen && k[i] == 0);
}

/* Read an int value (tag already consumed). */
static int int_val(struct rd *r, uint32_t type, int64_t *v)
{
	const uint8_t *p;
	if (type != IOMFB_SER_INT || take(r, 8, &p))
		return -1;
	*v = (int64_t)get64(p);
	return 0;
}

static uint32_t clamp_u32(int64_t v) { return v < 0 ? 0 : v > 0xffffffffll ? 0xffffffffu : (uint32_t)v; }

/* HorizontalAttributes / VerticalAttributes dict: Active, Total, PreciseSyncRate */
static int parse_attrs(struct rd *r, uint32_t n, int depth, uint32_t *active, uint32_t *total, uint32_t *rate)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t ks, kt, vs, vt;
		const uint8_t *k;
		if (tag(r, &ks, &kt) || kt != IOMFB_SER_STRING || take(r, ks, &k) || tag(r, &vs, &vt))
			return -1;
		int64_t v;
		if (key_is(k, ks, "Active") && !int_val(r, vt, &v)) *active = clamp_u32(v);
		else if (key_is(k, ks, "Total") && !int_val(r, vt, &v)) *total = clamp_u32(v);
		else if (rate && key_is(k, ks, "PreciseSyncRate") && !int_val(r, vt, &v)) *rate = clamp_u32(v);
		else if (skip_body(r, vs, vt, depth + 1)) return -1;
	}
	return 0;
}

static int parse_timing(struct rd *r, uint32_t n, int depth, struct iomfb_timing *t)
{
	zero((uint8_t *)t, sizeof(*t));
	for (uint32_t i = 0; i < n; i++) {
		uint32_t ks, kt, vs, vt;
		const uint8_t *k;
		int64_t v;
		if (tag(r, &ks, &kt) || kt != IOMFB_SER_STRING || take(r, ks, &k) || tag(r, &vs, &vt))
			return -1;
		if (key_is(k, ks, "HorizontalAttributes") && vt == IOMFB_SER_DICT) {
			if (parse_attrs(r, vs, depth + 1, &t->hactive, &t->htotal, 0)) return -1;
		} else if (key_is(k, ks, "VerticalAttributes") && vt == IOMFB_SER_DICT) {
			if (parse_attrs(r, vs, depth + 1, &t->vactive, &t->vtotal, &t->rate_q16)) return -1;
		} else if (key_is(k, ks, "ID") && !int_val(r, vt, &v)) t->id = clamp_u32(v);
		else if (key_is(k, ks, "MinimumVariableRefreshRate") && !int_val(r, vt, &v)) t->vrr_min_q16 = clamp_u32(v);
		else if (key_is(k, ks, "MaximumVariableRefreshRate") && !int_val(r, vt, &v)) t->vrr_max_q16 = clamp_u32(v);
		else if (key_is(k, ks, "IsPreferred") && vt == IOMFB_SER_BOOL) t->preferred = !!vs;
		else if (key_is(k, ks, "IsPromoted") && vt == IOMFB_SER_BOOL) t->promoted = !!vs;
		else if (key_is(k, ks, "IsVirtual") && vt == IOMFB_SER_BOOL) t->is_virtual = !!vs;
		else if (skip_body(r, vs, vt, depth + 1)) return -1;
	}
	return 0;
}

int iomfb_parse_timings(const uint8_t *blob, size_t len, struct iomfb_timing *out, int max)
{
	struct rd r = { blob, len, 0 };
	uint32_t s, t;
	const uint8_t *h;
	if (take(&r, 4, &h) || get32(h) != IOMFB_SER_HEADER)
		return -1;
	if (tag(&r, &s, &t) || t != IOMFB_SER_ARRAY || s > len)
		return -1;
	int n = 0;
	for (uint32_t i = 0; i < s; i++) {
		uint32_t es, et;
		if (tag(&r, &es, &et))
			return -1;
		if (et != IOMFB_SER_DICT) {
			if (skip_body(&r, es, et, 1)) return -1;
			continue;
		}
		struct iomfb_timing tm;
		if (parse_timing(&r, es, 1, &tm))
			return -1;
		if (n < max)
			out[n++] = tm;
	}
	return n;
}
