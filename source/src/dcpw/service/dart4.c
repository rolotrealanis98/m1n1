/* SPDX-License-Identifier: GPL-2.0-only */
/* dart4.c -- see dart4.h. */
#include "dart4.h"

static uint64_t pte_of(uint64_t pa) { return ((pa >> DART4_L3_SHIFT) << 10) | DART4_PTE_VALID; }
static int pte_ok(uint64_t pte) { return (pte & DART4_PTE_VALID) && !(pte & ~(DART4_PA_MASK | DART4_PTE_VALID)); }
static uint64_t pa_of(uint64_t pte) { return ((pte & DART4_PA_MASK) >> 10) << DART4_L3_SHIFT; }
static uint32_t ix(uint64_t dva, int sh) { return (uint32_t)(dva >> sh) & (DART4_ENTRIES - 1); }

int dart4_flush(struct dart4 *d)
{
	uint32_t v, i;
	for (i = 0; i < 100000 && (d->io.mmio_rd(d->io.ctx, d->base + DART4_REG_TLB_CMD) & DART4_TLB_BUSY); i++)
		;
	if (i == 100000)
		return d->failed = -1;
	v = DART4_TLB_OP_FLUSH_SID | (d->sid & 0xff);
	if (d->version >= 0x0202)
		v |= DART4_TLB_V2;
	d->io.barrier(d->io.ctx);
	d->io.mmio_wr(d->io.ctx, d->base + DART4_REG_TLB_CMD, v);
	for (i = 0; i < 100000 && (d->io.mmio_rd(d->io.ctx, d->base + DART4_REG_TLB_CMD) & DART4_TLB_BUSY); i++)
		;
	d->stat_flushes++;
	return i == 100000 ? (d->failed = -1) : 0;
}

int dart4_init(struct dart4 *d, const struct dart4_io *io, uint64_t base, uint32_t sid, uint32_t version,
               uint64_t adt_root, uint64_t vm_base, uint64_t vm_size, int nslots)
{
	uint32_t tcr, ttbr, prot, lo, hi;
	int64_t s;
	(void)0;
	uint64_t e0;
	uint8_t *z = (uint8_t *)d;
	for (unsigned i = 0; i < sizeof(*d); i++)
		z[i] = 0;
	d->io = *io;
	d->base = base;
	d->sid = sid;
	d->version = version;
	if (sid >= 32 || nslots < 1 || nslots > DART4_MAX_OWN_SLOTS || !vm_size || vm_base + vm_size < vm_base ||
	    ((vm_base + vm_size - 1) >> DART4_L0_SHIFT) != (vm_base >> DART4_L0_SHIFT) || (vm_base >> DART4_L0_SHIFT))
		return d->failed = -1;
	prot = io->mmio_rd(io->ctx, base + DART4_REG_PROTECT);
	tcr = io->mmio_rd(io->ctx, base + DART4_REG_TCR(sid));
	ttbr = io->mmio_rd(io->ctx, base + DART4_REG_TTBR(sid));
	d->root = ((uint64_t)((ttbr & 0x3ffffffcu) >> 2)) << 14;
	if (!(prot & 1) || (tcr & (DART4_TCR_TRANSLATE | DART4_TCR_4LEVEL)) != (DART4_TCR_TRANSLATE | DART4_TCR_4LEVEL) ||
	    !(ttbr & 1) || d->root != adt_root || (d->root & (DART4_PAGE - 1)))
		return d->failed = -2;
	e0 = io->rd64(io->ctx, d->root);
	if (!pte_ok(e0))
		return d->failed = -4;          /* firmware's L1 table must exist; we never create L0 entries */
	d->l1 = pa_of(e0);
	lo = ix(vm_base + DART4_SLOT_SPAN - 1, DART4_L1_SHIFT);   /* first entry wholly inside the window */
	hi = ix(vm_base + vm_size, DART4_L1_SHIFT);              /* exclusive */
	if (((vm_base + vm_size) >> DART4_L1_SHIFT) > DART4_ENTRIES)
		hi = DART4_ENTRIES;
	/* bottom-up: the first empty entries above firmware's keep our DVAs as short as macOS's
	 * (the dcp ADT node has "truncate-dvas"; INFERENCE: shorter DVAs are the safer choice) */
	for (s = (int64_t)lo; s < (int64_t)hi && d->nslots < nslots; s++)
		if (!io->rd64(io->ctx, d->l1 + 8ull * (uint64_t)s))
			d->slot[d->nslots++] = (uint32_t)s;
	if (d->nslots < nslots)
		return d->failed = -3;
	d->next_dva = (uint64_t)d->slot[0] << DART4_L1_SHIFT;
	return 0;
}

static int owned_index(struct dart4 *d, uint32_t slot)
{
	for (int i = 0; i < d->nslots; i++)
		if (d->slot[i] == slot)
			return i;
	return -1;
}

/* L3 table PA for dva in an owned L1 entry, creating L2/L3 tables as needed (0 = failure). */
static uint64_t leaf_for(struct dart4 *d, uint64_t dva, int create)
{
	uint32_t slot = ix(dva, DART4_L1_SHIFT), i2 = ix(dva, DART4_L2_SHIFT);
	int o = owned_index(d, slot);
	uint64_t l2, e;
	if (o < 0 || ix(dva, DART4_L0_SHIFT))
		return 0;
	l2 = d->l2[o];
	if (!l2) {
		if (!create)
			return 0;
		/* the entry must still be empty: firmware may have claimed it meanwhile */
		if (d->io.rd64(d->io.ctx, d->l1 + 8ull * slot))
			return 0;
		l2 = d->io.alloc_table(d->io.ctx);
		if (!l2 || (l2 & (DART4_PAGE - 1)))
			return 0;
		d->stat_tables++;
		d->l2[o] = l2;
		d->io.barrier(d->io.ctx);
		d->io.wr64(d->io.ctx, d->l1 + 8ull * slot, pte_of(l2));
	}
	e = d->io.rd64(d->io.ctx, l2 + 8ull * i2);
	if (e)
		return pte_ok(e) ? pa_of(e) : 0;
	if (!create)
		return 0;
	{
		uint64_t l3 = d->io.alloc_table(d->io.ctx);
		if (!l3 || (l3 & (DART4_PAGE - 1)))
			return 0;
		d->stat_tables++;
		d->io.barrier(d->io.ctx);
		d->io.wr64(d->io.ctx, l2 + 8ull * i2, pte_of(l3));
		return l3;
	}
}

uint64_t dart4_map(struct dart4 *d, uint64_t pa, uint64_t len)
{
	uint64_t dva, off;
	if (d->failed || !len || (pa | len) & (DART4_PAGE - 1) || pa + len < pa || pa_of(pte_of(pa)) != pa)
		return 0;
	dva = d->next_dva;
	/* stay inside one owned slot */
	if (((dva + len - 1) >> DART4_L1_SHIFT) != (dva >> DART4_L1_SHIFT)) {
		int o = owned_index(d, ix(dva, DART4_L1_SHIFT));
		if (o < 0 || o + 1 >= d->nslots || len > DART4_SLOT_SPAN)
			return 0;
		dva = (uint64_t)d->slot[o + 1] << DART4_L1_SHIFT;
	}
	for (off = 0; off < len; off += DART4_PAGE) {
		uint64_t leaf = leaf_for(d, dva + off, 1);
		uint32_t li = ix(dva + off, DART4_L3_SHIFT);
		if (!leaf || d->io.rd64(d->io.ctx, leaf + 8ull * li)) {
			dart4_unmap(d, dva, off);
			return 0;
		}
		d->io.wr64(d->io.ctx, leaf + 8ull * li, pte_of(pa + off));
	}
	d->next_dva = dva + len + DART4_PAGE;   /* one guard page between mappings */
	d->stat_maps++;
	if (dart4_flush(d))
		return 0;
	return dva;
}

int dart4_unmap(struct dart4 *d, uint64_t dva, uint64_t len)
{
	uint64_t off;
	if ((dva | len) & (DART4_PAGE - 1))
		return -1;
	for (off = 0; off < len; off += DART4_PAGE) {
		uint64_t leaf = leaf_for(d, dva + off, 0);
		uint32_t li = ix(dva + off, DART4_L3_SHIFT);
		if (!leaf)
			return -1;
		d->io.wr64(d->io.ctx, leaf + 8ull * li, 0);
	}
	return len ? dart4_flush(d) : 0;
}

uint64_t dart4_translate(struct dart4 *d, uint64_t dva)
{
	uint64_t t = d->root;
	static const int sh[4] = { DART4_L0_SHIFT, DART4_L1_SHIFT, DART4_L2_SHIFT, DART4_L3_SHIFT };
	for (int l = 0; l < 4; l++) {
		uint64_t e = d->io.rd64(d->io.ctx, t + 8ull * ix(dva, sh[l]));
		if (!pte_ok(e))
			return 0;
		t = pa_of(e);
	}
	return t | (dva & (DART4_PAGE - 1));
}
