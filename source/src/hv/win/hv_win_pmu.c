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
 * State is per 4 KiB frame; stage-2 permissions remain per 16 KiB block. Only an
 * observed, stage-1-validated execute fault authorizes a scan. At most one block
 * is RO+X; switching blocks or a tick revokes it. Siblings can execute during
 * that window without a scan (PMU coverage is incomplete, data is never scanned
 * speculatively). Stores are emulated with exact invalidation, never RWX-pinned.
 * All changes run under bhl and refuse to scan/store with another guest CPU active.
 */

#ifdef PMU_REWRITE_HOST_TEST
#include "pmu_host.h"
#include <stdlib.h>
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
#include "hv_win_pmu_rewrite.h"
#include "hv_win_pmu_4k.h"

/* ESR_EL2 ISS for instruction and data aborts */
#define ESR_ISS_ABORT_S1PTW BIT(7)
#define ESR_ISS_ABORT_WNR   BIT(6)
#define ESR_ISS_ABORT_FSC   GENMASK(5, 0)
#define FSC_PERM_LEVEL_MASK 0x3c
#define FSC_PERM            0x0c /* permission fault, level 0..3 */

#define PMU_PAGE        HV_XT_PAGE_SIZE
#define PMU_MAX_RANGES  32
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
        u8 *frames;
    } ranges[PMU_MAX_RANGES];
    u64 stats[HV_WIN_PMU_NR_STATS];
    u64 open_block;
    bool open;
    u64 printed;
    u64 skips_printed;
    struct pmu_log_entry log[HV_WIN_PMU_LOG_SIZE];
} pmu;

static u64 stuck_elr[MAX_CPUS];
static u32 stuck_count[MAX_CPUS];

/* The old host suite has no guest-register/store model. Missing weak hooks deny
 * the new operations rather than silently inventing executable mappings. */
#ifdef PMU_REWRITE_HOST_TEST
__attribute__((weak)) bool pmu_host_s1(struct pmu_s1 *s)
{
    (void)s;
    return false;
}
__attribute__((weak)) bool hv_emulate_pt_store(struct exc_info *ctx, u64 far, u64 ipa, u64 *bytes)
{
    (void)ctx; (void)far; (void)ipa; (void)bytes;
    return false;
}
#endif

static u8 *pmu_frame(u64 ipa)
{
    for (u32 i = 0; i < pmu.nr_ranges; i++) {
        if (ipa >= pmu.ranges[i].base && ipa - pmu.ranges[i].base < pmu.ranges[i].size)
            return &pmu.ranges[i].frames[(ipa - pmu.ranges[i].base) / PMU_FRAME_SIZE];
    }
    return NULL;
}

static bool pmu_read_pte(void *arg, u64 ipa, u64 *value)
{
    (void)arg;
    u64 pa;
    if ((ipa & 7) || hv_xt_get(ipa, &pa, NULL) == HV_XT_UNTRACKED || !pmu_frame(ipa))
        return false;
    /* hv_xt_get returns the 16 KiB output base, not the subframe address. */
    *value = *(volatile u64 *)(pa + (ipa & (PMU_PAGE - 1)));
    return true;
}

static bool pmu_exec_proof(struct exc_info *ctx, u64 ipa)
{
    if (__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE) || ctx->far != ctx->elr ||
        (ctx->esr >> 26) != 0x20 || (ctx->esr & BIT(10)))
        return false;
    struct pmu_s1 s;
#ifdef PMU_REWRITE_HOST_TEST
    if (!pmu_host_s1(&s))
        return false;
#else
    s = (struct pmu_s1){.sctlr = mrs(SCTLR_EL12), .tcr = mrs(TCR_EL12),
                       .ttbr = {mrs(TTBR0_EL12), mrs(TTBR1_EL12)}};
#endif
    u64 walked;
    return pmu_s1_exec(&s, ctx->far, ctx->spsr, pmu_read_pte, NULL, &walked) && walked == ipa;
}

static void pmu_close(void)
{
    if (!pmu.open)
        return;
    hv_xt_set(pmu.open_block, HV_XT_SCANNING, 0); /* RO + XN, never reopen writes */
    hv_xt_tlbi(true);
    pmu.open = false;
    pmu.stats[HV_WIN_PMU_REPROTECTS]++;
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
    if (size / PMU_FRAME_SIZE > heapblock_remaining() ||
        tables * PMU_PAGE + PMU_HEAP_MARGIN > heapblock_remaining() - size / PMU_FRAME_SIZE) {
        printf("HV/win: PMU rewrite: %lu L3 tables (%lu KiB) do not fit the heap (%lu KiB left)\n",
               tables, tables * PMU_PAGE >> 10, (u64)heapblock_remaining() >> 10);
        return -1;
    }

    u8 *frames = calloc(size / PMU_FRAME_SIZE, 1);
    if (!frames)
        return -1;
    pages = hv_xt_track(base, size, false, &tables);
    if (pages <= 0) {
        free(frames);
        return -1;
    }
    hv_xt_tlbi(true);

    pmu.ranges[pmu.nr_ranges].frames = frames;
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
    u64 block = ipa & ~(PMU_PAGE - 1), page = ipa & ~(PMU_FRAME_SIZE - 1), pa;
    u8 *frame = pmu_frame(page);
    if (!frame || hv_xt_get(block, &pa, NULL) == HV_XT_UNTRACKED) {
        pmu.stats[HV_WIN_PMU_NOT_OURS]++;
        return false;
    }
    pmu.stats[HV_WIN_PMU_EXEC_FAULTS]++;
    pmu_close();
    if (!pmu_exec_proof(ctx, ipa)) {
        pmu.stats[HV_WIN_PMU_EXEC_DENIED]++;
        pmu.stats[HV_WIN_PMU_WORDS_SKIP_EXEC] += PMU_FRAME_WORDS;
        return pmu_unproductive(ctx);
    }
    *frame |= PMU_FRAME_EXECUTED;
    hv_xt_set(block, HV_XT_SCANNING, 0);
    hv_xt_tlbi(true);
    pa += page - block;
    if (*frame & PMU_FRAME_SCANNED)
        goto executable;
    dc_civac_range((void *)pa, PMU_FRAME_SIZE);
    sysop("dsb sy");
    /* Assertion is checked before the first rewrite, and kept for the session. */
    if (!(*frame & PMU_FRAME_EXECUTED)) {
        pmu.stats[HV_WIN_PMU_MODIFIED_NEVER_EXECUTED]++;
        return false;
    }
    struct pmu_scan sc = {.ipa = page, .elr = ctx->elr};
    u64 kinds[PMU_RW_NR_KINDS] = {0};
    u32 n = pmu_rw_rewrite_frame((u32 *)pa, pmu_record, pmu_skip, &sc, kinds);
    pmu.stats[HV_WIN_PMU_DATA_SKIPS] += kinds[PMU_RW_SKIP_DATA];
    pmu.stats[HV_WIN_PMU_WORDS_SKIP_BOUNDARY] += kinds[PMU_RW_SKIP_BOUNDARY];
    pmu.stats[HV_WIN_PMU_WORDS_SKIP_OTHER] += kinds[PMU_RW_SKIP_OTHER];
    pmu.stats[HV_WIN_PMU_FRAMES_SCANNED]++;
    pmu.stats[HV_WIN_PMU_WORDS_MODIFIED] += n;
    *frame |= PMU_FRAME_SCANNED;

    if (n) {
        /* to PoC (an MMU-off fetch reads memory), then every CPU's I-cache: lines of an older
         * executable era of this page may be cached anywhere */
        dc_cvac_range((void *)pa, PMU_FRAME_SIZE);
        sysop("dsb ish");
        ic_ialluis();
        sysop("dsb ish");
        sysop("isb");

        pmu.stats[HV_WIN_PMU_PAGES_REWRITTEN]++;
        pmu.stats[HV_WIN_PMU_FRAMES_MODIFIED]++;
        pmu.stats[HV_WIN_PMU_INSNS_PMCCNTR] += kinds[PMU_RW_READ_PMCCNTR];
        pmu.stats[HV_WIN_PMU_INSNS_MRS] += kinds[PMU_RW_READ_OTHER];
        pmu.stats[HV_WIN_PMU_INSNS_MSR] += kinds[PMU_RW_WRITE];
        if (pmu.printed++ < PMU_PRINT_MAX)
            printf("HV/win: PMU rewrite: IPA 0x%lx (fetch at PC 0x%lx, CPU %d): %u word(s), first "
                   "+0x%x 0x%08x -> 0x%08x%s\n",
                   page, ctx->elr, smp_id(), n, sc.first_index * 4, sc.first_old, sc.first_new,
                   pmu.printed == PMU_PRINT_MAX ? " (further pages: counters and log only)" : "");
    }

    pmu.stats[HV_WIN_PMU_SCANS]++;
executable:
    if (hv_xt_set(block, HV_XT_CODE, 0) < 0)
        return false;
    hv_xt_tlbi(false);
    pmu.open_block = block;
    pmu.open = true;
    pmu_productive();
    return true;
}

static u32 pmu_invalidate(u64 ipa, u64 size);

static bool pmu_write_fault(struct exc_info *ctx, u64 ipa, bool walker)
{
    u64 block = ipa & ~(PMU_PAGE - 1);
    if (!pmu_frame(ipa) || hv_xt_get(block, NULL, NULL) == HV_XT_UNTRACKED) {
        pmu.stats[HV_WIN_PMU_NOT_OURS]++;
        return false;
    }
    pmu_close();
    hv_xt_set(block, HV_XT_SCANNING, 0);
    hv_xt_tlbi(true);
    /* HPFAR identifies only a 4 KiB table frame for S1PTW, not the descriptor.
     * Never guess its offset, emulate AF/DBM, or grant whole-block write access. */
    if (walker || __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE)) {
        pmu.stats[HV_WIN_PMU_STORE_DENIED]++;
        return false;
    }
    struct exc_info next = *ctx;
    u64 bytes;
    if (!hv_emulate_pt_store(&next, ctx->far, ipa, &bytes)) {
        pmu.stats[HV_WIN_PMU_STORE_DENIED]++;
        return false;
    }
    /* Guest-requested store, not a rewrite. X was revoked before the store;
     * the existing emulator rejects spans crossing a 16 KiB leaf. */
    pmu_invalidate(ipa, bytes);
    pmu.stats[HV_WIN_PMU_WRITE_FLIPS]++;
    *ctx = next;
    ctx->elr += 4; /* caller subtracts 4 for its retry convention */
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
        /* HPFAR is architecturally UNKNOWN here: no speculative scan fallback. */
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

/* Invalidate only intersecting 4 KiB frames. Permission revocation is necessarily
 * 16 KiB. Intersect with armed ranges first: a wrapping/huge DMA span is bounded. */
static u32 pmu_invalidate(u64 ipa, u64 size)
{
    if (!pmu.armed || !size)
        return 0;
    u64 last = ipa + size - 1;
    if (last < ipa)
        last = ~0UL;
    u32 n = 0;
    bool changed = false;
    for (u32 i = 0; i < pmu.nr_ranges; i++) {
        u64 base = pmu.ranges[i].base, end = base + pmu.ranges[i].size - 1;
        if (last < base || ipa > end)
            continue;
        u64 first = (ipa > base ? ipa : base) & ~(PMU_FRAME_SIZE - 1);
        u64 stop = (last < end ? last : end) & ~(PMU_FRAME_SIZE - 1);
        for (u64 page = first;; page += PMU_FRAME_SIZE) {
            u8 *frame = pmu_frame(page);
            if (hv_xt_get(page, NULL, NULL) != HV_XT_UNTRACKED) {
                if (*frame & PMU_FRAME_SCANNED)
                    n++;
                *frame = 0;
                hv_xt_set(page, HV_XT_SCANNING, 0);
                if (pmu.open && (page & ~(PMU_PAGE - 1)) == pmu.open_block)
                    pmu.open = false;
                changed = true;
            }
            if (page == stop)
                break;
        }
    }
    if (changed)
        hv_xt_tlbi(true);
    return n;
}

u32 hv_win_pmu_dma_write(u64 ipa, u64 size)
{
    u32 n = pmu_invalidate(ipa, size);
    pmu.stats[HV_WIN_PMU_DMA_RESETS] += n;
    return n;
}

void hv_win_pmu_tick(void)
{
    if (pmu.armed && pmu.open) {
        pmu_close();
        pmu.stats[HV_WIN_PMU_SWEEPS]++;
    }
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
    printf("HV/win: PMU 4K: frames scanned %lu modified %lu, words modified %lu; "
           "skipped exec %lu boundary %lu other %lu; modified-never-executed %lu; "
           "exec faults %lu denied %lu, stores denied %lu, reprotects %lu\n",
           s[HV_WIN_PMU_FRAMES_SCANNED], s[HV_WIN_PMU_FRAMES_MODIFIED],
           s[HV_WIN_PMU_WORDS_MODIFIED], s[HV_WIN_PMU_WORDS_SKIP_EXEC],
           s[HV_WIN_PMU_WORDS_SKIP_BOUNDARY], s[HV_WIN_PMU_WORDS_SKIP_OTHER],
           s[HV_WIN_PMU_MODIFIED_NEVER_EXECUTED], s[HV_WIN_PMU_EXEC_FAULTS],
           s[HV_WIN_PMU_EXEC_DENIED], s[HV_WIN_PMU_STORE_DENIED], s[HV_WIN_PMU_REPROTECTS]);
}

#ifdef PMU_REWRITE_HOST_TEST
u8 hv_win_pmu_host_frame(u64 ipa)
{
    u8 *frame = pmu_frame(ipa);
    return frame ? *frame : 0;
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
