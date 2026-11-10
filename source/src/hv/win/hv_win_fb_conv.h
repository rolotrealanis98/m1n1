/* SPDX-License-Identifier: MIT */
/*
 * BGRA8 -> X2R10G10B10 span conversion core for the EL2 framebuffer shadow.
 * Header-only so the resident and the host harness compile the exact same code.
 *
 * Constraints at EL2: built with -mgeneral-regs-only (the guest's FP/SIMD state is live in
 * the registers during hv_tick), so no NEON; the source (guest shadow) and destination
 * (native scanout) are both Normal-NC. The legacy loop did one volatile 32-bit NC load per
 * pixel, which serialises on memory latency. This core moves 8 pixels per iteration with
 * four independent 64-bit loads (the compiler emits LDP pairs) and SWAR channel expansion
 * on two pixels per register, so several NC loads can be in flight at once.
 *
 * Expansion is unchanged from the legacy/Mu encoding: each 8-bit channel c becomes
 * (c << 2) | (c >> 6) (bit replication; 0->0, 255->1023). Alpha/X is ignored, X2 = 0.
 */
#ifndef HV_WIN_FB_CONV_H
#define HV_WIN_FB_CONV_H

#ifndef FB_COST
#define FB_COST(rd_bytes, wr_bytes) do { } while (0) /* host harness hook: cost model */
#endif

/* Two BGRA8 pixels in one u64 -> two X2R10G10B10 pixels in one u64. */
static inline u64 fb_px2(u64 p)
{
    /* spread B,G,R into 10-bit lanes at bits 0, 10, 20 of each 32-bit half */
    u64 x = (p & 0x000000ff000000ffULL) | ((p & 0x0000ff000000ff00ULL) << 2) |
            ((p & 0x00ff000000ff0000ULL) << 4);
    /* lane = c << 2 | c >> 6; the >>6 spill from the next lane/half is masked off */
    return (x << 2) | ((x >> 6) & 0x00300c0300300c03ULL);
}

static inline u32 fb_px1(u32 p)
{
    return (u32)fb_px2(p);
}

/* Convert n pixels. src/dst must be 8-byte aligned for the wide path (both are: page-aligned
 * buffers, strides and in-row offsets are multiples of 8); a misaligned call falls back. */
static inline void fb_conv_span(u32 *dst, const u32 *src, u64 n)
{
    u64 i = 0;
    if (!(((u64)dst | (u64)src) & 7)) {
        const u64 *s = (const u64 *)src;
        u64 *d = (u64 *)dst;
        for (; i + 8 <= n; i += 8, s += 4, d += 4) {
            u64 a = s[0], b = s[1], c = s[2], e = s[3]; /* 4 independent loads first */
            d[0] = fb_px2(a);
            d[1] = fb_px2(b);
            d[2] = fb_px2(c);
            d[3] = fb_px2(e);
        }
    }
    for (; i < n; i++)
        dst[i] = fb_px1(src[i]);
    FB_COST(n * 4, n * 4);
}

/* 2x integer upscale: n source pixels -> 2n pixels on each of two destination rows. */
static inline void fb_conv_span_2x(u32 *dst0, u32 *dst1, const u32 *src, u64 n)
{
    u64 i = 0;
    if (!(((u64)dst0 | (u64)dst1 | (u64)src) & 7)) {
        const u64 *s = (const u64 *)src;
        u64 *d0 = (u64 *)dst0, *d1 = (u64 *)dst1;
        for (; i + 4 <= n; i += 4, s += 2, d0 += 4, d1 += 4) {
            u64 a = fb_px2(s[0]), b = fb_px2(s[1]);
            u64 a0 = (a & 0xffffffffULL) * 0x100000001ULL, a1 = (a >> 32) * 0x100000001ULL;
            u64 b0 = (b & 0xffffffffULL) * 0x100000001ULL, b1 = (b >> 32) * 0x100000001ULL;
            d0[0] = a0; d0[1] = a1; d0[2] = b0; d0[3] = b1;
            d1[0] = a0; d1[1] = a1; d1[2] = b0; d1[3] = b1;
        }
    }
    for (; i < n; i++) {
        u32 v = fb_px1(src[i]);
        dst0[2 * i] = dst0[2 * i + 1] = v;
        dst1[2 * i] = dst1[2 * i + 1] = v;
    }
    FB_COST(n * 4, n * 16);
}

#endif
