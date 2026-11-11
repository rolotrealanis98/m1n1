/* SPDX-License-Identifier: GPL-2.0-only */
/* iomfb_session.c -- see iomfb_session.h. Sequences ported from drm/apple (LINUX-MAP.md §4-§6). */
#include "iomfb_session.h"

static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void cpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int tag_eq(const char *a, const char *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

void iomfb_sess_init(struct iomfb_sess *s, uint8_t *shmem, const struct iomfb_transport *tp,
                     uint32_t cur_timing_id)
{
	zero(s, sizeof(*s));
	s->shmem = shmem;
	s->tp = *tp;
	s->color_id = 1;
	s->cur_timing = cur_timing_id;
	s->timeout_polls = 100000;
	s->bl_nits = 300; /* D300 pr_publish id 20 = 0x012c0000 (300.0 nits) on macOS 27.3 (DECODE:225) */
}

static void fail(struct iomfb_sess *s, const char *tag)
{
	if (!s->failed) {
		s->failed = 1;
		for (int i = 0; i < 4; i++)
			s->fail_tag[i] = tag ? tag[i] : '?';
		s->fail_tag[4] = 0;
		if (s->dev)
			dcpw_backend_failed(s->dev);
	}
}

/* ---- callbacks (DCP -> AP), each returns the out bytes it wrote or <0 = unhandled ---- */
static int cb_prop_start(struct iomfb_sess *s, const struct iomfb_pkt *p)
{
	if (p->in_len != 4 || p->out_len != 4)
		return -1;
	uint32_t len = get32(p->in);
	int ok = len > 0 && len <= IOMFB_PROP_MAX;
	s->prop_active = ok;
	s->prop_len = ok ? len : 0;
	s->prop_recv = 0;
	s->prop_contig = 1;
	if (ok)
		zero(s->prop, len);
	zero(p->out, 4);
	p->out[0] = (uint8_t)ok;
	return 4;
}

static int cb_prop_chunk(struct iomfb_sess *s, const struct iomfb_pkt *p)
{
	if (p->in_len != 0x1008 || p->out_len != 4)
		return -1;
	uint32_t off = get32(p->in + 0x1000), len = get32(p->in + 0x1004);
	int ok = s->prop_active && off <= s->prop_len && len <= 0x1000 && len <= s->prop_len - off;
	if (ok) {
		if (off != s->prop_recv)
			s->prop_contig = 0;
		else
			s->prop_recv += len;
		cpy(s->prop + off, p->in, len);
	} else {
		s->prop_contig = 0;
	}
	zero(p->out, 4);
	p->out[0] = (uint8_t)ok;
	return 4;
}

static int key_is(const uint8_t *k, const char *want)
{
	int i = 0;
	for (; want[i]; i++)
		if (k[i] != (uint8_t)want[i])
			return 0;
	return k[i] == 0;
}

static int cb_prop_end(struct iomfb_sess *s, const struct iomfb_pkt *p)
{
	if (p->in_len != 0x40 || p->out_len != 4)
		return -1;
	int nul = 0;
	for (int i = 0; i < 0x40; i++)
		if (!p->in[i]) { nul = 1; break; }
	int ok = nul && s->prop_active && s->prop_contig && s->prop_recv == s->prop_len;
	if (ok && key_is(p->in, "TimingElements")) {
		struct iomfb_timing t[IOMFB_MAX_TIMINGS];
		int n = iomfb_parse_timings(s->prop, s->prop_len, t, IOMFB_MAX_TIMINGS);
		if (n > 0) {
			cpy(s->timings, t, sizeof(t[0]) * (uint32_t)n);
			s->ntimings = n;
		} else {
			ok = 0;
		}
	}
	s->prop_active = 0;
	zero(p->out, 4);
	p->out[0] = (uint8_t)ok;
	return 4;
}

static int cb_swap_complete(struct iomfb_sess *s, const struct iomfb_pkt *p)
{
	if (p->in_len != 0x730 || p->out_len != 0)
		return -1;
	uint32_t id = get32(p->in);
	if (id && id == s->swap_inflight) {
		s->swap_inflight = 0;
		s->swaps_done++;
		if (s->dev) {
			uint64_t now = s->tp.now_ns ? s->tp.now_ns(s->tp.ctx) : 0;
			dcpw_backend_vblank(s->dev, now);
			dcpw_backend_present_done(s->dev);
		}
	}
	return 0;
}

static void handle_callback(struct iomfb_sess *s, const struct iomfb_msg *m)
{
	int base = iomfb_channel_offset(m->ctx);
	struct iomfb_pkt p;
	if (base < 0 || iomfb_pkt_read(s->shmem + base, IOMFB_CHAN_SIZE, m->offset, m->length, &p)) {
		fail(s, "BAD!");
		return;
	}
	s->stat_cb++;
	int r = -1;
	if (p.tag[0] == 'D') {
		if (tag_eq(p.tag, IOMFB_D_PROP_START)) r = cb_prop_start(s, &p);
		else if (tag_eq(p.tag, IOMFB_D_PROP_CHUNK)) r = cb_prop_chunk(s, &p);
		else if (tag_eq(p.tag, IOMFB_D_PROP_END)) r = cb_prop_end(s, &p);
		else if (tag_eq(p.tag, IOMFB_D_SWAP_COMPLETE)) r = cb_swap_complete(s, &p);
	}
	if (r < 0) {
		fail(s, p.tag); /* not acked on purpose, see header */
		return;
	}
	s->tp.send(s->tp.ctx, IOMFB_EP, iomfb_msg_ack(m->ctx));
}

void iomfb_sess_rx(struct iomfb_sess *s, uint8_t ep, uint64_t w)
{
	struct iomfb_msg m;
	if (ep != IOMFB_EP || iomfb_msg_decode(w, &m)) {
		fail(s, "MSG?");
		return;
	}
	if (m.type != IOMFB_T_MSG)
		return; /* SET_SHMEM echo / INITIALIZED belong to bring-up (M3) */
	if (m.ack) {
		if (m.ctx == IOMFB_CTX_CMD && s->cmd_busy && !s->cmd_acked)
			s->cmd_acked = 1;
		else
			fail(s, "ACK?");
		return;
	}
	if (m.ctx == IOMFB_CTX_CB || m.ctx == IOMFB_CTX_ASYNC || m.ctx == IOMFB_CTX_OOBCB)
		handle_callback(s, &m);
	else
		fail(s, "CTX?");
}

int iomfb_sess_call(struct iomfb_sess *s, const char tag[4], const void *in, uint32_t in_len,
                    uint32_t out_len, void *out)
{
	if (s->failed || s->cmd_busy || out_len > sizeof(s->cmd_resp))
		return -1;
	uint8_t *chan = s->shmem + iomfb_tx_offset(IOMFB_CTX_CMD);
	size_t used = iomfb_pkt_write(chan, IOMFB_CHAN_SIZE, 0, tag, in, in_len, out_len);
	if (!used)
		return -1;
	s->cmd_busy = 1;
	s->cmd_acked = 0;
	cpy(s->cmd_tag, tag, 4);
	s->cmd_tag[4] = 0;
	s->stat_cmd++;
	s->tp.send(s->tp.ctx, IOMFB_EP, iomfb_msg_rpc(IOMFB_CTX_CMD, (uint32_t)used, 0));
	for (uint32_t i = 0; !s->cmd_acked && !s->failed && i < s->timeout_polls; i++)
		s->tp.pump(s->tp.ctx);
	s->cmd_busy = 0;
	if (!s->cmd_acked) {
		fail(s, tag);
		return -1;
	}
	if (out_len)
		cpy(out, chan + IOMFB_HDR_SIZE + in_len, out_len);
	return 0;
}

/* ---- backend ops ---- */
int iomfb_timings_to_modes(const struct iomfb_timing *t, int n, struct dcpw_mode *out, uint32_t max,
                           uint32_t cur_id, uint32_t *cur_idx)
{
	uint32_t top = 0, k = 0;
	int found = 0;
	for (int i = 0; i < n; i++)
		if (!t[i].is_virtual && t[i].rate_q16 > top)
			top = t[i].rate_q16;
	for (int i = 0; i < n && k < max; i++) {
		if (t[i].is_virtual)
			continue;
		struct dcpw_mode *m = &out[k];
		zero(m, sizeof(*m));
		m->timing_id = t[i].id;
		m->width = t[i].hactive; m->height = t[i].vactive;
		m->htotal = t[i].htotal; m->vtotal = t[i].vtotal;
		m->refresh_mhz = (uint32_t)(((uint64_t)t[i].rate_q16 * 1000 + 0x8000) >> 16);
		if (t[i].preferred)
			m->flags |= DCPW_MODE_PREFERRED;
		uint32_t lo = t[i].vrr_min_q16, hi = t[i].vrr_max_q16;
		if (!lo && !hi && t[i].rate_q16 == top && t[i].preferred) {
			lo = 24u << 16; /* firmware floor: IOMFBDisplayRefresh max interval 41.67 ms (vrr/REPORT.md:15) */
			hi = t[i].rate_q16;
		}
		if (lo && hi >= lo) {
			m->flags |= DCPW_MODE_VRR;
			m->vrr_min_mhz = (uint32_t)(((uint64_t)lo * 1000 + 0x8000) >> 16);
			m->vrr_max_mhz = (uint32_t)(((uint64_t)hi * 1000 + 0x8000) >> 16);
		}
		if (t[i].id == cur_id) {
			*cur_idx = k;
			found = 1;
		}
		k++;
	}
	return found ? (int)k : -1;
}

static int ss_get_modes(void *ctx, struct dcpw_mode *out, uint32_t max, uint32_t *cur)
{
	struct iomfb_sess *s = ctx;
	if (s->failed || s->ntimings <= 0)
		return -1;
	return iomfb_timings_to_modes(s->timings, s->ntimings, out, max, s->cur_timing, cur);
}

static int ss_set_mode(void *ctx, const struct dcpw_mode *m, int vrr_on)
{
	struct iomfb_sess *s = ctx;
	uint8_t req[0x28], out[4];
	/* VRR-capable timing: adaptive sync first (min rate or 0), then the mode (vrr/0002:228-235,
	 * REF/iomfb_template.c dcp_on_set_adaptive_sync -> set_digital_out_mode). */
	if (m->flags & DCPW_MODE_VRR) {
		uint32_t q16 = vrr_on ? (uint32_t)(((uint64_t)m->vrr_min_mhz << 16) / 1000) : 0;
		iomfb_build_adaptive_sync(req, q16);
		if (iomfb_sess_call(s, IOMFB_A_SET_PARAMETER, req, IOMFB_SET_PARAM_LEN, 4, out))
			return -1;
	} else if (vrr_on) {
		return -1;
	}
	iomfb_build_digital_out(req, s->color_id, m->timing_id);
	if (iomfb_sess_call(s, IOMFB_A_SET_DIGITAL_OUT, req, IOMFB_DIGITAL_OUT_LEN, 4, out))
		return -1;
	if (get32(out) != 0)
		return -1;
	s->cur_timing = m->timing_id;
	s->vrr = vrr_on;
	return 0;
}

static int do_swap(struct iomfb_sess *s, const struct dcpw_present *p, int with_bl)
{
	static uint8_t wire[IOMFB_SWAP_SUBMIT_LEN]; /* single caller under the device lock */
	uint8_t start_in[IOMFB_SWAP_START_LEN], start_out[8], sub_out[12];
	if (s->swap_inflight)
		return -1; /* DCPW reports BUSY via E_BACKEND; Windows waits for IRQ_PRESENT */
	uint64_t len = (uint64_t)p->stride * p->height;
	uint64_t iova = s->tp.iova_for_gpa ? s->tp.iova_for_gpa(s->tp.ctx, p->gpa, len) : 0;
	if (!iova)
		return -1;
	zero(start_in, sizeof(start_in));
	if (iomfb_sess_call(s, IOMFB_A_SWAP_START, start_in, IOMFB_SWAP_START_LEN, IOMFB_SWAP_START_OUT, start_out))
		return -1;
	uint32_t id = get32(start_out), ret = get32(start_out + 4);
	if (ret || !id)
		return -1;
	struct iomfb_swap_desc d = { id, iova, p->width, p->height, p->stride, with_bl, s->bl_nits };
	if (iomfb_build_swap_submit(wire, &d))
		return -1;
	s->swap_inflight = id;
	if (iomfb_sess_call(s, IOMFB_A_SWAP_SUBMIT, wire, IOMFB_SWAP_SUBMIT_LEN, IOMFB_SWAP_SUBMIT_OUT, sub_out)) {
		s->swap_inflight = 0;
		return -1;
	}
	if (get32(sub_out + 5) != 0) { /* dcp_swap_submit_resp_v27.ret at +5 (REF/iomfb_v27.h:65-70) */
		s->swap_inflight = 0;
		return -1;
	}
	s->last = *p;
	s->have_last = 1;
	return 0;
}

static int ss_present(void *ctx, const struct dcpw_present *p)
{
	struct iomfb_sess *s = ctx;
	int bl = s->bl_pending;
	if (do_swap(s, p, bl))
		return -1;
	s->bl_pending = 0;
	return 0;
}

/* Brightness rides in the swap record, so a level change re-presents the last surface
 * (REF/iomfb_template.c:105-124; the level only takes effect with a present). */
static int ss_set_brightness(void *ctx, uint32_t mnits)
{
	struct iomfb_sess *s = ctx;
	s->bl_nits = (mnits + 500) / 1000;
	s->bl_pending = 1;
	if (!s->have_last || s->swap_inflight)
		return 0; /* applied with the next present */
	struct dcpw_present p = s->last;
	if (do_swap(s, &p, 1))
		return -1;
	s->bl_pending = 0;
	return 0;
}

void iomfb_sess_backend(struct iomfb_sess *s, struct dcpw_backend *be)
{
	be->ctx = s;
	be->caps = DCPW_CAP_MODESET | DCPW_CAP_VRR | DCPW_CAP_BRIGHTNESS | DCPW_CAP_FLIP | DCPW_CAP_HW_VBLANK;
	be->get_modes = ss_get_modes;
	be->set_mode = ss_set_mode;
	be->set_brightness = ss_set_brightness;
	be->present = ss_present;
}
