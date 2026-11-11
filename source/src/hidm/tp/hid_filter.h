/* SPDX-License-Identifier: MIT */
/*
 * Make a coprocessor HID report descriptor acceptable to Windows / the HIDM driver
 * core by removing vendor-defined top-level collections (usage page >= 0xFF00).
 *
 * Why (hardware run n3tp1, 2026-11-11): the J716s keyboard descriptor (189 bytes) ends
 * with a vendor TLC (usage page 0xFF00, usage 0x06, report 0x3f) containing
 * "76 00 40 95 02 b1 01" = a constant feature field of Report Size 16384 bits. The
 * HIDM driver core rejects Report Size > 32 (and the report exceeds every HIDM limit),
 * so hidm5bus failed to start (0xC0000186) and no HID child appeared. Linux keeps the
 * whole descriptor (hid-apple uses the vendor channel only for diagnostics); Windows'
 * keyboard and consumer-control clients need only the standard TLCs.
 *
 * Freestanding, no allocation. Own code (MIT).
 */
#ifndef HID_FILTER_H
#define HID_FILTER_H
#include "../abi/hidm_abi.h"

struct hid_filter_result {
    hm_u32 len;                /* bytes kept */
    hm_u32 removed_tlcs;       /* vendor TLCs removed */
    hm_u8 input_ids[32];       /* bitmap of input report IDs still declared (bit 0 = no IDs) */
};

/* Filters d[0..n) in place. Returns 0 on success, -1 on a malformed descriptor
 * (unbalanced collections, truncated item): then d is untouched. */
int hid_strip_vendor_tlcs(hm_u8 *d, hm_u32 n, struct hid_filter_result *r);
static inline int hid_filter_keeps_input(const struct hid_filter_result *r, hm_u8 id)
{ return (r->input_ids[id >> 3] >> (id & 7)) & 1; }
#endif
