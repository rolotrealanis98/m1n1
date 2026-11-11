/* SPDX-License-Identifier: MIT */
/*
 * perf-exits host test: src/hv/win/hv_win_perf.h, the code the firmware runs in hv_exc_sync_fast()
 * (hv_exc.c) and hv_win_perf_cpu_apply() (hv_win.c).
 *  1. hv_perf_is_mdscr(): exhaustive over every (op0, op1, CRn, CRm, op2) x direction x Rt with EC 0x18:
 *     true exactly for MDSCR_EL1 (cross-checked against the firmware's own SYSREG_ISS(SYS_MDSCR_EL1));
 *     false for every other EC, for IL = 0, and with the reserved ISS bits set it still decodes only the
 *     register fields.
 *  2. hv_perf_mdscr_emulate() == the slow path (a verbatim model of hv_handle_msr_unlocked()'s
 *     SYS_MDSCR_EL1 case, PERCPU shadow, regs[31] = 0 first) on random frames: x0..x30 and the shadow
 *     identical afterwards for every Rt and direction (regs[31] is frame padding, never restored).
 *  3. hv_perf_mdcr_pass(): TDE/TDA cleared, TDOSA/TDRA set, every other bit kept.
 *  4. statistics table: counts per key, collisions, overflow into "other", top-k order, decode.
 *  5. rate/permille helpers incl. overflow-free large deltas.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TYPES_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define BIT(x) (1UL << (x))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#ifndef PERF_HDR
#define PERF_HDR "../../src/hv/win/hv_win_perf.h"
#endif
#include PERF_HDR

/* the firmware's ISS encoding macros (arm_cpu_regs.h), for the cross-check */
#define sys_reg(op0, op1, CRn, CRm, op2) , _S, op0, op1, CRn, CRm, op2 /* types.h, C branch */
#include "arm_cpu_regs.h"

static int fails;
static u64 checks;
#define CHECK(name, cond)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            if (fails++ < 20)                                                                      \
                printf("FAIL: %s\n", name);                                                        \
        }                                                                                          \
    } while (0)

static u64 esr_sr(u32 ec, bool il, u32 op0, u32 op1, u32 crn, u32 crm, u32 op2, u32 rt, bool rd)
{
    return ((u64)ec << 26) | (il ? BIT(25) : 0) | HV_PERF_ISS_SR(op0, op1, crn, crm, op2) | ((u64)rt << 5) |
           (rd ? 1 : 0);
}

/* verbatim model of the slow path: hv_handle_msr_unlocked() for SYS_MDSCR_EL1 (hv_exc.c) */
static bool slow_path(u64 esr, u64 *regs, u64 *shadow)
{
    u64 iss = esr & 0x1ffffff;
    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn | ESR_ISS_MSR_CRm);
    u64 rt = (iss & ESR_ISS_MSR_Rt) >> 5;
    bool is_read = iss & ESR_ISS_MSR_DIR;
    if (((esr >> 26) & 0x3f) != 0x18)
        return false;
    regs[31] = 0;
    if (reg != (u64)SYSREG_ISS(SYS_MDSCR_EL1))
        return false;
    if (is_read)
        regs[rt] = *shadow;
    else
        *shadow = regs[rt];
    return true;
}

static u64 rnd(void)
{
    static u64 s = 0x9e3779b97f4a7c15ULL;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

int main(void)
{
    /* 1. decode */
    CHECK("HV_PERF_ISS_MDSCR == SYSREG_ISS(SYS_MDSCR_EL1)", HV_PERF_ISS_MDSCR == (u64)SYSREG_ISS(SYS_MDSCR_EL1));
    CHECK("HV_PERF_ISS_REGMASK == the firmware's ESR_ISS_MSR field mask",
          HV_PERF_ISS_REGMASK ==
              (u64)(ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn | ESR_ISS_MSR_CRm));
    u64 hits = 0;
    for (u32 op0 = 0; op0 < 4; op0++)
        for (u32 op1 = 0; op1 < 8; op1++)
            for (u32 crn = 0; crn < 16; crn++)
                for (u32 crm = 0; crm < 16; crm++)
                    for (u32 op2 = 0; op2 < 8; op2++) {
                        bool want = op0 == 2 && op1 == 0 && crn == 0 && crm == 2 && op2 == 2;
                        for (u32 rt = 0; rt < 32; rt += (want ? 1 : 31)) /* every Rt for MDSCR, 0/31 otherwise */
                            for (int rd = 0; rd < 2; rd++) {
                                u32 grt = 99;
                                bool gr = !rd;
                                bool got = hv_perf_is_mdscr(esr_sr(0x18, true, op0, op1, crn, crm, op2, rt, rd), &grt, &gr);
                                CHECK("is_mdscr exactly for S2_0_C0_C2_2", got == want);
                                if (got) {
                                    hits++;
                                    CHECK("decoded Rt", grt == rt);
                                    CHECK("decoded direction", gr == (bool)rd);
                                }
                            }
                    }
    CHECK("64 MDSCR encodings matched (32 Rt x 2 directions)", hits == 64);
    for (u32 ec = 0; ec < 64; ec++) {
        u32 rt;
        bool rd;
        if (ec != 0x18)
            CHECK("other EC rejected", !hv_perf_is_mdscr(esr_sr(ec, true, 2, 0, 0, 2, 2, 3, 1), &rt, &rd));
    }
    {
        u32 rt;
        bool rd;
        CHECK("IL = 0 rejected", !hv_perf_is_mdscr(esr_sr(0x18, false, 2, 0, 0, 2, 2, 3, 1), &rt, &rd));
        /* upper ESR bits (ISS2 etc.) do not matter, a set bit 24 / 22 inside ISS is not a register field */
        u64 e = esr_sr(0x18, true, 2, 0, 0, 2, 2, 7, 0) | BIT(32) | BIT(40);
        CHECK("ISS2/RES0 bits above 31 ignored", hv_perf_is_mdscr(e, &rt, &rd) && rt == 7 && !rd);
        CHECK("bits 24:22 (not register fields) ignored",
              hv_perf_is_mdscr(esr_sr(0x18, true, 2, 0, 0, 2, 2, 7, 0) | BIT(22) | BIT(24), &rt, &rd));
    }

    /* 2. emulation == slow path */
    for (int it = 0; it < 200000; it++) {
        u64 a[32], b[32], sa, sb;
        for (int i = 0; i < 32; i++)
            a[i] = b[i] = rnd();
        sa = sb = rnd();
        u32 rt = rnd() % 32;
        bool rd = rnd() & 1;
        bool mdscr = (rnd() % 4) != 0;
        u64 esr = mdscr ? esr_sr(0x18, true, 2, 0, 0, 2, 2, rt, rd)
                        : esr_sr(0x18, true, rnd() % 4, rnd() % 8, rnd() % 16, rnd() % 16, rnd() % 8, rt, rd);
        if (!mdscr && (esr & HV_PERF_ISS_REGMASK) == HV_PERF_ISS_MDSCR)
            mdscr = true;
        bool f = hv_perf_mdscr_emulate(esr, a, &sa);
        bool s = slow_path(esr, b, &sb);
        CHECK("fast handles exactly what the slow path handles as MDSCR", f == s && f == mdscr);
        if (!f) {
            CHECK("declined: frame untouched", memcmp(a, b, 31 * 8) == 0 && sa == sb);
            continue;
        }
        CHECK("x0..x30 equal after", memcmp(a, b, 31 * 8) == 0);
        CHECK("shadow equal after", sa == sb);
        if (rt == 31 && !rd)
            CHECK("MSR from XZR writes 0", sa == 0);
        if (rt == 31 && rd)
            CHECK("MRS to XZR changes nothing", memcmp(a, b, 31 * 8) == 0);
    }

    /* 3. MDCR */
    for (int it = 0; it < 10000; it++) {
        u64 m = rnd(), p = hv_perf_mdcr_pass(m);
        CHECK("TDE/TDA cleared", !(p & (HV_PERF_MDCR_TDE | HV_PERF_MDCR_TDA)));
        CHECK("TDOSA/TDRA set", (p & (HV_PERF_MDCR_TDOSA | HV_PERF_MDCR_TDRA)) == (HV_PERF_MDCR_TDOSA | HV_PERF_MDCR_TDRA));
        CHECK("other bits kept", (p & ~(u64)0xf00) == (m & ~(u64)0xf00));
    }
    CHECK("the hardware value 0xf00 -> 0xc00", hv_perf_mdcr_pass(0xf00) == 0xc00);
    CHECK("FGT debug bits are 0..3", HV_PERF_HDFG_DBGBW == 0xf);

    /* 4. statistics table */
    {
        struct hv_perf_sr t[HV_PERF_SR_N];
        u64 other = 0;
        memset(t, 0, sizeof(t));
        u64 e_md = esr_sr(0x18, true, 2, 0, 0, 2, 2, 0, 1), e_cv = esr_sr(0x18, true, 3, 3, 14, 3, 1, 9, 0);
        for (int i = 0; i < 1000; i++)
            hv_perf_sr_note(t, &other, e_md | ((u64)(i % 31) << 5)); /* Rt does not split the key */
        for (int i = 0; i < 300; i++)
            hv_perf_sr_note(t, &other, e_cv);
        bool taken[HV_PERF_SR_N] = {false};
        int i0 = hv_perf_sr_top(t, taken);
        CHECK("top is MDSCR", i0 >= 0 && t[i0].count == 1000);
        u32 o0, o1, cn, cm, o2;
        bool rd;
        hv_perf_sr_decode(t[i0].key, &o0, &o1, &cn, &cm, &o2, &rd);
        CHECK("decode MDSCR mrs", o0 == 2 && o1 == 0 && cn == 0 && cm == 2 && o2 == 2 && rd);
        taken[i0] = true;
        int i1 = hv_perf_sr_top(t, taken);
        CHECK("second is CNTV_CTL msr", i1 >= 0 && t[i1].count == 300);
        hv_perf_sr_decode(t[i1].key, &o0, &o1, &cn, &cm, &o2, &rd);
        CHECK("decode CNTV_CTL_EL0 msr", o0 == 3 && o1 == 3 && cn == 14 && cm == 3 && o2 == 1 && !rd);
        taken[i1] = true;
        CHECK("nothing else", hv_perf_sr_top(t, taken) < 0);
        t[i0].reported = t[i0].count;
        bool none[HV_PERF_SR_N] = {false};
        CHECK("reported entries drop out of the top", hv_perf_sr_top(t, none) == i1);
        /* fill: 40 distinct registers into 32 slots */
        memset(t, 0, sizeof(t));
        other = 0;
        for (u32 k = 0; k < 40; k++)
            for (u32 n = 0; n <= k; n++)
                hv_perf_sr_note(t, &other, esr_sr(0x18, true, 3, k & 7, (k >> 3) & 15, 1, 0, 0, 1));
        u64 sum = 0;
        int used = 0;
        for (int i = 0; i < HV_PERF_SR_N; i++) {
            sum += t[i].count;
            used += t[i].key != 0;
        }
        CHECK("table full", used == HV_PERF_SR_N);
        CHECK("every event counted once (table + other)", sum + other == 40 * 41 / 2);
        CHECK("direction splits the key",
              hv_perf_sr_key(esr_sr(0x18, true, 2, 0, 0, 2, 2, 0, 1)) != hv_perf_sr_key(esr_sr(0x18, true, 2, 0, 0, 2, 2, 0, 0)));
        CHECK("key never 0", hv_perf_sr_key(0) != 0);
    }

    /* 5. helpers */
    CHECK("rate 1000 over 5 s at 24 MHz = 200", hv_perf_rate(1000, 5 * 24000000ULL, 24000000) == 200);
    CHECK("rate dt 0 = 0", hv_perf_rate(5, 0, 24000000) == 0);
    CHECK("rate large delta no overflow", hv_perf_rate(1ULL << 50, 24000000ULL, 24000000) == 1ULL << 50);
    CHECK("permille half", hv_perf_permille(50, 100) == 500);
    CHECK("permille capped", hv_perf_permille(150, 100) == 1000);
    CHECK("permille dt 0", hv_perf_permille(1, 0) == 0);
    CHECK("flags", HV_PERF_ALL == 7 && HV_PERF_STATS == 1 && HV_PERF_FAST_MDSCR == 2 && HV_PERF_MDSCR_PASS == 4);

    printf("win-perf: %lu checks, %d failed\n", (unsigned long)checks, fails);
    return fails ? 1 : 0;
}
