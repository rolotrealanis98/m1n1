/* Host compute benchmark (cached memory, so this is the ALU/compute floor, not NC memory cost):
 * legacy per-pixel volatile loop vs the wide SWAR core, full 3456x2234 frame and 2x mode. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t u64; typedef uint32_t u32;
#include "hv_win_fb_conv.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(void)
{
    const u64 W = 3456, H = 2234, S = 13824;
    u32 *src, *dst;
    if (posix_memalign((void **)&src, 16384, S * H) || posix_memalign((void **)&dst, 16384, S * H))
        return 1;
    for (u64 i = 0; i < W * H; i++) src[i] = (u32)(i * 2654435761u);
    memset(dst, 0, S * H);
    double best_l = 1e18, best_w = 1e18, best_2 = 1e18;
    for (int r = 0; r < 7; r++) {
        double t = now();
        for (u64 y = 0; y < H; y++) {
            const volatile u32 *s = (const volatile u32 *)((char *)src + y * S);
            volatile u32 *d = (volatile u32 *)((char *)dst + y * S);
            for (u64 x = 0; x < W; x++) {
                u32 p = s[x];
                u32 b = p & 0xff, g = (p >> 8) & 0xff, rr = (p >> 16) & 0xff;
                d[x] = ((b << 2) | (b >> 6)) | (((g << 2) | (g >> 6)) << 10) | (((rr << 2) | (rr >> 6)) << 20);
            }
        }
        t = now() - t; if (t < best_l) best_l = t;
        t = now();
        for (u64 y = 0; y < H; y++)
            fb_conv_span((u32 *)((char *)dst + y * S), (const u32 *)((char *)src + y * S), W);
        t = now() - t; if (t < best_w) best_w = t;
        t = now();
        for (u64 y = 0; y < H / 2; y++) /* 1728x1117 source rows of 6912 bytes */
            fb_conv_span_2x((u32 *)((char *)dst + 2 * y * S), (u32 *)((char *)dst + (2 * y + 1) * S),
                            (const u32 *)((char *)src + y * 6912), W / 2);
        t = now() - t; if (t < best_2) best_2 = t;
    }
    printf("host compute floor (cached RAM, general regs only): full frame legacy %.2f ms, wide %.2f ms, "
           "2x-upscale %.2f ms (per row: %.1f / %.1f us)\n", best_l / 1e6, best_w / 1e6, best_2 / 1e6,
           best_l / H / 1e3, best_w / H / 1e3);
    return 0;
}
