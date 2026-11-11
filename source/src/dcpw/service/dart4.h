/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dart4.h -- map buffers into a boot-locked, four-level T8110 DART stream (T6050 dart-dcp SID 23,
 * dart-disp0 SID 0) without touching the firmware's mappings.
 *
 * Behaviour ported from the Aurora apple-dart "fw_mirror" path used by the owner's Linux on this
 * machine (kernel-dcp/0004 + 0005; apple-dart.c au/t6050/dcp: fw_mirror at :4161, foreign-slot
 * rule :2134, PTE validity :816, TLB command :1261-1300). GPL-2.0-only; resident only.
 *
 *  - TTBR/TCR/PROTECT are never written (the stream stays locked on iBoot's root).
 *  - Four levels of 16 KiB tables, 11 index bits each: L0 [57:47] | L1 [46:36] (64 GiB per entry)
 *    | L2 [35:25] (32 MiB) | L3 [24:14] (16 KiB pages). With vm-base 0x100_0000_0000 and
 *    vm-size 3 TiB (ADT dart-dcp) every DVA has L0 index 0, so the unit we can own is an L1 entry.
 *  - Only L1 entries inside [vm_base, vm_base + vm_size) that are EMPTY when we start are ours; we
 *    hang our own L2 and L3 tables under them. Firmware-held entries are never modified.
 *    INFERENCE to confirm against apple-dart fw_mirror (apple_dart_fw_slot_owned) before the first
 *    DART write on hardware: that writing an empty entry of firmware's L1 table is what Linux does.
 *  - Leaf/table entries: (pa >> 14) << 10 | VALID, nothing else (apple-dart.c:816-817).
 *  - After changing entries: dsb, then TLB_CMD FLUSH_SID for the stream (with the V2 bit on DART
 *    version >= 0x0202), polling BUSY before and after (apple-dart.c:1277-1295).
 */
#ifndef DART4_H
#define DART4_H
#include <stdint.h>

#define DART4_PAGE        0x4000ull
#define DART4_ENTRIES     2048u
#define DART4_PTE_VALID   1ull
#define DART4_PA_MASK     0x0000003FFFFFFC00ull   /* bits 37:10 */
#define DART4_L3_SHIFT    14
#define DART4_L2_SHIFT    25
#define DART4_L1_SHIFT    36
#define DART4_L0_SHIFT    47
#define DART4_SLOT_SPAN   (1ull << DART4_L1_SHIFT)   /* 64 GiB of DVA per owned L1 entry */

#define DART4_REG_TCR(sid)   (0x1000u + 4u * (sid))
#define DART4_REG_TTBR(sid)  (0x1400u + 4u * (sid))
#define DART4_REG_PROTECT    0x200u
#define DART4_REG_TLB_CMD    0x80u
#define DART4_REG_PARAMS4    0x0cu   /* not read by us; version comes from the caller (ADT/probe) */
#define DART4_TLB_BUSY       (1u << 31)
#define DART4_TLB_V2         (1u << 15)
#define DART4_TLB_OP_FLUSH_SID (1u << 8)
#define DART4_TCR_TRANSLATE  (1u << 0)
#define DART4_TCR_4LEVEL     (1u << 3)

struct dart4_io {
	void *ctx;
	uint64_t (*rd64)(void *ctx, uint64_t pa);             /* DRAM (table pages) */
	void (*wr64)(void *ctx, uint64_t pa, uint64_t v);
	uint32_t (*mmio_rd)(void *ctx, uint64_t pa);
	void (*mmio_wr)(void *ctx, uint64_t pa, uint32_t v);
	uint64_t (*alloc_table)(void *ctx);                  /* zeroed, 16 KiB aligned, not guest-visible; 0 = none */
	void (*barrier)(void *ctx);                          /* dsb sy (+ cache clean if tables are cacheable) */
};

#define DART4_MAX_OWN_SLOTS 4
struct dart4 {
	struct dart4_io io;
	uint64_t base;          /* DART MMIO base */
	uint32_t sid, version;
	uint64_t root;          /* locked L0 table PA (from TTBR, == ADT l1-tt) */
	uint64_t l1;            /* firmware's L1 table under L0 entry 0 */
	int nslots;
	uint32_t slot[DART4_MAX_OWN_SLOTS];   /* L1 entries we own */
	uint64_t l2[DART4_MAX_OWN_SLOTS];     /* our L2 table per owned entry (0 = not yet) */
	uint64_t next_dva;      /* bump allocator inside our slots */
	uint32_t stat_maps, stat_flushes, stat_tables;
	int failed;
};

/* Check the stream is locked, translating, four-level, rooted at adt_root; pick nslots empty L1
 * entries inside [vm_base, vm_base+vm_size), lowest first (short DVAs, like macOS's). */
int dart4_init(struct dart4 *d, const struct dart4_io *io, uint64_t base, uint32_t sid, uint32_t version,
               uint64_t adt_root, uint64_t vm_base, uint64_t vm_size, int nslots);
/* Map [pa, pa+len) (16 KiB aligned) at a fresh DVA in our slots; returns the DVA or 0. Flushes. */
uint64_t dart4_map(struct dart4 *d, uint64_t pa, uint64_t len);
/* Unmap a range we mapped (entries cleared, flush). 0 = ok. */
int dart4_unmap(struct dart4 *d, uint64_t dva, uint64_t len);
/* Software walk (read only) of the whole tree, ours or firmware's. 0 = not mapped. */
uint64_t dart4_translate(struct dart4 *d, uint64_t dva);
int dart4_flush(struct dart4 *d);

#endif
