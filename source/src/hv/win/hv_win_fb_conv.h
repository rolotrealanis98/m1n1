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
 *
 * disp2 (2026-11-10): the *_diff variants compare each 16-pixel (64-byte) source chunk with a
 * cacheable copy of what was last converted and write (and update the copy) only the chunks
 * that changed. BasicDisplay re-copies whole dirty rectangles, so most of a dirty page is
 * usually unchanged; skipping those chunks removes their NC writes (4x the source bytes in 2x
 * mode) and their compute. The eight source loads of a chunk are issued before any compare.
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

#define FB_CHUNK_PX 16 /* one 64-byte source line */

/* Load one 16-pixel chunk (8 independent 64-bit loads) / compare it with / store it to a copy. */
#define FB_LOAD8(s, v)                                                                             \
    u64 v##0 = (s)[0], v##1 = (s)[1], v##2 = (s)[2], v##3 = (s)[3], v##4 = (s)[4],               \
        v##5 = (s)[5], v##6 = (s)[6], v##7 = (s)[7]
#define FB_SAME8(c, v)                                                                             \
    (!((v##0 ^ (c)[0]) | (v##1 ^ (c)[1]) | (v##2 ^ (c)[2]) | (v##3 ^ (c)[3]) | (v##4 ^ (c)[4]) | \
       (v##5 ^ (c)[5]) | (v##6 ^ (c)[6]) | (v##7 ^ (c)[7])))
#define FB_STORE8(c, v)                                                                            \
    do {                                                                                           \
        (c)[0] = v##0; (c)[1] = v##1; (c)[2] = v##2; (c)[3] = v##3;                                \
        (c)[4] = v##4; (c)[5] = v##5; (c)[6] = v##6; (c)[7] = v##7;                                \
    } while (0)

/*
 * Diff conversion, 1x: n source pixels at src; copy[] holds what the native row was last
 * converted from. A chunk is written when force is set or any of its 16 pixels differs.
 * Returns the number of chunks written; *examined += chunks looked at. Every written chunk is
 * bit-exact with fb_conv_span; unchanged chunks are left untouched in dst.
 */
static inline u64 fb_conv_span_diff(u32 *dst, const u32 *src, u32 *copy, u64 n, int force,
                                    u64 *examined)
{
    u64 i = 0, wr = 0, ex = 0, tail_wr = 0;
    if (!(((u64)dst | (u64)src | (u64)copy) & 7)) {
        const u64 *s = (const u64 *)src;
        u64 *c = (u64 *)copy, *d = (u64 *)dst;
        for (; i + FB_CHUNK_PX <= n; i += FB_CHUNK_PX, s += 8, c += 8, d += 8) {
            FB_LOAD8(s, v);
            ex++;
            if (!force && FB_SAME8(c, v))
                continue;
            FB_STORE8(c, v);
            d[0] = fb_px2(v0); d[1] = fb_px2(v1); d[2] = fb_px2(v2); d[3] = fb_px2(v3);
            d[4] = fb_px2(v4); d[5] = fb_px2(v5); d[6] = fb_px2(v6); d[7] = fb_px2(v7);
            wr++;
        }
    }
    if (i < n) {
        ex++;
        for (; i < n; i++) {
            u32 v = src[i];
            if (force || v != copy[i]) {
                copy[i] = v;
                dst[i] = fb_px1(v);
                tail_wr++;
            }
        }
    }
    *examined += ex;
    FB_COST(n * 4, wr * 64 + tail_wr * 4);
    return wr + (tail_wr != 0);
}

/*
 * Diff conversion, 2x: each changed 16-pixel source chunk becomes 32 native pixels on each of
 * the two destination rows, written row by row (128 contiguous bytes, then the other row) so
 * every NC write burst covers whole 64-byte lines.
 */
static inline u64 fb_conv_span_2x_diff(u32 *dst0, u32 *dst1, const u32 *src, u32 *copy, u64 n,
                                       int force, u64 *examined)
{
    u64 i = 0, wr = 0, ex = 0, tail_wr = 0;
    if (!(((u64)dst0 | (u64)dst1 | (u64)src | (u64)copy) & 7)) {
        const u64 *s = (const u64 *)src;
        u64 *c = (u64 *)copy, *d0 = (u64 *)dst0, *d1 = (u64 *)dst1;
        for (; i + FB_CHUNK_PX <= n; i += FB_CHUNK_PX, s += 8, c += 8, d0 += 16, d1 += 16) {
            FB_LOAD8(s, v);
            ex++;
            if (!force && FB_SAME8(c, v))
                continue;
            FB_STORE8(c, v);
            const u64 in[8] = {v0, v1, v2, v3, v4, v5, v6, v7};
            u64 o[16];
            for (int k = 0; k < 8; k++) {
                u64 a = fb_px2(in[k]);
                o[2 * k] = (a & 0xffffffffULL) * 0x100000001ULL;
                o[2 * k + 1] = (a >> 32) * 0x100000001ULL;
            }
            for (int k = 0; k < 16; k++)
                d0[k] = o[k];
            for (int k = 0; k < 16; k++)
                d1[k] = o[k];
            wr++;
        }
    }
    if (i < n) {
        ex++;
        for (; i < n; i++) {
            u32 v = src[i];
            if (force || v != copy[i]) {
                u32 p = fb_px1(v);
                copy[i] = v;
                dst0[2 * i] = dst0[2 * i + 1] = p;
                dst1[2 * i] = dst1[2 * i + 1] = p;
                tail_wr++;
            }
        }
    }
    *examined += ex;
    FB_COST(n * 4, wr * 256 + tail_wr * 16);
    return wr + (tail_wr != 0);
}

#endif
