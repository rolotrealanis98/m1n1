/* SPDX-License-Identifier: MIT */
/*
 * gpu-pt: pure decision logic of the GPU power-state guard (hv_win_gpu.c), shared with the host test
 * (tests/win-gpu). No hardware access here.
 *
 * The Windows guest owns the GPU (sgx window passthrough), including PMGR PS group 3 (GPX +0x0, AFR +0x100,
 * CS +0x108, GFX +0x110). EL2 powered GPX -> AFR -> GFX before the guest started (launcher --gpu passthrough,
 * the stage-0a sequence). A guest write may never take GPX, AFR or GFX out of ACTIVE: the iBoot-loaded GPU
 * firmware would be lost (no reload without a reboot), and any later sgx/ASC access with GFX off can stall
 * the bus for the whole SoC, the resident's disk path included. CS is left to the GPU firmware (as on Linux).
 *
 * PS word layout (Linux drivers/pmdomain/apple/pmgr-pwrstate.c, m1n1 src/pmgr.c): TARGET 3:0, ACTUAL 7:4,
 * WAS_PWRGATED 8, WAS_CLKGATED 9, DEV_DISABLE 10, AUTO_ENABLE 28, RESET 31.
 */
#ifndef HV_WIN_GPU_LOGIC_H
#define HV_WIN_GPU_LOGIC_H

#define GPU_PS_TARGET       0xfu
#define GPU_PS_ACTUAL_SHIFT 4
#define GPU_PS_ACTIVE       0xfu
#define GPU_PS_DEV_DISABLE  (1u << 10)
#define GPU_PS_AUTO_ENABLE  (1u << 28)
#define GPU_PS_RESET        (1u << 31)

/*
 * The value EL2 writes for a guest write of `val` to a guarded PS word: TARGET forced to ACTIVE, DEV_DISABLE
 * and RESET cleared; every other bit (AUTO_ENABLE, the write-1-to-clear WAS_* bits) as the guest wrote it.
 */
static inline unsigned int gpu_ps_filter(unsigned int val)
{
    return (val & ~(GPU_PS_TARGET | GPU_PS_DEV_DISABLE | GPU_PS_RESET)) | GPU_PS_ACTIVE;
}

/* Linux apple_pmgr_ps_is_active(): ACTUAL == ACTIVE, or TARGET == ACTIVE with AUTO_ENABLE (auto-gated idle) */
static inline int gpu_ps_is_active(unsigned int val)
{
    return ((val >> GPU_PS_ACTUAL_SHIFT) & 0xfu) == GPU_PS_ACTIVE ||
           ((val & GPU_PS_TARGET) == GPU_PS_ACTIVE && (val & GPU_PS_AUTO_ENABLE));
}

#endif
