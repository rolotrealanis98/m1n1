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

    /* n7cpu Stage C: scaled DELIVERED counter (struct cppc_dl) */
    {
        struct cppc_dl d;
        cppc_dl_init(&d, 1000);
        CHECK(cppc_dl_value(&d, 1000) == 1000 && cppc_dl_value(&d, 5000) == 5000); /* unscaled = REFERENCE */
        cppc_dl_rebase(&d, 2000, 1, 2);                                             /* half speed from 2000 */
        CHECK(cppc_dl_value(&d, 2000) == 2000);
        CHECK(cppc_dl_value(&d, 2001) == 2000 && cppc_dl_value(&d, 2002) == 2001);
        CHECK(cppc_dl_value(&d, 1500) == 2000); /* a read before the base clamps, never goes back */
        cppc_dl_rebase(&d, 2003, 1, 2);         /* remainder carried: 3 ticks at 1/2 = 1.5 */
        CHECK(cppc_dl_value(&d, 2003) == 2001 && cppc_dl_value(&d, 2004) == 2002);
        cppc_dl_rebase(&d, 2004, 3, 2); /* 1.5x */
        CHECK(cppc_dl_value(&d, 2004) == 2002 && cppc_dl_value(&d, 2006) == 2005);
        cppc_dl_rebase(&d, 2006, 0, 0); /* back to unscaled: 1:1 from the current value, no jump */
        CHECK(cppc_dl_value(&d, 2006) == 2005 && cppc_dl_value(&d, 2010) == 2009);

        /* exactness: rebasing every tick gives the same value as one long interval (no drift) */
        struct cppc_dl a, b;
        cppc_dl_init(&a, 0);
        cppc_dl_init(&b, 0);
        cppc_dl_rebase(&a, 0, 796, 957);
        cppc_dl_rebase(&b, 0, 796, 957);
        u64 prev = 0, last = 0;
        bool mono = true;
        for (u64 t = 1; t <= 1000000; t += 997) {
            cppc_dl_rebase(&a, t, 796, 957);
            u64 v = cppc_dl_value(&a, t);
            mono &= v >= prev;
            prev = v;
            last = t;
        }
        CHECK(mono);
        CHECK(cppc_dl_value(&a, last + 5) == cppc_dl_value(&b, last + 5));
        CHECK(cppc_dl_value(&b, last + 5) == (last + 5) * 796 / 957);

        /* monotonic across rate changes in both directions, 1 GHz ticks, an hour without a rebase */
        cppc_dl_init(&a, 0);
        cppc_dl_rebase(&a, 0, 1024, 957);
        u64 hour = 3600ULL * 1000000000ULL;
        u64 v1 = cppc_dl_value(&a, hour);
        CHECK(v1 == hour / 957 * 1024 + (hour % 957) * 1024 / 957);
        cppc_dl_rebase(&a, hour, 67, 957);
        CHECK(cppc_dl_value(&a, hour) == v1 && cppc_dl_value(&a, hour + 957) == v1 + 67);
    }
    {
        /* cppc_access2: REFERENCE words = now, DELIVERED words = the delivered argument */
        u64 v = 0;
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_REFERENCE, &v, false, 3, 0x1122334455667788ULL, 7) &&
              v == 0x1122334455667788ULL);
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_DELIVERED, &v, false, 3, 5, 0x99aabbccddeeff00ULL) &&
              v == 0x99aabbccddeeff00ULL);
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_DELIVERED + 4, &v, false, 2, 5, 0x99aabbccddeeff00ULL) &&
              v == 0x99aabbccULL);
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_DELIVERED, &v, false, 2, 5, 0x99aabbccddeeff00ULL) &&
              v == 0xddeeff00ULL);
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_REFERENCE + 4, &v, false, 2, 0x1122334455667788ULL, 9) &&
              v == 0x11223344ULL);
        u64 w = 0x1234; /* writes to the counters are dropped */
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_DELIVERED, &w, true, 3, 5, 6));
        CHECK(cppc_access2(&s, 3 * CPPC_SLOT + CPPC_DELIVERED, &v, false, 3, 5, 6) && v == 6);
    }

    printf("%s: win-cppc %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
