/* SPDX-License-Identifier: MIT */
/* dcpw_core.c -- see dcpw_core.h. Our own code. */
#include "dcpw_core.h"

static void z(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }

uint64_t dcpw_period_ns(uint32_t mhz)
{
	return mhz ? 1000000000000ull / mhz : 0;
}

static void update_irq(struct dcpw_dev *d)
{
	if (d->env.set_irq)
		d->env.set_irq(d->env.ctx, (d->irq_status & d->irq_enable) != 0);
}

static void raise(struct dcpw_dev *d, uint32_t bits)
{
	d->irq_status |= bits;
	update_irq(d);
}

/* Sanitise a backend mode table: drop malformed entries, keep the order. */
static int mode_ok(const struct dcpw_mode *m)
{
	if (!m->width || !m->height || m->width > 8192 || m->height > 8192)
		return 0;
	if (m->refresh_mhz < 1000 || m->refresh_mhz > 500000)
		return 0;
	if (m->htotal && m->htotal < m->width)
		return 0;
	if (m->vtotal && m->vtotal < m->height)
		return 0;
	if (m->flags & DCPW_MODE_VRR) {
		if (!m->vrr_min_mhz || m->vrr_min_mhz > m->vrr_max_mhz || m->vrr_max_mhz > m->refresh_mhz)
			return 0;
	}
	return 1;
}

static int load_modes(struct dcpw_dev *d)
{
	struct dcpw_mode tmp[DCPW_MAX_MODES];
	uint32_t cur = 0, n = 0;
	z(tmp, sizeof(tmp));
	int got = d->be.get_modes ? d->be.get_modes(d->be.ctx, tmp, DCPW_MAX_MODES, &cur) : -1;
	if (got <= 0 || got > (int)DCPW_MAX_MODES || cur >= (uint32_t)got)
		return -1;
	uint32_t newcur = DCPW_MAX_MODES;
	z(d->modes, sizeof(d->modes));
	for (int i = 0; i < got; i++) {
		if (!mode_ok(&tmp[i]))
			continue;
		if ((uint32_t)i == cur)
			newcur = n;
		d->modes[n] = tmp[i];
		d->modes[n].flags |= DCPW_MODE_VALID;
		d->modes[n].flags &= DCPW_MODE_VALID | DCPW_MODE_PREFERRED | DCPW_MODE_VRR;
		if (!(d->caps & DCPW_CAP_VRR))
			d->modes[n].flags &= ~DCPW_MODE_VRR;
		n++;
	}
	if (newcur == DCPW_MAX_MODES)
		return -1; /* the active mode itself was malformed */
	d->nmodes = n;
	d->cur = newcur;
	return 0;
}

int dcpw_configure(struct dcpw_dev *d, const struct dcpw_backend *be, const struct dcpw_env *env,
                   uint32_t bl_max_mnits, uint32_t bl_initial_mnits, uint64_t now_ns)
{
	z(d, sizeof(*d));
	if (!be || !env || !env->gpa_ok || !be->get_modes || !be->present)
		return -1;
	d->be = *be;
	d->env = *env;
	d->caps = be->caps & (DCPW_CAP_MODESET | DCPW_CAP_VRR | DCPW_CAP_BRIGHTNESS | DCPW_CAP_FLIP |
	                      DCPW_CAP_HW_VBLANK);
	if ((d->caps & DCPW_CAP_BRIGHTNESS) && (!be->set_brightness || !bl_max_mnits))
		d->caps &= ~DCPW_CAP_BRIGHTNESS;
	if ((d->caps & (DCPW_CAP_MODESET | DCPW_CAP_VRR)) && !be->set_mode)
		d->caps &= ~(DCPW_CAP_MODESET | DCPW_CAP_VRR);
	if (load_modes(d))
		return -1;
	d->bl_max_mnits = (d->caps & DCPW_CAP_BRIGHTNESS) ? bl_max_mnits : 0;
	d->bl_mnits = bl_initial_mnits <= d->bl_max_mnits ? bl_initial_mnits : 0;
	d->next_vbl_ns = now_ns + dcpw_period_ns(d->modes[d->cur].refresh_mhz);
	d->status = DCPW_ST_READY;
	d->ready = 1;
	update_irq(d);
	return 0;
}

/* ---------------- requests ---------------- */
static uint32_t do_set_mode(struct dcpw_dev *d, uint32_t idx, uint32_t vrr)
{
	if (idx >= d->nmodes)
		return DCPW_E_RANGE;
	if (vrr && !(d->modes[idx].flags & DCPW_MODE_VRR))
		return DCPW_E_UNSUP;
	if (idx == d->cur && vrr == d->vrr)
		return DCPW_OK;
	if (!(d->caps & DCPW_CAP_MODESET) && idx != d->cur)
		return DCPW_E_UNSUP;
	if (vrr != d->vrr && !(d->caps & DCPW_CAP_VRR))
		return DCPW_E_UNSUP;
	if (d->be.set_mode(d->be.ctx, &d->modes[idx], (int)vrr))
		return DCPW_E_BACKEND;
	d->cur = idx;
	d->vrr = vrr;
	raise(d, DCPW_IRQ_MODE);
	return DCPW_OK;
}

static uint32_t do_present(struct dcpw_dev *d, const uint32_t *a, uint32_t *ret)
{
	struct dcpw_present p;
	const struct dcpw_mode *m = &d->modes[d->cur];
	p.gpa = a[0] | ((uint64_t)a[1] << 32);
	p.stride = a[2]; p.width = a[3]; p.height = a[4]; p.format = a[5];
	p.dx = a[6]; p.dy = a[7]; p.dw = a[8]; p.dh = a[9];
	if (p.format != DCPW_FMT_B8G8R8A8)
		return DCPW_E_UNSUP;
	if (p.width != m->width || p.height != m->height)
		return DCPW_E_RANGE;
	if ((uint64_t)p.stride < (uint64_t)p.width * 4 || p.stride % 64)
		return DCPW_E_RANGE;
	if (p.gpa & 0x3fff) /* DART granule */
		return DCPW_E_RANGE;
	uint64_t len = (uint64_t)p.stride * p.height;
	if (p.gpa + len < p.gpa || !d->env.gpa_ok(d->env.ctx, p.gpa, len))
		return DCPW_E_RANGE;
	if (p.dw == 0 || p.dh == 0 || p.dx >= p.width || p.dy >= p.height ||
	    p.dw > p.width - p.dx || p.dh > p.height - p.dy)
		return DCPW_E_RANGE;
	if (d->be.present(d->be.ctx, &p))
		return DCPW_E_BACKEND;
	ret[0] = ++d->presents;
	if (!(d->caps & DCPW_CAP_FLIP)) /* copy backends complete synchronously */
		raise(d, DCPW_IRQ_PRESENT);
	return DCPW_OK;
}

static void handle_doorbell(struct dcpw_dev *d, uint32_t seq)
{
	struct dcpw_req q;
	uint32_t *qw = (uint32_t *)&q;
	for (unsigned i = 0; i < sizeof(q) / 4; i++) /* one snapshot, validated after */
		qw[i] = d->req_raw[i];
	z(&d->rsp, sizeof(d->rsp));
	d->stat_requests++;
	uint32_t st;
	if (!d->ready)
		st = DCPW_E_NOTREADY;
	else if (!seq || seq != q.seq)
		st = DCPW_E_BADSEQ;
	else if (d->status & DCPW_ST_FAILED && q.op != DCPW_OP_NOP)
		st = DCPW_E_BACKEND;
	else switch (q.op) {
	case DCPW_OP_NOP: st = DCPW_OK; break;
	case DCPW_OP_SET_MODE: {
		/* VRR stays on across a mode change only where the new mode offers it. */
		uint32_t i = q.arg[0];
		uint32_t v = (i < d->nmodes && d->vrr && (d->modes[i].flags & DCPW_MODE_VRR)) ? 1 : 0;
		st = do_set_mode(d, i, v);
		break;
	}
	case DCPW_OP_SET_VRR:
		st = q.arg[0] > 1 ? DCPW_E_RANGE : do_set_mode(d, d->cur, q.arg[0]);
		break;
	case DCPW_OP_SET_BRIGHTNESS:
		if (!(d->caps & DCPW_CAP_BRIGHTNESS)) st = DCPW_E_UNSUP;
		else if (q.arg[0] > d->bl_max_mnits) st = DCPW_E_RANGE;
		else if (d->be.set_brightness(d->be.ctx, q.arg[0])) st = DCPW_E_BACKEND;
		else { d->bl_mnits = q.arg[0]; st = DCPW_OK; }
		break;
	case DCPW_OP_PRESENT: st = do_present(d, q.arg, d->rsp.ret); break;
	case DCPW_OP_REFRESH_MODES: {
		struct dcpw_mode keep = d->modes[d->cur];
		if (load_modes(d)) { st = DCPW_E_BACKEND; break; }
		if (keep.timing_id != d->modes[d->cur].timing_id) raise(d, DCPW_IRQ_MODE);
		st = DCPW_OK;
		break;
	}
	default: st = DCPW_E_BADOP; break;
	}
	if (st != DCPW_OK)
		d->stat_rejects++;
	d->rsp.seq = q.seq;
	d->rsp.status = st;
}

uint32_t dcpw_read32(struct dcpw_dev *d, uint32_t off)
{
	if (off & 3 || off >= DCPW_REG_SIZE) {
		d->stat_bad_access++;
		return 0;
	}
	if (off >= DCPW_RSP_OFF && off < DCPW_RSP_OFF + sizeof(struct dcpw_rsp))
		return ((const uint32_t *)&d->rsp)[(off - DCPW_RSP_OFF) / 4];
	if (off >= DCPW_REQ_OFF && off < DCPW_REQ_OFF + sizeof(struct dcpw_req))
		return d->req_raw[(off - DCPW_REQ_OFF) / 4];
	if (off >= DCPW_MODES_OFF && off < DCPW_MODES_OFF + DCPW_MAX_MODES * sizeof(struct dcpw_mode)) {
		uint32_t i = (off - DCPW_MODES_OFF) / sizeof(struct dcpw_mode);
		return i < d->nmodes ? ((const uint32_t *)&d->modes[i])[((off - DCPW_MODES_OFF) % sizeof(struct dcpw_mode)) / 4] : 0;
	}
	switch (off) {
	case DCPW_REG_MAGIC: return DCPW_MAGIC;
	case DCPW_REG_VERSION: return DCPW_VERSION;
	case DCPW_REG_STATUS: return d->status;
	case DCPW_REG_CAPS: return d->caps;
	case DCPW_REG_IRQ_STATUS: return d->irq_status;
	case DCPW_REG_IRQ_ENABLE: return d->irq_enable;
	case DCPW_REG_VBL_LO: d->vbl_latch_hi = d->vbl_count >> 32; d->vbl_ns_latch_hi = d->vbl_ns >> 32; return (uint32_t)d->vbl_count;
	case DCPW_REG_VBL_HI: return (uint32_t)d->vbl_latch_hi;
	case DCPW_REG_VBL_NS_LO: return (uint32_t)d->vbl_ns;
	case DCPW_REG_VBL_NS_HI: return (uint32_t)d->vbl_ns_latch_hi;
	case DCPW_REG_CUR_MODE: return d->cur;
	case DCPW_REG_CUR_MHZ: return d->ready ? d->modes[d->cur].refresh_mhz : 0;
	case DCPW_REG_BL_MNITS: return d->bl_mnits;
	case DCPW_REG_VRR: return d->vrr;
	case DCPW_REG_BL_MAX_MNITS: return d->bl_max_mnits;
	case DCPW_REG_PRESENTS_LO: return d->presents;
	case DCPW_REG_MODE_COUNT: return d->nmodes;
	default: return 0;
	}
}

void dcpw_write32(struct dcpw_dev *d, uint32_t off, uint32_t val, uint64_t now_ns)
{
	(void)now_ns;
	if (off & 3 || off >= DCPW_REG_SIZE) {
		d->stat_bad_access++;
		return;
	}
	if (off >= DCPW_REQ_OFF && off < DCPW_REQ_OFF + sizeof(struct dcpw_req)) {
		d->req_raw[(off - DCPW_REQ_OFF) / 4] = val;
		return;
	}
	switch (off) {
	case DCPW_REG_DOORBELL: handle_doorbell(d, val); break;
	case DCPW_REG_IRQ_STATUS: d->irq_status &= ~(val & DCPW_IRQ_ALL); update_irq(d); break;
	case DCPW_REG_IRQ_ENABLE: d->irq_enable = val & DCPW_IRQ_ALL; update_irq(d); break;
	default: break; /* RO / reserved: WI */
	}
}

void dcpw_tick(struct dcpw_dev *d, uint64_t now)
{
	if (!d->ready || (d->caps & DCPW_CAP_HW_VBLANK))
		return;
	uint64_t per = dcpw_period_ns(d->modes[d->cur].refresh_mhz);
	if (!per || now < d->next_vbl_ns)
		return;
	uint64_t missed = (now - d->next_vbl_ns) / per;
	d->vbl_count += missed + 1;
	d->vbl_ns = d->next_vbl_ns + missed * per;
	d->next_vbl_ns = d->vbl_ns + per;
	raise(d, DCPW_IRQ_VSYNC);
}

void dcpw_backend_vblank(struct dcpw_dev *d, uint64_t now)
{
	if (!d->ready)
		return;
	d->vbl_count++;
	d->vbl_ns = now;
	raise(d, DCPW_IRQ_VSYNC);
}

void dcpw_backend_present_done(struct dcpw_dev *d)
{
	if (d->ready)
		raise(d, DCPW_IRQ_PRESENT);
}

void dcpw_backend_failed(struct dcpw_dev *d)
{
	d->status |= DCPW_ST_FAILED;
	d->status &= ~DCPW_ST_LIVE;
	raise(d, DCPW_IRQ_ERROR);
}
