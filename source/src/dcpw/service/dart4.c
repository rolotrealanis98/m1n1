/* SPDX-License-Identifier: GPL-2.0-only */
/* dart4.c -- see dart4.h. */
#include "dart4.h"

static uint64_t tbl_pte(uint64_t pa) { return ((pa >> 14) << 10) | DART4_PTE_VALID; }
static uint64_t leaf_pte(uint64_t pa) { return ((pa >> 14) << 10) | DART4_SP_END | DART4_PTE_VALID; }
static int tbl_ok(uint64_t e) { return (e & DART4_PTE_VALID) && !(e & ~(DART4_PA_MASK | DART4_PTE_VALID)); }
static int leaf_ok(uint64_t e)   /* apple_dart_fw_leaf_decode */
{
	return (e & DART4_PTE_VALID) && !(e >> 52) && (e & DART4_SP_END) == DART4_SP_END;
}
static uint64_t pa_of(uint64_t e) { return (e & DART4_PA_MASK) << 4; }
static uint32_t ix(uint64_t dva, int sh) { return (uint32_t)(dva >> sh) & (DART4_ENTRIES - 1); }

static int wait_idle(struct dart4 *d)
{
	for (uint32_t i = 0; i < 100000; i++)
		if (!(d->io.mmio_rd(d->io.ctx, d->base + DART4_REG_TLB_CMD) & DART4_TLB_BUSY))
			return 0;
	return d->failed = -1;
}

/* range invalidate of [first, last] for our stream (apple-dart.c:1277-1295 with range = true) */
static int flush_range(struct dart4 *d, uint64_t first, uint64_t last)
{
	uint32_t v = DART4_TLB_OP_FLUSH_SID | (d->sid & 0xff) | DART4_TLB_RANGE | DART4_TLB_STT;
	if (d->version >= 0x0202)
		v |= DART4_TLB_V2;
	if (wait_idle(d))
		return -1;
	d->io.mmio_wr(d->io.ctx, d->base + DART4_REG_TLB_START, (uint32_t)((first >> 14) << 2));
	d->io.mmio_wr(d->io.ctx, d->base + DART4_REG_TLB_END, (uint32_t)((last >> 14) << 2));
	d->io.barrier(d->io.ctx);
	d->io.mmio_wr(d->io.ctx, d->base + DART4_REG_TLB_CMD, v);
	d->stat_flushes++;
	return wait_idle(d);
}

int dart4_init(struct dart4 *d, const struct dart4_io *io, uint64_t base, uint32_t sid,
               uint64_t adt_root, uint64_t win_base, uint64_t win_size, int nslots)
{
	uint32_t tcr, ttbr, prot, lo, hi;
	uint8_t *z = (uint8_t *)d;
	for (unsigned i = 0; i < sizeof(*d); i++)
		z[i] = 0;
	d->io = *io;
	d->base = base;
	d->sid = sid;
	if (sid >= 32 || nslots < 1 || nslots > DART4_MAX_OWN_SLOTS || !win_size || win_base + win_size < win_base ||
	    win_base + win_size > (1ull << 47) || (win_base | win_size) & (DART4_SLOT_SPAN - 1))
		return d->failed = -1;
	prot = io->mmio_rd(io->ctx, base + DART4_REG_PROTECT);
	tcr = io->mmio_rd(io->ctx, base + DART4_REG_TCR(sid));
	ttbr = io->mmio_rd(io->ctx, base + DART4_REG_TTBR(sid));
	d->version = io->mmio_rd(io->ctx, base + DART4_REG_PARAMS3) & 0xffff;   /* read by apple-dart.c:4058 */
	d->root = ((uint64_t)((ttbr & 0x3ffffffcu) >> 2)) << 14;
	if (!(prot & 1) || (tcr & (DART4_TCR_TRANSLATE | DART4_TCR_4LEVEL)) != (DART4_TCR_TRANSLATE | DART4_TCR_4LEVEL) ||
	    !(ttbr & 1) || d->root != adt_root || (d->root & (DART4_PAGE - 1)))
		return d->failed = -2;
	lo = (uint32_t)(win_base >> DART4_ROOT_SHIFT);
	hi = (uint32_t)((win_base + win_size) >> DART4_ROOT_SHIFT);
	for (int64_t s = (int64_t)hi - 1; s >= (int64_t)lo && d->nslots < nslots; s--)
		if (!io->rd64(io->ctx, d->root + 8ull * (uint64_t)s))
			d->slot[d->nslots++] = (uint32_t)s;
	if (d->nslots < nslots)
		return d->failed = -3;
	d->next_dva = (uint64_t)d->slot[0] << DART4_ROOT_SHIFT;
	return 0;
}

static int owned_index(struct dart4 *d, uint32_t slot)
{
	for (int i = 0; i < d->nslots; i++)
		if (d->slot[i] == slot)
			return i;
	return -1;
}

/* leaf table PA for dva in an owned slot, creating middle/leaf tables as needed (0 = failure) */
static uint64_t leaf_for(struct dart4 *d, uint64_t dva, int create)
{
	uint32_t slot = ix(dva, DART4_ROOT_SHIFT), mi = ix(dva, DART4_MID_SHIFT);
	int o = owned_index(d, slot);
	uint64_t mid, e;
	if (o < 0 || (dva >> 47))
		return 0;
	mid = d->mid[o];
	if (!mid) {
		if (!create)
			return 0;
		if (d->io.rd64(d->io.ctx, d->root + 8ull * slot))   /* firmware claimed it meanwhile */
			return 0;
		mid = d->io.alloc_table(d->io.ctx);
		if (!mid || (mid & (DART4_PAGE - 1)))
			return 0;
		d->stat_tables++;
		d->mid[o] = mid;
		d->io.barrier(d->io.ctx);                            /* the zeroed table is visible first */
		d->io.wr64(d->io.ctx, d->root + 8ull * slot, tbl_pte(mid));
	}
	e = d->io.rd64(d->io.ctx, mid + 8ull * mi);
	if (e)
		return tbl_ok(e) ? pa_of(e) : 0;
	if (!create)
		return 0;
	{
		uint64_t leaf = d->io.alloc_table(d->io.ctx);
		if (!leaf || (leaf & (DART4_PAGE - 1)))
			return 0;
		d->stat_tables++;
		d->io.barrier(d->io.ctx);
		d->io.wr64(d->io.ctx, mid + 8ull * mi, tbl_pte(leaf));
		return leaf;
	}
}

uint64_t dart4_map(struct dart4 *d, uint64_t pa, uint64_t len)
{
	uint64_t dva, off;
	if (d->failed || !len || (pa | len) & (DART4_PAGE - 1) || pa + len < pa || pa_of(tbl_pte(pa)) != pa ||
	    len > DART4_SLOT_SPAN)
		return 0;
	dva = d->next_dva;
	if (((dva + len - 1) >> DART4_ROOT_SHIFT) != (dva >> DART4_ROOT_SHIFT)) {
		int o = owned_index(d, ix(dva, DART4_ROOT_SHIFT));
		if (o < 0 || o + 1 >= d->nslots)
			return 0;
		dva = (uint64_t)d->slot[o + 1] << DART4_ROOT_SHIFT;
	}
	for (off = 0; off < len; off += DART4_PAGE) {
		uint64_t leaf = leaf_for(d, dva + off, 1);
		uint32_t li = ix(dva + off, DART4_LEAF_SHIFT);
		if (!leaf || d->io.rd64(d->io.ctx, leaf + 8ull * li)) {
			dart4_unmap(d, dva, off);
			return 0;
		}
		d->io.wr64(d->io.ctx, leaf + 8ull * li, leaf_pte(pa + off));
	}
	d->io.barrier(d->io.ctx);
	d->next_dva = dva + len + DART4_PAGE;   /* one guard page; DVAs never reused, so no TLB entry can be stale */
	d->stat_maps++;
	return dva;
}

int dart4_unmap(struct dart4 *d, uint64_t dva, uint64_t len)
{
	uint64_t off;
	if ((dva | len) & (DART4_PAGE - 1))
		return -1;
	for (off = 0; off < len; off += DART4_PAGE) {
		uint64_t leaf = leaf_for(d, dva + off, 0);
		uint32_t li = ix(dva + off, DART4_LEAF_SHIFT);
		if (!leaf)
			return -1;
		d->io.wr64(d->io.ctx, leaf + 8ull * li, 0);
	}
	return len ? flush_range(d, dva, dva + len - 1) : 0;
}

uint64_t dart4_translate(struct dart4 *d, uint64_t dva)
{
	uint64_t e;
	if (dva >> 47)
		return 0;
	e = d->io.rd64(d->io.ctx, d->root + 8ull * ix(dva, DART4_ROOT_SHIFT));
	if (!tbl_ok(e))
		return 0;
	e = d->io.rd64(d->io.ctx, pa_of(e) + 8ull * ix(dva, DART4_MID_SHIFT));
	if (!tbl_ok(e))
		return 0;
	e = d->io.rd64(d->io.ctx, pa_of(e) + 8ull * ix(dva, DART4_LEAF_SHIFT));
	if (!leaf_ok(e))
		return 0;
	return pa_of(e) | (dva & (DART4_PAGE - 1));
}
