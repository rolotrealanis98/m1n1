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
 * Per 16 KiB stage-2 block of guest RAM (state in the stage-2 leaf, hv_vm.c "execute tracking"):
 *
 *   DIRTY (RW, XN)  --first fetch-->  SCANNING (RO, XN), PMUv3 words rewritten  -->  CODE (RO, X)
 *   CODE  --write or walker update-->  DIRTY, or PINNED (RW, X) at the PMU_FLIP_CAP-th flip
 *   PINNED  --sweep, every PMU_SWEEP_TICKS hypervisor ticks-->  DIRTY
 *   CODE, PINNED  --EL2 device write into the page (hv_win_pmu_dma_write)-->  DIRTY
 *
 * Two scan granules (hv_win_pmu_set_mode(), launcher --pmu-granule; fixed before the first arm):
 *
 * 4 KiB (default, nightly-5). Windows uses 4 KiB pages, so the four frames of a block can belong to
 * unrelated owners (code next to a page table: bugcheck 0x1A/0x404, 2026-11-10/11). A bit per
 * 4 KiB frame records SCANNED (n5b: one byte per 16 KiB block holds the four bits); a frame is scanned
 * only right after its proof (its instruction fetch faulted at stage 2 and a walk of the guest's
 * stage-1 tables confirmed that VA executable at the faulting EL and translated to this frame). Only the faulting frame is scanned, and only after that proof; a frame that
 * fails the proof is never read or written (the block still opens, so the guest always makes
 * progress). Frame state is valid only while the block is read-only (CODE, or SCANNING after a
 * re-protect): every transition to a writable state (write flip, pin, device refill of a PINNED
 * block) clears all four frames, so a frame written by the guest is rescanned at its next proven
 * fetch. Guest stores are never emulated: a store to a read-only block flips it to DIRTY and
 * re-executes natively, exactly as in the 16 KiB scheme, so no guest write is lost or altered.
 * Stage 2 cannot tell which frame of an executable block runs, so a CODE block with unscanned
 * frames is put on the open list and re-protected (CODE -> SCANNING, RO + XN, frame states kept)
 * every PMU_CLOSE_TICKS ticks; the next fetch from an unscanned frame then faults and is proven and
 * scanned, a fetch from a scanned frame only reopens the block. Between those points an unscanned
 * sibling can run without a scan (a PMU coverage gap, never a data write).
 *   SCANNING (RO, XN) is therefore also a stable state in this mode: a fetch reopens it (scanning
 *   the faulting frame if needed), a store flips it to DIRTY like CODE, a device refill clears the
 *   touched frames and keeps it.
 *
 * 16 KiB (legacy, --pmu-granule 16k): the nightly-4 behaviour, unchanged: the whole block is
 * scanned at its first fetch (with the data guard), no stage-1 proof, no open list.
 *
 * Everything runs under the big HV lock (the locked half of hv_exc_sync() and hv_tick()). A rewrite
 * happens only in SCANNING: no CPU can write the block (write permission revoked, broadcast TLBI
 * completed) or execute it (XN), so no instruction is modified while another CPU may execute it.
 */

#ifdef PMU_REWRITE_HOST_TEST
#include "pmu_host.h" /* also declares calloc() and free() */
#else
#include "cpu_regs.h"
#include "heapblock.h"
#include "malloc.h"
#include "memory.h"
#include "smp.h"
#include "utils.h"
#include "xnuboot.h"

#include "../hv.h"
#endif

#include "hv_win.h"
#include "hv_win_pmu_4k.h"
#include "hv_win_pmu_rewrite.h"

/* ESR_EL2 ISS for instruction and data aborts */
#define ESR_ISS_ABORT_S1PTW BIT(7)
#define ESR_ISS_ABORT_WNR   BIT(6)
#define ESR_ISS_ABORT_FSC   GENMASK(5, 0)
#define ESR_ISS_ABORT_FNV   BIT(10)
#define FSC_PERM_LEVEL_MASK 0x3c
#define FSC_PERM            0x0c /* permission fault, level 0..3 */
#define EC_IABORT_LOWER     0x20

#define PMU_PAGE         HV_XT_PAGE_SIZE
#define PMU_PAGE_WORDS   (PMU_PAGE / 4)
#define PMU_BLOCK_FRAMES (PMU_PAGE / PMU_FRAME_SIZE)
#define PMU_MAX_RANGES   32
#define PMU_FLIP_CAP     4    /* write-after-execute flips before a page is pinned RWX */
#define PMU_MAX_PINNED   256  /* pages pinned at once; past it a page keeps flipping */
#define PMU_SWEEP_TICKS  100  /* hv_tick() runs at 1 kHz: pinned pages re-armed after 100 ms */
#ifndef PMU_MAX_OPEN /* host tests shrink it to reach the list-full path */
#define PMU_MAX_OPEN 1024 /* 4 KiB: CODE blocks with an unscanned frame, re-protected ... */
#endif
#define PMU_CLOSE_TICKS  10   /* ... every 10 ticks, or at once when the list is full */
#define PMU_PRINT_MAX    64   /* rewritten pages reported on the console (all are counted/logged) */
#define PMU_STUCK_LIMIT                                                                            \
    256 /* unproductive faults in a row at one PC before the stock path gets it */
#define PMU_HEAP_MARGIN    (16UL << 20)
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
    bool mode16k;
    u32 nr_ranges;
    struct {
        u64 base, size;
        u8 *frames; /* 4 KiB mode: one byte per 16 KiB block: PMU_BS_SCANNED(frame), PMU_BS_LISTED */
    } ranges[PMU_MAX_RANGES];
    u64 stats[HV_WIN_PMU_NR_STATS];
    u64 pinned[PMU_MAX_PINNED];
    u32 nr_pinned;
    u32 ticks;
    u64 open[PMU_MAX_OPEN];
    u32 nr_open;
    u32 close_ticks;
    u64 printed;
    u64 skips_printed;
    struct pmu_log_entry log[HV_WIN_PMU_LOG_SIZE];
} pmu;

static u64 stuck_elr[MAX_CPUS];
static u32 stuck_count[MAX_CPUS];

#ifdef PMU_REWRITE_HOST_TEST
/* A host suite without a stage-1 model gets no proof: the 4 KiB mode then opens blocks unscanned */
__attribute__((weak)) bool pmu_host_s1(struct pmu_s1 *s)
{
    (void)s;
    return false;
}
#endif

static bool overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
    return a < b + b_size && b < a + a_size;
}

/*
 * 4 KiB mode state: one byte per 16 KiB block (n5b: was one byte per 4 KiB frame; 41.4 GiB of guest RAM
 * needed 10.6 MiB of it and the arm did not fit the heap on hardware). Bits 0..3: frame N scanned (a frame
 * is only ever scanned right after its execution proof, so no separate "executed" bit is kept); bit 7: the
 * block is on the open list.
 */
#define PMU_BS_SCANNED(frame) (1U << (frame))
#define PMU_BS_ALL_SCANNED    0x0fU
#define PMU_BS_LISTED         0x80U

/* the state byte of the block holding ipa, or NULL outside the armed ranges (or in the 16 KiB mode) */
static u8 *pmu_frames(u64 ipa)
{
    for (u32 i = 0; i < pmu.nr_ranges; i++) {
        if (pmu.ranges[i].frames && ipa >= pmu.ranges[i].base &&
            ipa - pmu.ranges[i].base < pmu.ranges[i].size)
            return &pmu.ranges[i].frames[(ipa - pmu.ranges[i].base) / PMU_PAGE];
    }
    return NULL;
}

static void pmu_clear_frames(u8 *fr)
{
    *fr &= PMU_BS_LISTED;
}

static bool pmu_all_scanned(const u8 *fr)
{
    return (*fr & PMU_BS_ALL_SCANNED) == PMU_BS_ALL_SCANNED;
}

int hv_win_pmu_set_mode(u64 flags)
{
    bool mode16k = flags & HV_WIN_PMU_MODE_16K;
    if (flags & ~(u64)HV_WIN_PMU_MODE_16K)
        return -1;
    if (pmu.armed && mode16k != pmu.mode16k) {
        printf("HV/win: PMU rewrite: granule change refused after arming\n");
        return -1;
    }
    pmu.mode16k = mode16k;
    return 0;
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
    u64 frame_bytes = pmu.mode16k ? 0 : size / PMU_PAGE;
    if (tables * PMU_PAGE + PMU_HEAP_MARGIN + frame_bytes > heapblock_remaining()) {
        printf("HV/win: PMU rewrite: %lu L3 tables (%lu KiB) + %lu KiB 4 KiB-granule state + %lu KiB margin "
               "do not fit the heap (%lu KiB left)\n",
               tables, tables * PMU_PAGE >> 10, frame_bytes >> 10, PMU_HEAP_MARGIN >> 10,
               (u64)heapblock_remaining() >> 10);
        return -1;
    }

    u8 *frames = NULL;
    if (frame_bytes) {
        frames = calloc(frame_bytes, 1);
        if (!frames) {
            printf("HV/win: PMU rewrite: no heap for %lu block state bytes\n", frame_bytes);
            return -1;
        }
    }

    pages = hv_xt_track(base, size, false, &tables);
    hv_xt_tlbi(true);

    pmu.ranges[pmu.nr_ranges].base = base;
    pmu.ranges[pmu.nr_ranges].size = size;
    pmu.ranges[pmu.nr_ranges].frames = frames;
    pmu.nr_ranges++;
    pmu.stats[HV_WIN_PMU_RANGES]++;
    pmu.stats[HV_WIN_PMU_PAGES] += pages;
    pmu.stats[HV_WIN_PMU_TABLES] += tables;
    pmu.armed = true;

    printf(
        "HV/win: PMU rewrite armed on 0x%lx..0x%lx: %d page(s) of 16 KiB execute-tracked, %lu L3 "
        "table(s) (%lu KiB)%s\n",
        base, base + size, pages, tables, tables * PMU_PAGE >> 10,
        pmu.mode16k ? "" : ", 4 KiB scan granule");
    return pages;
}

u64 hv_win_pmu_stat(u64 idx)
{
    if (idx == HV_WIN_PMU_ID)
        return HV_WIN_PMU_MAGIC << 32 | (u64)HV_WIN_PMU_VERSION << 16 |
               (pmu.mode16k ? 0 : HV_WIN_PMU_ID_4K) | pmu.armed;
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
        case HV_WIN_PMU_STAT_HEAP: /* n5b: launcher preflight (need vs available before select) */
            return slot == 0 ? (u64)heapblock_remaining() : slot == 1 ? PMU_HEAP_MARGIN : ~0UL;
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
    u32 frame_mask; /* 4 KiB frames (of the scanned range) holding a rewrite */
};

static void pmu_record(void *arg, u32 index, u32 old_insn, u32 new_insn)
{
    struct pmu_scan *sc = arg;
    struct pmu_log_entry *e = &pmu.log[pmu.stats[HV_WIN_PMU_INSNS]++ % HV_WIN_PMU_LOG_SIZE];

    e->ipa = sc->ipa + 4UL * index;
    e->elr = sc->elr;
    e->old_insn = old_insn;
    e->new_insn = new_insn;
    sc->frame_mask |= 1U << (index / PMU_FRAME_WORDS);
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

/* Shared tail of a scan that rewrote n words of [pa, pa + len) */
static void pmu_scanned(struct exc_info *ctx, u64 pa, u64 len, u32 n, const struct pmu_scan *sc,
                        const u64 *kinds)
{
    pmu.stats[HV_WIN_PMU_DATA_SKIPS] += kinds[PMU_RW_SKIP_DATA];
    pmu.stats[HV_WIN_PMU_WORDS_SKIP_BOUNDARY] += kinds[PMU_RW_SKIP_BOUNDARY];
    pmu.stats[HV_WIN_PMU_WORDS_SKIP_OTHER] += kinds[PMU_RW_SKIP_OTHER];
    pmu.stats[HV_WIN_PMU_WORDS_MODIFIED] += n;
    pmu.stats[HV_WIN_PMU_FRAMES_MODIFIED] += __builtin_popcount(sc->frame_mask);
    if (!n)
        return;

    /* to PoC (an MMU-off fetch reads memory), then every CPU's I-cache: lines of an older
     * executable era of this page may be cached anywhere */
    dc_cvac_range((void *)pa, len);
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
               sc->ipa, ctx->elr, smp_id(), n, sc->first_index * 4, sc->first_old, sc->first_new,
               pmu.printed == PMU_PRINT_MAX ? " (further pages: counters and log only)" : "");
}

/* ---- 16 KiB granule (legacy, nightly-4 behaviour) --------------------------------------------- */

static bool pmu_exec_fault_16k(struct exc_info *ctx, u64 ipa)
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
    pmu.stats[HV_WIN_PMU_FRAMES_SCANNED] += PMU_BLOCK_FRAMES;
    /* this granule's hazard, counted: rewrites in a frame other than the one whose fetch faulted */
    pmu.stats[HV_WIN_PMU_MODIFIED_NEVER_EXECUTED] +=
        __builtin_popcount(sc.frame_mask & ~(1U << ((ipa - page) / PMU_FRAME_SIZE)));
    pmu_scanned(ctx, pa, PMU_PAGE, n, &sc, kinds);

    /* 3. Executable and read-only. Adding a permission needs only this CPU's TLB flushed */
    hv_xt_set(page, HV_XT_CODE, flips);
    hv_xt_tlbi(false);

    pmu.stats[HV_WIN_PMU_SCANS]++;
    pmu_productive();
    return true;
}

/* ---- 4 KiB granule --------------------------------------------------------------------------- */

static bool pmu_read_pte(void *arg, u64 ipa, u64 *value)
{
    (void)arg;
    u64 pa;
    /* guest stage-1 tables are read only from tracked guest RAM: never MMIO, never EL2 memory */
    if ((ipa & 7) || !pmu_frames(ipa) || hv_xt_get(ipa, &pa, NULL) == HV_XT_UNTRACKED)
        return false;
    /* hv_xt_get returns the 16 KiB output base, not the subframe address */
    *value = *(volatile u64 *)(pa + (ipa & (PMU_PAGE - 1)));
    return true;
}

/*
 * The faulting fetch proves the frame executable only if the fault is a precise lower-EL
 * instruction abort at FAR == ELR whose IPA came from AT (not an HPFAR guess), and the guest's
 * current stage-1 tables map that VA executable at the faulting EL to exactly that IPA (a stale TLB
 * entry or a racing table update could otherwise send the scan to a data frame). Unsupported table
 * formats deny.
 */
static bool pmu_exec_proof(struct exc_info *ctx, u64 ipa, bool hint)
{
    if (hint || (ctx->esr >> 26) != EC_IABORT_LOWER || (ctx->esr & ESR_ISS_ABORT_FNV) ||
        ctx->far != ctx->elr)
        return false;
    struct pmu_s1 s;
#ifdef PMU_REWRITE_HOST_TEST
    if (!pmu_host_s1(&s))
        return false;
#else
    s = (struct pmu_s1){.sctlr = mrs(SCTLR_EL12),
                        .tcr = mrs(TCR_EL12),
                        .ttbr = {mrs(TTBR0_EL12), mrs(TTBR1_EL12)}};
#endif
    u64 walked;
    return pmu_s1_exec(&s, ctx->far, ctx->spsr, pmu_read_pte, NULL, &walked) && walked == ipa;
}

/* Scan one proven frame (block in SCANNING). frame = 4 KiB index in the block, pa = block PA. */
static void pmu_scan_frame(struct exc_info *ctx, u64 block, u64 pa, u32 frame, u8 *fr, bool proven)
{
    u64 fipa = block + frame * PMU_FRAME_SIZE, fpa = pa + frame * PMU_FRAME_SIZE;

    /* Session assertion, checked before anything is read: only a proven frame is scanned. */
    if (!proven) {
        pmu.stats[HV_WIN_PMU_MODIFIED_NEVER_EXECUTED]++;
        return;
    }

    dc_civac_range((void *)fpa, PMU_FRAME_SIZE);
    sysop("dsb sy");

    /* The data guard looks at the word after the frame's last word: the next frame of the same
     * block, or the first word of the next tracked block. That word is only read, never written. */
    const u32 *next = NULL;
    u64 npa;
    if (frame + 1 < PMU_BLOCK_FRAMES)
        next = (const u32 *)(fpa + PMU_FRAME_SIZE);
    else if (block + PMU_PAGE > block && pmu_frames(block + PMU_PAGE) &&
             hv_xt_get(block + PMU_PAGE, &npa, NULL) != HV_XT_UNTRACKED)
        next = (const u32 *)npa;

    struct pmu_scan sc = {.ipa = fipa, .elr = ctx->elr};
    u64 kinds[PMU_RW_NR_KINDS] = {0};
    u32 n = pmu_rw_rewrite_frame((u32 *)fpa, next, pmu_record, pmu_skip, &sc, kinds);
    *fr |= PMU_BS_SCANNED(frame);
    pmu.stats[HV_WIN_PMU_FRAMES_SCANNED]++;
    pmu_scanned(ctx, fpa, PMU_FRAME_SIZE, n, &sc, kinds);
    pmu.stats[HV_WIN_PMU_SCANS]++;
}

/* Re-protect every listed block that is still CODE with an unscanned frame. */
static void pmu_close_open(void)
{
    bool changed = false;

    for (u32 i = 0; i < pmu.nr_open; i++) {
        u64 block = pmu.open[i];
        u8 *fr = pmu_frames(block);
        u32 flips;
        if (!fr)
            continue;
        *fr &= ~PMU_BS_LISTED;
        if (hv_xt_get(block, NULL, &flips) == HV_XT_CODE && !pmu_all_scanned(fr)) {
            hv_xt_set(block, HV_XT_SCANNING, flips);
            pmu.stats[HV_WIN_PMU_REPROTECTS]++;
            changed = true;
        }
    }
    pmu.nr_open = 0;
    pmu.close_ticks = 0;
    if (changed)
        hv_xt_tlbi(true);
}

static void pmu_open_add(u64 block, u8 *fr)
{
    if (*fr & PMU_BS_LISTED)
        return;
    if (pmu.nr_open >= PMU_MAX_OPEN)
        pmu_close_open();
    *fr |= PMU_BS_LISTED;
    pmu.open[pmu.nr_open++] = block;
}

static bool pmu_exec_fault_4k(struct exc_info *ctx, u64 ipa, bool hint)
{
    u64 block = ipa & ~(PMU_PAGE - 1), pa = 0;
    u32 flips = 0, frame = (ipa - block) / PMU_FRAME_SIZE;
    u8 *fr = pmu_frames(block);
    enum hv_xt_state st = fr ? hv_xt_get(block, &pa, &flips) : HV_XT_UNTRACKED;

    switch (st) {
        case HV_XT_UNTRACKED:
            pmu.stats[HV_WIN_PMU_NOT_OURS]++;
            return false;
        case HV_XT_DIRTY:
            /* writable until now: no frame state survives, and writes go away everywhere first */
            pmu_clear_frames(fr);
            hv_xt_set(block, HV_XT_SCANNING, flips);
            hv_xt_tlbi(true);
            break;
        case HV_XT_SCANNING:
            /* re-protected: read-only and XN on every CPU already, frame states valid */
            break;
        default:
            /* CODE or PINNED: executable already, this CPU's TLB entry was stale */
            pmu.stats[HV_WIN_PMU_SPURIOUS]++;
            return pmu_unproductive(ctx);
    }

    pmu.stats[HV_WIN_PMU_EXEC_FAULTS]++;
    if (!(*fr & PMU_BS_SCANNED(frame))) {
        if (pmu_exec_proof(ctx, ipa, hint)) {
            pmu_scan_frame(ctx, block, pa, frame, fr, true);
        } else {
            /* not proven: neither read nor written; the block opens anyway (progress) */
            pmu.stats[HV_WIN_PMU_EXEC_DENIED]++;
            pmu.stats[HV_WIN_PMU_WORDS_SKIP_EXEC] += PMU_FRAME_WORDS;
        }
    }

    /* Executable and read-only. Adding a permission needs only this CPU's TLB flushed */
    hv_xt_set(block, HV_XT_CODE, flips);
    hv_xt_tlbi(false);
    if (!pmu_all_scanned(fr))
        pmu_open_add(block, fr);
    pmu_productive();
    return true;
}

static bool pmu_exec_fault(struct exc_info *ctx, u64 ipa, bool hint)
{
    return pmu.mode16k ? pmu_exec_fault_16k(ctx, ipa) : pmu_exec_fault_4k(ctx, ipa, hint);
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
        case HV_XT_SCANNING:
            /* 4 KiB mode: a re-protected block, written like CODE (16 KiB: never seen here) */
            if (!pmu.mode16k)
                break;
            /* fall through */
        default:
            /* DIRTY or PINNED: writable already, this CPU's TLB entry was stale */
            pmu.stats[HV_WIN_PMU_SPURIOUS]++;
            return pmu_unproductive(ctx);
    }

    /* the block becomes writable: no frame state can stay valid (4 KiB mode) */
    u8 *fr = pmu_frames(page);
    if (fr)
        pmu_clear_frames(fr);

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
         * architecture leaves it UNKNOWN for this fault, so only a DIRTY page is taken from it.
         * 16 KiB: a wrong guess costs one scan and one later write fault. 4 KiB: a hint is never a
         * proof, so the block only opens; nothing is read or written.
         */
        if (walker || !hv_xt_fault_ipa(ctx, true, false, &ipa) ||
            hv_xt_get(ipa & ~(PMU_PAGE - 1), NULL, NULL) != HV_XT_DIRTY)
            return pmu_unproductive(ctx);
        return pmu_exec_fault(ctx, ipa, true);
    }

    return walker ? pmu_write_fault(ctx, ipa, true) : pmu_exec_fault(ctx, ipa, false);
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
 * scanned code before, and that code would run with its PMUv3 accesses unrewritten. Call before the
 * first byte is stored, under the big HV lock (the storage dispatch holds it for the whole
 * command). One broadcast TLBI per call, only if a page lost execute permission, so execute
 * permission is gone on every CPU before EL2 writes. O(pages touched), no allocation, one test
 * when the rewrite is off. Returns the number of 16 KiB pages whose execute permission was revoked.
 *   16 KiB: every CODE or PINNED page the range touches goes back to DIRTY (the flips counter is
 *   kept: a refill is not a write-after-execute by the guest). DIRTY pages need nothing.
 *   4 KiB: a CODE block is re-protected (SCANNING: RO + XN) and only the touched frames lose their
 *   state; a re-protected block stays SCANNING and loses the touched frames' state; a PINNED block
 *   goes to DIRTY (its frames hold no state). DIRTY blocks hold no frame state and need nothing.
 *   Either way the filled bytes are rescanned at their next proven fetch, and the copy that follows
 *   writes exactly the device's bytes (stage 2 does not apply to EL2's stores).
 */
u32 hv_win_pmu_dma_write(u64 ipa, u64 size)
{
    if (!pmu.armed || !size)
        return 0;

    u64 last = ipa + (size - 1);
    if (last < ipa)
        last = ~0UL;
    u64 last_page = last & ~(PMU_PAGE - 1);
    u32 n = 0;

    for (u64 page = ipa & ~(PMU_PAGE - 1);; page += PMU_PAGE) {
        u32 flips;
        enum hv_xt_state st = hv_xt_get(page, NULL, &flips);
        u8 *fr = st == HV_XT_UNTRACKED ? NULL : pmu_frames(page);
        if (fr && (st == HV_XT_CODE || st == HV_XT_SCANNING)) {
            u64 lo = ipa > page ? ipa : page;
            u64 hi = last < page + (PMU_PAGE - 1) ? last : page + (PMU_PAGE - 1);
            for (u64 f = (lo - page) / PMU_FRAME_SIZE; f <= (hi - page) / PMU_FRAME_SIZE; f++)
                *fr &= ~PMU_BS_SCANNED(f);
            if (st == HV_XT_CODE) {
                hv_xt_set(page, HV_XT_SCANNING, flips);
                n++;
            }
        } else if (st == HV_XT_CODE || st == HV_XT_PINNED) {
            /* hv_xt_set: leaf store, then DSB ISHST before the TLBI below */
            if (fr)
                pmu_clear_frames(fr);
            hv_xt_set(page, HV_XT_DIRTY, flips);
            n++;
        }
        if (page == last_page)
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
    if (!pmu.armed)
        return;

    if (pmu.nr_open && ++pmu.close_ticks >= PMU_CLOSE_TICKS)
        pmu_close_open();

    if (!pmu.nr_pinned || ++pmu.ticks < PMU_SWEEP_TICKS)
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
    printf("HV/win: PMU rewrite (%s granule): frames scanned %lu modified %lu, words modified %lu; "
           "skipped: exec-unproven %lu, boundary %lu, data-guard %lu, non-PMU %lu; "
           "modified-never-executed %lu; exec faults %lu (unproven %lu), re-protects %lu\n",
           pmu.mode16k ? "16 KiB" : "4 KiB", s[HV_WIN_PMU_FRAMES_SCANNED],
           s[HV_WIN_PMU_FRAMES_MODIFIED], s[HV_WIN_PMU_WORDS_MODIFIED],
           s[HV_WIN_PMU_WORDS_SKIP_EXEC], s[HV_WIN_PMU_WORDS_SKIP_BOUNDARY],
           s[HV_WIN_PMU_DATA_SKIPS], s[HV_WIN_PMU_WORDS_SKIP_OTHER],
           s[HV_WIN_PMU_MODIFIED_NEVER_EXECUTED], s[HV_WIN_PMU_EXEC_FAULTS],
           s[HV_WIN_PMU_EXEC_DENIED], s[HV_WIN_PMU_REPROTECTS]);
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
u8 hv_win_pmu_host_frame(u64 ipa)
{
    /* the old per-frame view: SCANNED implies EXECUTED (proof precedes every scan); LISTED on frame 0 */
    u8 *fr = pmu_frames(ipa);
    u32 f = (ipa & (PMU_PAGE - 1)) / PMU_FRAME_SIZE;
    if (!fr)
        return 0;
    return ((*fr & PMU_BS_SCANNED(f)) ? PMU_FRAME_EXECUTED | PMU_FRAME_SCANNED : 0) |
           (f == 0 ? (*fr & PMU_BS_LISTED) : 0);
}
void hv_win_pmu_host_reset(void)
{
    for (u32 i = 0; i < pmu.nr_ranges; i++)
        free(pmu.ranges[i].frames);
    memset(&pmu, 0, sizeof(pmu));
    memset(stuck_elr, 0, sizeof(stuck_elr));
    memset(stuck_count, 0, sizeof(stuck_count));
}
#endif
