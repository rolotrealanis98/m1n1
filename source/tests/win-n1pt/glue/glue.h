/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: state of the resident stand-ins (glue.c) */
#ifndef N1PT_GLUE_H
#define N1PT_GLUE_H
#include "n1pt_host.h"
#include "hv.h"
#define G_MAPS 64
struct glue {
    struct { u64 from, size, to; hv_hook_t *hook; bool dev, ram; } map[G_MAPS];
    u32 nmap;
    bool disk_used, smcp_attempted, n1_touched, storage_claims, spi_taken, aic_bad;
    bool spi_level;
    u32 spi_rises, spi_intid;
    u32 unmask[4096], mask[4096];
    bool masked[4096];
    void *pool;
    u32 hook_fail_at; /* hv_map_hook call number that fails (0 = none) */
    u32 nhook_calls;
    bool ram_fail;       /* hv_map_hw fails */
    u64 foreign_ipa;     /* a non-n1pt stage-2 entry planted inside the pool (0 = none) */
    bool tables_built;   /* the launcher's final stage-2 build happened (hv_pt_walk is valid) */
};
extern struct glue G;
void glue_reset(void);
/* guest access through the recorded stage-2 map: hook, device map (model MMIO) or unmapped (-1) */
int guest_read(u64 ipa, int width, u64 *val);
int guest_write(u64 ipa, int width, u64 val);
/* a guest access as the CPU would make it: through the map, or the abort path (hv_handle_dabort's n1pt branch);
 * returns 1 device/RAM, 0 hook, -1 unresolved (would have been escalated to the host) */
int guest_access(u64 ipa, bool write, u64 *val);
void glue_unmap(u64 from, u64 size); /* test: drop every map overlapping [from, from+size) */
#endif
