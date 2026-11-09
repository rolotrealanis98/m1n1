/* SPDX-License-Identifier: MIT */
/*
 * Host test for the Windows EL2 runtime's stage-2 PMUv3 rewrite:
 *   src/hv/win/hv_win_pmu_rewrite.c  classification and in-place rewrite of instruction words
 *   src/hv/win/hv_win_pmu.c          the per-page state machine, against a mock of hv_vm.c's
 * hv_xt_*
 *
 *   test_pmu_rewrite [-v]                        synthetic tests (exit status 1 on any failure)
 *   test_pmu_rewrite --dis FILE.dis [...]        every instruction of an llvm-objdump listing: the
 *                                                classifier against LLVM's decoding of the same
 * word test_pmu_rewrite --pe FILE [...]             PMU-looking words per PE section, code and data
 *   test_pmu_rewrite --emit-sysreg-space FILE    every op0 = 3 MRS/MSR encoding (for --dis via
 * LLVM)
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv_win.h"
#include "hv_win_pmu_rewrite.h"
#include "pmu_host.h"

/* pmu_host.h routes the firmware's printf to host_printf(); this file prints for itself */
#undef printf

void hv_win_pmu_host_reset(void);

static int verbose;
static unsigned long checks, failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                        \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
        }                                                                                          \
    } while (0)

static u64 xs_state = 0x9e3779b97f4a7c15UL;
static u32 xorshift32(void)
{
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return (u32)(xs_state >> 16);
}

/* ---- encodings ----------------------------------------------------------------------------------
 */

static u32 sysreg_insn(int l, u32 op0, u32 op1, u32 crn, u32 crm, u32 op2, u32 rt)
{
    return 0xd5000000U | (u32)l << 21 | (op0 & 3) << 19 | op1 << 16 | crn << 12 | crm << 8 |
           op2 << 5 | rt;
}

struct reg {
    const char *name;
    u32 op1, crn, crm, op2;
};

/* Allocated PMUv3 registers (Arm ARM, FEAT_PMUv3 .. FEAT_PMUv3p9), PMEVCNTR/TYPER added in code */
static const struct reg pmu_regs[] = {
    {"PMCR_EL0", 3, 9, 12, 0},       {"PMCNTENSET_EL0", 3, 9, 12, 1},
    {"PMCNTENCLR_EL0", 3, 9, 12, 2}, {"PMOVSCLR_EL0", 3, 9, 12, 3},
    {"PMSWINC_EL0", 3, 9, 12, 4},    {"PMSELR_EL0", 3, 9, 12, 5},
    {"PMCEID0_EL0", 3, 9, 12, 6},    {"PMCEID1_EL0", 3, 9, 12, 7},
    {"PMCCNTR_EL0", 3, 9, 13, 0},    {"PMXEVTYPER_EL0", 3, 9, 13, 1},
    {"PMXEVCNTR_EL0", 3, 9, 13, 2},  {"PMZR_EL0", 3, 9, 13, 4},
    {"PMUSERENR_EL0", 3, 9, 14, 0},  {"PMOVSSET_EL0", 3, 9, 14, 3},
    {"PMINTENSET_EL1", 0, 9, 14, 1}, {"PMINTENCLR_EL1", 0, 9, 14, 2},
    {"PMUACR_EL1", 0, 9, 14, 4},     {"PMECR_EL1", 0, 9, 14, 5},
    {"PMMIR_EL1", 0, 9, 14, 6},      {"PMIAR_EL1", 0, 9, 14, 7},
    {"PMCCFILTR_EL0", 3, 14, 15, 7},
};

/* Registers next to the PMU space that must never be touched */
static const struct reg neighbours[] = {
    {"CNTFRQ_EL0", 3, 14, 0, 0},      {"CNTPCT_EL0", 3, 14, 0, 1},
    {"CNTVCT_EL0", 3, 14, 0, 2},      {"CNTPCTSS_EL0", 3, 14, 0, 5},
    {"CNTVCTSS_EL0", 3, 14, 0, 6},    {"CNTP_TVAL_EL0", 3, 14, 2, 0},
    {"CNTP_CTL_EL0", 3, 14, 2, 1},    {"CNTP_CVAL_EL0", 3, 14, 2, 2},
    {"CNTV_CTL_EL0", 3, 14, 3, 1},    {"CNTKCTL_EL1", 0, 14, 1, 0},
    {"PMSCR_EL1", 0, 9, 9, 0},        {"PMSIDR_EL1", 0, 9, 9, 7},
    {"PMBLIMITR_EL1", 0, 9, 10, 0},   {"PMBPTR_EL1", 0, 9, 10, 1},
    {"PMBSR_EL1", 0, 9, 10, 3},       {"PMBIDR_EL1", 0, 9, 10, 7},
    {"PMSCR_EL2", 4, 9, 9, 0},        {"PMICNTR_EL0", 3, 9, 4, 0},
    {"PMICFILTR_EL0", 3, 9, 6, 0},    {"AMCR_EL0", 3, 13, 2, 0},
    {"AMEVCNTR00_EL0", 3, 13, 4, 0},  {"ICC_SGI1R_EL1", 0, 12, 11, 5},
    {"ICC_IAR1_EL1", 0, 12, 12, 0},   {"MDCR_EL2", 4, 1, 1, 1},
    {"TPIDR_EL0", 3, 13, 0, 2},       {"S3_2_C9_C13_0", 2, 9, 13, 0},
    {"S3_4_C9_C13_0", 4, 9, 13, 0},   {"S3_7_C9_C12_0", 7, 9, 12, 0},
    {"S3_1_C9_C14_1", 1, 9, 14, 1},   {"S3_3_C9_C11_0", 3, 9, 11, 0},
    {"S3_3_C9_C15_0", 3, 9, 15, 0},   {"S3_3_C14_C7_7", 3, 14, 7, 7},
    {"S3_0_C9_C13_3", 0, 9, 13, 3},   {"S3_0_C14_C8_0", 0, 14, 8, 0},
    {"S3_3_C15_C13_0", 3, 15, 13, 0},
};

static enum pmu_rw_kind expected_kind(int l, const struct reg *r)
{
    if (!l)
        return PMU_RW_WRITE;
    return strcmp(r->name, "PMCCNTR_EL0") ? PMU_RW_READ_OTHER : PMU_RW_READ_PMCCNTR;
}

static u32 expected_replacement(int l, const struct reg *r, u32 rt)
{
    switch (expected_kind(l, r)) {
        case PMU_RW_READ_PMCCNTR:
            return 0xd53be040U | rt;
        case PMU_RW_READ_OTHER:
            return 0xd2800000U | rt;
        default:
            return 0xd503201fU;
    }
}

static void check_reg(const struct reg *r, bool pmu)
{
    for (int l = 0; l < 2; l++) {
        for (u32 rt = 0; rt < 32; rt++) {
            u32 insn = sysreg_insn(l, 3, r->op1, r->crn, r->crm, r->op2, rt);
            enum pmu_rw_kind k = pmu_rw_classify(insn);
            u32 rep = pmu_rw_replacement(insn);
            if (pmu) {
                CHECK(k == expected_kind(l, r), "%s %s x%u: kind %d", l ? "mrs" : "msr", r->name,
                      rt, k);
                CHECK(rep == expected_replacement(l, r, rt), "%s x%u: 0x%08x -> 0x%08x", r->name,
                      rt, insn, rep);
                CHECK(pmu_rw_classify(rep) == PMU_RW_NONE, "replacement 0x%08x is PMU", rep);
            } else {
                CHECK(k == PMU_RW_NONE, "%s %s x%u (0x%08x) classified %d", l ? "mrs" : "msr",
                      r->name, rt, insn, k);
                CHECK(rep == insn, "%s rewritten to 0x%08x", r->name, rep);
            }
        }
    }
}

static void test_known_encodings(void)
{
    /* assembled with llvm-mc --triple=aarch64 -mattr=+all */
    static const struct {
        u32 insn, rep;
        enum pmu_rw_kind kind;
    } known[] = {
        {0xd53b9d00, 0xd53be040,
         PMU_RW_READ_PMCCNTR}, /* mrs x0, PMCCNTR_EL0 -> mrs x0, CNTVCT_EL0 */
        {0xd53b9d1e, 0xd53be05e, PMU_RW_READ_PMCCNTR}, /* x30 */
        {0xd51b9c00, 0xd503201f, PMU_RW_WRITE},        /* msr PMCR_EL0, x0 -> nop */
        {0xd53bebde, 0xd280001e, PMU_RW_READ_OTHER},   /* mrs x30, PMEVCNTR30_EL0 -> mov x30, #0 */
        {0xd51befe3, 0xd503201f, PMU_RW_WRITE},        /* msr PMCCFILTR_EL0, x3 */
        {0xd5389e22, 0xd2800002, PMU_RW_READ_OTHER},   /* mrs x2, PMINTENSET_EL1 */
        {0xd53b9e00, 0xd2800000, PMU_RW_READ_OTHER},   /* mrs x0, PMUSERENR_EL0 */
        {0xd51b9e60, 0xd503201f, PMU_RW_WRITE},        /* msr PMOVSSET_EL0, x0 */
        {0xd5389ec0, 0xd2800000, PMU_RW_READ_OTHER},   /* mrs x0, PMMIR_EL1 */
        {0xd53be041, 0xd53be041, PMU_RW_NONE},         /* mrs x1, CNTVCT_EL0 */
        {0xd53be020, 0xd53be020, PMU_RW_NONE},         /* mrs x0, CNTPCT_EL0 */
        {0xd5389900, 0xd5389900, PMU_RW_NONE},         /* mrs x0, PMSCR_EL1 (SPE) */
        {0xd503201f, 0xd503201f, PMU_RW_NONE},         /* nop */
        {0xd2800005, 0xd2800005, PMU_RW_NONE},         /* mov x5, #0 */
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        CHECK(pmu_rw_classify(known[i].insn) == known[i].kind, "0x%08x", known[i].insn);
        CHECK(pmu_rw_replacement(known[i].insn) == known[i].rep, "0x%08x -> 0x%08x", known[i].insn,
              pmu_rw_replacement(known[i].insn));
    }
}

static void test_registers(void)
{
    for (size_t i = 0; i < sizeof(pmu_regs) / sizeof(pmu_regs[0]); i++)
        check_reg(&pmu_regs[i], true);

    for (u32 n = 0; n < 31; n++) {
        char name[32];
        struct reg c = {name, 3, 14, 8 + (n >> 3), n & 7};
        struct reg t = {name, 3, 14, 12 + (n >> 3), n & 7};
        snprintf(name, sizeof(name), "PMEVCNTR%u_EL0", n);
        check_reg(&c, true);
        snprintf(name, sizeof(name), "PMEVTYPER%u_EL0", n);
        check_reg(&t, true);
    }

    for (size_t i = 0; i < sizeof(neighbours) / sizeof(neighbours[0]); i++)
        check_reg(&neighbours[i], false);

    /* the same fields with op0 = 0, 1, 2 (MSR immediate/hints, SYS/SYSL, debug registers) */
    for (u32 op0 = 0; op0 < 3; op0++)
        for (int l = 0; l < 2; l++)
            for (u32 rt = 0; rt < 32; rt++) {
                u32 insn = sysreg_insn(l, op0, 3, 9, 13, 0, rt);
                CHECK(pmu_rw_classify(insn) == PMU_RW_NONE, "op0 %u: 0x%08x", op0, insn);
            }
}

/* The whole 0xd5xxxxxx space against a table of the (op1, CRn, CRm) blocks; everything else at
 * random */
static bool in_pmu_block(u32 op1, u32 crn, u32 crm)
{
    static const u32 blocks[][3] = {{3, 9, 12},  {3, 9, 13},  {3, 9, 14},  {0, 9, 14},
                                    {3, 14, 8},  {3, 14, 9},  {3, 14, 10}, {3, 14, 11},
                                    {3, 14, 12}, {3, 14, 13}, {3, 14, 14}, {3, 14, 15}};
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (blocks[i][0] == op1 && blocks[i][1] == crn && blocks[i][2] == crm)
            return true;
    return false;
}

static void test_exhaustive(void)
{
    unsigned long hits = 0, bad = 0;

    for (u32 w = 0xd5000000U; w <= 0xd5ffffffU; w++) {
        bool sysreg_op03 = (w >> 22) == 0x354 && ((w >> 19) & 3) == 3;
        bool pmu = sysreg_op03 && in_pmu_block((w >> 16) & 7, (w >> 12) & 15, (w >> 8) & 15);
        enum pmu_rw_kind k = pmu_rw_classify(w);
        if ((k != PMU_RW_NONE) != pmu) {
            if (bad++ < 10)
                fprintf(stderr, "exhaustive: 0x%08x kind %d, table says %d\n", w, k, pmu);
        } else if (pmu) {
            hits++;
            u32 rep = pmu_rw_replacement(w);
            if (pmu_rw_classify(rep) != PMU_RW_NONE && bad++ < 10)
                fprintf(stderr, "exhaustive: replacement 0x%08x of 0x%08x is PMU\n", rep, w);
            if (k == PMU_RW_WRITE && rep != 0xd503201fU && bad++ < 10)
                fprintf(stderr, "exhaustive: msr 0x%08x -> 0x%08x\n", w, rep);
        } else if (pmu_rw_replacement(w) != w && bad++ < 10) {
            fprintf(stderr, "exhaustive: 0x%08x changed\n", w);
        }
    }
    checks++;
    if (bad)
        failures++;
    /* 12 blocks x 8 op2 x 2 directions x 32 Rt */
    CHECK(hits == 12 * 8 * 2 * 32, "hits %lu", hits);

    unsigned long outside = 0;
    for (unsigned long i = 0; i < 64UL * 1024 * 1024; i++) {
        u32 w = xorshift32();
        if ((w >> 24) == 0xd5)
            continue;
        if (pmu_rw_classify(w) != PMU_RW_NONE || pmu_rw_replacement(w) != w)
            outside++;
    }
    CHECK(outside == 0, "%lu random words outside 0xd5xxxxxx classified as PMU", outside);
    if (verbose)
        printf("exhaustive: %lu PMU encodings in 0xd5000000..0xd5ffffff, 64M random words clean\n",
               hits);
}

/* ---- page rewriter ------------------------------------------------------------------------------
 */

struct rec_log {
    u32 n;
    u32 index[4096];
    u32 old_insn[4096], new_insn[4096];
};

static void rec_fn(void *arg, u32 index, u32 old_insn, u32 new_insn)
{
    struct rec_log *l = arg;
    l->index[l->n] = index;
    l->old_insn[l->n] = old_insn;
    l->new_insn[l->n] = new_insn;
    l->n++;
}

static u32 random_pmu_insn(void)
{
    const struct reg *r = &pmu_regs[xorshift32() % (sizeof(pmu_regs) / sizeof(pmu_regs[0]))];
    return sysreg_insn(xorshift32() & 1, 3, r->op1, r->crn, r->crm, r->op2, xorshift32() & 31);
}

static u32 random_other_insn(void)
{
    for (;;) {
        u32 w = xorshift32();
        if (xorshift32() % 8 == 0) { /* plenty of system-register moves that are not PMU */
            const struct reg *r =
                &neighbours[xorshift32() % (sizeof(neighbours) / sizeof(neighbours[0]))];
            w = sysreg_insn(xorshift32() & 1, 3, r->op1, r->crn, r->crm, r->op2, xorshift32() & 31);
        }
        if (pmu_rw_classify(w) == PMU_RW_NONE)
            return w;
    }
}

static void test_page(void)
{
    static u32 page[4096], orig[4096], pmu_at[4096];
    static struct rec_log log;

    for (int round = 0; round < 200; round++) {
        u32 want = 0;
        u64 counts[PMU_RW_NR_KINDS] = {0}, want_counts[PMU_RW_NR_KINDS] = {0};

        for (u32 i = 0; i < 4096; i++) {
            bool pmu = (round == 0 && (i == 0 || i == 4095)) || xorshift32() % 97 == 0;
            page[i] = pmu ? random_pmu_insn() : random_other_insn();
            pmu_at[i] = pmu;
            if (pmu) {
                want++;
                want_counts[pmu_rw_classify(page[i])]++;
            }
        }
        memcpy(orig, page, sizeof(page));
        log.n = 0;

        u32 n = pmu_rw_rewrite_words(page, 4096, rec_fn, &log, counts);
        CHECK(n == want && log.n == want, "round %d: %u rewritten, %u expected, %u recorded", round,
              n, want, log.n);
        for (int k = 0; k < PMU_RW_NR_KINDS; k++)
            CHECK(counts[k] == want_counts[k], "round %d kind %d: %lu vs %lu", round, k, counts[k],
                  want_counts[k]);

        u32 j = 0;
        for (u32 i = 0; i < 4096; i++) {
            if (pmu_at[i]) {
                CHECK(j < log.n && log.index[j] == i, "round %d: record %u index", round, j);
                CHECK(j < log.n && log.old_insn[j] == orig[i] && log.new_insn[j] == page[i],
                      "round %d: record %u words", round, j);
                CHECK(page[i] == pmu_rw_replacement(orig[i]), "round %d word %u", round, i);
                j++;
            } else {
                CHECK(page[i] == orig[i], "round %d: word %u changed 0x%08x -> 0x%08x", round, i,
                      orig[i], page[i]);
            }
        }

        memcpy(orig, page, sizeof(page));
        CHECK(pmu_rw_rewrite_words(page, 4096, NULL, NULL, NULL) == 0, "second pass rewrote");
        CHECK(!memcmp(orig, page, sizeof(page)), "second pass changed the page");
    }
}

/* ---- mock of hv_vm.c's execute tracking and the rest of m1n1
 * ------------------------------------- */

#define EC_IABORT_LOWER 0x20UL
#define EC_DABORT_LOWER 0x24UL
#define ESR(ec, iss)    ((ec) << 26 | 1UL << 25 | (iss))
#define FSC_PERM_L3     0x0f
#define FSC_TRANS_L3    0x07
#define ISS_S1PTW       (1UL << 7)
#define ISS_WNR         (1UL << 6)

struct hv_win_state hv_win;
u64 hv_cpus_in_guest;
struct boot_args cur_boot_args;
int host_cpu;

#define M_BASE  0x10000000000UL
#define M_PAGES 320
#define M_WORDS 4096

static u32 *m_mem;
static struct {
    bool mapped, writable, tracked;
    enum hv_xt_state st;
    u32 flips;
} m_pg[M_PAGES];
static size_t m_heap = 128UL << 20;
static bool m_fault_ok = true; /* AT succeeds */
static u64 m_fault_ipa;        /* what AT (or HPFAR for a walker fault) gives */
static u64 m_hpfar_ipa;        /* HPFAR_EL2 for a non-walker fault: UNKNOWN, 0 = unmapped */
static bool m_fault_s1ptw_seen;

enum ev_kind { EV_SET, EV_TLBI_BCAST, EV_TLBI_LOCAL, EV_CIVAC, EV_CVAC, EV_IC };
static struct {
    enum ev_kind kind;
    int page;
    enum hv_xt_state st;
} ev[64];
static int nev;
static bool scan_order_ok = true;
static char last_line[512];

static void ev_add(enum ev_kind k, int page, enum hv_xt_state st)
{
    if (nev < 64) {
        ev[nev].kind = k;
        ev[nev].page = page;
        ev[nev].st = st;
    }
    nev++;
}

static int m_page_of(u64 ipa)
{
    if (ipa < M_BASE || ipa >= M_BASE + (u64)M_PAGES * HV_XT_PAGE_SIZE)
        return -1;
    return (int)((ipa - M_BASE) / HV_XT_PAGE_SIZE);
}

static int m_page_of_pa(void *pa)
{
    return (int)(((u32 *)pa - m_mem) / M_WORDS);
}

int hv_xt_track(u64 ipa, u64 size, bool dry_run, u64 *new_tables)
{
    int pages = 0;
    u64 last_block = ~0UL;

    *new_tables = 0;
    if ((ipa | size) & (HV_XT_PAGE_SIZE - 1) || !size)
        return -1;
    for (u64 a = ipa; a < ipa + size; a += HV_XT_PAGE_SIZE) {
        int p = m_page_of(a);
        if (p < 0 || !m_pg[p].mapped || !m_pg[p].writable)
            continue;
        if (a >> 25 != last_block) {
            (*new_tables)++;
            last_block = a >> 25;
        }
        if (!dry_run) {
            m_pg[p].tracked = true;
            m_pg[p].st = HV_XT_DIRTY;
            m_pg[p].flips = 0;
        }
        pages++;
    }
    return pages;
}

enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips)
{
    int p = m_page_of(ipa);
    if (p < 0 || !m_pg[p].tracked)
        return HV_XT_UNTRACKED;
    if (pa)
        *pa = (u64)&m_mem[(size_t)p * M_WORDS];
    if (flips)
        *flips = m_pg[p].flips;
    return m_pg[p].st;
}

int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips)
{
    int p = m_page_of(ipa);
    if (p < 0 || !m_pg[p].tracked || (ipa & (HV_XT_PAGE_SIZE - 1)))
        return -1;
    m_pg[p].st = state;
    m_pg[p].flips = min(flips, HV_XT_FLIPS_MAX);
    ev_add(EV_SET, p, state);
    return 0;
}

void hv_xt_tlbi(bool broadcast)
{
    ev_add(broadcast ? EV_TLBI_BCAST : EV_TLBI_LOCAL, -1, 0);
}

bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa)
{
    (void)ctx;
    (void)write;
    m_fault_s1ptw_seen = s1ptw;
    if (s1ptw && !(ctx->esr & ISS_S1PTW)) {
        *ipa = m_hpfar_ipa; /* HPFAR consulted as a hint for a plain fetch */
        return true;
    }
    if (!m_fault_ok)
        return false;
    *ipa = m_fault_ipa;
    return true;
}

void dc_civac_range(void *addr, size_t length)
{
    int p = m_page_of_pa(addr);
    /* the scan starts here: the page must already be read-only for every CPU */
    if (length != HV_XT_PAGE_SIZE || m_pg[p].st != HV_XT_SCANNING || nev < 2 ||
        ev[nev - 1].kind != EV_TLBI_BCAST || ev[nev - 2].kind != EV_SET ||
        ev[nev - 2].st != HV_XT_SCANNING)
        scan_order_ok = false;
    ev_add(EV_CIVAC, p, 0);
}

void dc_cvac_range(void *addr, size_t length)
{
    if (length != HV_XT_PAGE_SIZE)
        scan_order_ok = false;
    ev_add(EV_CVAC, m_page_of_pa(addr), 0);
}

void host_ic_ialluis(void)
{
    ev_add(EV_IC, -1, 0);
}

size_t heapblock_remaining(void)
{
    return m_heap;
}

int host_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(last_line, sizeof(last_line), fmt, ap);
    va_end(ap);
    if (verbose)
        fputs(last_line, stdout);
    return n;
}

/* ---- state machine tests ------------------------------------------------------------------------
 */

static u64 ipa_of(int page, u32 word)
{
    return M_BASE + (u64)page * HV_XT_PAGE_SIZE + 4UL * word;
}

static u32 *words_of(int page)
{
    return &m_mem[(size_t)page * M_WORDS];
}

static bool iabort(int page, u32 word, u64 iss, u64 elr)
{
    struct exc_info ctx = {.esr = ESR(EC_IABORT_LOWER, iss), .elr = elr, .far = elr};
    m_fault_ipa = ipa_of(page, word);
    nev = 0;
    return hv_win_handle_iabort(&ctx);
}

static bool dabort(int page, u64 iss, u64 elr)
{
    struct exc_info ctx = {.esr = ESR(EC_DABORT_LOWER, iss), .elr = elr, .far = 0x1234};
    m_fault_ipa = ipa_of(page, 5);
    nev = 0;
    return hv_win_handle_dabort(&ctx, ctx.esr);
}

static bool ev_is(int i, enum ev_kind k, int page, enum hv_xt_state st)
{
    return i < nev && ev[i].kind == k && (k != EV_SET || (ev[i].page == page && ev[i].st == st)) &&
           ((k != EV_CIVAC && k != EV_CVAC) || ev[i].page == page);
}

static void reset_world(void)
{
    hv_win_pmu_host_reset();
    memset(m_pg, 0, sizeof(m_pg));
    for (int i = 0; i < M_PAGES; i++) {
        m_pg[i].mapped = i != 7;   /* a hole */
        m_pg[i].writable = i != 8; /* a read-only HW page: never tracked */
    }
    for (size_t i = 0; i < (size_t)M_PAGES * M_WORDS; i++)
        m_mem[i] = 0xd503201f;
    hv_win.irq_model = HV_WIN_IRQ_AIC_PASSTHROUGH;
    hv_cpus_in_guest = 0;
    cur_boot_args.video.base = M_BASE + 0x100000000UL;
    cur_boot_args.video.stride = 0x3600;
    cur_boot_args.video.height = 2234;
    m_heap = 128UL << 20;
    m_fault_ok = true;
    m_hpfar_ipa = 0;
    host_cpu = 0;
}

static void test_arm(void)
{
    reset_world();
    struct exc_info ctx = {.esr = ESR(EC_IABORT_LOWER, FSC_PERM_L3)};
    CHECK(!hv_win_handle_iabort(&ctx), "handled before arming");
    CHECK(!hv_win_handle_dabort(&ctx, ESR(EC_DABORT_LOWER, ISS_WNR | FSC_PERM_L3)),
          "dabort handled before arming");
    hv_win_pmu_tick();
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_ID) == (HV_WIN_PMU_MAGIC << 32 | 1UL << 16),
          "id 0x%lx before arming", hv_win_pmu_stat(HV_WIN_PMU_ID));

    u64 size = (u64)M_PAGES * HV_XT_PAGE_SIZE;
    hv_win.irq_model = HV_WIN_IRQ_NONE;
    CHECK(hv_win_pmu_arm(M_BASE, size) == -1, "armed before CONFIGURE");
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    hv_cpus_in_guest = 2;
    CHECK(hv_win_pmu_arm(M_BASE, size) == -1, "armed with a guest CPU running");
    hv_cpus_in_guest = 0;
    CHECK(hv_win_pmu_arm(M_BASE + 0x1000, size) == -1, "unaligned base");
    CHECK(hv_win_pmu_arm(M_BASE, 0x2000) == -1, "unaligned size");
    CHECK(hv_win_pmu_arm(M_BASE, 0) == -1, "empty range");
    CHECK(hv_win_pmu_arm(0x20000000000UL, size) == -1, "unmapped range");
    CHECK(hv_win_pmu_arm(cur_boot_args.video.base - 0x4000, 0x8000) == -1, "framebuffer overlap");
    m_heap = 16UL << 20;
    CHECK(hv_win_pmu_arm(M_BASE, size) == -1, "heap check");
    m_heap = 128UL << 20;
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_ID) == (HV_WIN_PMU_MAGIC << 32 | 1UL << 16),
          "armed by a refusal");

    /* two ranges, holes and read-only pages skipped */
    int a = hv_win_pmu_arm(M_BASE, 16 * HV_XT_PAGE_SIZE);
    CHECK(a == 14, "first range: %d pages", a);
    CHECK(hv_win_pmu_arm(M_BASE + 8 * HV_XT_PAGE_SIZE, 16 * HV_XT_PAGE_SIZE) == -1, "overlap");
    int b = hv_win_pmu_arm(M_BASE + 16 * HV_XT_PAGE_SIZE, size - 16 * HV_XT_PAGE_SIZE);
    CHECK(b == M_PAGES - 16, "second range: %d pages", b);
    CHECK(nev > 0 && ev[nev - 1].kind == EV_TLBI_BCAST, "no TLB flush after arming");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_ID) == (HV_WIN_PMU_MAGIC << 32 | 1UL << 16 | 1), "not armed");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PAGES) == (u64)(a + b), "pages stat");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_RANGES) == 2, "ranges stat");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_TABLES) == 2, "tables stat %lu",
          hv_win_pmu_stat(HV_WIN_PMU_TABLES));
    CHECK(m_pg[0].st == HV_XT_DIRTY && !m_pg[7].tracked && !m_pg[8].tracked, "tracked set");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_NR_STATS) == ~0UL && hv_win_pmu_stat(0x400) == ~0UL,
          "out-of-range stat");
}

static void arm_all(void)
{
    reset_world();
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    nev = 0;
    hv_win_pmu_arm(M_BASE, (u64)M_PAGES * HV_XT_PAGE_SIZE);
}

static void test_exec_and_write(void)
{
    arm_all();

    u32 *w = words_of(3);
    w[10] = 0xd53b9d05;   /* mrs x5, PMCCNTR_EL0 */
    w[11] = 0xd51b9c01;   /* msr PMCR_EL0, x1 */
    w[12] = 0xd53be041;   /* mrs x1, CNTVCT_EL0 */
    w[4095] = 0xd53b9c1e; /* mrs x30, PMCR_EL0 */

    CHECK(iabort(3, 12, FSC_PERM_L3, 0xfffff80001234000UL), "exec fault not handled");
    CHECK(m_pg[3].st == HV_XT_CODE, "state %d after the scan", m_pg[3].st);
    CHECK(w[10] == 0xd53be045 && w[11] == 0xd503201f && w[12] == 0xd53be041 &&
              w[4095] == 0xd280001e,
          "words %08x %08x %08x %08x", w[10], w[11], w[12], w[4095]);
    CHECK(scan_order_ok, "scan before write permission was revoked everywhere");
    CHECK(nev == 7 && ev_is(0, EV_SET, 3, HV_XT_SCANNING) && ev_is(1, EV_TLBI_BCAST, 0, 0) &&
              ev_is(2, EV_CIVAC, 3, 0) && ev_is(3, EV_CVAC, 3, 0) && ev_is(4, EV_IC, 0, 0) &&
              ev_is(5, EV_SET, 3, HV_XT_CODE) && ev_is(6, EV_TLBI_LOCAL, 0, 0),
          "event order (%d events)", nev);
    CHECK(!m_fault_s1ptw_seen, "IPA taken from HPFAR for a plain fetch");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_SCANS) == 1 &&
              hv_win_pmu_stat(HV_WIN_PMU_PAGES_REWRITTEN) == 1 &&
              hv_win_pmu_stat(HV_WIN_PMU_INSNS) == 3 &&
              hv_win_pmu_stat(HV_WIN_PMU_INSNS_PMCCNTR) == 1 &&
              hv_win_pmu_stat(HV_WIN_PMU_INSNS_MRS) == 1 &&
              hv_win_pmu_stat(HV_WIN_PMU_INSNS_MSR) == 1,
          "rewrite stats");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_IPA + 0) == ipa_of(3, 10) &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_OP + 0) == (0xd53b9d05UL << 32 | 0xd53be045) &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_ELR + 0) == 0xfffff80001234000UL &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_IPA + 2) == ipa_of(3, 4095) &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_OP + 1) == (0xd51b9c01UL << 32 | 0xd503201f),
          "log entries");
    CHECK(strstr(last_line, "IPA 0x1000000c000") != NULL, "console line: %s", last_line);

    /* a page with nothing to rewrite: no cache maintenance for the I-side */
    CHECK(iabort(4, 0, FSC_PERM_L3, 0x1000), "clean page");
    CHECK(nev == 5 && ev_is(0, EV_SET, 4, HV_XT_SCANNING) && ev_is(1, EV_TLBI_BCAST, 0, 0) &&
              ev_is(2, EV_CIVAC, 4, 0) && ev_is(3, EV_SET, 4, HV_XT_CODE) &&
              ev_is(4, EV_TLBI_LOCAL, 0, 0),
          "clean page events (%d)", nev);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_SCANS) == 2 &&
              hv_win_pmu_stat(HV_WIN_PMU_PAGES_REWRITTEN) == 1,
          "clean page stats");

    /* spurious fetch fault on a CODE page: local flush, retry, nothing else */
    CHECK(iabort(3, 0, FSC_PERM_L3, 0x2000), "spurious exec");
    CHECK(nev == 1 && ev_is(0, EV_TLBI_LOCAL, 0, 0) && m_pg[3].st == HV_XT_CODE,
          "spurious exec events");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_SPURIOUS) == 1, "spurious stat");

    /* a write to CODE: writable again, never executable on any CPU first */
    CHECK(dabort(3, ISS_WNR | FSC_PERM_L3, 0x3000), "write fault");
    CHECK(m_pg[3].st == HV_XT_DIRTY && m_pg[3].flips == 1, "after write: %d/%u", m_pg[3].st,
          m_pg[3].flips);
    CHECK(nev == 2 && ev_is(0, EV_SET, 3, HV_XT_DIRTY) && ev_is(1, EV_TLBI_BCAST, 0, 0),
          "write events");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WRITE_FLIPS) == 1, "write flip stat");

    /* the guest writes a new PMU instruction; the next fetch rescans */
    w[20] = 0xd53b9d00;
    CHECK(iabort(3, 20, FSC_PERM_L3, 0x4000) && w[20] == 0xd53be040 && m_pg[3].st == HV_XT_CODE,
          "rescan after a write");
    CHECK(m_pg[3].flips == 1, "flips kept across the scan");

    /* a write fault on a DIRTY page is a stale TLB entry */
    CHECK(dabort(4, ISS_WNR | FSC_PERM_L3, 0x5000) && m_pg[4].st == HV_XT_DIRTY, "write to CODE 4");
    CHECK(dabort(4, ISS_WNR | FSC_PERM_L3, 0x5000) && nev == 1 && ev_is(0, EV_TLBI_LOCAL, 0, 0),
          "spurious write");

    /* not ours: reads, translation faults, untracked IPAs, the read-only page */
    CHECK(!dabort(3, FSC_PERM_L3, 0x6000), "read permission fault taken");
    CHECK(!dabort(3, ISS_WNR | FSC_TRANS_L3, 0x6000), "translation fault taken");
    CHECK(!iabort(3, 0, FSC_TRANS_L3, 0x6000), "instruction translation fault taken");
    u64 before = hv_win_pmu_stat(HV_WIN_PMU_NOT_OURS);
    CHECK(!iabort(8, 0, FSC_PERM_L3, 0x6000) && !dabort(8, ISS_WNR | FSC_PERM_L3, 0x6000),
          "untracked page taken");
    m_fault_ipa = 0x20000000000UL;
    struct exc_info ctx = {.esr = ESR(EC_IABORT_LOWER, FSC_PERM_L3)};
    CHECK(!hv_win_handle_iabort(&ctx), "IPA outside RAM taken");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_NOT_OURS) == before + 3, "not-ours stat");
}

static void test_walker(void)
{
    arm_all();

    CHECK(iabort(9, 0, FSC_PERM_L3, 0x1000) && m_pg[9].st == HV_XT_CODE, "scan 9");
    /* hardware AF/DBM update of a descriptor that lives in a CODE page, during a fetch's walk */
    CHECK(iabort(9, 0, ISS_S1PTW | FSC_PERM_L3, 0x2000), "walker fault on a fetch");
    CHECK(m_fault_s1ptw_seen, "walker IPA not from HPFAR");
    CHECK(m_pg[9].st == HV_XT_DIRTY && ev_is(1, EV_TLBI_BCAST, 0, 0), "walker flip");
    CHECK(iabort(9, 0, FSC_PERM_L3, 0x1000) && m_pg[9].st == HV_XT_CODE, "rescan 9");
    /* the same during a data access's walk (WnR may read either way) */
    CHECK(dabort(9, ISS_S1PTW | FSC_PERM_L3, 0x3000) && m_pg[9].st == HV_XT_DIRTY,
          "walker on a load");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WALK_FLIPS) == 2 &&
              hv_win_pmu_stat(HV_WIN_PMU_WRITE_FLIPS) == 0,
          "walker stats");
}

static void test_pinning(void)
{
    arm_all();

    for (int i = 1; i < 4; i++) {
        CHECK(iabort(10, 0, FSC_PERM_L3, 0x1000) && m_pg[10].st == HV_XT_CODE, "exec %d", i);
        CHECK(dabort(10, ISS_WNR | FSC_PERM_L3, 0x2000) && m_pg[10].st == HV_XT_DIRTY &&
                  m_pg[10].flips == (u32)i,
              "write %d: state %d flips %u", i, m_pg[10].st, m_pg[10].flips);
    }
    CHECK(iabort(10, 0, FSC_PERM_L3, 0x1000) && m_pg[10].st == HV_XT_CODE, "exec 4");
    CHECK(dabort(10, ISS_WNR | FSC_PERM_L3, 0x2000), "write 4");
    CHECK(m_pg[10].st == HV_XT_PINNED && nev == 2 && ev_is(1, EV_TLBI_LOCAL, 0, 0),
          "pinned on the 4th flip with a local flush (state %d)", m_pg[10].st);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PINS) == 1, "pins stat");
    CHECK(iabort(10, 0, FSC_PERM_L3, 0x1000) && m_pg[10].st == HV_XT_PINNED, "exec on PINNED");
    CHECK(dabort(10, ISS_WNR | FSC_PERM_L3, 0x2000) && m_pg[10].st == HV_XT_PINNED,
          "write on PINNED");

    for (int t = 0; t < 99; t++)
        hv_win_pmu_tick();
    CHECK(m_pg[10].st == HV_XT_PINNED, "swept early");
    nev = 0;
    hv_win_pmu_tick();
    CHECK(m_pg[10].st == HV_XT_DIRTY && m_pg[10].flips == 0, "sweep: state %d flips %u",
          m_pg[10].st, m_pg[10].flips);
    CHECK(nev == 2 && ev_is(1, EV_TLBI_BCAST, 0, 0), "sweep events");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_SWEEPS) == 1 && hv_win_pmu_stat(HV_WIN_PMU_UNPINS) == 1,
          "sweep stats");
    for (int t = 0; t < 1000; t++)
        hv_win_pmu_tick();
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_SWEEPS) == 1, "sweeps with nothing pinned");

    /* a pinned page the guest wrote PMU code into is scanned after the sweep */
    words_of(10)[100] = 0xd51b9c00;
    CHECK(iabort(10, 100, FSC_PERM_L3, 0x1000) && words_of(10)[100] == 0xd503201f,
          "rescan after the sweep");

    /* pin list full: the page keeps flipping (correct, only slower) */
    for (int p = 11; p < 11 + 256; p++) {
        for (int i = 0; i < 4; i++) {
            iabort(p, 0, FSC_PERM_L3, 0x1000);
            dabort(p, ISS_WNR | FSC_PERM_L3, 0x2000);
        }
    }
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PINS) == 1 + 256, "pins %lu",
          hv_win_pmu_stat(HV_WIN_PMU_PINS));
    for (int i = 0; i < 4; i++) {
        iabort(300, 0, FSC_PERM_L3, 0x1000);
        CHECK(dabort(300, ISS_WNR | FSC_PERM_L3, 0x2000), "write with the list full");
    }
    CHECK(m_pg[300].st == HV_XT_DIRTY && hv_win_pmu_stat(HV_WIN_PMU_PIN_FULL) == 1 &&
              ev_is(1, EV_TLBI_BCAST, 0, 0),
          "list full: state %d", m_pg[300].st);
    CHECK(m_pg[300].flips == 4, "flips %u", m_pg[300].flips);
    for (int t = 0; t < 100; t++)
        hv_win_pmu_tick();
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_UNPINS) == 257, "unpins %lu",
          hv_win_pmu_stat(HV_WIN_PMU_UNPINS));
    CHECK(m_pg[11].st == HV_XT_DIRTY && m_pg[266].st == HV_XT_DIRTY, "all swept");
}

static void test_stuck(void)
{
    arm_all();

    m_fault_ok = false;
    for (int i = 1; i < 256; i++)
        CHECK(iabort(20, 0, FSC_PERM_L3, 0x7000), "retry %d", i);
    CHECK(nev == 1 && ev_is(0, EV_TLBI_LOCAL, 0, 0), "retry flushes this CPU");
    CHECK(!iabort(20, 0, FSC_PERM_L3, 0x7000), "never gave up");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STUCK) == 1 && hv_win_pmu_stat(HV_WIN_PMU_IPA_FAIL) == 256,
          "stuck stats");
    CHECK(iabort(20, 0, FSC_PERM_L3, 0x7000), "counter not reset after giving up");

    /* per CPU and per PC */
    for (int i = 1; i < 200; i++)
        iabort(20, 0, FSC_PERM_L3, 0x8000);
    host_cpu = 1;
    for (int i = 1; i < 200; i++)
        CHECK(iabort(20, 0, FSC_PERM_L3, 0x8000), "CPU 1 retry %d", i);
    host_cpu = 0;
    CHECK(iabort(20, 0, FSC_PERM_L3, 0x9000), "new PC");
    m_fault_ok = true;
    CHECK(iabort(20, 0, FSC_PERM_L3, 0x9000) && m_pg[20].st == HV_XT_CODE,
          "progress after AT works");
    CHECK(dabort(20, ISS_WNR | FSC_PERM_L3, 0x9000), "write");
    m_fault_ok = false;
    CHECK(dabort(20, ISS_WNR | FSC_PERM_L3, 0x9000) && hv_win_pmu_stat(HV_WIN_PMU_IPA_FAIL) > 256,
          "dabort AT failure retried");

    /* AT fails on a fetch, HPFAR names a DIRTY page: scanned from the hint */
    words_of(21)[3] = 0xd53b9d02;
    m_hpfar_ipa = ipa_of(21, 3);
    CHECK(iabort(21, 3, FSC_PERM_L3, 0xa000) && m_pg[21].st == HV_XT_CODE &&
              words_of(21)[3] == 0xd53be042,
          "HPFAR hint not used");
    /* ... but never a CODE page (or garbage): that is only a retry */
    u64 fails = hv_win_pmu_stat(HV_WIN_PMU_IPA_FAIL), scans = hv_win_pmu_stat(HV_WIN_PMU_SCANS);
    CHECK(iabort(22, 0, FSC_PERM_L3, 0xb000) && m_pg[22].st == HV_XT_DIRTY &&
              hv_win_pmu_stat(HV_WIN_PMU_SCANS) == scans &&
              hv_win_pmu_stat(HV_WIN_PMU_IPA_FAIL) == fails + 1,
          "HPFAR naming a CODE page acted on");
    m_hpfar_ipa = 0x30000000000UL;
    CHECK(iabort(22, 0, FSC_PERM_L3, 0xb000) && m_pg[22].st == HV_XT_DIRTY,
          "garbage HPFAR acted on");
    /* walker faults never fall back */
    m_hpfar_ipa = ipa_of(23, 0);
    CHECK(iabort(23, 0, ISS_S1PTW | FSC_PERM_L3, 0xc000), "walker retry");
}

static void test_log_ring(void)
{
    arm_all();

    u32 *w = words_of(30);
    for (u32 i = 0; i < M_WORDS; i++)
        w[i] = 0xd53b9d00 | (i & 31); /* mrs xN, PMCCNTR_EL0 */
    for (int i = 0; i < 70; i++)      /* past the console limit */
        words_of(40 + i)[1] = 0xd51b9c00;

    CHECK(iabort(30, 0, FSC_PERM_L3, 0xabc0), "full page");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_INSNS) == M_WORDS, "insns %lu",
          hv_win_pmu_stat(HV_WIN_PMU_INSNS));
    /* rewrite n lives in slot n % 256: the last one is word 4095 */
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_IPA + 4095 % 256) == ipa_of(30, 4095), "ring tail");
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_OP + 0) == (0xd53b9d00UL << 32 | 0xd53be040) &&
              hv_win_pmu_stat(HV_WIN_PMU_STAT_LOG_IPA + 0) == ipa_of(30, 3840),
          "slot 0 holds rewrite 3840");
    for (u32 i = 0; i < M_WORDS; i++)
        CHECK(w[i] == (0xd53be040 | (i & 31)), "word %u", i);

    for (int i = 0; i < 70; i++)
        iabort(40 + i, 1, FSC_PERM_L3, 0x100);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PAGES_REWRITTEN) == 71, "pages rewritten");
    CHECK(strstr(last_line, "IPA") != NULL, "last console line: %s", last_line);
    hv_win_pmu_report();
    CHECK(strstr(last_line, "71 pages rewritten") != NULL, "report: %s", last_line);
}

/* ---- llvm-objdump listings ----------------------------------------------------------------------
 */

static bool pmu_name(const char *r, bool *pmccntr)
{
    static const char *const names[] = {
        "pmcr_el0",       "pmcntenset_el0", "pmcntenclr_el0", "pmovsclr_el0", "pmswinc_el0",
        "pmselr_el0",     "pmceid0_el0",    "pmceid1_el0",    "pmccntr_el0",  "pmxevtyper_el0",
        "pmxevcntr_el0",  "pmzr_el0",       "pmuserenr_el0",  "pmovsset_el0", "pmintenset_el1",
        "pmintenclr_el1", "pmuacr_el1",     "pmecr_el1",      "pmmir_el1",    "pmiar_el1",
        "pmccfiltr_el0",
    };
    unsigned n;
    char tail[8];

    *pmccntr = !strcmp(r, "pmccntr_el0");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(r, names[i]))
            return true;
    if ((sscanf(r, "pmevcntr%u_%7s", &n, tail) == 2 ||
         sscanf(r, "pmevtyper%u_%7s", &n, tail) == 2) &&
        n <= 30 && !strcmp(tail, "el0"))
        return true;
    return false;
}

struct dis_totals {
    unsigned long lines, insns, kinds[PMU_RW_NR_KINDS], named_pmu, unnamed_in_block, spe, mismatch;
};

static void dis_line(char *line, struct dis_totals *t, const char *file)
{
    char *p = line;
    while (*p == ' ')
        p++;
    char *colon = strchr(p, ':');
    if (!colon || colon == p)
        return;
    for (char *q = p; q < colon; q++)
        if (!isxdigit((unsigned char)*q))
            return;
    p = colon + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    char hex[9];
    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)p[i]))
            return;
        hex[i] = p[i];
    }
    hex[8] = 0;
    if (isxdigit((unsigned char)p[8]))
        return;
    u32 word = (u32)strtoul(hex, NULL, 16);
    p += 8;
    while (*p == ' ' || *p == '\t')
        p++;

    char mnem[32] = {0}, ops[256] = {0};
    sscanf(p, "%31s %255[^\n]", mnem, ops);
    for (char *q = ops; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    t->insns++;

    enum pmu_rw_kind want = PMU_RW_NONE;
    bool unnamed = false;
    char reg[64] = {0};
    if (!strcmp(mnem, "mrs")) {
        char *c = strchr(ops, ',');
        if (c)
            sscanf(c + 1, " %63[^ ;/\t]", reg);
    } else if (!strcmp(mnem, "msr")) {
        sscanf(ops, "%63[^, ]", reg);
    }
    if (reg[0]) {
        bool pmccntr;
        unsigned op0, op1, crn, crm, op2;
        if (pmu_name(reg, &pmccntr)) {
            want =
                mnem[1] == 'r' ? (pmccntr ? PMU_RW_READ_PMCCNTR : PMU_RW_READ_OTHER) : PMU_RW_WRITE;
            t->named_pmu++;
        } else if (sscanf(reg, "s%u_%u_c%u_c%u_%u", &op0, &op1, &crn, &crm, &op2) == 5) {
            /* no architectural name in LLVM: the block table decides, counted apart */
            if (op0 == 3 && in_pmu_block(op1, crn, crm)) {
                want = mnem[1] == 'r' ? PMU_RW_READ_OTHER : PMU_RW_WRITE;
                unnamed = true;
                t->unnamed_in_block++;
            }
        } else if (!strncmp(reg, "pmb", 3) ||
                   (!strncmp(reg, "pms", 3) && strcmp(reg, "pmselr_el0") &&
                    strcmp(reg, "pmswinc_el0") && strncmp(reg, "pmsscr", 6))) {
            t->spe++;
        }
    }

    enum pmu_rw_kind got = pmu_rw_classify(word);
    if (got != want) {
        if (t->mismatch++ < 20)
            fprintf(stderr, "%s: 0x%08x '%s %s': classified %d, LLVM's decoding says %d%s\n", file,
                    word, mnem, ops, got, want, unnamed ? " (unnamed)" : "");
    } else if (got != PMU_RW_NONE) {
        t->kinds[got]++;
        u32 rep = pmu_rw_replacement(word);
        if (pmu_rw_classify(rep) != PMU_RW_NONE && t->mismatch++ < 20)
            fprintf(stderr, "%s: replacement 0x%08x of 0x%08x is PMU\n", file, rep, word);
    }
}

static int run_dis(int argc, char **argv)
{
    int status = 0;
    static char line[4096];

    for (int i = 0; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        struct dis_totals t = {0};
        while (fgets(line, sizeof(line), f)) {
            t.lines++;
            dis_line(line, &t, argv[i]);
        }
        fclose(f);
        unsigned long total = t.kinds[1] + t.kinds[2] + t.kinds[3];
        printf("%s: %lu instructions, PMUv3 %lu (mrs PMCCNTR %lu, other mrs %lu, msr %lu; "
               "LLVM-named %lu, "
               "unnamed in a PMU block %lu), SPE (not rewritten) %lu, disagreements %lu\n",
               argv[i], t.insns, total, t.kinds[PMU_RW_READ_PMCCNTR], t.kinds[PMU_RW_READ_OTHER],
               t.kinds[PMU_RW_WRITE], t.named_pmu, t.unnamed_in_block, t.spe, t.mismatch);
        if (t.mismatch)
            status = 1;
    }
    return status;
}

/* ---- PE sections --------------------------------------------------------------------------------
 */

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static int run_pe(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *img = malloc((size_t)size);
        if (!img || fread(img, 1, (size_t)size, f) != (size_t)size) {
            fclose(f);
            free(img);
            return 2;
        }
        fclose(f);

        u32 pe = size > 0x40 ? le32(img + 0x3c) : 0;
        if (size < 0x40 || pe + 24 > (u64)size || memcmp(img + pe, "PE\0\0", 4)) {
            fprintf(stderr, "%s: not a PE image\n", argv[i]);
            free(img);
            return 2;
        }
        u16 nsec = le16(img + pe + 6), opt = le16(img + pe + 20);
        const u8 *sec = img + pe + 24 + opt;
        unsigned long code_hits = 0, data_hits = 0, data_words = 0;

        printf("%s:\n", argv[i]);
        for (u16 s = 0; s < nsec && sec + 40 <= img + size; s++, sec += 40) {
            char name[9] = {0};
            memcpy(name, sec, 8);
            u32 raw_size = le32(sec + 16), raw_off = le32(sec + 20), chr = le32(sec + 36);
            bool exec = chr & 0x20000000;
            if ((u64)raw_off + raw_size > (u64)size)
                continue;
            unsigned long k[PMU_RW_NR_KINDS] = {0};
            for (u32 o = 0; o + 4 <= raw_size; o += 4)
                k[pmu_rw_classify(le32(img + raw_off + o))]++;
            unsigned long hits = k[1] + k[2] + k[3];
            printf("  %-8s %s %8u bytes: %lu PMU-looking word(s) (PMCCNTR %lu, mrs %lu, msr %lu)\n",
                   name, exec ? "code" : "data", raw_size, hits, k[1], k[2], k[3]);
            if (exec) {
                code_hits += hits;
            } else {
                data_hits += hits;
                data_words += raw_size / 4;
            }
        }
        printf("  total: code %lu, data %lu in %lu data words\n", code_hits, data_hits, data_words);
        free(img);
    }
    return 0;
}

static int emit_sysreg_space(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 2;
    }
    for (u32 v = 0; v < (1U << 15); v++) {
        u32 l = v >> 14, op1 = (v >> 11) & 7, crn = (v >> 7) & 15, crm = (v >> 3) & 15, op2 = v & 7;
        u32 insn = sysreg_insn((int)l, 3, op1, crn, crm, op2, v % 31);
        fwrite(&insn, 4, 1, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--dis"))
        return run_dis(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "--pe"))
        return run_pe(argc - 2, argv + 2);
    if (argc == 3 && !strcmp(argv[1], "--emit-sysreg-space"))
        return emit_sysreg_space(argv[2]);
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    m_mem = aligned_alloc(HV_XT_PAGE_SIZE, (size_t)M_PAGES * M_WORDS * 4);
    if (!m_mem)
        return 2;

    test_known_encodings();
    test_registers();
    test_exhaustive();
    test_page();
    test_arm();
    test_exec_and_write();
    test_walker();
    test_pinning();
    test_stuck();
    test_log_ring();

    free(m_mem);
    printf("win-pmu-rewrite: %lu checks, %lu failed\n", checks, failures);
    return failures ? 1 : 0;
}
