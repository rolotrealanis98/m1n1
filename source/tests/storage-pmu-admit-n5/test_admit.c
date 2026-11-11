/* SPDX-License-Identifier: MIT */
/*
 * nightly-5: the storage memory admission (src/hv/win/hv_win_storage_mem.c, hv_storage_guest_span)
 * against the real PMU rewrite (hv_win_pmu.c, 4 KiB and 16 KiB granules) over one mock stage 2 whose
 * leaves use the real descriptor bits (hv_vm.c: S2AP 7:6, XN 54, XT 55, flips 58:56). Before
 * nightly-5 a tracked RO+XN leaf (SCANNING) existed only inside a scan under the big HV lock and the
 * admission refused it; the 4 KiB granule keeps blocks in SCANNING between execution windows, so the
 * admission now accepts it. What must still hold:
 *   A1 every PMU state of a tracked guest-RAM leaf (DIRTY, SCANNING, CODE, PINNED) is admitted, both
 *      directions; nothing else about leaves changes (untracked RO, RO+XN, wrong attributes, tracked
 *      32 MiB blocks, untracked leaves with flip bits: refused as before);
 *   A2 resident and protected ranges and non-RAM are refused whatever the PMU state;
 *   A3 a refused span changes no PMU state (the re-arm runs only after every check passed);
 *   A4 a write admission leaves no admitted leaf executable before the copy, the copied bytes are
 *      exactly the device's, and a filled frame is rescanned before it executes (its PMU words
 *      rewritten at its next proven fetch, never before);
 *   A5 a read admission changes nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmu_host.h"
#include "hv_win.h"
#include "hv_win_pmu_4k.h"
#include "hv_win_storage_mem.h"
#include "hv.h"    /* shim: struct hv_s2_leaf */
#include "utils.h" /* shim: spinlock_t */
#undef printf

#define BLOCKS 24
#define NOP    0xd503201fU
#define IESR   ((0x20UL << 26) | 15)
#define DESR   ((0x24UL << 26) | 15 | BIT(6))

/* real stage-2 descriptor bits (hv_vm.c) */
#define S2AP_MASK (3UL << 6)
#define S2AP_RW   (3UL << 6)
#define S2AP_RO   (1UL << 6)
#define S2_XN     BIT(54)
#define SW_XT     BIT(55)
#define SW_FLIPS  GENMASK(58, 56)
#define LEAF_RW   (BIT(10) | (3UL << 8) | S2AP_RW | (15UL << 2) | 3) /* normal WB, AF, ISH, L3 */

static u32 ram[BLOCKS * 4096] ALIGNED(16384);  /* guest RAM, IPA == host address */
static u32 resident_mem[4096] ALIGNED(16384);  /* "EL2": a separate 16 KiB */
static u64 desc[BLOCKS], resident_desc;
static u64 big_leaf = ~0UL;                    /* block index reported as a 32 MiB leaf */
static unsigned long checks;

int host_cpu;
int boot_cpu_idx;
u64 hv_cpus_in_guest;
u64 host_sysreg_DAIF = BIT(7) | BIT(6);
bool hv_started_cpus[MAX_CPUS];
spinlock_t bhl;
struct hv_win_state hv_win;
struct boot_args cur_boot_args;
void hv_win_pmu_host_reset(void);
u8 hv_win_pmu_host_frame(u64 ipa);
/* nightly-7 (smp-storage merge): hv_win_storage_mem.c asks the storage cap and the storage lock. This
 * harness exercises the single-vCPU admission only: cap 1 (the r4.14 path, unchanged), lock held. */
u32 hv_win_storage_max_cpus(void) { return 1; }
bool hv_storage_lock_held(void) { return true; }

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static u64 RAM(void)
{
    return (u64)ram;
}
static int blk(u64 ipa)
{
    return ipa >= RAM() && ipa - RAM() < sizeof(ram) ? (int)((ipa - RAM()) / 16384) : -1;
}

/* ---- the shared stage 2 ------------------------------------------------------------------------ */

bool hv_pt_resolve_leaf(u64 ipa, struct hv_s2_leaf *out)
{
    int b = blk(ipa);
    if (b >= 0) {
        bool big = (u64)b == big_leaf;
        u64 base = big ? (ipa & ~((1UL << 25) - 1)) : RAM() + b * 16384UL;
        *out = (struct hv_s2_leaf){base, base, big ? 1UL << 25 : 16384, desc[b]};
        return true;
    }
    if (ipa >= (u64)resident_mem && ipa - (u64)resident_mem < sizeof(resident_mem)) {
        *out = (struct hv_s2_leaf){(u64)resident_mem, (u64)resident_mem, 16384, resident_desc};
        return true;
    }
    return false;
}
int hv_xt_track(u64 ipa, u64 size, bool dry, u64 *tables)
{
    int n = 0;
    *tables = 0;
    for (u64 a = ipa; a < ipa + size; a += 16384) {
        int b = blk(a);
        if (b < 0 || (desc[b] & S2AP_MASK) != S2AP_RW)
            continue;
        if (!dry)
            desc[b] = (desc[b] & ~SW_FLIPS) | S2_XN | SW_XT;
        n++;
    }
    return n;
}
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips)
{
    int b = blk(ipa);
    if (b < 0 || !(desc[b] & SW_XT))
        return HV_XT_UNTRACKED;
    u64 d = desc[b];
    bool rw = (d & S2AP_MASK) == S2AP_RW;
    if (pa)
        *pa = RAM() + b * 16384UL;
    if (flips)
        *flips = FIELD_GET(SW_FLIPS, d);
    if (d & S2_XN)
        return rw ? HV_XT_DIRTY : HV_XT_SCANNING;
    return rw ? HV_XT_PINNED : HV_XT_CODE;
}
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips)
{
    int b = blk(ipa);
    if (b < 0 || !(desc[b] & SW_XT) || state == HV_XT_UNTRACKED)
        return -1;
    u64 d = desc[b] & ~(S2AP_MASK | S2_XN | SW_FLIPS);
    d |= (state == HV_XT_DIRTY || state == HV_XT_PINNED) ? S2AP_RW : S2AP_RO;
    if (state == HV_XT_DIRTY || state == HV_XT_SCANNING)
        d |= S2_XN;
    d |= FIELD_PREP(SW_FLIPS, flips < 7 ? flips : 7);
    desc[b] = d;
    return 0;
}
void hv_xt_tlbi(bool broadcast)
{
    (void)broadcast;
}
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa)
{
    (void)s1ptw;
    (void)write;
    *ipa = ctx->far; /* guest MMU off: VA = IPA */
    return true;
}
bool pmu_host_s1(struct pmu_s1 *s)
{
    *s = (struct pmu_s1){.sctlr = 0}; /* MMU off: the walk is the identity */
    return true;
}
void dc_civac_range(void *a, size_t n)
{
    (void)a;
    (void)n;
}
void dc_cvac_range(void *a, size_t n)
{
    (void)a;
    (void)n;
}
void host_ic_ialluis(void) {}
size_t heapblock_remaining(void)
{
    return 1UL << 30;
}
int host_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

/* ---- world -------------------------------------------------------------------------------------- */

static struct hv_storage_mem sm;
static struct hv_sm_range ram_r, prot_r;
static bool granule16k;

static void world(void)
{
    hv_win_pmu_host_reset();
    memset(&sm, 0, sizeof(sm));
    memset(&hv_sm_last_refusal, 0, sizeof(hv_sm_last_refusal));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    memset(&hv_win, 0, sizeof(hv_win));
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    hv_win.nr_vcpus = 1;
    hv_win.vcpu_cpu[0] = 0;
    host_cpu = boot_cpu_idx = 0;
    hv_cpus_in_guest = 0;
    big_leaf = ~0UL;
    for (unsigned b = 0; b < BLOCKS; b++)
        desc[b] = (RAM() + b * 16384UL) | LEAF_RW;
    resident_desc = (u64)resident_mem | LEAF_RW;
    for (unsigned i = 0; i < BLOCKS * 4096; i++)
        ram[i] = NOP;
    /* blocks 20..23 are left to the launcher's protected list; block 19 is untracked read-only */
    desc[19] = (desc[19] & ~S2AP_MASK) | S2AP_RO;
    ram_r = (struct hv_sm_range){RAM(), sizeof(ram)};
    prot_r = (struct hv_sm_range){RAM() + 20 * 16384UL, 4 * 16384UL};
    struct hv_sm_config c = {.epoch = 7,
                             .resident = {(u64)resident_mem, sizeof(resident_mem)},
                             .ram = &ram_r,
                             .protected = &prot_r,
                             .nr_ram = 1,
                             .nr_protected = 1};
    CHECK(hv_storage_mem_register(&sm, &c));
    CHECK(hv_win_pmu_set_mode(granule16k ? HV_WIN_PMU_MODE_16K : 0) == 0);
    CHECK(hv_win_pmu_arm(RAM(), 19 * 16384UL) == 19); /* blocks 0..18 execute-tracked */
    bhl.lock = 0; /* held by CPU 0 for the dispatch */
    bhl.count = 1;
    hv_started_cpus[0] = true; /* the guest started on CPU 0 only */
    CHECK(hv_storage_mem_begin(&sm, 7));
}

static u64 ipa_of(unsigned b, unsigned word)
{
    return RAM() + b * 16384UL + word * 4UL;
}
static bool fetch(unsigned b, unsigned word)
{
    struct exc_info c = {.far = ipa_of(b, word), .elr = ipa_of(b, word), .esr = IESR, .spsr = 5};
    return hv_win_handle_iabort(&c);
}
static bool guest_store(unsigned b, unsigned word)
{
    struct exc_info c = {.far = ipa_of(b, word), .elr = 0x1000, .esr = DESR, .spsr = 5};
    return hv_win_handle_dabort(&c, c.esr);
}
static void close_ticks(void)
{
    for (int t = 0; t < 10; t++)
        hv_win_pmu_tick();
}
static void pin(unsigned b)
{
    for (int i = 0; i < 4; i++) {
        CHECK(fetch(b, 1));
        CHECK(guest_store(b, 2));
    }
    CHECK(hv_xt_get(ipa_of(b, 0), NULL, NULL) == HV_XT_PINNED);
}
static enum hv_xt_state st(unsigned b)
{
    return hv_xt_get(ipa_of(b, 0), NULL, NULL);
}
static bool executable(unsigned b)
{
    return st(b) == HV_XT_CODE || st(b) == HV_XT_PINNED;
}

/* Puts block b into state s (tracked blocks only). */
static void make(unsigned b, enum hv_xt_state s)
{
    switch (s) {
        case HV_XT_DIRTY:
            break;
        case HV_XT_CODE:
            CHECK(fetch(b, 1) && st(b) == HV_XT_CODE);
            break;
        case HV_XT_SCANNING:
            /* 4 KiB: re-protected after a run; 16 KiB: never stable, built for the admission */
            CHECK(fetch(b, 1));
            if (granule16k)
                hv_xt_set(ipa_of(b, 0), HV_XT_SCANNING, 0);
            else
                close_ticks();
            CHECK(st(b) == HV_XT_SCANNING);
            break;
        case HV_XT_PINNED:
            pin(b);
            break;
        default:
            CHECK(0);
    }
}

static const enum hv_xt_state all[] = {HV_XT_DIRTY, HV_XT_SCANNING, HV_XT_CODE, HV_XT_PINNED};

/* A1/A4/A5 for every state and both directions, one leaf and spans across leaves */
static void states_admitted(void)
{
    for (unsigned k = 0; k < 4; k++) {
        for (int w = 0; w < 2; w++) {
            world();
            make(3, all[k]);
            u64 before = desc[3];
            u8 f0 = hv_win_pmu_host_frame(ipa_of(3, 0));
            void *p = hv_storage_guest_span(&sm, ipa_of(3, 1024), 4096, w);
            CHECK(p == (void *)ipa_of(3, 1024));
            CHECK(hv_sm_last_refusal.count[HV_SM_WHY_SCANNING] == 0);
            if (!w) {
                CHECK(desc[3] == before); /* A5 */
                continue;
            }
            CHECK(!executable(3)); /* A4: no execute permission while EL2 copies */
            if (granule16k && all[k] == HV_XT_SCANNING)
                continue; /* unreachable in the 16 KiB granule (transient): admission checked only */
            if (!granule16k && all[k] != HV_XT_PINNED && all[k] != HV_XT_DIRTY)
                CHECK(hv_win_pmu_host_frame(ipa_of(3, 0)) == f0 &&
                      !(hv_win_pmu_host_frame(ipa_of(3, 1024)) & PMU_FRAME_SCANNED));
            /* the device's bytes land exactly: a PMU read and a word that looks like one */
            u32 *dst = p;
            for (unsigned i = 0; i < 1024; i++)
                dst[i] = NOP;
            dst[10] = 0xd53b9d03; /* mrs x3, PMCCNTR_EL0 */
            dst[700] = 0xd53b9d04;
            for (unsigned i = 0; i < 1024; i++)
                CHECK(ram[3 * 4096 + 1024 + i] == dst[i]);
            CHECK(ram[3 * 4096 + 1024 + 10] == 0xd53b9d03); /* not rewritten before execution */
            CHECK(hv_storage_mem_end(&sm));
            /* the guest runs the filled frame: its fetch faults and the frame is rescanned */
            CHECK(fetch(3, 1024 + 10) && executable(3));
            CHECK(ram[3 * 4096 + 1024 + 10] == 0xd53be043 && ram[3 * 4096 + 1024 + 700] == 0xd53be044);
        }
    }
    /* a 128 KiB unaligned span across nine leaves in every state */
    for (int w = 0; w < 2; w++) {
        world();
        for (unsigned b = 4; b <= 12; b++)
            make(b, all[b % 4]);
        u64 before[BLOCKS];
        memcpy(before, desc, sizeof(desc));
        void *p = hv_storage_guest_span(&sm, ipa_of(4, 1000), HV_SM_MAX_SPAN, w);
        CHECK(p == (void *)ipa_of(4, 1000));
        for (unsigned b = 4; b <= 12; b++)
            CHECK(w ? !executable(b) : desc[b] == before[b]);
    }
}

/* A1 (unchanged refusals), A2, A3 */
static void refusals(void)
{
    world();
    make(5, HV_XT_SCANNING);
    make(6, HV_XT_CODE);
    u64 before[BLOCKS];
    memcpy(before, desc, sizeof(desc));
    u8 fr6 = hv_win_pmu_host_frame(ipa_of(6, 0));
    /* untracked read-only leaf (block 19): refused, also when the span starts in SCANNING */
    CHECK(!hv_storage_guest_span(&sm, ipa_of(19, 0), 4096, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    CHECK(!hv_storage_guest_span(&sm, ipa_of(18, 4000), 1024, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    /* untracked RO + XN */
    desc[19] |= S2_XN;
    CHECK(!hv_storage_guest_span(&sm, ipa_of(19, 0), 4096, false));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    /* untracked leaf with flip bits */
    desc[19] = (RAM() + 19 * 16384UL) | LEAF_RW | FIELD_PREP(SW_FLIPS, 1);
    CHECK(!hv_storage_guest_span(&sm, ipa_of(19, 0), 4096, false));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    /* tracked SCANNING leaf with device memory attributes */
    u64 keep = desc[5];
    desc[5] &= ~(15UL << 2);
    CHECK(!hv_storage_guest_span(&sm, ipa_of(5, 0), 4096, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    desc[5] = keep;
    /* tracked leaf reported as a 32 MiB block */
    big_leaf = 5;
    CHECK(!hv_storage_guest_span(&sm, ipa_of(5, 0), 4096, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_LEAF_ATTR);
    big_leaf = ~0UL;
    /* A3: a write span that starts in a CODE leaf and runs into a refused leaf changes nothing */
    desc[7] = (desc[7] & ~S2AP_MASK & ~SW_XT) | S2AP_RO; /* block 7: untracked read-only */
    before[7] = desc[7];
    CHECK(!hv_storage_guest_span(&sm, ipa_of(6, 3000), 8192, true));
    CHECK(desc[6] == before[6] && st(6) == HV_XT_CODE && hv_win_pmu_host_frame(ipa_of(6, 0)) == fr6);
    /* A2: protected range (blocks 20..23), resident memory, outside RAM */
    CHECK(!hv_storage_guest_span(&sm, ipa_of(20, 0), 4096, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_PROTECTED);
    CHECK(!hv_storage_guest_span(&sm, ipa_of(17, 0), 4 * 16384UL, true)); /* into protected */
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_PROTECTED);
    CHECK(!hv_storage_guest_span(&sm, (u64)resident_mem, 4096, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_NOT_RAM || hv_sm_last_refusal.why == HV_SM_WHY_RESIDENT);
    CHECK(!hv_storage_guest_span(&sm, RAM() + sizeof(ram), 4096, true));
    for (unsigned b = 0; b < 19; b++)
        if (b != 5)
            CHECK(desc[b] == before[b]); /* no refused span re-armed anything */
    CHECK(hv_sm_last_refusal.count[HV_SM_WHY_SCANNING] == 0);
    /* a protected SCANNING leaf stays refused although SCANNING is now admitted elsewhere */
    world();
    make(5, HV_XT_SCANNING);
    prot_r = (struct hv_sm_range){ipa_of(5, 0), 4096};
    memset(&sm, 0, sizeof(sm));
    hv_started_cpus[0] = false;
    struct hv_sm_config c = {.epoch = 9,
                             .resident = {(u64)resident_mem, sizeof(resident_mem)},
                             .ram = &ram_r,
                             .protected = &prot_r,
                             .nr_ram = 1,
                             .nr_protected = 1};
    CHECK(hv_storage_mem_register(&sm, &c));
    hv_started_cpus[0] = true;
    CHECK(hv_storage_mem_begin(&sm, 9));
    u64 d5 = desc[5];
    CHECK(!hv_storage_guest_span(&sm, ipa_of(5, 0), 64, true));
    CHECK(hv_sm_last_refusal.why == HV_SM_WHY_PROTECTED && desc[5] == d5);
    CHECK(hv_storage_guest_span(&sm, ipa_of(5, 1024), 64, true) == (void *)ipa_of(5, 1024));
}

/* the rewrite off: tracked bits never appear, admission is the plain RW check */
static void rewrite_off(void)
{
    hv_win_pmu_host_reset();
    world();
    hv_win_pmu_host_reset(); /* disarm (state only; descriptors keep XT from the arm above) */
    for (unsigned b = 0; b < BLOCKS; b++)
        desc[b] &= ~(SW_XT | S2_XN | SW_FLIPS);
    desc[19] = (desc[19] & ~S2AP_MASK) | S2AP_RO;
    CHECK(hv_storage_guest_span(&sm, ipa_of(2, 0), 16384, true) == (void *)ipa_of(2, 0));
    CHECK(!hv_storage_guest_span(&sm, ipa_of(19, 0), 16384, true));
}

int main(void)
{
    for (int g = 0; g < 2; g++) {
        granule16k = g;
        states_admitted();
        refusals();
    }
    rewrite_off();
    hv_win_pmu_host_reset();
    printf("storage-pmu-admit-n5: %lu checks, 0 failed\n", checks);
    return 0;
}
