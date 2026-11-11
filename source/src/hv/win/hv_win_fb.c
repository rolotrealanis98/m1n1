/* SPDX-License-Identifier: MIT */
/* Copyright The Asahi Linux Contributors. See LICENSE.MIT. */
/* BGRA shadow scanout. The original single-row raster sweep was adapted from aurora-silicon/aura
 * 98599af29c644e89ab7f844ce94633cd451fd63a src/hv/windows/hv.c; the dirty-page tracking, pass
 * scheduler, wide conversion core, 2x mode and benchmark below are our own (display/improve-20261109,
 * second round disp2-20261110).
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
 *   bit3 BENCH    at PREPARE, time legacy vs wide conversion of 8 rows (into native rows 0..15)
 *                 and NC read / write streams; results in stats 22..25 and 70..73.
 * PREPARE arg2 (tuning):
 *   bits 15:0  base per-tick budget in us (0: 500; r3.2-disp: 250)
 *   bits 31:16 escalated budget in us (0: 1000, max 4000)
 *   bits 39:32 defer limit in ms (0: 8); a pass older than the defer limit escalates
 *              (r3.2-disp: twice the defer limit)
 *   bits 41:40 TLB maintenance after a fault makes a page writable:
 *              0 the four 4 KiB stage-1 pages of the faulting 16 KiB window (default),
 *              1 the whole stage-1 TLB of the VMID on this CPU (tlbi vmalle1),
 *              2 the faulting 4 KiB page only (r3.2-disp behaviour)
 *   bit 42     no write elision (no cacheable copy; convert every chunk of a dirty page)
 *   bit 43     merge-ahead (off by default): pages dirtied during a pass below the conversion
 *              cursor join the running pass instead of waiting for the next one
 *   bits 47:44 nightly-6-perf: background refresh in DIRTY mode, one full round every N x 500 ms
 *              (0: off, the old behaviour). A refresh round walks every page round-robin, at most
 *              FB_REFRESH_US of converter time per tick, independent of the dirty passes (which keep
 *              their own budget and priority). Diff conversion: only chunks whose source differs
 *              from the last converted copy are written; every 4th round is forced (every chunk
 *              rewritten). The scanout therefore matches the shadow within one period plus one
 *              round, whatever the dirty tracking missed. Chunks a non-forced round had to write on
 *              a page that was neither dirty nor in a pass are counted (ST_REFRESH_FIXES): non-zero
 *              means guest stores reached the shadow without a stage-2 fault.
 *   bits 55:48 stats print period in seconds (0: off): one FBS/FBS2 console line pair from the
 *              tick, read by the host from the log without pausing the guest
 *   all other bits must be zero.
 *
 * Pass scheduler (tearing): a pass snapshots the dirty set, write-protects it again (so later
 * stores re-mark it), and converts it top-to-bottom. A pass starts only when the guest took no
 * new fault during the last tick (its blit finished) or the oldest dirty page has waited the defer
 * limit. With merge-ahead (opt-in) pages dirtied during a pass below the conversion cursor are
 * protected and merged into the running pass. Write elision keeps a cacheable copy of the
 * converted source and writes only changed 64-byte chunks to the scanout.
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
#define FB_VERSION     3  /* stat 35: disp3 counter layout (disp2 + ST_PRINT) */

#define FB_T_TLBI_SHIFT  40
#define FB_T_NO_DIFF     BIT(42)
#define FB_T_MERGE       BIT(43)
#define FB_T_PERIOD_SHIFT 48
#define FB_T_REFRESH_SHIFT 44 /* nightly-6-perf: bits 47:44, refresh round every N x 500 ms */
#define FB_T_RESERVED    GENMASK(63, 56)
#define FB_REFRESH_FORCE_EVERY 4  /* every 4th refresh round rewrites every chunk */
#define FB_REFRESH_US          60 /* converter time per tick for the refresh walk */
enum { FB_TLBI_SUB = 0, FB_TLBI_ALL = 1, FB_TLBI_FAR = 2 };

#define FB_RT_SAMPLE   128 /* one runtime read/convert split sample per this many pages */
#define FB_HIST        12  /* pass latency buckets: <1, <2, <4, ... <1024, >=1024 ms */

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
    ST_SPURIOUS, ST_DEFERRED, ST_BUDGET,
    /* new in disp2-20261110; 0..34 keep their meaning. (R) = reset by reading idx | 0x100 */
    ST_VERSION, ST_TUNING, ST_LAT_NET_MAX, ST_LAT_GROSS_MAX, /* (R) (R) */
    ST_HIST0, ST_HIST_END = ST_HIST0 + FB_HIST - 1,
    ST_GAP_MAX, ST_GAP_AT, ST_GAP_FAULTS, /* (R): longest interval with no FB tick */
    ST_GAPS_5MS, ST_GAPS_50MS, ST_STOPPED,
    ST_FAULT_TICKS, ST_FAULT_MAX, /* (R) */
    ST_SP_SAME, ST_SP_SUB, ST_SP_ALIAS, ST_SP_REPEAT, ST_WP_FAIL,
    ST_CHUNKS, ST_CHUNKS_WR, ST_RT_SAMPLES, ST_RT_READ, ST_RT_CONV, ST_MERGED,
    ST_BENCH_2X, ST_BENCH_RD32, ST_BENCH_RD64, ST_BENCH_DIFF, ST_DIFF_ON,
    ST_BACKLOG_MAX, ST_TICK_MAX, /* (R) */
    /* new in disp3; 0..76 keep their meaning */
    ST_PRINT, /* ticks spent printing FBS/FBS2 lines (also in ST_BUSY) */
    /* new in nightly-6-perf; 0..77 keep their meaning */
    ST_REFRESHES,      /* refresh rounds completed (tuning bits 47:44) */
    ST_REFRESH_FIXES,  /* chunks a non-forced round wrote on clean pages: missed updates */
    ST_REFRESH_PAGES,  /* pages converted by the refresh walk */
    ST_WP_REFAIL,      /* re-protection failures at a pass start (page left writable; refresh covers it) */
    ST_REFRESH_FIXPAGES, /* clean pages with at least one fixed chunk */
    ST_NR
};
_Static_assert(ST_PRINT == 77 && ST_REFRESHES == 78 && ST_REFRESH_FIXPAGES == 82,
               "stat indices are an interface (hv_exc.c PF line, launcher)");
#define FB_STAT_RESET 0x100UL

/* maxima kept twice: [0] reset when the host reads them with FB_STAT_RESET, [1] per print period */
struct fb_max {
    u64 lat_net, lat_gross, gap, gap_at, gap_faults, fault, backlog, tick;
};

static struct {
    u64 state; /* 0 disabled, 1 prepared, 2 active, 3 failed/retained */
    u64 shadow, native, size, stride, width, height, depth, cursor;
    u64 gstride, gwidth, gheight, pages, flags, tuning, copy;
    u64 calls, lines, max_ticks, budget_ticks, budget_max_ticks, defer_ticks, escalate_ticks;
    u64 ms_ticks, print_ticks, tlbi_mode;
    u64 remaining_before, remaining_after;
    u64 faults, passes, pages_done, max_lat, sum_lat, max_backlog, escalated, ipa_fail, busy;
    u64 start, spurious, deferred;
    u64 bench[8];
    /* disp2 counters */
    u64 hist[FB_HIST];
    u64 gaps5, gaps50, stopped, fault_ticks;
    u64 sp_same, sp_sub, sp_alias, sp_repeat, wp_fail;
    u64 chunks, chunks_wr, rt_samples, rt_read, rt_conv, merged;
    struct fb_max mx[2];
    u64 last_end, fault_mark, last_print, print_sum;
    /* nightly-6-perf background refresh (tuning bits 47:44) */
    u64 refresh_ticks, refresh_slice, last_refresh, refresh_next, refreshes, refresh_fixes;
    u64 refresh_pages, refresh_fixpages, wp_refail;
    bool refresh_active, refresh_force;
    /* dirty: written by the fault path; pass: snapshot being converted by the tick;
     * force: converted without comparing (initial frame); spur: page took a spurious fault */
    u64 dirty[FB_WORDS], pass[FB_WORDS], force[FB_WORDS], spur[FB_WORDS];
    u32 fva[FB_MAX_PAGES];  /* 4 KiB VA page (VA >> 12, low 32 bits) of the fault that dirtied it */
    u32 fwin[FB_MAX_PAGES]; /* VA >> 12 of the 16 KiB window holding that fault */
    u64 ndirty, npass, first_dirty, first_dirty_stop, pass_t0, pass_stop0, next_page;
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

static u64 fb_popcount(u64 m)
{
    u64 n = 0;
    for (; m; m &= m - 1)
        n++;
    return n;
}

static void fb_max_update(u64 *slot0, u64 v)
{
    /* slot0 points into mx[0]; the same field in mx[1] is at a fixed offset */
    u64 *slot1 = (u64 *)((u8 *)slot0 + sizeof(struct fb_max));
    if (v > *slot0)
        *slot0 = v;
    if (v > *slot1)
        *slot1 = v;
}

/* ---- conversion of one guest shadow page (byte range) into the native surface ------------- */
static void fb_convert_page(u64 page, int force)
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
                u32 *d1 = (u32 *)((u64)d0 + win_fb.stride);
                if (win_fb.copy)
                    win_fb.chunks_wr += fb_conv_span_2x_diff(
                        d0, d1, src, (u32 *)(win_fb.copy + y * win_fb.gstride + x0), n, force,
                        &win_fb.chunks);
                else
                    fb_conv_span_2x(d0, d1, src, n);
            } else {
                u32 *d = (u32 *)(win_fb.native + y * win_fb.stride + x0);
                if (win_fb.copy)
                    win_fb.chunks_wr += fb_conv_span_diff(
                        d, src, (u32 *)(win_fb.copy + y * win_fb.gstride + x0), n, force,
                        &win_fb.chunks);
                else
                    fb_conv_span(d, src, n);
            }
            win_fb.lines += n; /* pixels; ST_LINES reports row-equivalents */
        }
        off = row_end;
    }
}

/* NC read-only stream over one shadow page, 8 independent loads per 64 bytes (rt sample/bench) */
static u64 fb_read_stream(const u64 *s, u64 words)
{
    u64 acc = 0;
    for (u64 i = 0; i + 8 <= words; i += 8, s += 8) {
        FB_LOAD8(s, v);
        acc ^= v0 ^ v1 ^ v2 ^ v3 ^ v4 ^ v5 ^ v6 ^ v7;
    }
    FB_COST(words * 8, 0);
    return acc;
}

/* Convert one page; every FB_RT_SAMPLE pages first time an NC read-only pass over it, so the
 * hardware run can split runtime cost into shadow reads vs (writes + compute). */
static void fb_convert_page_sampled(u64 page, int force)
{
    if (win_fb.pages_done % FB_RT_SAMPLE) {
        fb_convert_page(page, force);
        return;
    }
    u64 off = page << FB_PAGE_SHIFT, limit = win_fb.gstride * win_fb.gheight;
    u64 len = off + FB_PAGE <= limit ? FB_PAGE : limit - off;
    u64 t0 = get_ticks();
    u64 acc = fb_read_stream((const u64 *)(win_fb.shadow + off), len / 8);
    __asm__ volatile("" : : "r"(acc));
    u64 t1 = get_ticks();
    fb_convert_page(page, force);
    sysop("dsb st");
    u64 t2 = get_ticks();
    win_fb.rt_samples++;
    win_fb.rt_read += t1 - t0;
    win_fb.rt_conv += t2 - t1;
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
    /* disp2: the 2x path as it runs (8 guest rows -> native rows 0..15) */
    if (win_fb.flags & FB_F_SCALE2) {
        t = get_ticks();
        for (u64 y = 0; y < rows; y++) {
            u32 *d0 = (u32 *)(win_fb.native + 2 * y * st);
            fb_conv_span_2x(d0, (u32 *)((u64)d0 + st), (const u32 *)(win_fb.shadow + y * gs), gw);
        }
        sysop("dsb st");
        win_fb.bench[4] = get_ticks() - t;
    }
    t = get_ticks(); /* NC read, one volatile 32-bit load per pixel (legacy read side alone) */
    u32 acc32 = 0;
    for (u64 y = 0; y < rows; y++) {
        const volatile u32 *s = (const volatile u32 *)(win_fb.shadow + y * gs);
        for (u64 x = 0; x < gw; x++)
            acc32 ^= s[x];
    }
    __asm__ volatile("" : : "r"(acc32));
    win_fb.bench[5] = get_ticks() - t;
    t = get_ticks(); /* NC read, 8 independent 64-bit loads per 64 bytes (the diff path's loads) */
    acc = 0;
    for (u64 y = 0; y < rows; y++)
        acc ^= fb_read_stream((const u64 *)(win_fb.shadow + y * gs), gw / 2);
    __asm__ volatile("" : : "r"(acc));
    win_fb.bench[6] = get_ticks() - t;
    if (win_fb.copy) { /* diff path on unchanged rows: NC reads + cached compare, no writes */
        u64 ex = 0;
        t = get_ticks();
        for (u64 y = 0; y < rows; y++)
            fb_conv_span_diff((u32 *)(win_fb.native + y * st), (const u32 *)(win_fb.shadow + y * gs),
                              (u32 *)(win_fb.copy + y * gs), gw, 0, &ex);
        sysop("dsb st");
        win_fb.bench[7] = get_ticks() - t;
    }
}

/* EL2's own (EL2&0) translations. With HCR_EL2.{E2H,TGE} = {1,0}, as configured by the time the
 * launcher calls PREPARE, "tlbi vmalle1is" (what mmu_add_mapping issues) targets the guest's
 * EL1&0 regime and leaves stale EL2 entries for the shadow's old cacheable mapping. */
static void fb_tlbi_el2(void)
{
    sysop("dsb ishst");
    sysop("tlbi alle2is");
    sysop("dsb ish");
    sysop("isb");
}

/* Host-only proxy operation, before final guest memory planning. */
s64 hv_win_fb_prepare(u64 expected_native, u64 flags, u64 tuning)
{
    if ((flags & ~FB_F_ALL) || win_fb.attempted || !fb_idle_single_cpu())
        return -1;
    win_fb.attempted = true;
    win_fb.state = 3;
    win_fb.flags = flags;
    win_fb.tuning = tuning;
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
    u64 tl = (tuning >> FB_T_TLBI_SHIFT) & 3, per = (tuning >> FB_T_PERIOD_SHIFT) & 0xff;
    u64 rf = (tuning >> FB_T_REFRESH_SHIFT) & 0xf;
    b0 = b0 ? b0 : 500;
    b1 = b1 ? b1 : 1000;
    if (!us || (tuning & FB_T_RESERVED) || tl > FB_TLBI_FAR || b1 > 4000 || b0 > b1)
        return -4;
    if (rf && !(flags & FB_F_DIRTY)) /* nightly-6-perf: the refresh exists for dirty tracking only */
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
    fb_tlbi_el2();
    for (unsigned int i = 0; i < 4; i++)
        mmu_add_mapping(shadow | aliases[i], shadow, size, MAIR_IDX_NORMAL_NC,
                        i ? PERM_RW_EL0 : PERM_RW);
    fb_tlbi_el2();
    /* Lines a stale cacheable translation may have pulled in since the first clean: maintenance
     * by VA acts on the physical lines whatever the attributes of the (now NC) alias. */
    dc_civac_range((void *)shadow, size);
    sysop("dsb sy");
    memset((void *)shadow, 0, size);
    sysop("dsb sy");
    /* Write elision: a cacheable copy of the converted source, only with a further 32 MiB spare. */
    if (!(flags & FB_F_NO_COPY) && !(tuning & FB_T_NO_DIFF) &&
        heapblock_remaining() >= size + FB_PAGE + (64UL << 20)) {
        u64 copy = (u64)heapblock_alloc_aligned(size, FB_PAGE);
        if (copy && !(copy < base + nsize && base < copy + size) &&
            !(copy < shadow + size && shadow < copy + size)) {
            memset((void *)copy, 0, size);
            win_fb.copy = copy;
        }
    }
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
    win_fb.escalate_ticks = win_fb.defer_ticks; /* 8 ms by default (r3.2-disp: 16 ms) */
    win_fb.ms_ticks = 1000 * us;
    win_fb.print_ticks = per * 1000000 * us;
    win_fb.tlbi_mode = tl;
    win_fb.refresh_ticks = (flags & FB_F_DIRTY) ? rf * 500 * 1000 * us : 0;
    win_fb.refresh_slice = FB_REFRESH_US * us;
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
    /* nightly-6c: the shadow and its copy come from the primary heap (PREPARE runs before the launcher adds the
     * n5c extension pool, and checks heapblock_remaining(), which is primary-only). The pool is cut from the top
     * of guest RAM and is never mapped to the guest; refuse to map a shadow that overlaps it. */
    {
        u64 pb = 0, ps = 0, pu = 0;
        heapblock_pool_info(&pb, &ps, &pu);
        if (ps && ((win_fb.shadow < pb + ps && pb < win_fb.shadow + win_fb.size) ||
                   (win_fb.copy && win_fb.copy < pb + ps && pb < win_fb.copy + win_fb.size))) {
            win_fb.state = 3;
            return false;
        }
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
    /* Everything starts due: the whole frame is converted once, without comparing. */
    for (u64 p = 0; p < win_fb.pages; p++) {
        win_fb.pass[p / 64] |= BIT(p % 64);
        win_fb.force[p / 64] |= BIT(p % 64);
    }
    win_fb.npass = win_fb.pages;
    win_fb.pass_active = true;
    win_fb.next_page = 0;
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    win_fb.start = win_fb.pass_t0 = win_fb.last_print = win_fb.last_refresh = get_ticks();
    win_fb.pass_stop0 = 0;
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
    u64 t0 = get_ticks();
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
    /* the guest VA of the 16 KiB window holding the fault (VA and IPA agree in bits 11:0) */
    u64 win = (ctx->far - (ipa & (FB_PAGE - 1))) & ~0xfffUL;
    u32 va4k = (u32)(ctx->far >> 12), win4k = (u32)(win >> 12);
    if (win_fb.dirty[page / 64] & bit) {
        /* Already writable: a stale read-only TLB entry. Classify it, then fall back to a
         * VMID-wide local flush of both stages. */
        win_fb.spurious++;
        if (win_fb.spur[page / 64] & bit)
            win_fb.sp_repeat++; /* a second one after the full flush: not a TLB matter */
        else if (va4k == win_fb.fva[page])
            win_fb.sp_same++; /* same 4 KiB VA: the targeted TLBI did not take */
        else if (win4k == win_fb.fwin[page])
            win_fb.sp_sub++; /* another 4 KiB stage-1 page of the same 16 KiB window */
        else
            win_fb.sp_alias++; /* another VA mapping of the same IPA page */
        win_fb.spur[page / 64] |= bit;
        hv_xt_tlbi(false);
        u64 dt = get_ticks() - t0;
        win_fb.fault_ticks += dt;
        fb_max_update(&win_fb.mx[0].fault, dt);
        return true;
    }
    if (hv_wp_set(win_fb.shadow + (page << FB_PAGE_SHIFT), true) < 0) {
        win_fb.wp_fail++; /* leaf vanished: let the generic path report it */
        return false;
    }
    /* Adding a permission. Stage 2 by IPA first, then every stage-1 entry that may combine with
     * the old read-only leaf: the guest maps 4 KiB pages, so a 16 KiB stage-2 page backs four
     * stage-1 entries, and the core may already hold read-only entries for the neighbours of the
     * faulting one (prefetched or walked by younger stores). Local: single vCPU on this CPU.
     * The IPA operand names the 16 KiB stage-2 page, so its bits 13:12 are zero; the four VAs
     * win + 0/4/8/12 KiB are exactly the stage-1 pages of that IPA page (linear guest mapping). */
    sysop("dsb nshst");
    FB_TLBI("ipas2e1", ((ipa & ~(FB_PAGE - 1)) >> 12) & GENMASK(35, 0));
    sysop("dsb nsh");
    if (win_fb.tlbi_mode == FB_TLBI_ALL) {
        sysop("tlbi vmalle1");
    } else if (win_fb.tlbi_mode == FB_TLBI_FAR) {
        FB_TLBI("vaae1", (ctx->far >> 12) & GENMASK(43, 0));
    } else {
        for (u64 k = 0; k < FB_PAGE; k += 0x1000)
            FB_TLBI("vaae1", ((win + k) >> 12) & GENMASK(43, 0));
    }
    sysop("dsb nsh");
    sysop("isb");
    win_fb.fva[page] = va4k;
    win_fb.fwin[page] = win4k;
    win_fb.dirty[page / 64] |= bit;
    if (!win_fb.ndirty++) {
        win_fb.first_dirty = get_ticks();
        win_fb.first_dirty_stop = win_fb.stopped;
    }
    if (win_fb.ndirty > win_fb.max_backlog)
        win_fb.max_backlog = win_fb.ndirty;
    fb_max_update(&win_fb.mx[0].backlog, win_fb.ndirty + win_fb.npass);
    win_fb.faults++;
    win_fb.fault_this_tick = true;
    u64 dt = get_ticks() - t0;
    win_fb.fault_ticks += dt;
    fb_max_update(&win_fb.mx[0].fault, dt);
    return true;
}

/* Write-protect the pages of mask m in word w again; TLB maintenance completed by fb_wp_done.
 * Small passes invalidate stage 2 per IPA. The read-only leaf store must be visible to the table
 * walker before that TLBI, or a walk between the two can refill a writable stage-2 entry that the
 * later stage-1 flush (vmalle1is) does not remove: the page would stay writable but untracked.
 * Same order as KVM's __kvm_tlb_flush_vmid_ipa and as upstream m1n1's stage-2 changes
 * (dsb ishst; tlbi; dsb ish; isb). Large passes order the stores with the dsb in fb_wp_done. */
static void fb_wp_word(u64 w, u64 m, bool per_ipa)
{
    win_fb.spur[w] &= ~m;
    while (m) {
        u64 a = win_fb.shadow + ((w * 64 + __builtin_ctzll(m)) << FB_PAGE_SHIFT);
        m &= m - 1;
        if (hv_wp_set(a, false) < 0)
            win_fb.wp_refail++; /* left writable and untracked; the periodic refresh still converts it */
        if (per_ipa) {
            sysop("dsb ishst");
            FB_TLBI("ipas2e1is", (a >> 12) & GENMASK(35, 0));
        }
    }
}

static void fb_wp_done(bool small)
{
    /* Removing write permission must be complete before any shadow byte is read, so a store
     * racing the snapshot either landed already or faults and re-marks the page. This dsb ish
     * completes the per-IPA TLBIs (small) or orders the leaf stores before vmalls12e1is (large). */
    sysop("dsb ish");
    if (small)
        sysop("tlbi vmalle1is");
    else
        sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
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
        if (wp)
            fb_wp_word(w, m, small);
    }
    if (wp)
        fb_wp_done(small);
    sysop("dsb sy"); /* the guest's own NC stores have completed before we read */
    win_fb.npass += win_fb.ndirty;
    win_fb.ndirty = 0;
    win_fb.pass_t0 = win_fb.first_dirty;
    win_fb.pass_stop0 = win_fb.first_dirty_stop;
    win_fb.pass_active = true;
    win_fb.cursor = 0;
    win_fb.next_page = 0;
}

/* Pages dirtied during the running pass at or below its cursor (address order = scanout order)
 * join the pass: protected again first, exactly like a pass snapshot. */
static void fb_merge_ahead(void)
{
    u64 words = (win_fb.pages + 63) / 64, w0 = win_fb.next_page / 64, n = 0, nn = 0;
    u64 first = ~(BIT(win_fb.next_page % 64) - 1); /* pages >= next_page within word w0 */
    for (u64 w = w0; w < words; w++)
        n += fb_popcount(win_fb.dirty[w] & (w == w0 ? first : ~0UL));
    if (!n)
        return;
    bool small = n <= FB_SMALL_PASS;
    for (u64 w = w0; w < words; w++) {
        u64 m = win_fb.dirty[w] & (w == w0 ? first : ~0UL);
        if (!m)
            continue;
        nn += fb_popcount(m & ~win_fb.pass[w]); /* a page still queued in the pass counts once */
        win_fb.pass[w] |= m;
        win_fb.dirty[w] &= ~m;
        fb_wp_word(w, m, small);
    }
    fb_wp_done(small);
    sysop("dsb sy");
    win_fb.npass += nn;
    win_fb.ndirty -= n;
    win_fb.merged += n;
    /* first_dirty of what is left stays as it was: conservative for the next pass's latency */
}

static unsigned int fb_lat_bucket(u64 t)
{
    u64 ms = t / win_fb.ms_ticks;
    if (!ms)
        return 0;
    unsigned int b = 64 - __builtin_clzll(ms);
    return b >= FB_HIST ? FB_HIST - 1 : b;
}

/* Interval since the end of the previous FB tick. The tick is the 1 kHz EL2 timer FIQ, which the
 * guest cannot mask, so an interval much longer than the 1 ms re-arm means the only CPU was held
 * at EL2 elsewhere: a proxy round trip (host pause, a sysreg/MMIO trap forwarded to the host) or
 * another long EL2 path. The guest was stopped for that time. */
static void fb_note_gap(u64 now)
{
    if (!win_fb.last_end)
        return;
    u64 gap = now - win_fb.last_end;
    u64 ms = win_fb.ms_ticks;
    for (int i = 0; i < 2; i++) {
        if (gap > win_fb.mx[i].gap) {
            win_fb.mx[i].gap = gap;
            win_fb.mx[i].gap_at = now;
            /* faults inside the gap mean the guest did run: the tick itself was late */
            win_fb.mx[i].gap_faults = win_fb.faults + win_fb.spurious - win_fb.fault_mark;
        }
    }
    if (gap > 5 * ms)
        win_fb.gaps5++;
    if (gap > 50 * ms)
        win_fb.gaps50++;
    if (gap > 2 * ms)
        win_fb.stopped += gap - 2 * ms;
}

static void fb_print(u64 now)
{
    u64 us = win_fb.ms_ticks / 1000;
    struct fb_max *m = &win_fb.mx[1];
    printf("FBS t=%lx calls=%lu faults=%lu spur=%lu/%lu/%lu/%lu/%lu passes=%lu pages=%lu merged=%lu "
           "chunks=%lu/%lu busy_us=%lu fault_us=%lu stop_us=%lu gaps=%lu/%lu rt=%lu/%lu/%lu print_us=%lu\n",
           now, win_fb.calls, win_fb.faults, win_fb.spurious, win_fb.sp_same, win_fb.sp_sub,
           win_fb.sp_alias, win_fb.sp_repeat, win_fb.passes, win_fb.pages_done, win_fb.merged,
           win_fb.chunks_wr, win_fb.chunks, win_fb.busy / us, win_fb.fault_ticks / us,
           win_fb.stopped / us, win_fb.gaps5, win_fb.gaps50, win_fb.rt_samples,
           win_fb.rt_read / us, win_fb.rt_conv / us, win_fb.print_sum / us);
    printf("FBS2 t=%lx iv_lat_us=%lu/%lu iv_gap_us=%lu@%lx gapf=%lu iv_fault_us=%lu iv_tick_us=%lu "
           "iv_backlog=%lu hist=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
           now, m->lat_net / us, m->lat_gross / us, m->gap / us, m->gap_at, m->gap_faults,
           m->fault / us, m->tick / us, m->backlog, win_fb.hist[0], win_fb.hist[1],
           win_fb.hist[2], win_fb.hist[3], win_fb.hist[4], win_fb.hist[5], win_fb.hist[6],
           win_fb.hist[7], win_fb.hist[8], win_fb.hist[9], win_fb.hist[10], win_fb.hist[11]);
    memset(m, 0, sizeof(*m));
    win_fb.last_print = now;
}

/* start: when this tick began. Printing is converter time with the guest stopped: it goes into
 * busy, the tick maxima and its own counter (print_us), and stays out of the gap accounting. */
static void fb_tick_end(u64 now, u64 start)
{
    if (win_fb.print_ticks && now - win_fb.last_print >= win_fb.print_ticks) {
        fb_print(now);
        u64 after = get_ticks();
        win_fb.print_sum += after - now;
        win_fb.busy += after - now;
        if (after - start > win_fb.max_ticks)
            win_fb.max_ticks = after - start;
        fb_max_update(&win_fb.mx[0].tick, after - start);
        now = after;
    }
    win_fb.last_end = now;
    win_fb.fault_mark = win_fb.faults + win_fb.spurious;
}

/*
 * nightly-6-perf: the background refresh walk (DIRTY mode with tuning bits 47:44). A round starts
 * when the period has elapsed; each tick converts pages from refresh_next on, round-robin, for at
 * most refresh_slice ticks of converter time; a round ends after the last page. It never touches the
 * dirty/pass sets or the protection: a page converted here that is dirty or queued in a pass is
 * simply converted again by its pass. Returns the converter ticks used.
 */
static u64 fb_refresh_step(u64 now)
{
    if (!win_fb.refresh_active) {
        if (now - win_fb.last_refresh < win_fb.refresh_ticks)
            return 0;
        win_fb.refresh_active = true;
        win_fb.refresh_next = 0;
        win_fb.refresh_force = win_fb.refreshes % FB_REFRESH_FORCE_EVERY == FB_REFRESH_FORCE_EVERY - 1;
    }
    sysop("dsb sy"); /* as fb_begin_pass: the guest's own NC stores have completed before we read */
    u64 t0 = get_ticks();
    while (win_fb.refresh_next < win_fb.pages) {
        u64 page = win_fb.refresh_next++;
        u64 w = page / 64, bit = BIT(page % 64);
        bool clean = !((win_fb.dirty[w] | win_fb.pass[w]) & bit);
        u64 wr0 = win_fb.chunks_wr;
        fb_convert_page(page, win_fb.refresh_force);
        win_fb.refresh_pages++;
        if (clean && !win_fb.refresh_force && win_fb.chunks_wr != wr0) {
            win_fb.refresh_fixes += win_fb.chunks_wr - wr0;
            win_fb.refresh_fixpages++;
        }
        if (get_ticks() - t0 >= win_fb.refresh_slice)
            break;
    }
    if (win_fb.refresh_next >= win_fb.pages) {
        win_fb.refresh_active = false;
        win_fb.refreshes++;
        win_fb.last_refresh = get_ticks();
    }
    sysop("dsb st");
    return get_ticks() - t0;
}

/* At the tail of hv_tick (before the storage tick). Single-vCPU only. */
static void hv_win_fb_tick_passes(void);

void hv_win_fb_tick(void)
{
    if (win_fb.state != 2 || (win_fb.flags & FB_F_NO_COPY))
        return;
    hv_win_fb_tick_passes();
    if (win_fb.refresh_ticks) {
        u64 used = fb_refresh_step(get_ticks());
        if (used) {
            win_fb.busy += used;
            win_fb.last_end = get_ticks(); /* the gap accounting starts after the refresh work */
        }
    }
}

static void hv_win_fb_tick_passes(void)
{
    u64 start = get_ticks();
    fb_note_gap(start);
    bool fault = win_fb.fault_this_tick;
    win_fb.fault_this_tick = false;
    if (!win_fb.pass_active && !win_fb.ndirty && (win_fb.flags & FB_F_DIRTY)) {
        fb_tick_end(start, start); /* idle: one clock read, no barrier */
        return;
    }

    if (!win_fb.pass_active) {
        if (!(win_fb.flags & FB_F_DIRTY)) {
            /* sweep mode: the whole frame is always due */
            for (u64 p = 0; p < win_fb.pages; p++)
                win_fb.dirty[p / 64] |= BIT(p % 64);
            win_fb.ndirty = win_fb.pages;
            win_fb.first_dirty = start;
            win_fb.first_dirty_stop = win_fb.stopped;
        } else if (fault && start - win_fb.first_dirty < win_fb.defer_ticks) {
            win_fb.deferred++; /* guest still drawing: wait for its blit to finish */
            fb_tick_end(start, start);
            return;
        }
        fb_begin_pass();
    } else if (win_fb.ndirty && (win_fb.flags & FB_F_DIRTY) && (win_fb.tuning & FB_T_MERGE)) {
        fb_merge_ahead();
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
        u64 page = w * 64 + __builtin_ctzll(m);
        int force = !!(win_fb.force[w] & bit);
        fb_convert_page_sampled(page, force);
        win_fb.pass[w] &= ~bit;
        win_fb.force[w] &= ~bit;
        win_fb.npass--;
        win_fb.pages_done++;
        win_fb.cursor = w;
        win_fb.next_page = page + 1;
        if (get_ticks() - start >= budget)
            break;
    }
    sysop("dsb st");
    u64 end = get_ticks();
    if (!win_fb.npass) {
        win_fb.pass_active = false;
        u64 lat = end - win_fb.pass_t0;
        u64 stop = win_fb.stopped - win_fb.pass_stop0;
        u64 net = lat > stop ? lat - stop : 0;
        if (lat > win_fb.max_lat)
            win_fb.max_lat = lat;
        win_fb.sum_lat += lat;
        win_fb.passes++;
        win_fb.hist[fb_lat_bucket(net)]++;
        fb_max_update(&win_fb.mx[0].lat_net, net);
        fb_max_update(&win_fb.mx[0].lat_gross, lat);
        win_fb.cursor = 0;
        win_fb.next_page = 0;
    }
    u64 elapsed = end - start;
    if (elapsed > win_fb.max_ticks)
        win_fb.max_ticks = elapsed;
    fb_max_update(&win_fb.mx[0].tick, elapsed);
    win_fb.busy += elapsed;
    win_fb.calls++;
    fb_tick_end(end, start);
}

/* idx | FB_STAT_RESET returns a (R) maximum and clears it (the gap triple clears together). */
u64 hv_win_fb_stat(u64 idx)
{
    bool reset = idx & FB_STAT_RESET;
    idx &= ~FB_STAT_RESET;
    if (idx >= ST_NR)
        return UINT64_MAX;
    struct fb_max *r = &win_fb.mx[0];
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
        [ST_VERSION] = FB_VERSION, [ST_TUNING] = win_fb.tuning,
        [ST_LAT_NET_MAX] = r->lat_net, [ST_LAT_GROSS_MAX] = r->lat_gross,
        [ST_GAP_MAX] = r->gap, [ST_GAP_AT] = r->gap_at, [ST_GAP_FAULTS] = r->gap_faults,
        [ST_GAPS_5MS] = win_fb.gaps5, [ST_GAPS_50MS] = win_fb.gaps50,
        [ST_STOPPED] = win_fb.stopped, [ST_FAULT_TICKS] = win_fb.fault_ticks,
        [ST_FAULT_MAX] = r->fault, [ST_SP_SAME] = win_fb.sp_same, [ST_SP_SUB] = win_fb.sp_sub,
        [ST_SP_ALIAS] = win_fb.sp_alias, [ST_SP_REPEAT] = win_fb.sp_repeat,
        [ST_WP_FAIL] = win_fb.wp_fail, [ST_CHUNKS] = win_fb.chunks,
        [ST_CHUNKS_WR] = win_fb.chunks_wr, [ST_RT_SAMPLES] = win_fb.rt_samples,
        [ST_RT_READ] = win_fb.rt_read, [ST_RT_CONV] = win_fb.rt_conv,
        [ST_MERGED] = win_fb.merged, [ST_BENCH_2X] = win_fb.bench[4],
        [ST_BENCH_RD32] = win_fb.bench[5], [ST_BENCH_RD64] = win_fb.bench[6],
        [ST_BENCH_DIFF] = win_fb.bench[7], [ST_DIFF_ON] = win_fb.copy != 0,
        [ST_BACKLOG_MAX] = r->backlog, [ST_TICK_MAX] = r->tick, [ST_PRINT] = win_fb.print_sum,
        [ST_REFRESHES] = win_fb.refreshes, [ST_REFRESH_FIXES] = win_fb.refresh_fixes,
        [ST_REFRESH_PAGES] = win_fb.refresh_pages, [ST_WP_REFAIL] = win_fb.wp_refail,
        [ST_REFRESH_FIXPAGES] = win_fb.refresh_fixpages,
    };
    for (unsigned int i = 0; i < FB_HIST; i++)
        v[ST_HIST0 + i] = win_fb.hist[i];
    if (reset) {
        switch (idx) {
            case ST_LAT_NET_MAX: r->lat_net = 0; break;
            case ST_LAT_GROSS_MAX: r->lat_gross = 0; break;
            case ST_GAP_MAX: case ST_GAP_AT: case ST_GAP_FAULTS:
                r->gap = r->gap_at = r->gap_faults = 0;
                break;
            case ST_FAULT_MAX: r->fault = 0; break;
            case ST_BACKLOG_MAX: r->backlog = 0; break;
            case ST_TICK_MAX: r->tick = 0; break;
            default: break;
        }
    }
    return v[idx];
}
