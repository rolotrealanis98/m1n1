/* SPDX-License-Identifier: MIT */
/*
 * Bare-metal build of src/hv/win/hv_win_pmu.c for the QEMU EL2 harness
 * (tests/win-pmu-rewrite/qemu): real system register accesses, barriers, cache and TLB maintenance
 * at EL2; printf to the PL011; one CPU.
 */

#ifndef PMU_HOST_H
#define PMU_HOST_H

#include "exception.h"
#include "smp_asm.h"
#include "types.h"

#define min(a, b)        (((a) < (b)) ? (a) : (b))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#define sysop(op)    __asm__ volatile(op::: "memory")
#define ic_ialluis() sysop("ic ialluis")
#define mrs(reg)                                                                                   \
    ({                                                                                             \
        u64 _val;                                                                                  \
        __asm__ volatile("mrs\t%0, " #reg : "=r"(_val));                                           \
        _val;                                                                                      \
    })
#define msr(reg, val)                                                                              \
    ({                                                                                             \
        u64 _v = (val);                                                                            \
        __asm__ volatile("msr\t" #reg ", %0" : : "r"(_v));                                         \
    })

int bm_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf bm_printf
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

void dc_civac_range(void *addr, size_t length);
void dc_cvac_range(void *addr, size_t length);
size_t heapblock_remaining(void);
void *heapblock_alloc_aligned(size_t size, size_t align);

static inline int smp_id(void)
{
    return 0;
}

struct boot_video {
    u64 base, display, stride, width, height, depth;
};
struct boot_args {
    struct boot_video video;
};
extern struct boot_args cur_boot_args;

#define HV_XT_PAGE_SIZE 0x4000UL
#define HV_XT_FLIPS_MAX 7U
enum hv_xt_state {
    HV_XT_UNTRACKED = 0,
    HV_XT_DIRTY,
    HV_XT_SCANNING,
    HV_XT_CODE,
    HV_XT_PINNED,
    HV_XT_CODE_XO,
};
int hv_xt_track(u64 ipa, u64 size, bool dry_run, u64 *new_tables);
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips);
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips);
int hv_xt_remap(u64 ipa, u64 pa, enum hv_xt_state state, u32 flips);
void hv_xt_tlbi(bool broadcast);
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa);

#endif
