/* SPDX-License-Identifier: MIT */
/*
 * Windows EL2 runtime: stage-2 first-execute rewrite of PMUv3 accesses
 * (m5pro-windows docs/PMU-EL2-REWRITE.md).
 *
 * The T6050 UNDEFs every EL1 PMUv3 access at EL1, before MDCR_EL2.TPM or the FGT bits can trap it,
 * and Windows reads PMCCNTR_EL0 without looking at ID_AA64DFR0_EL1.PMUVer. The primary fix is in Mu
 * (it emulates PMU UNDEFs while it owns the vectors and rewrites the loaded Windows images before
 * hand-off); this file is the insurance for code Mu cannot see: drivers the kernel loads later and
 * anything Mu missed.
 *
 * Per 16 KiB page of guest RAM (state in the stage-2 leaf, hv_vm.c "execute tracking"):
 *
 *   DIRTY (RW, XN)  --first fetch-->  SCANNING (RO, XN), PMUv3 words rewritten  -->  CODE (RO, X)
 *   CODE  --write or walker update-->  DIRTY, or PINNED (RW, X) at the PMU_FLIP_CAP-th flip
 *   PINNED  --sweep, every PMU_SWEEP_TICKS hypervisor ticks-->  DIRTY
 *   CODE, PINNED  --EL2 device write into the page (hv_win_pmu_dma_write)-->  DIRTY
 *
 * Everything runs under the big HV lock (the locked half of hv_exc_sync() and hv_tick()). The
 * rewrite happens only in SCANNING: no CPU can write the page (write permission revoked, broadcast
 * TLBI completed) or execute it (XN since it became DIRTY), so no instruction is modified while
 * another CPU may execute it.
 */

#ifdef PMU_REWRITE_HOST_TEST
#include "pmu_host.h"
#else
#include "cpu_regs.h"
#include "heapblock.h"
#include "memory.h"
#include "smp.h"
#include "utils.h"
#include "xnuboot.h"

#include "../hv.h"
#endif

#include "hv_win.h"
#include "hv_win_pmu_rewrite.h"

/* ESR_EL2 ISS for instruction and data aborts */
#define ESR_ISS_ABORT_S1PTW BIT(7)
#define ESR_ISS_ABORT_WNR   BIT(6)
#define ESR_ISS_ABORT_FSC   GENMASK(5, 0)
#define FSC_PERM_LEVEL_MASK 0x3c
#define FSC_PERM            0x0c /* permission fault, level 0..3 */

#define PMU_PAGE        HV_XT_PAGE_SIZE
#define PMU_PAGE_WORDS  (PMU_PAGE / 4)
#define PMU_MAX_RANGES  32
#define PMU_FLIP_CAP    4   /* write-after-execute flips before a page is pinned RWX */
#define PMU_MAX_PINNED  256 /* pages pinned at once; past it a page keeps flipping */
#define PMU_SWEEP_TICKS 100 /* hv_tick() runs at 1 kHz: pinned pages re-armed after 100 ms */
#define PMU_PRINT_MAX   64  /* rewritten pages reported on the console (all are counted/logged) */
#define PMU_STUCK_LIMIT                                                                            \
    256 /* unproductive faults in a row at one PC before the stock path gets it */
#define PMU_HEAP_MARGIN (16UL << 20)
#define PMU_SKIP_PRINT_MAX 64 /* data-guard skips reported on the console (all are counted) */

extern u64 hv_cpus_in_guest;

struct pmu_log_entry {
    u64 ipa;
    u64 elr;
    u32 old_insn;
    u32 new_insn;
};

static struct {
    bool armed;
    u32 nr_ranges;
    struct {
        u64 base, size;
    } ranges[PMU_MAX_RANGES];
    u64 stats[HV_WIN_PMU_NR_STATS];
    u64 pinned[PMU_MAX_PINNED];
    u32 nr_pinned;
    u32 ticks;
    u64 printed;
    u64 skips_printed;
    struct pmu_log_entry log[HV_WIN_PMU_LOG_SIZE];
} pmu;

static u64 stuck_elr[MAX_CPUS];
static u32 stuck_count[MAX_CPUS];

static bool overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
    return a < b + b_size && b < a + a_size;
}

int hv_win_pmu_arm(u64 base, u64 size)
{
    if (!hv_win_enabled()) {
        printf("HV/win: PMU rewrite: P_HV_WIN_CONFIGURE first\n");
        return -1;
    }
    if (hv_cpus_in_guest) {
        printf("HV/win: PMU rewrite: refused while guest CPUs run (0x%lx)\n", hv_cpus_in_guest);
        return -1;
    }
    if (((base | size) & (PMU_PAGE - 1)) || !size || base + size < base) {
        printf("HV/win: PMU rewrite: range 0x%lx+0x%lx is not 16 KiB aligned\n", base, size);
        return -1;
    }
    if (pmu.nr_ranges >= PMU_MAX_RANGES) {
        printf("HV/win: PMU rewrite: more than %d ranges\n", PMU_MAX_RANGES);
        return -1;
    }
    for (u32 i = 0; i < pmu.nr_ranges; i++) {
        if (overlaps(base, size, pmu.ranges[i].base, pmu.ranges[i].size)) {
            printf("HV/win: PMU rewrite: 0x%lx+0x%lx overlaps an armed range\n", base, size);
            return -1;
        }
    }

    /* RAM only: never the framebuffer (or MMIO, which is never a writable HW page of RAM here) */
    u64 fb = cur_boot_args.video.base;
    u64 fb_size = cur_boot_args.video.stride * cur_boot_args.video.height;
    if (fb && fb_size && overlaps(base, size, fb, fb_size)) {
        printf("HV/win: PMU rewrite: 0x%lx+0x%lx overlaps the framebuffer 0x%lx+0x%lx\n", base,
               size, fb, fb_size);
        return -1;
    }

    u64 tables;
    int pages = hv_xt_track(base, size, true, &tables);
    if (pages <= 0) {
        printf("HV/win: PMU rewrite: no writable RAM mapped in stage 2 at 0x%lx+0x%lx (map it "
               "first)\n",
               base, size);
        return -1;
    }
    if (tables * PMU_PAGE + PMU_HEAP_MARGIN > heapblock_remaining()) {
        printf("HV/win: PMU rewrite: %lu L3 tables (%lu KiB) do not fit the heap (%lu KiB left)\n",
               tables, tables * PMU_PAGE >> 10, (u64)heapblock_remaining() >> 10);
        return -1;
    }

    pages = hv_xt_track(base, size, false, &tables);
    hv_xt_tlbi(true);

    pmu.ranges[pmu.nr_ranges].base = base;
    pmu.ranges[pmu.nr_ranges].size = size;
    pmu.nr_ranges++;
    pmu.stats[HV_WIN_PMU_RANGES]++;
    pmu.stats[HV_WIN_PMU_PAGES] += pages;
    pmu.stats[HV_WIN_PMU_TABLES] += tables;
    pmu.armed = true;

    printf(
        "HV/win: PMU rewrite armed on 0x%lx..0x%lx: %d page(s) of 16 KiB execute-tracked, %lu L3 "
        "table(s) (%lu KiB)\n",
        base, base + size, pages, tables, tables * PMU_PAGE >> 10);
    return pages;
}

u64 hv_win_pmu_stat(u64 idx)
{
    if (idx == HV_WIN_PMU_ID)
        return HV_WIN_PMU_MAGIC << 32 | (u64)HV_WIN_PMU_VERSION << 16 | pmu.armed;
    if (idx < HV_WIN_PMU_NR_STATS)
        return pmu.stats[idx];

    u64 slot = idx & 0xff;
    switch (idx & ~0xffUL) {
        case HV_WIN_PMU_STAT_LOG_IPA:
            return pmu.log[slot].ipa;
        case HV_WIN_PMU_STAT_LOG_OP:
            return (u64)pmu.log[slot].old_insn << 32 | pmu.log[slot].new_insn;
        case HV_WIN_PMU_STAT_LOG_ELR:
            return pmu.log[slot].elr;
        default:
            return ~0UL;
    }
}

/* A fault that changed nothing (stale TLB, failed translation): retry, but not forever */
static bool pmu_unproductive(struct exc_info *ctx)
{
    int cpu = smp_id();

    /* drop this CPU's stale stage-1 and stage-2 entries for the guest before the retry */
    hv_xt_tlbi(false);

    if (stuck_elr[cpu] != ctx->elr) {
        stuck_elr[cpu] = ctx->elr;
        stuck_count[cpu] = 0;
    }
    if (++stuck_count[cpu] < PMU_STUCK_LIMIT)
        return true;

    stuck_count[cpu] = 0;
    pmu.stats[HV_WIN_PMU_STUCK]++;
    printf(
        "HV/win: PMU rewrite: CPU %d made no progress at PC 0x%lx (ESR 0x%lx FAR 0x%lx) after %d "
        "tries; handing the fault on\n",
        cpu, ctx->elr, ctx->esr, ctx->far, PMU_STUCK_LIMIT);
    return false;
}

static void pmu_productive(void)
{
    stuck_count[smp_id()] = 0;
}

struct pmu_scan {
    u64 ipa;
    u64 elr;
    u32 first_index;
    u32 first_old, first_new;
    u32 n;
};

static void pmu_record(void *arg, u32 index, u32 old_insn, u32 new_insn)
{
    struct pmu_scan *sc = arg;
    struct pmu_log_entry *e = &pmu.log[pmu.stats[HV_WIN_PMU_INSNS]++ % HV_WIN_PMU_LOG_SIZE];

    e->ipa = sc->ipa + 4UL * index;
    e->elr = sc->elr;
    e->old_insn = old_insn;
    e->new_insn = new_insn;
    if (!sc->n++) {
        sc->first_index = index;
        sc->first_old = old_insn;
        sc->first_new = new_insn;
    }
}

static void pmu_skip(void *arg, u32 index, u32 old_insn, u32 next_word)
{
    struct pmu_scan *sc = arg;

    if (pmu.skips_printed++ < PMU_SKIP_PRINT_MAX)
        printf("HV/win: PMU rewrite: data guard kept IPA 0x%lx: 0x%08x (next word 0x%08x, fetch at "
               "PC 0x%lx)\n",
               sc->ipa + 4UL * index, old_insn, next_word, sc->elr);
}

static bool pmu_exec_fault(struct exc_info *ctx, u64 ipa)
{
    u64 page = ipa & ~(PMU_PAGE - 1), pa;
    u32 flips;

    switch (hv_xt_get(page, &pa, &flips)) {
        case HV_XT_UNTRACKED:
            pmu.stats[HV_WIN_PMU_NOT_OURS]++;
            return false;
        case HV_XT_DIRTY:
            break;
        default:
            /* CODE or PINNED: executable already, this CPU's TLB entry was stale */
            pmu.stats[HV_WIN_PMU_SPURIOUS]++;
            return pmu_unproductive(ctx);
    }

    /* 1. No writer: after the broadcast TLBI completes no guest store to the page is in flight */
    hv_xt_set(page, HV_XT_SCANNING, flips);
    hv_xt_tlbi(true);

    /*
     * 2. Scan what memory holds: the guest may have written the page with its MMU or caches off
     * (Mu early on), so drop any line EL2's cacheable view may hold (dirty lines are written back).
     */
    dc_civac_range((void *)pa, PMU_PAGE);
    sysop("dsb sy");

    struct pmu_scan sc = {.ipa = page, .elr = ctx->elr};
    u64 kinds[PMU_RW_NR_KINDS] = {0};
    u32 n = pmu_rw_rewrite_words_ex((u32 *)pa, PMU_PAGE_WORDS, pmu_record, pmu_skip, &sc, kinds);
    pmu.stats[HV_WIN_PMU_DATA_SKIPS] += kinds[PMU_RW_SKIP_DATA];

    if (n) {
        /* to PoC (an MMU-off fetch reads memory), then every CPU's I-cache: lines of an older
         * executable era of this page may be cached anywhere */
        dc_cvac_range((void *)pa, PMU_PAGE);
        sysop("dsb ish");
        ic_ialluis();
        sysop("dsb ish");
        sysop("isb");

        pmu.stats[HV_WIN_PMU_PAGES_REWRITTEN]++;
        pmu.stats[HV_WIN_PMU_INSNS_PMCCNTR] += kinds[PMU_RW_READ_PMCCNTR];
        pmu.stats[HV_WIN_PMU_INSNS_MRS] += kinds[PMU_RW_READ_OTHER];
        pmu.stats[HV_WIN_PMU_INSNS_MSR] += kinds[PMU_RW_WRITE];
        if (pmu.printed++ < PMU_PRINT_MAX)
            printf("HV/win: PMU rewrite: IPA 0x%lx (fetch at PC 0x%lx, CPU %d): %u word(s), first "
                   "+0x%x 0x%08x -> 0x%08x%s\n",
                   page, ctx->elr, smp_id(), n, sc.first_index * 4, sc.first_old, sc.first_new,
                   pmu.printed == PMU_PRINT_MAX ? " (further pages: counters and log only)" : "");
    }

    /* 3. Executable and read-only. Adding a permission needs only this CPU's TLB flushed */
    hv_xt_set(page, HV_XT_CODE, flips);
    hv_xt_tlbi(false);

    pmu.stats[HV_WIN_PMU_SCANS]++;
    pmu_productive();
    return true;
}

static bool pmu_write_fault(struct exc_info *ctx, u64 ipa, bool walker)
{
    u64 page = ipa & ~(PMU_PAGE - 1);
    u32 flips;

    switch (hv_xt_get(page, NULL, &flips)) {
        case HV_XT_UNTRACKED:
            pmu.stats[HV_WIN_PMU_NOT_OURS]++;
            return false;
        case HV_XT_CODE:
            break;
        default:
            /* DIRTY or PINNED: writable already, this CPU's TLB entry was stale */
            pmu.stats[HV_WIN_PMU_SPURIOUS]++;
            return pmu_unproductive(ctx);
    }

    flips++;
    if (flips >= PMU_FLIP_CAP) {
        if (pmu.nr_pinned < PMU_MAX_PINNED) {
            /* Written and executed too often: leave it RWX until the next sweep */
            if (!pmu.nr_pinned)
                pmu.ticks = 0;
            pmu.pinned[pmu.nr_pinned++] = page;
            hv_xt_set(page, HV_XT_PINNED, flips);
            hv_xt_tlbi(false);
            pmu.stats[HV_WIN_PMU_PINS]++;
            pmu_productive();
            return true;
        }
        pmu.stats[HV_WIN_PMU_PIN_FULL]++;
    }

    /* Execute permission goes away on every CPU before the guest's store can land */
    hv_xt_set(page, HV_XT_DIRTY, flips);
    hv_xt_tlbi(true);
    pmu.stats[walker ? HV_WIN_PMU_WALK_FLIPS : HV_WIN_PMU_WRITE_FLIPS]++;
    pmu_productive();
    return true;
}

static bool pmu_is_perm_fault(u64 esr)
{
    return (FIELD_GET(ESR_ISS_ABORT_FSC, esr) & FSC_PERM_LEVEL_MASK) == FSC_PERM;
}

bool hv_win_handle_iabort(struct exc_info *ctx)
{
    if (!pmu.armed || !pmu_is_perm_fault(ctx->esr))
        return false;

    /*
     * Stage-2 permission fault on an instruction fetch. Every tracked state is readable, so with
     * S1PTW set this is the stage-1 walker updating a descriptor (hardware AF/DBM) in a CODE page.
     */
    bool walker = ctx->esr & ESR_ISS_ABORT_S1PTW;
    u64 ipa;
    if (!hv_xt_fault_ipa(ctx, walker, walker, &ipa)) {
        pmu.stats[HV_WIN_PMU_IPA_FAIL]++;
        /*
         * AT S1E1R fails if the guest changed its tables under us, or for a page EL1 may execute
         * but not read (permission indirection). HPFAR_EL2 may then still name the page: the
         * architecture leaves it UNKNOWN for this fault, so only a DIRTY page is taken from it (a
         * wrong guess costs one scan and one later write fault).
         */
        if (walker || !hv_xt_fault_ipa(ctx, true, false, &ipa) ||
            hv_xt_get(ipa & ~(PMU_PAGE - 1), NULL, NULL) != HV_XT_DIRTY)
            return pmu_unproductive(ctx);
    }

    return walker ? pmu_write_fault(ctx, ipa, true) : pmu_exec_fault(ctx, ipa);
}

bool hv_win_handle_dabort(struct exc_info *ctx, u64 esr)
{
    if (!pmu.armed || !pmu_is_perm_fault(esr))
        return false;

    /* Every tracked state is readable: a data read permission fault is never ours */
    bool walker = esr & ESR_ISS_ABORT_S1PTW;
    if (!walker && !(esr & ESR_ISS_ABORT_WNR))
        return false;

    u64 ipa;
    if (!hv_xt_fault_ipa(ctx, walker, true, &ipa)) {
        pmu.stats[HV_WIN_PMU_IPA_FAIL]++;
        return pmu_unproductive(ctx);
    }

    return pmu_write_fault(ctx, ipa, walker);
}

/*
 * EL2 is about to fill guest RAM [ipa, ipa + size) on the guest's behalf (an emulated device's DMA
 * write: an NVMe READ's data, a mediated-storage reply). EL2 stores through its own identity
 * mapping, which stage 2 does not check, so a CODE page would keep its execute permission while
 * its contents change: an installed Windows pages code back in from disk into frames that held
 * scanned code before, and that code would run with its PMUv3 accesses unrewritten. Every CODE or
 * PINNED 16 KiB page the range touches (partial pages included: a 4 KiB guest page re-arms the
 * whole 16 KiB leaf) goes back to DIRTY, so the next instruction fetch rescans it, as after a
 * guest store (pmu_write_fault); the flips counter is kept (a refill is not a write-after-execute
 * by the guest). DIRTY pages need nothing; SCANNING exists only inside pmu_exec_fault under the
 * same lock. Call before the first byte is stored, under the big HV lock (the storage dispatch
 * holds it for the whole command). One broadcast TLBI per call, only if a page changed state, so
 * execute permission is gone on every CPU before EL2 writes. O(pages touched), no allocation, one
 * test when the rewrite is off. Returns the number of pages reset.
 */
u32 hv_win_pmu_dma_write(u64 ipa, u64 size)
{
    if (!pmu.armed || !size)
        return 0;

    u64 last = ipa + (size - 1);
    if (last < ipa)
        last = ~0UL;
    last &= ~(PMU_PAGE - 1);
    u32 n = 0;

    for (u64 page = ipa & ~(PMU_PAGE - 1);; page += PMU_PAGE) {
        u32 flips;
        enum hv_xt_state st = hv_xt_get(page, NULL, &flips);
        if (st == HV_XT_CODE || st == HV_XT_PINNED) {
            /* hv_xt_set: leaf store, then DSB ISHST before the TLBI below */
            hv_xt_set(page, HV_XT_DIRTY, flips);
            n++;
        }
        if (page == last)
            break;
    }

    if (n) {
        /* DSB ISHST; TLBI VMALLS12E1IS; DSB ISH; ISB (hv_vm.c) */
        hv_xt_tlbi(true);
        pmu.stats[HV_WIN_PMU_DMA_RESETS] += n;
    }
    return n;
}

void hv_win_pmu_tick(void)
{
    if (!pmu.armed || !pmu.nr_pinned || ++pmu.ticks < PMU_SWEEP_TICKS)
        return;

    for (u32 i = 0; i < pmu.nr_pinned; i++) {
        if (hv_xt_get(pmu.pinned[i], NULL, NULL) == HV_XT_PINNED) {
            hv_xt_set(pmu.pinned[i], HV_XT_DIRTY, 0);
            pmu.stats[HV_WIN_PMU_UNPINS]++;
        }
    }
    pmu.nr_pinned = 0;
    pmu.ticks = 0;
    hv_xt_tlbi(true);
    pmu.stats[HV_WIN_PMU_SWEEPS]++;
}

void hv_win_pmu_report(void)
{
    if (!pmu.armed)
        return;

    u64 *s = pmu.stats;
    printf(
        "HV/win: PMU rewrite: %lu pages tracked, %lu scans, %lu pages rewritten, %lu words "
        "(PMCCNTR %lu, other mrs %lu, msr %lu); flips: write %lu, walker %lu; pinned %lu (list "
        "full %lu), sweeps %lu, unpinned %lu; spurious %lu, AT failures %lu, not ours %lu, stuck "
        "%lu, device refills %lu, data-guard skips %lu\n",
        s[HV_WIN_PMU_PAGES], s[HV_WIN_PMU_SCANS], s[HV_WIN_PMU_PAGES_REWRITTEN],
        s[HV_WIN_PMU_INSNS], s[HV_WIN_PMU_INSNS_PMCCNTR], s[HV_WIN_PMU_INSNS_MRS],
        s[HV_WIN_PMU_INSNS_MSR], s[HV_WIN_PMU_WRITE_FLIPS], s[HV_WIN_PMU_WALK_FLIPS],
        s[HV_WIN_PMU_PINS], s[HV_WIN_PMU_PIN_FULL], s[HV_WIN_PMU_SWEEPS], s[HV_WIN_PMU_UNPINS],
        s[HV_WIN_PMU_SPURIOUS], s[HV_WIN_PMU_IPA_FAIL], s[HV_WIN_PMU_NOT_OURS],
        s[HV_WIN_PMU_STUCK], s[HV_WIN_PMU_DMA_RESETS], s[HV_WIN_PMU_DATA_SKIPS]);
}

#ifdef PMU_REWRITE_HOST_TEST
void hv_win_pmu_host_reset(void)
{
    memset(&pmu, 0, sizeof(pmu));
    memset(stuck_elr, 0, sizeof(stuck_elr));
    memset(stuck_count, 0, sizeof(stuck_count));
}
#endif
