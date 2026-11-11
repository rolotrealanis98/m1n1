/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dart4.h -- map buffers into a boot-locked "four-level" T8110 DART stream (T6050 dart-dcp SID 23,
 * dart-disp0 SIDs 0 and 16) without touching the firmware's mappings.
 *
 * Behaviour of the Aurora apple-dart fw_mirror path the owner's Linux runs on this machine
 * (kernel-dcp/0004 + 0005; apple-dart.c au/t6050/dcp, extracted copy in the scratchpad):
 *  - "Four-level" (TCR bit 3) = the TTBR table + two more: the TTBR (root) table is indexed by
 *    DVA[46:36] (64 GiB per slot), then DVA[35:25], then the leaf at DVA[24:14]
 *    (apple-dart.c:1366-1372 slot_shift = pgshift + 2*bits; :2089-2090).
 *  - Linux publishes only into VACANT root slots and never writes a firmware-held slot or any
 *    table below one (apple_dart_publish_root, :906-950). We do the same: claim empty root slots
 *    in a window, hang our own middle and leaf tables under them.
 *  - Table entry: (pa >> 14) << 10 | VALID. Leaf entry: the same | SP_END 0xfff in bits 51:40,
 *    bits 63:52 zero, protection bits 3:1 clear = read+write+cacheable
 *    (apple_dart_fw_leaf_decode :829-841; m1n1 dart.c:176,192).
 *  - TTBR/TCR/PROTECT are never written. No TLB invalidate on map: the entries were never valid
 *    (the ADT says avoid-tlbi-in-map, flush-by-dva). On unmap, a RANGE invalidate of our DVAs only,
 *    never a whole-stream flush (which made the DCP panic: apple-dart.c:1328-1340, 1497-1499).
 * Window: Linux's DCP shmem on this machine sits at DVA 0x1ffffe00000 / 0x1ffffffc000, root slot 31
 * (m5pro-linux contrib/validated/display/stage-c-dcp-journal-excerpt.txt:7-12), and the firmware
 * accepted it ("shared memory dva: 0x1ffffe00000"). We claim slots top-down from the window's end.
 */
#ifndef DART4_H
#define DART4_H
#include <stdint.h>

#define DART4_PAGE        0x4000ull
#define DART4_ENTRIES     2048u
#define DART4_PTE_VALID   1ull
#define DART4_PA_MASK     0x0000003FFFFFFC00ull   /* bits 37:10 */
#define DART4_SP_END      (0xfffull << 40)
#define DART4_LEAF_SHIFT  14
#define DART4_MID_SHIFT   25
#define DART4_ROOT_SHIFT  36
#define DART4_SLOT_SPAN   (1ull << DART4_ROOT_SHIFT)   /* 64 GiB of DVA per root slot */

#define DART4_REG_PARAMS3    0x08u
#define DART4_REG_TLB_CMD    0x80u
#define DART4_REG_TLB_START  0x98u
#define DART4_REG_TLB_END    0xa0u
#define DART4_REG_PROTECT    0x200u
#define DART4_REG_TCR(sid)   (0x1000u + 4u * (sid))
#define DART4_REG_TTBR(sid)  (0x1400u + 4u * (sid))
#define DART4_TLB_BUSY       (1u << 31)
#define DART4_TLB_V2         (1u << 15)
#define DART4_TLB_RANGE      (1u << 14)
#define DART4_TLB_STT        (1u << 13)
#define DART4_TLB_OP_FLUSH_SID (1u << 8)
#define DART4_TCR_TRANSLATE  (1u << 0)
#define DART4_TCR_4LEVEL     (1u << 3)

struct dart4_io {
	void *ctx;
	uint64_t (*rd64)(void *ctx, uint64_t pa);             /* DRAM (table pages) */
	void (*wr64)(void *ctx, uint64_t pa, uint64_t v);     /* must clean the entry to PoC */
	uint32_t (*mmio_rd)(void *ctx, uint64_t pa);
	void (*mmio_wr)(void *ctx, uint64_t pa, uint32_t v);
	uint64_t (*alloc_table)(void *ctx);                  /* zeroed, 16 KiB aligned, not guest-visible; 0 = none */
	void (*barrier)(void *ctx);                          /* dsb sy */
};

#define DART4_MAX_OWN_SLOTS 4
struct dart4 {
	struct dart4_io io;
	uint64_t base;          /* DART MMIO base */
	uint32_t sid, version;  /* version = PARAMS3[15:0] (major << 8 | minor) */
	uint64_t root;          /* locked root (TTBR) table PA, == ADT l1-tt */
	int nslots;
	uint32_t slot[DART4_MAX_OWN_SLOTS];   /* root slots we own */
	uint64_t mid[DART4_MAX_OWN_SLOTS];    /* our middle table per owned slot (0 = not yet) */
	uint64_t next_dva;      /* bump allocator; DVAs are never reused */
	uint32_t stat_maps, stat_flushes, stat_tables;
	int failed;
};

/* Check the stream is locked, translating, four-level, rooted at adt_root; claim nslots EMPTY root
 * slots inside [win_base, win_base + win_size), highest first. 0 = ok; nothing is written. */
int dart4_init(struct dart4 *d, const struct dart4_io *io, uint64_t base, uint32_t sid,
               uint64_t adt_root, uint64_t win_base, uint64_t win_size, int nslots);
/* Map [pa, pa+len) (16 KiB aligned) at a fresh DVA in our slots; returns the DVA or 0. */
uint64_t dart4_map(struct dart4 *d, uint64_t pa, uint64_t len);
/* Unmap a range we mapped (entries cleared, range invalidate). 0 = ok. */
int dart4_unmap(struct dart4 *d, uint64_t dva, uint64_t len);
/* Read-only walk of the whole tree, ours or firmware's. 0 = not mapped. */
uint64_t dart4_translate(struct dart4 *d, uint64_t dva);

#endif
