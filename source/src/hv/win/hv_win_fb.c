/* SPDX-License-Identifier: MIT */
/* Copyright The Asahi Linux Contributors. See LICENSE.MIT. */
/* BGRA shadow scanout. The original single-row raster sweep was adapted from aurora-silicon/aura
 * 98599af29c644e89ab7f844ce94633cd451fd63a src/hv/windows/hv.c; the dirty-page tracking, pass
 * scheduler, wide conversion core, 2x mode and benchmark below are our own (display/improve-20261109).
 * Opt-in, single-vCPU. No DCP programming. Allocation is permanent until reboot: host mappings
 * change attributes and must never return to malloc.
 *
 * Modes (PREPARE arg1):
 *   bit0 NO_COPY  map/advertise the shadow, never convert (diagnostic, unchanged)
 *   bit1 DIRTY    stage-2 write-protect the shadow; a guest store to a clean 16 KiB page takes one
 *                 permission fault that marks the page dirty and re-enables writes. hv_tick only
 *                 converts dirty pages. Without DIRTY every page is converted continuously (sweep).
 *   bit2 SCALE2   the guest sees a half-resolution BGRA mode (width/2 x height/2); each guest pixel
 *                 becomes a 2x2 native block. Quarter the shadow reads, quarter the dirty pages.
 *   bit3 BENCH    at PREPARE, time legacy vs wide conversion of 8 rows (into native rows 0..7)
 *                 and NC read-only / write-only streams; results in stats 22..25.
 * PREPARE arg2: bits 15:0 base per-tick budget in us (0: 250), bits 31:16 escalated budget in us
 *   (0: 1000, max 4000), bits 39:32 defer limit in ms (0: 8).
 *
 * Pass scheduler (tearing): a pass snapshots the dirty set, write-protects it again (so later
 * stores re-mark it), and converts it top-to-bottom. A pass starts only when the guest took no
 * new fault during the last tick (its blit finished) or the oldest dirty page has waited the defer
 * limit; a pass that has run longer than 16 ms switches to the escalated budget.
 */
#include "hv_win.h"
#include "../hv.h"
#include "heapblock.h"
#include "memory.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#include "hv_win_fb_conv.h"

#define FB_PAGE        0x4000UL
#define FB_PAGE_SHIFT  14
#define FB_MAX_BYTES   (64UL << 20)
#define FB_MAX_PAGES   (FB_MAX_BYTES / FB_PAGE)
#define FB_WORDS       (FB_MAX_PAGES / 64)
#define FB_F_NO_COPY   BIT(0)
#define FB_F_DIRTY     BIT(1)
#define FB_F_SCALE2    BIT(2)
#define FB_F_BENCH     BIT(3)
#define FB_F_ALL       (FB_F_NO_COPY | FB_F_DIRTY | FB_F_SCALE2 | FB_F_BENCH)
#define FB_SMALL_PASS  32 /* pages: per-IPA TLBI at or below, VMID-wide above */

/* TLB maintenance by register operand; the host harness overrides this to record calls. */
#ifndef FB_TLBI
#define FB_TLBI(op, v) __asm__ volatile("tlbi " op ", %0" : : "r"(v))
#endif

#define FB_ESR_FSC_MASK 0x3cUL
#define FB_ESR_FSC_PERM 0x0cUL
#define FB_ESR_WNR      BIT(6)
#define FB_ESR_S1PTW    BIT(7)

extern u64 hv_cpus_in_guest;
extern bool hv_started_cpus[MAX_CPUS];

enum {
    ST_STATE, ST_SHADOW, ST_NATIVE, ST_SIZE, ST_STRIDE, ST_HEIGHT, ST_CALLS, ST_LINES,
    ST_MAX_TICKS, ST_CURSOR, ST_HEAP_BEFORE, ST_HEAP_AFTER, ST_HEAP_NOW, ST_NO_COPY,
    /* new in improve-20261109; 0..13 keep their meaning */
    ST_FLAGS, ST_FAULTS, ST_PASSES, ST_PAGES, ST_MAX_LAT, ST_SUM_LAT, ST_MAX_BACKLOG,
    ST_ESCALATED, ST_BENCH_LEGACY, ST_BENCH_WIDE, ST_BENCH_READ, ST_BENCH_WRITE,
    ST_GUEST_WIDTH, ST_GUEST_HEIGHT, ST_GUEST_STRIDE, ST_IPA_FAIL, ST_BUSY, ST_START,
    ST_SPURIOUS, ST_DEFERRED, ST_BUDGET, ST_NR
};

static struct {
    u64 state; /* 0 disabled, 1 prepared, 2 active, 3 failed/retained */
    u64 shadow, native, size, stride, width, height, depth, cursor;
    u64 gstride, gwidth, gheight, pages, flags;
    u64 calls, lines, max_ticks, budget_ticks, budget_max_ticks, defer_ticks, escalate_ticks;
    u64 remaining_before, remaining_after;
    u64 faults, passes, pages_done, max_lat, sum_lat, max_backlog, escalated, ipa_fail, busy;
    u64 start, spurious, deferred;
    u64 bench[4];
    /* dirty: written by the fault path; pass: snapshot being converted by the tick */
    u64 dirty[FB_WORDS], pass[FB_WORDS];
    u64 ndirty, npass, first_dirty, pass_t0;
    bool fault_this_tick, pass_active;
    bool attempted;
} win_fb;

static bool fb_idle_single_cpu(void)
{
    if (!hv_win_vgic_enabled() || hv_win.nr_vcpus != 1 || hv_cpus_in_guest)
        return false;
    for (unsigned int i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return false;
    return true;
}

/* ---- conversion of one guest shadow page (byte range) into the native surface ------------- */
static void fb_convert_page(u64 page)
{
    u64 off = page << FB_PAGE_SHIFT, end = off + FB_PAGE;
    u64 rowbytes = win_fb.gwidth * 4, limit = win_fb.gstride * win_fb.gheight;
    if (end > limit)
        end = limit;
    while (off < end) {
        u64 y = off / win_fb.gstride, x0 = off % win_fb.gstride;
        u64 row_end = (y + 1) * win_fb.gstride;
        u64 x1 = row_end <= end ? win_fb.gstride : end - y * win_fb.gstride;
        if (x1 > rowbytes)
            x1 = rowbytes; /* never touch guest/native row padding */
        if (x0 < x1) {
            const u32 *src = (const u32 *)(win_fb.shadow + y * win_fb.gstride + x0);
            u64 n = (x1 - x0) / 4;
            if (win_fb.flags & FB_F_SCALE2) {
                u32 *d0 = (u32 *)(win_fb.native + 2 * y * win_fb.stride + 2 * x0);
                fb_conv_span_2x(d0, (u32 *)((u64)d0 + win_fb.stride), src, n);
            } else {
                fb_conv_span((u32 *)(win_fb.native + y * win_fb.stride + x0), src, n);
            }
            win_fb.lines += n; /* pixels; ST_LINES reports row-equivalents */
        }
        off = row_end;
    }
}

/* ---- benchmark (host-only, before guest entry) --------------------------------------------- */
static void fb_bench(void)
{
    u64 rows = 8, w = win_fb.width, st = win_fb.stride;
    u64 gs = win_fb.gstride, gw = win_fb.gwidth;
    if (gw > w)
        gw = w;
    u64 t = get_ticks();
    for (u64 y = 0; y < rows; y++) { /* legacy: one volatile 32-bit NC load per pixel */
        const volatile u32 *s = (const volatile u32 *)(win_fb.shadow + y * gs);
        volatile u32 *d = (volatile u32 *)(win_fb.native + y * st);
        for (u64 x = 0; x < gw; x++)
            d[x] = fb_px1(s[x]);
    }
    sysop("dsb st");
    win_fb.bench[0] = get_ticks() - t;
    t = get_ticks();
    for (u64 y = 0; y < rows; y++)
        fb_conv_span((u32 *)(win_fb.native + y * st), (const u32 *)(win_fb.shadow + y * gs), gw);
    sysop("dsb st");
    win_fb.bench[1] = get_ticks() - t;
    t = get_ticks();
    u64 acc = 0;
    for (u64 y = 0; y < rows; y++) { /* NC read stream only */
        const u64 *s = (const u64 *)(win_fb.shadow + y * gs);
        for (u64 i = 0; i + 4 <= gw / 2; i += 4)
            acc += s[i] ^ s[i + 1] ^ s[i + 2] ^ s[i + 3];
    }
    __asm__ volatile("" : : "r"(acc));
    win_fb.bench[2] = get_ticks() - t;
    t = get_ticks();
    for (u64 y = 0; y < rows; y++) { /* NC write stream only (black) */
        u64 *d = (u64 *)(win_fb.native + y * st);
        for (u64 i = 0; i < gw / 2; i++)
            d[i] = 0;
    }
    sysop("dsb st");
    win_fb.bench[3] = get_ticks() - t;
}

/* Host-only proxy operation, before final guest memory planning. */
s64 hv_win_fb_prepare(u64 expected_native, u64 flags, u64 tuning)
{
    if ((flags & ~FB_F_ALL) || win_fb.attempted || !fb_idle_single_cpu())
        return -1;
    win_fb.attempted = true;
    win_fb.state = 3;
    win_fb.flags = flags;
    const struct boot_args *ba = &cur_boot_args;
    u64 base = ba->video.base, stride = ba->video.stride;
    u64 width = ba->video.width, height = ba->video.height;
    if (expected_native != base || !base || (base & (FB_PAGE - 1)) ||
        (ba->video.depth & 0xff) != 30 || !width || width > 16384 || !height ||
        height > 16384 || stride < width * 4 || stride > 65536 || (stride & 7))
        return -2;
    u64 gw = width, gh = height, gs = stride;
    if (flags & FB_F_SCALE2) {
        if ((width | height) & 1)
            return -2;
        gw = width / 2;
        gh = height / 2;
        gs = ALIGN_UP(gw * 4, 64);
    }
    u64 size = ALIGN_UP(gs * gh, FB_PAGE);
    u64 nsize = ALIGN_UP(stride * height, FB_PAGE);
    if (!size || size > FB_MAX_BYTES || base > UINT64_MAX - nsize)
        return -2;
    u64 us = mrs(CNTFRQ_EL0) / 1000000;
    u64 b0 = tuning & 0xffff, b1 = (tuning >> 16) & 0xffff, dl = (tuning >> 32) & 0xff;
    b0 = b0 ? b0 : 250;
    b1 = b1 ? b1 : 1000;
    if (!us || (tuning >> 40) || b1 > 4000 || b0 > b1)
        return -4;
    /* Preserve 32 MiB for mapping splits and later PMU page tables. */
    size_t remaining = heapblock_remaining();
    win_fb.remaining_before = remaining;
    if (remaining == SIZE_MAX || remaining < size + FB_PAGE + (32UL << 20))
        return -3;
    u64 shadow = (u64)heapblock_alloc_aligned(size, FB_PAGE);
    win_fb.shadow = shadow;
    win_fb.size = size;
    if (!shadow || shadow > UINT64_MAX - size || (shadow < base + nsize && base < shadow + size))
        return -3;
    dc_civac_range((void *)shadow, size);
    sysop("dsb sy");
    const u64 aliases[] = {0, REGION_RWX_EL0, REGION_RW_EL0, REGION_RX_EL1};
    for (unsigned int i = 0; i < 4; i++)
        mmu_rm_mapping(shadow | aliases[i], size);
    sysop("dsb ishst");
    sysop("tlbi vmalle1is");
    sysop("dsb ish");
    sysop("isb");
    for (unsigned int i = 0; i < 4; i++)
        mmu_add_mapping(shadow | aliases[i], shadow, size, MAIR_IDX_NORMAL_NC,
                        i ? PERM_RW_EL0 : PERM_RW);
    memset((void *)shadow, 0, size);
    sysop("dsb sy");
    win_fb.native = base;
    win_fb.stride = stride;
    win_fb.width = width;
    win_fb.height = height;
    win_fb.depth = ba->video.depth;
    win_fb.gwidth = gw;
    win_fb.gheight = gh;
    win_fb.gstride = gs;
    win_fb.pages = size >> FB_PAGE_SHIFT;
    win_fb.budget_ticks = b0 * us;
    win_fb.budget_max_ticks = b1 * us;
    win_fb.defer_ticks = (dl ? dl : 8) * 1000 * us;
    win_fb.escalate_ticks = 16000 * us;
    win_fb.remaining_after = heapblock_remaining();
    if (flags & FB_F_BENCH)
        fb_bench();
    win_fb.state = 1;
    return shadow;
}

/* Called once, after launcher pt_update and before any guest instruction. */
bool hv_win_fb_start(const struct boot_args *gba)
{
    if (!win_fb.attempted)
        return true;
    if (win_fb.state != 1 || !fb_idle_single_cpu() || !gba ||
        gba->video.base != win_fb.shadow || gba->video.stride != win_fb.gstride ||
        gba->video.width != win_fb.gwidth || gba->video.height != win_fb.gheight ||
        gba->video.depth != ((win_fb.depth & ~0xffUL) | 32)) {
        win_fb.state = 3;
        return false;
    }
    if (hv_map_hw_nc(win_fb.shadow, win_fb.shadow, win_fb.size) < 0) {
        win_fb.state = 3;
        return false;
    }
    /* In DIRTY mode split the shadow's stage-2 leaves to 16 KiB now, while no guest CPU runs, so
     * the fault path never allocates, and start every page write-protected. */
    if ((win_fb.flags & FB_F_DIRTY) && hv_wp_prepare(win_fb.shadow, win_fb.size) != (int)win_fb.pages) {
        win_fb.state = 3;
        return false;
    }
    /* Everything starts due: the whole frame is converted once. */
    for (u64 p = 0; p < win_fb.pages; p++)
        win_fb.pass[p / 64] |= BIT(p % 64);
    win_fb.npass = win_fb.pages;
    win_fb.pass_active = true;
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    win_fb.start = win_fb.pass_t0 = get_ticks();
    win_fb.state = 2;
    return true;
}

/*
 * Stage-2 permission fault from the guest. True: the access is re-executed. Only stores to a
 * write-protected shadow page are ours; everything else falls through to the PMU / emulation.
 */
bool hv_win_fb_handle_dabort(struct exc_info *ctx, u64 esr)
{
    if (win_fb.state != 2 || !(win_fb.flags & FB_F_DIRTY) ||
        (esr & FB_ESR_FSC_MASK) != FB_ESR_FSC_PERM || !(esr & FB_ESR_WNR) || (esr & FB_ESR_S1PTW))
        return false;
    u64 ipa;
    if (!hv_xt_fault_ipa(ctx, false, true, &ipa)) {
        /* AT failed (guest changed its tables); HPFAR_EL2 may still name the page */
        win_fb.ipa_fail++;
        if (!hv_xt_fault_ipa(ctx, true, true, &ipa))
            return false;
    }
    if (ipa < win_fb.shadow || ipa >= win_fb.shadow + win_fb.size)
        return false;
    u64 page = (ipa - win_fb.shadow) >> FB_PAGE_SHIFT;
    u64 bit = BIT(page % 64);
    if (win_fb.dirty[page / 64] & bit) {
        /* already writable: a stale TLB entry. Fall back to a VMID-wide local flush. */
        win_fb.spurious++;
        hv_xt_tlbi(false);
        return true;
    }
    hv_wp_set(win_fb.shadow + (page << FB_PAGE_SHIFT), true);
    /* Adding a permission: local invalidation of this IPA and of the faulting VA suffices. */
    sysop("dsb nshst");
    FB_TLBI("ipas2e1", (ipa >> 12) & GENMASK(35, 0));
    sysop("dsb nsh");
    FB_TLBI("vaae1", (ctx->far >> 12) & GENMASK(43, 0));
    sysop("dsb nsh");
    sysop("isb");
    win_fb.dirty[page / 64] |= bit;
    if (!win_fb.ndirty++)
        win_fb.first_dirty = get_ticks();
    if (win_fb.ndirty > win_fb.max_backlog)
        win_fb.max_backlog = win_fb.ndirty;
    win_fb.faults++;
    win_fb.fault_this_tick = true;
    return true;
}

/* Move the dirty set into the pass set; in DIRTY mode protect those pages again first. */
static void fb_begin_pass(void)
{
    bool wp = win_fb.flags & FB_F_DIRTY;
    bool small = win_fb.ndirty <= FB_SMALL_PASS;
    for (u64 w = 0; w < (win_fb.pages + 63) / 64; w++) {
        u64 m = win_fb.dirty[w];
        if (!m)
            continue;
        win_fb.pass[w] |= m;
        win_fb.dirty[w] = 0;
        while (wp && m) {
            u64 a = win_fb.shadow + ((w * 64 + __builtin_ctzll(m)) << FB_PAGE_SHIFT);
            m &= m - 1;
            hv_wp_set(a, false);
            if (small)
                FB_TLBI("ipas2e1is", (a >> 12) & GENMASK(35, 0));
        }
    }
    if (wp) {
        /* Removing write permission must be complete before any shadow byte is read, so a store
         * racing the snapshot either landed already or faults and re-marks the page. */
        sysop("dsb ish");
        if (small)
            sysop("tlbi vmalle1is");
        else
            sysop("tlbi vmalls12e1is");
        sysop("dsb ish");
        sysop("isb");
    }
    sysop("dsb sy"); /* the guest's own NC stores have completed before we read */
    win_fb.npass += win_fb.ndirty;
    win_fb.ndirty = 0;
    win_fb.pass_t0 = win_fb.first_dirty;
    win_fb.pass_active = true;
    win_fb.cursor = 0;
}

/* At the tail of hv_tick (before the storage tick). Single-vCPU only. */
void hv_win_fb_tick(void)
{
    if (win_fb.state != 2 || (win_fb.flags & FB_F_NO_COPY))
        return;
    bool fault = win_fb.fault_this_tick;
    win_fb.fault_this_tick = false;
    if (!win_fb.pass_active && !win_fb.ndirty && (win_fb.flags & FB_F_DIRTY))
        return; /* idle: no clock read, no barrier */
    u64 start = get_ticks();

    if (!win_fb.pass_active) {
        if (!(win_fb.flags & FB_F_DIRTY)) {
            /* sweep mode: the whole frame is always due */
            for (u64 p = 0; p < win_fb.pages; p++)
                win_fb.dirty[p / 64] |= BIT(p % 64);
            win_fb.ndirty = win_fb.pages;
            win_fb.first_dirty = start;
        } else if (fault && start - win_fb.first_dirty < win_fb.defer_ticks) {
            win_fb.deferred++; /* guest still drawing: wait for its blit to finish */
            return;
        }
        fb_begin_pass();
    }

    u64 budget = win_fb.budget_ticks;
    if (start - win_fb.pass_t0 > win_fb.escalate_ticks && win_fb.budget_max_ticks > budget) {
        budget = win_fb.budget_max_ticks;
        win_fb.escalated++;
    }
    u64 words = (win_fb.pages + 63) / 64;
    for (u64 w = win_fb.cursor; w < words;) {
        u64 m = win_fb.pass[w];
        if (!m) {
            w++;
            continue;
        }
        u64 bit = m & -m;
        fb_convert_page(w * 64 + __builtin_ctzll(m));
        win_fb.pass[w] &= ~bit;
        win_fb.npass--;
        win_fb.pages_done++;
        win_fb.cursor = w;
        if (get_ticks() - start >= budget)
            break;
    }
    sysop("dsb st");
    u64 end = get_ticks();
    if (!win_fb.npass) {
        win_fb.pass_active = false;
        u64 lat = end - win_fb.pass_t0;
        if (lat > win_fb.max_lat)
            win_fb.max_lat = lat;
        win_fb.sum_lat += lat;
        win_fb.passes++;
        win_fb.cursor = 0;
    }
    u64 elapsed = end - start;
    if (elapsed > win_fb.max_ticks)
        win_fb.max_ticks = elapsed;
    win_fb.busy += elapsed;
    win_fb.calls++;
}

u64 hv_win_fb_stat(u64 idx)
{
    if (idx >= ST_NR)
        return UINT64_MAX;
    u64 v[ST_NR] = {
        [ST_STATE] = win_fb.state, [ST_SHADOW] = win_fb.shadow, [ST_NATIVE] = win_fb.native,
        [ST_SIZE] = win_fb.size, [ST_STRIDE] = win_fb.stride, [ST_HEIGHT] = win_fb.height,
        [ST_CALLS] = win_fb.calls,
        [ST_LINES] = win_fb.gwidth ? win_fb.lines / win_fb.gwidth : 0,
        [ST_MAX_TICKS] = win_fb.max_ticks, [ST_CURSOR] = win_fb.cursor,
        [ST_HEAP_BEFORE] = win_fb.remaining_before, [ST_HEAP_AFTER] = win_fb.remaining_after,
        [ST_HEAP_NOW] = heapblock_remaining(), [ST_NO_COPY] = win_fb.flags & FB_F_NO_COPY,
        [ST_FLAGS] = win_fb.flags, [ST_FAULTS] = win_fb.faults, [ST_PASSES] = win_fb.passes,
        [ST_PAGES] = win_fb.pages_done, [ST_MAX_LAT] = win_fb.max_lat,
        [ST_SUM_LAT] = win_fb.sum_lat, [ST_MAX_BACKLOG] = win_fb.max_backlog,
        [ST_ESCALATED] = win_fb.escalated, [ST_BENCH_LEGACY] = win_fb.bench[0],
        [ST_BENCH_WIDE] = win_fb.bench[1], [ST_BENCH_READ] = win_fb.bench[2],
        [ST_BENCH_WRITE] = win_fb.bench[3], [ST_GUEST_WIDTH] = win_fb.gwidth,
        [ST_GUEST_HEIGHT] = win_fb.gheight, [ST_GUEST_STRIDE] = win_fb.gstride,
        [ST_IPA_FAIL] = win_fb.ipa_fail, [ST_BUSY] = win_fb.busy, [ST_START] = win_fb.start,
        [ST_SPURIOUS] = win_fb.spurious, [ST_DEFERRED] = win_fb.deferred,
        [ST_BUDGET] = win_fb.budget_ticks | (win_fb.budget_max_ticks << 32),
    };
    return v[idx];
}
