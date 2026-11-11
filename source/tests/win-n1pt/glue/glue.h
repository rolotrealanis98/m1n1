/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: state of the resident stand-ins (glue.c) */
#ifndef N1PT_GLUE_H
#define N1PT_GLUE_H
#include "n1pt_host.h"
#include "hv.h"
#define G_MAPS 64
struct glue {
    struct { u64 from, size, to; hv_hook_t *hook; bool dev; } map[G_MAPS];
    u32 nmap;
    bool disk_used, smcp_attempted, n1_touched, storage_claims, spi_taken, aic_bad;
    bool spi_level;
    u32 spi_rises, spi_intid;
    u32 unmask[4096], mask[4096];
    bool masked[4096];
    void *pool;
    u32 hook_fail_at; /* hv_map_hook call number that fails (0 = none) */
    u32 nhook_calls;
};
extern struct glue G;
void glue_reset(void);
/* guest access through the recorded stage-2 map: hook, device map (model MMIO) or unmapped (-1) */
int guest_read(u64 ipa, int width, u64 *val);
int guest_write(u64 ipa, int width, u64 val);
#endif
