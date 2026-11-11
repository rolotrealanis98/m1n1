/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef N1PT_DART_H
#define N1PT_DART_H

/*
 * n1pt (wireless/n1-native-20261111): one offset translation (IOVA -> PA, IOVA != PA) for the N1's DMA pool on
 * /arm-io/dart-apcie0 streams 1-3 (RID 0x100-0x102). Same T8110 table format as dart_identity.c (16 KiB tables,
 * PA[43:14] in bits 39:10, VALID bit 0, leaf subpage range 0..0xfff), adapted from it for a single offset range.
 * Needs u32/u64/uintptr_t from the includer.
 */

#define N1PT_DART_UNMAPPED (~0ULL)
#define N1PT_DART_MAX_TABLES 8

struct n1pt_dart_tables {
    u64 *pool;    /* ntables 16 KiB tables: [0] top (DVA[46:36]), [1] middle (DVA[35:25]), [2..] leaves */
    u32 ntables;
    u64 iova, pa, size;
    u64 root4;    /* TTBR target with TCR.FOUR_LEVEL: the top table */
    u64 root3;    /* without FOUR_LEVEL (DVA < 64 GiB): the middle table */
};

/*
 * Build the tables mapping [iova, iova + size) -> [pa, pa + size). All 16 KiB aligned; iova + size <= 2^36 (one
 * middle table, so the 3-level fallback covers it) and < 2^40 (the contract); the range must not contain the
 * MSI doorbell page 0xfffff000; pa + size <= 2^42; the tables (one heap allocation) must not overlap [pa, pa+size).
 * 0, or -1 with the reason printed.
 */
int n1pt_dart_build(struct n1pt_dart_tables *t, u64 iova, u64 pa, u64 size);
void n1pt_dart_free(struct n1pt_dart_tables *t);
/* DVA -> PA through the tables (from root4 or root3), N1PT_DART_UNMAPPED when not mapped */
u64 n1pt_dart_lookup(const struct n1pt_dart_tables *t, u64 dva, int four_level);

/*
 * Point the streams in sid_mask (bits 1..3 for the N1) of the T8110 instance at `base` at the tables: TCR 0,
 * TTBR, enable the streams, flush, then TCR TRANSLATE_ENABLE | FOUR_LEVEL; if FOUR_LEVEL does not stick on the
 * first stream, every stream uses the 3-level root with TRANSLATE_ENABLE only. Refuses a locked instance
 * (PROTECT), a stream beyond PARAMS4, a VA width below what the range needs, or a pending error. Returns 0 (and
 * *four_level), else negative after blocking the streams it touched (TCR = TTBR = 0).
 */
int n1pt_dart_attach(uintptr_t base, const struct n1pt_dart_tables *t, u32 sid_mask, int *four_level);
/* TCR = TTBR = 0 on the streams in sid_mask, disable them, flush (PARK: no DMA path left) */
int n1pt_dart_block(uintptr_t base, u32 sid_mask);

#endif
