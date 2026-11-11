/* SPDX-License-Identifier: MIT */
/*
 * Host unit test for hv_win_cppc_core.h (cpuperf-A). Drives cppc_access() with every offset of the
 * 16 KiB page at widths 1/2/4/8 bytes, read and write, and checks the Stage A contract:
 * DESIRED stored per CPU, REFERENCE == DELIVERED == the counter, LIMITED reads 0, everything else
 * RAZ/WI, never a refusal (the guest must not take a data abort from this page).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#include "hv_win_cppc_core.h"

static int checks, fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
        }                                                                                          \
    } while (0)

static struct cppc_state s;

static u64 rd(u64 off, int width, u64 now)
{
    u64 v = 0xdeadbeefcafef00dULL;
    CHECK(cppc_access(&s, off, &v, false, width, now));
    return v;
}

static void wr(u64 off, int width, u64 v)
{
    CHECK(cppc_access(&s, off, &v, true, width, 0));
}

int main(void)
{
    const u64 base = 0xf3c000000ULL;
    const u32 n = 18;

    /* configuration checks */
    CHECK(cppc_check_config(base, n) == 0);
    CHECK(cppc_check_config(0, n) < 0);
    CHECK(cppc_check_config(base + 0x40, n) < 0);
    CHECK(cppc_check_config(base + 0x2000, n) < 0);
    CHECK(cppc_check_config(base, 0) < 0);
    CHECK(cppc_check_config(base, CPPC_MAX_CPUS) == 0);
    CHECK(cppc_check_config(base, CPPC_MAX_CPUS + 1) < 0);
    CHECK(CPPC_MAX_CPUS == 256);

    cppc_reset(&s, base, n);
    CHECK(s.configured && s.base == base && s.ncpus == n && s.odd_accesses == 0);

    /* reset state: desired 0, counters follow 'now' */
    for (u32 c = 0; c < n; c++) {
        u64 slot = c * CPPC_SLOT;
        CHECK(rd(slot + CPPC_DESIRED, 2, 5) == 0);
        CHECK(rd(slot + CPPC_LIMITED, 2, 5) == 0);
        u64 now = 0x123456789abcdefULL + c;
        CHECK(rd(slot + CPPC_REFERENCE, 3, now) == now);
        CHECK(rd(slot + CPPC_DELIVERED, 3, now) == now);
        CHECK(rd(slot + CPPC_REFERENCE, 2, now) == (now & 0xffffffffU));
        CHECK(rd(slot + CPPC_REFERENCE + 4, 2, now) == (now >> 32));
        CHECK(rd(slot + CPPC_DELIVERED, 2, now) == (now & 0xffffffffU));
        CHECK(rd(slot + CPPC_DELIVERED + 4, 2, now) == (now >> 32));
        CHECK(s.slot[c].counter_reads == 6);
    }
    CHECK(s.odd_accesses == 0);

    /* desired: per CPU, last/min/max/count; no cross-talk */
    for (u32 c = 0; c < n; c++)
        wr(c * CPPC_SLOT + CPPC_DESIRED, 2, 100 + c);
    for (u32 c = 0; c < n; c++) {
        CHECK(rd(c * CPPC_SLOT + CPPC_DESIRED, 2, 0) == 100 + c);
        CHECK(s.slot[c].desired_writes == 1);
        CHECK(s.slot[c].desired_min == 100 + c && s.slot[c].desired_max == 100 + c);
    }
    wr(13 * CPPC_SLOT + CPPC_DESIRED, 2, 1024);
    wr(13 * CPPC_SLOT + CPPC_DESIRED, 2, 50);
    wr(13 * CPPC_SLOT + CPPC_DESIRED, 2, 0x1ffffffffULL); /* upper bits of the GPR ignored */
    CHECK(rd(13 * CPPC_SLOT + CPPC_DESIRED, 2, 0) == 0xffffffffU);
    CHECK(s.slot[13].desired_writes == 4 && s.slot[13].desired_min == 50 &&
          s.slot[13].desired_max == 0xffffffffU);
    CHECK(rd(12 * CPPC_SLOT + CPPC_DESIRED, 2, 0) == 112);
    CHECK(rd(14 * CPPC_SLOT + CPPC_DESIRED, 2, 0) == 114);

    /* counters are read-only, limited drops writes */
    wr(2 * CPPC_SLOT + CPPC_REFERENCE, 3, 7);
    CHECK(rd(2 * CPPC_SLOT + CPPC_REFERENCE, 3, 99) == 99);
    wr(2 * CPPC_SLOT + CPPC_LIMITED, 2, 0xffffffff);
    CHECK(rd(2 * CPPC_SLOT + CPPC_LIMITED, 2, 99) == 0);
    CHECK(s.odd_accesses == 0);

    /* sweep: every offset x width x direction. Known registers behave as above; the rest RAZ/WI */
    u64 odd_before = s.odd_accesses;
    unsigned odd = 0, known = 0;
    struct cppc_slot snap[CPPC_MAX_CPUS];
    memcpy(snap, s.slot, sizeof(snap));
    for (u64 off = 0; off < CPPC_PAGE_SIZE; off++) {
        for (int w = 0; w <= 3; w++) {
            u64 reg = off % CPPC_SLOT, cpu = off / CPPC_SLOT;
            bool aligned = !(off & ((1UL << w) - 1));
            bool is_known = cpu < n && aligned &&
                            ((w == 2 && (reg == CPPC_DESIRED || reg == CPPC_LIMITED)) ||
                             ((w == 2 || w == 3) && reg >= CPPC_REFERENCE && reg < CPPC_LIMITED));
            u64 v = rd(off, w, 0x1111222233334444ULL);
            if (!is_known) {
                CHECK(v == 0);
                odd++;
            } else {
                known++;
            }
            if (!is_known) {
                wr(off, w, 0x5a5a5a5a5a5a5a5aULL);
                odd++;
            }
        }
    }
    CHECK(s.odd_accesses - odd_before == odd);
    for (u32 c = 0; c < n; c++)
        CHECK(s.slot[c].desired == snap[c].desired && s.slot[c].desired_writes == snap[c].desired_writes);
    CHECK(known == n * (2 /*desired,limited*/ + 4 /*32-bit counter words*/ + 2 /*64-bit*/));

    /* CPUs beyond ncpus: RAZ/WI */
    wr(n * CPPC_SLOT + CPPC_DESIRED, 2, 77);
    CHECK(rd(n * CPPC_SLOT + CPPC_DESIRED, 2, 0) == 0);
    CHECK(rd(n * CPPC_SLOT + CPPC_REFERENCE, 3, 5) == 0);

    printf("%s: win-cppc %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
