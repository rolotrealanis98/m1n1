/* SPDX-License-Identifier: MIT */
/* gpu-pt: host test of the GPU PS guard decision (src/hv/win/hv_win_gpu_logic.h). */
#include <stdio.h>
#include "../../src/hv/win/hv_win_gpu_logic.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

int main(void)
{
    /* every write value with all combinations of the low 12 bits and bits 24..31 */
    unsigned long n = 0;
    for (unsigned int hi = 0; hi < 256; hi++) {
        for (unsigned int lo = 0; lo < 4096; lo++) {
            unsigned int v = (hi << 24) | lo | 0x00a5a000u; /* middle bits must pass through */
            unsigned int f = gpu_ps_filter(v);
            n++;
            CHECK((f & GPU_PS_TARGET) == GPU_PS_ACTIVE, "target not active for 0x%x -> 0x%x", v, f);
            CHECK(!(f & GPU_PS_DEV_DISABLE), "dev-disable kept for 0x%x", v);
            CHECK(!(f & GPU_PS_RESET), "reset kept for 0x%x", v);
            unsigned int keep = ~(GPU_PS_TARGET | GPU_PS_DEV_DISABLE | GPU_PS_RESET);
            CHECK((f & keep) == (v & keep), "other bits changed 0x%x -> 0x%x", v, f);
            CHECK(gpu_ps_filter(f) == f, "not idempotent 0x%x", v);
        }
    }
    /* the Linux genpd power-on write and the stage-0a auto-enable write pass unchanged */
    CHECK(gpu_ps_filter(0x0f04030fu) == 0x0f04030fu, "power-on write altered");
    CHECK(gpu_ps_filter(0x1f0403ffu) == 0x1f0403ffu, "auto-enable write altered");
    /* a power-off (target 0) becomes active; reset/dev-disable stripped */
    CHECK(gpu_ps_filter(0x0f0403f0u) == 0x0f0403ffu, "power-off not forced");
    CHECK(gpu_ps_filter(0x8f0407ffu) == 0x0f0403ffu, "reset/dev-disable not stripped");
    /* is_active: the stage-0a hardware readings (runs/20261110-204506-gpu-m0a FIXES.md #1) */
    CHECK(gpu_ps_is_active(0x1f0003ffu), "GPX reading");
    CHECK(gpu_ps_is_active(0x1404034fu), "AFR auto-gated reading");
    CHECK(gpu_ps_is_active(0x1f0403ffu), "GFX reading");
    CHECK(!gpu_ps_is_active(0x0f040300u), "handover GFX off reading");
    CHECK(!gpu_ps_is_active(0x0f04030fu), "target ACTIVE without AUTO_ENABLE and ACTUAL 0 is not active");
    if (fails) {
        printf("win-gpu FAIL (%d of %lu)\n", fails, n);
        return 1;
    }
    printf("win-gpu PASS (%lu filter values, 7 fixed vectors)\n", n);
    return 0;
}
