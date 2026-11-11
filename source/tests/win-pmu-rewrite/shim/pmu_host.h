/* SPDX-License-Identifier: MIT */
/*
 * Host build of src/hv/win/hv_win_pmu.c (tests/win-pmu-rewrite): what the file takes from m1n1 at
 * EL2 -- the stage-2 execute-tracking primitives of hv_vm.c, cache maintenance, the heap,
 * boot_args, printf, smp_id() -- is provided by the mocks in test_pmu_rewrite.c. The hv_xt
 * declarations repeat src/hv/hv.h's.
 */

#ifndef PMU_HOST_H
#define PMU_HOST_H

#include <stdio.h>
#include <stdlib.h> /* calloc, free: hv_win_pmu.c's 4 KiB frame bytes (nightly-5) */
#include <string.h>

#include "exception.h"
#include "smp_asm.h"
#include "types.h"

#define min(a, b) (((a) < (b)) ? (a) : (b))

/* src/utils.h */
#define sysop(op)                                                                                  \
    do {                                                                                           \
    } while (0)
void host_ic_ialluis(void);
#define ic_ialluis() host_ic_ialluis()
int host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf host_printf

/* src/memory.h */
void dc_civac_range(void *addr, size_t length);
void dc_cvac_range(void *addr, size_t length);

/* src/heapblock.h */
size_t heapblock_remaining(void);
size_t heapblock_total_remaining(void);                  /* n5c */
void heapblock_pool_info(u64 *base, u64 *size, u64 *used); /* n5c */

/* src/smp.h */
extern int host_cpu;
static inline int smp_id(void)
{
    return host_cpu;
}

/* src/xnuboot.h (the part hv_win_pmu.c reads) */
struct boot_video {
    u64 base;
    u64 display;
    u64 stride;
    u64 width;
    u64 height;
    u64 depth;
};
struct boot_args {
    struct boot_video video;
};
extern struct boot_args cur_boot_args;

/* src/hv/hv.h: execute tracking */
#define HV_XT_PAGE_SIZE 0x4000UL
#define HV_XT_FLIPS_MAX 7U
enum hv_xt_state {
    HV_XT_UNTRACKED = 0,
    HV_XT_DIRTY,
    HV_XT_SCANNING,
    HV_XT_CODE,
    HV_XT_PINNED,
};
int hv_xt_track(u64 ipa, u64 size, bool dry_run, u64 *new_tables);
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips);
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips);
void hv_xt_tlbi(bool broadcast);
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa);

#endif
