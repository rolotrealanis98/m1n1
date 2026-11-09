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
#include "string.h"
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

/*
 * Shadow split view (m5pro-windows ci/EL2-USERMODE-CI-FIX.md). A rewritten code page keeps its
 * signed original as well as the rewritten copy, so Windows' user-mode Code Integrity, which
 * re-hashes a page-hash-signed image when a process maps it (winsta.dll in lsass), reads the
 * original bytes while the CPU fetches the PMU-safe rewrite. The rewrite is in place (the real
 * guest page holds the rewritten words); the shadow is a 16 KiB snapshot of the original:
 *
 *   rewritten page (real PA)  -- execute-only (CODE_XO): fetches run, data reads fault
 *   original snapshot (shadow) -- RO + XN: data reads served, fetches fault
 *
 * A data-read fault flips the leaf to the shadow (original, RO+XN); a fetch fault flips it back to
 * the rewritten page (CODE_XO). CI hashes a page in a read burst with no interleaved execution, so
 * it stays on the original for the whole hash. A guest write drops the shadow and the page re-joins
 * the normal DIRTY/pin path. The snapshot is bounded by pages rewritten (~4 MiB on the measured
 * run); when the pool is exhausted the page is rewritten in place with no shadow, as before.
 */
#define PMU_MAX_SHADOWS 512 /* 512 * 16 KiB = 8 MiB cap (observed run: 251 pages) */
#define PMU_SHADOW_FLIP_CAP                                                                        \
    8 /* read<->exec flips before a shadowed page is pinned RO+X (hash may then fail) */

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
    struct pmu_log_entry log[HV_WIN_PMU_LOG_SIZE];
} pmu;

static u64 stuck_elr[MAX_CPUS];
static u32 stuck_count[MAX_CPUS];

/*
 * Pages that carry a shadow (signed original). The real guest page holds the rewritten words;
 * shadow_pa is the 16 KiB snapshot of the original. on_exec says which copy the leaf maps now:
 *   true  -> the rewritten real page, execute-only (serving fetches)
 *   false -> the shadow (original), RO + XN (serving data reads)
 * pinned -> read<->exec thrash hit the cap: the leaf maps the rewritten page RO + X (its hash may
 * fail, but none was observed; see PMU_SHADOW_FLIP_CAP).
 */
struct pmu_shadow {
    u64 page;         /* IPA page base (16 KiB aligned) */
    u64 rewritten_pa; /* the real guest RAM page, rewritten in place */
    u64 shadow_pa;    /* heap snapshot of the signed original */
    bool on_exec;
    bool pinned;
    u32 flips; /* read<->exec flips */
};

static struct pmu_shadow pmu_shadows[PMU_MAX_SHADOWS];
static u32 pmu_nr_shadows;
static u8 *pmu_shadow_pool[PMU_MAX_SHADOWS]; /* freed snapshots, recycled (heapblock has no free) */
static u32 pmu_shadow_pool_n;
static u32 pmu_shadow_bufs; /* distinct 16 KiB snapshots ever allocated (the pool cap) */

static struct pmu_shadow *pmu_shadow_find(u64 page)
{
    for (u32 i = 0; i < pmu_nr_shadows; i++)
        if (pmu_shadows[i].page == page)
            return &pmu_shadows[i];
    return NULL;
}

/* A 16 KiB snapshot buffer: a recycled one, else a fresh one from the heap, else NULL (full) */
static u8 *pmu_shadow_buf(void)
{
    if (pmu_shadow_pool_n)
        return pmu_shadow_pool[--pmu_shadow_pool_n];
    if (pmu_shadow_bufs >= PMU_MAX_SHADOWS || heapblock_remaining() < PMU_PAGE + PMU_HEAP_MARGIN)
        return NULL;
    u8 *buf = heapblock_alloc_aligned(PMU_PAGE, PMU_PAGE);
    if (buf)
        pmu_shadow_bufs++;
    return buf;
}

static void pmu_shadow_recycle(u8 *buf)
{
    if (buf && pmu_shadow_pool_n < PMU_MAX_SHADOWS)
        pmu_shadow_pool[pmu_shadow_pool_n++] = buf;
}

static struct pmu_shadow *pmu_shadow_add(u64 page, u64 rewritten_pa, u64 shadow_pa)
{
    struct pmu_shadow *s = &pmu_shadows[pmu_nr_shadows++];
    s->page = page;
    s->rewritten_pa = rewritten_pa;
    s->shadow_pa = shadow_pa;
    s->on_exec = true;
    s->pinned = false;
    s->flips = 0;
    pmu.stats[HV_WIN_PMU_SHADOWS]++;
    return s;
}

static void pmu_shadow_remove(struct pmu_shadow *s)
{
    if (pmu_shadow_pool_n < PMU_MAX_SHADOWS)
        pmu_shadow_pool[pmu_shadow_pool_n++] = (u8 *)s->shadow_pa;
    *s = pmu_shadows[--pmu_nr_shadows];
}

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

    /*
     * Snapshot the signed original before the rewrite, so Code Integrity's later page-hash read of
     * a user-mode image sees it (the shadow split view). Bounded by pages rewritten; a NULL buffer
     * (pool or heap exhausted) just means this page gets no shadow, as before.
     */
    u8 *shadow = pmu_shadow_buf();
    if (shadow)
        memcpy(shadow, (void *)pa, PMU_PAGE);

    struct pmu_scan sc = {.ipa = page, .elr = ctx->elr};
    u64 kinds[PMU_RW_NR_KINDS] = {0};
    u32 n = pmu_rw_rewrite_words((u32 *)pa, PMU_PAGE_WORDS, pmu_record, &sc, kinds);

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

    if (n && shadow) {
        /*
         * 3a. The page was rewritten and keeps a shadow: execute-only, so the triggering fetch runs
         * the PMU-safe words while a data read faults and is served the original (pmu_shadow_read).
         */
        pmu_shadow_add(page, pa, (u64)shadow);
        hv_xt_set(page, HV_XT_CODE_XO, flips);
        hv_xt_tlbi(false);
    } else {
        /* 3b. No rewrite, or no shadow available: ordinary read-only executable (as before). A
         * rewritten page with no shadow has its data reads see the rewrite -- only when the pool is
         * exhausted. */
        if (shadow)
            pmu_shadow_recycle(shadow);
        else if (n)
            pmu.stats[HV_WIN_PMU_SHADOW_FULL]++;
        hv_xt_set(page, HV_XT_CODE, flips);
        hv_xt_tlbi(false);
    }

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
        case HV_XT_CODE_XO: /* a shadowed page the guest is writing, its shadow already dropped */
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

/* Fetch fault on a shadowed page: flip the leaf to the rewritten copy (execute-only) and re-run */
static bool pmu_shadow_exec(struct exc_info *ctx, struct pmu_shadow *s)
{
    if (s->on_exec || s->pinned) {
        /* already executable: this CPU's TLB entry was stale */
        pmu.stats[HV_WIN_PMU_SPURIOUS]++;
        return pmu_unproductive(ctx);
    }
    u32 flips;
    hv_xt_get(s->page, NULL, &flips);
    s->on_exec = true;
    hv_xt_remap(s->page, s->rewritten_pa, HV_XT_CODE_XO, flips);
    pmu.stats[HV_WIN_PMU_SHADOW_EXECS]++;
    pmu_productive();
    return true;
}

/* Data-read fault on a shadowed page: flip the leaf to the original copy (RO + XN) and re-run */
static bool pmu_shadow_read(struct exc_info *ctx, struct pmu_shadow *s)
{
    if (!s->on_exec || s->pinned) {
        /* already readable: stale TLB entry */
        pmu.stats[HV_WIN_PMU_SPURIOUS]++;
        return pmu_unproductive(ctx);
    }
    u32 flips;
    hv_xt_get(s->page, NULL, &flips);
    if (++s->flips >= PMU_SHADOW_FLIP_CAP) {
        /* Executed and read in a tight interleave: pin the rewritten copy RO + X and stop flipping
         * (its hash may fail; none observed). A guest write still faults and drops the shadow. */
        s->pinned = true;
        s->on_exec = false;
        hv_xt_remap(s->page, s->rewritten_pa, HV_XT_CODE, flips);
        pmu.stats[HV_WIN_PMU_SHADOW_PINS]++;
        pmu_productive();
        return true;
    }
    s->on_exec = false;
    hv_xt_remap(s->page, s->shadow_pa, HV_XT_SCANNING,
                flips); /* RO + XN: the original, for reads */
    pmu.stats[HV_WIN_PMU_SHADOW_READS]++;
    pmu_productive();
    return true;
}

/*
 * A write (or stage-1 walker update) to a shadowed page: the guest is modifying its own code, so
 * drop the shadow and let the page re-join the normal DIRTY/pin path. If the leaf was on the
 * original copy, repoint it to the real (rewritten) page first, so the store lands in guest RAM.
 */
static bool pmu_shadow_write(struct exc_info *ctx, struct pmu_shadow *s, u64 ipa, bool walker)
{
    if (!s->on_exec && !s->pinned) {
        u32 flips;
        hv_xt_get(s->page, NULL, &flips);
        hv_xt_remap(s->page, s->rewritten_pa, HV_XT_CODE_XO, flips);
    }
    pmu_shadow_remove(s);
    pmu.stats[HV_WIN_PMU_SHADOW_WRITES]++;
    return pmu_write_fault(ctx, ipa, walker);
}

bool hv_win_handle_iabort(struct exc_info *ctx)
{
    if (!pmu.armed || !pmu_is_perm_fault(ctx->esr))
        return false;

    /*
     * Stage-2 permission fault on an instruction fetch: a DIRTY page (XN) to scan, a shadowed page
     * resting on the original (XN) to flip to its rewritten copy, or -- with S1PTW -- the stage-1
     * walker updating a descriptor (hardware AF/DBM) in such a page.
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

    struct pmu_shadow *s = pmu_shadow_find(ipa & ~(PMU_PAGE - 1));
    if (s)
        return walker ? pmu_shadow_write(ctx, s, ipa, true) : pmu_shadow_exec(ctx, s);

    return walker ? pmu_write_fault(ctx, ipa, true) : pmu_exec_fault(ctx, ipa);
}

bool hv_win_handle_dabort(struct exc_info *ctx, u64 esr)
{
    if (!pmu.armed || !pmu_is_perm_fault(esr))
        return false;

    bool walker = esr & ESR_ISS_ABORT_S1PTW;
    bool write = walker || (esr & ESR_ISS_ABORT_WNR);

    u64 ipa;
    if (!hv_xt_fault_ipa(ctx, walker, write, &ipa)) {
        /* A read we cannot place is not ours; a write may be (as today) -- retry it */
        if (!write)
            return false;
        pmu.stats[HV_WIN_PMU_IPA_FAIL]++;
        return pmu_unproductive(ctx);
    }

    struct pmu_shadow *s = pmu_shadow_find(ipa & ~(PMU_PAGE - 1));
    if (s)
        return write ? pmu_shadow_write(ctx, s, ipa, walker) : pmu_shadow_read(ctx, s);

    /* A plain data-read permission fault on a non-shadowed page is never ours */
    if (!write)
        return false;

    return pmu_write_fault(ctx, ipa, walker);
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
        "%lu\n",
        s[HV_WIN_PMU_PAGES], s[HV_WIN_PMU_SCANS], s[HV_WIN_PMU_PAGES_REWRITTEN],
        s[HV_WIN_PMU_INSNS], s[HV_WIN_PMU_INSNS_PMCCNTR], s[HV_WIN_PMU_INSNS_MRS],
        s[HV_WIN_PMU_INSNS_MSR], s[HV_WIN_PMU_WRITE_FLIPS], s[HV_WIN_PMU_WALK_FLIPS],
        s[HV_WIN_PMU_PINS], s[HV_WIN_PMU_PIN_FULL], s[HV_WIN_PMU_SWEEPS], s[HV_WIN_PMU_UNPINS],
        s[HV_WIN_PMU_SPURIOUS], s[HV_WIN_PMU_IPA_FAIL], s[HV_WIN_PMU_NOT_OURS],
        s[HV_WIN_PMU_STUCK]);
    printf("HV/win: PMU rewrite: shadows: %lu allocated (%lu live, %lu KiB), %lu reads served the "
           "original, %lu fetch flips, %lu dropped by a write, %lu pinned, %lu in place (pool "
           "full)\n",
           s[HV_WIN_PMU_SHADOWS], (u64)pmu_nr_shadows, (u64)pmu_shadow_bufs * (PMU_PAGE >> 10),
           s[HV_WIN_PMU_SHADOW_READS], s[HV_WIN_PMU_SHADOW_EXECS], s[HV_WIN_PMU_SHADOW_WRITES],
           s[HV_WIN_PMU_SHADOW_PINS], s[HV_WIN_PMU_SHADOW_FULL]);
}

#ifdef PMU_REWRITE_HOST_TEST
void hv_win_pmu_host_reset(void)
{
    memset(&pmu, 0, sizeof(pmu));
    memset(stuck_elr, 0, sizeof(stuck_elr));
    memset(stuck_count, 0, sizeof(stuck_count));
    memset(pmu_shadows, 0, sizeof(pmu_shadows));
    memset(pmu_shadow_pool, 0, sizeof(pmu_shadow_pool));
    pmu_nr_shadows = 0;
    pmu_shadow_pool_n = 0;
    pmu_shadow_bufs = 0;
}
#endif
