/* SPDX-License-Identifier: MIT */
/* Copyright The Asahi Linux Contributors. See LICENSE.MIT. */
/* BGRA shadow scanout, adapted from aurora-silicon/aura
 * 98599af29c644e89ab7f844ce94633cd451fd63a src/hv/windows/hv.c.
 * Opt-in, single-vCPU diagnostic. No DCP programming. Allocation is permanent
 * until reboot: host mappings change attributes and must never return to malloc.
 */
#include "hv_win.h"
#include "../hv.h"
#include "heapblock.h"
#include "memory.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define FB_PAGE 0x4000UL
#define FB_MAX_BYTES (64UL << 20)
#define FB_SLICE_BYTES (64UL << 10)
#define FB_SLICE_ROWS 4

extern u64 hv_cpus_in_guest;
extern bool hv_started_cpus[MAX_CPUS];

static struct {
    u64 state; /* 0 disabled, 1 prepared, 2 active, 3 failed/retained */
    u64 shadow, native, size, stride, width, height, depth, cursor;
    u64 calls, lines, max_ticks, budget_ticks;
    u64 remaining_before, remaining_after;
    u64 no_copy; /* PREPARE arg1 bit0: map/advertise shadow, skip host conversion */
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

/* Host-only proxy operation, before final guest memory planning. */
s64 hv_win_fb_prepare(u64 expected_native, u64 flags)
{
    if ((flags & ~1UL) || win_fb.attempted || !fb_idle_single_cpu())
        return -1;
    win_fb.attempted = true;
    win_fb.state = 3;
    win_fb.no_copy = flags & 1;
    const struct boot_args *ba = &cur_boot_args;
    u64 base = ba->video.base, stride = ba->video.stride;
    u64 width = ba->video.width, height = ba->video.height;
    if (expected_native != base || !base || (base & (FB_PAGE - 1)) ||
        (ba->video.depth & 0xff) != 30 || !width || width > 16384 || !height ||
        height > 16384 || stride < width * 4 || stride > 65536 || (stride & 3))
        return -2;
    u64 size = ALIGN_UP(stride * height, FB_PAGE);
    if (!size || size > FB_MAX_BYTES || base > UINT64_MAX - size)
        return -2;
    /* Preserve32MiB for mapping splits and later PMU page tables (the current
     * baseline uses roughly22MiB). Allocator itself is non-recovering. */
    size_t remaining = heapblock_remaining();
    win_fb.remaining_before = remaining;
    if (remaining == SIZE_MAX || remaining < size + FB_PAGE + (32UL << 20))
        return -3;
    u64 shadow = (u64)heapblock_alloc_aligned(size, FB_PAGE);
    /* Record ownership before changing cache attributes. Never free/reuse. */
    win_fb.shadow = shadow;
    win_fb.size = size;
    if (!shadow || shadow > UINT64_MAX - size ||
        (shadow < base + size && base < shadow + size))
        return -3;
    dc_civac_range((void *)shadow, size);
    sysop("dsb sy");
    /* No cached host aliases may remain for these dedicated data pages.
     * Break before changing attributes; all four map the same physical pages. */
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
    win_fb.budget_ticks = mrs(CNTFRQ_EL0) / 10000; /* 100us, checked between rows */
    win_fb.remaining_after = heapblock_remaining();
    if (!win_fb.budget_ticks)
        return -4;
    win_fb.state = 1;
    return shadow;
}

/* Called once, after launcher pt_update and before any guest instruction. */
bool hv_win_fb_start(const struct boot_args *gba)
{
    if (!win_fb.attempted)
        return true;
    if (win_fb.state != 1 || !fb_idle_single_cpu() || !gba ||
        gba->video.base != win_fb.shadow || gba->video.stride != win_fb.stride ||
        gba->video.width != win_fb.width || gba->video.height != win_fb.height ||
        gba->video.depth != ((win_fb.depth & ~0xffUL) | 32)) {
        win_fb.state = 3;
        return false;
    }
    /* Replace the launcher's ordinary RAM mapping. Force Normal-NC even if
     * Windows later requests cached memory; shadow is not in m5w,memory. */
    if (hv_map_hw_nc(win_fb.shadow, win_fb.shadow, win_fb.size) < 0) {
        win_fb.state = 3;
        return false;
    }
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    win_fb.state = 2;
    return true;
}

/* At the tail of hv_tick, after USB/watchdog/PL011/PMU work. Single-vCPU only.
 * No allocation, console, DCP RPC or guest-memory write. Native scanout is the
 * only destination. Limits are per invocation, not a hard latency guarantee. */
void hv_win_fb_tick(void)
{
    if (win_fb.state != 2 || win_fb.no_copy)
        return;
    u64 start = get_ticks();
    u64 rows = FB_SLICE_BYTES / win_fb.stride;
    if (rows > FB_SLICE_ROWS)
        rows = FB_SLICE_ROWS;
    for (u64 n = 0; n < rows; n++) {
        u64 y = win_fb.cursor;
        const volatile u32 *src = (const volatile u32 *)(win_fb.shadow + y * win_fb.stride);
        volatile u32 *dst = (volatile u32 *)(win_fb.native + y * win_fb.stride);
        /* Leave native row padding alone; it is not visible pixel content. */
        for (u64 x = 0; x < win_fb.width; x++) {
            u32 p = src[x];
            u32 b = p & 0xff, g = (p >> 8) & 0xff, r = (p >> 16) & 0xff;
            /* Match Mu SimpleFbEncodePixel: replicate the top two bits into
             * the low bits, so channel255 maps to1023, not1020. */
            dst[x] = ((b << 2) | (b >> 6)) | (((g << 2) | (g >> 6)) << 10) |
                     (((r << 2) | (r >> 6)) << 20);
        }
        win_fb.cursor = y + 1 == win_fb.height ? 0 : y + 1;
        if (win_fb.lines != UINT64_MAX)
            win_fb.lines++;
        if (get_ticks() - start >= win_fb.budget_ticks)
            break;
    }
    sysop("dsb st");
    u64 elapsed = get_ticks() - start;
    if (elapsed > win_fb.max_ticks)
        win_fb.max_ticks = elapsed;
    if (win_fb.calls != UINT64_MAX)
        win_fb.calls++;
}

u64 hv_win_fb_stat(u64 idx)
{
    const u64 values[] = {win_fb.state, win_fb.shadow, win_fb.native, win_fb.size,
                          win_fb.stride, win_fb.height, win_fb.calls, win_fb.lines,
                          win_fb.max_ticks, win_fb.cursor, win_fb.remaining_before,
                          win_fb.remaining_after, heapblock_remaining(), win_fb.no_copy};
    return idx < sizeof(values) / sizeof(values[0]) ? values[idx] : UINT64_MAX;
}
