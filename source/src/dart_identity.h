/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DART_IDENTITY_H
#define DART_IDENTITY_H

/*
 * Four-level identity tables (DVA == PA) for T8110-type DARTs, see dart_identity.c. Needs u32/u64
 * from the includer (dart.h, or the host test's shim).
 */

struct dart_range {
    u64 base;
    u64 size;
};

struct dart_identity {
    u64 *pool;   /* ntables 16 KiB tables, pool[0..2047] is the top level */
    u32 ntables; /* tables in the pool */
    u32 leaves;  /* last-level tables */
    u64 root;    /* PA of the top-level table (= pool) */
};

/*
 * Build the tables for 1..64 sorted, disjoint, 16 KiB aligned ranges below 2^42, from one heap
 * allocation that must not overlap any range. 0, or -1 with the reason printed.
 */
int dart_identity_build(struct dart_identity *id, const struct dart_range *ranges, u32 count);
void dart_identity_free(struct dart_identity *id);
/* DVA -> PA through the tables, for checks; DART_IDENTITY_UNMAPPED when not mapped. */
#define DART_IDENTITY_UNMAPPED (~0ULL)
u64 dart_identity_lookup(const struct dart_identity *id, u64 dva);

#endif
