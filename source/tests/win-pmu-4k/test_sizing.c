/* SPDX-License-Identifier: MIT */
/*
 * n5b: hv_win_pmu_arm()'s heap need against the exact guest RAM layout of the first nightly-5 hardware run
 * (runB-nightly5 runs/20261111-060921-nightly5-a-n5-a1, hv.log lines 833-837): five ranges, the last one
 * 0x1010e274000+0xa5a78c000 (41.4 GiB, 1324 L3 tables to split), 47872 KiB of heap left when it was armed.
 * nightly-5 kept one byte per 4 KiB frame (10.35 MiB for that range) and refused it; n5b keeps one byte per
 * 16 KiB block. The comparison is exact: need = tables * 16 KiB + size / 16 KiB + 16 MiB margin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmu_host.h"
#include "hv_win.h"
#undef printf

int host_cpu;
u64 hv_cpus_in_guest;
struct hv_win_state hv_win;
struct boot_args cur_boot_args;
void hv_win_pmu_host_reset(void);
static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static const struct { u64 base, size, tables; } R[] = {
    {0x1001e618000UL, 0x10024000000UL - 0x1001e618000UL, 2},
    {0x10072700000UL, 0x100b4000000UL - 0x10072700000UL, 32},
    {0x100c1f30000UL, 0x100c22a4000UL - 0x100c1f30000UL, 0},
    {0x100c2328000UL, 0x10100000000UL - 0x100c2328000UL, 30},
    {0x1010e274000UL, 0xa5a78c000UL, 1324},
};
#define BIG 4
static size_t heap;      /* heapblock_remaining() */
static int last_range;   /* which range hv_xt_track() is asked about */

size_t heapblock_remaining(void) { return heap; }
int host_printf(const char *fmt, ...) { (void)fmt; return 0; }
void host_ic_ialluis(void) {}
void dc_civac_range(void *a, size_t n) { (void)a; (void)n; }
void dc_cvac_range(void *a, size_t n) { (void)a; (void)n; }
int hv_xt_track(u64 ipa, u64 size, bool dry, u64 *tables)
{
    (void)dry;
    for (int i = 0; i < 5; i++)
        if (R[i].base == ipa && R[i].size == size) {
            last_range = i;
            *tables = R[i].tables;
            return (int)(size / HV_XT_PAGE_SIZE);
        }
    return -1;
}
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips) { (void)ipa; (void)pa; (void)flips; return HV_XT_UNTRACKED; }
int hv_xt_set(u64 ipa, enum hv_xt_state s, u32 f) { (void)ipa; (void)s; (void)f; return -1; }
void hv_xt_tlbi(bool b) { (void)b; }
bool hv_xt_fault_ipa(struct exc_info *c, bool s, bool w, u64 *ipa) { (void)c; (void)s; (void)w; (void)ipa; return false; }

static u64 need(int i, bool g4k)
{
    return R[i].tables * 16384 + (g4k ? R[i].size / 16384 : 0) + (16UL << 20);
}

static void arm_all(bool g4k, u64 big_heap)
{
    hv_win_pmu_host_reset();
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    CHECK(hv_win_pmu_set_mode(g4k ? 0 : HV_WIN_PMU_MODE_16K) == 0);
    heap = 64UL << 20;
    for (int i = 0; i < BIG; i++) {
        CHECK(hv_win_pmu_arm(R[i].base, R[i].size) == (int)(R[i].size / 16384));
        heap -= need(i, g4k) - (16UL << 20); /* what the arm took */
    }
    heap = big_heap;
}

int main(void)
{
    u64 hw_left = 47872UL << 10, big_state_old = R[BIG].size / 4096;
    /* the hardware refusal: nightly-5's need included 10.35 MiB of per-frame bytes the message did not print */
    CHECK(R[BIG].tables * 16384 + (16UL << 20) + big_state_old > hw_left);
    CHECK(R[BIG].tables * 16384 + (16UL << 20) <= hw_left); /* the printed tables alone fitted */
    for (int g = 0; g < 2; g++) {
        u64 n = need(BIG, g);
        CHECK(n <= hw_left); /* n5b: fits with the hardware's heap, both granules */
        arm_all(g, hw_left);
        CHECK(hv_win_pmu_arm(R[BIG].base, R[BIG].size) > 0 && last_range == BIG);
        arm_all(g, n);       /* exactly the need: accepted */
        CHECK(hv_win_pmu_arm(R[BIG].base, R[BIG].size) > 0);
        arm_all(g, n - 1);   /* one byte short: refused */
        CHECK(hv_win_pmu_arm(R[BIG].base, R[BIG].size) == -1);
        CHECK(hv_win_pmu_stat(HV_WIN_PMU_STAT_HEAP + 0) == n - 1 &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_HEAP + 1) == 16UL << 20);
        printf("sizing (%s granule): big range need %lu KiB (tables %lu + state %lu + margin 16384), "
               "hardware had %lu KiB: slack %lu KiB\n", g ? "4 KiB" : "16 KiB", n >> 10,
               R[BIG].tables * 16, g ? R[BIG].size / 16384 >> 10 : 0, hw_left >> 10, (hw_left - n) >> 10);
    }
    hv_win_pmu_host_reset();
    printf("win-pmu-4k sizing: %lu checks, 0 failed\n", checks);
    return 0;
}
