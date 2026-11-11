/* SPDX-License-Identifier: MIT */
/*
 * gpu-pt: GPU passthrough support in the resident (launcher --gpu passthrough, P_HV_WIN_CONFIGURE flag
 * HV_WIN_F_GPU_PS_GUARD). Everything else of the passthrough is host-side policy (stage-2 identity maps of the
 * GPU carve-outs, AIC -> SPI routing of the four mailbox lines, the power-up): see
 * gpu/passthrough-20261111/DESIGN.md.
 *
 * Here: a software MMIO hook on the three guarded PMGR PS words (GPX, AFR, GFX; addresses from the ADT through
 * m1n1's pmgr tables). Reads return the hardware value; writes are filtered by gpu_ps_filter() so the guest can
 * never take a guarded domain out of ACTIVE. The rest of the 16 KiB PS page stays a hardware access (emulated
 * by hv_vm's sub-page mapping, so every access to that page is an EL2 exit; the GPU driver touches it only at
 * power transitions). Installed at guest start, after the host's stage-2 updates, and refused when GFX is not
 * active (the launcher powers it first): with GFX off the guest's first sgx access could stall the bus.
 */

#include "hv_win.h"
#include "adt.h"
#include "pmgr.h"
#include "utils.h"
#include "../hv.h"
#include "hv_win_gpu_logic.h"

static const char *const gpu_ps_names[3] = {"GPX", "AFR", "GFX"};
static u64 gpu_ps_addr[3];
static u64 gpu_ps_forced, gpu_ps_dropped, gpu_ps_writes;
static bool gpu_ps_armed;

static int gpu_ps_index(u64 addr)
{
    for (int i = 0; i < 3; i++)
        if (gpu_ps_addr[i] && addr == gpu_ps_addr[i])
            return i;
    return -1;
}

static bool hv_win_gpu_ps_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    int i = gpu_ps_index(addr & ~7UL);

    if (i < 0)
        return false;

    if (!write) {
        if (width == 3)
            *val = read64(addr);
        else if (width == 2)
            *val = read32(addr);
        else
            *val = 0; /* byte/halfword reads of a PS word: never done by any driver */
        return true;
    }

    __atomic_fetch_add(&gpu_ps_writes, 1, __ATOMIC_RELAXED);
    if (width < 2 || (addr & 7)) {
        /* sub-word or upper-half writes could clear TARGET bits piecemeal: dropped */
        u64 n = __atomic_add_fetch(&gpu_ps_dropped, 1, __ATOMIC_RELAXED);
        if (n <= 8)
            printf("HV/win gpu: PS %s width %d write @0x%lx dropped (#%ld)\n", gpu_ps_names[i], 1 << width,
                   addr, (long)n);
        return true;
    }

    u32 want = (u32)*val;
    u32 put = gpu_ps_filter(want);
    if (put != want) {
        u64 n = __atomic_add_fetch(&gpu_ps_forced, 1, __ATOMIC_RELAXED);
        if (n <= 8)
            printf("HV/win gpu: PS %s write 0x%x -> 0x%x (guarded: stays ACTIVE) (#%ld)\n", gpu_ps_names[i],
                   want, put, (long)n);
    }
    if (width == 3)
        write64(addr, (*val & ~0xffffffffUL) | put);
    else
        write32(addr, put);
    return true;
}

bool hv_win_gpu_start(void)
{
    if (!(hv_win.flags & HV_WIN_F_GPU_PS_GUARD))
        return true;

    for (int i = 0; i < 3; i++) {
        gpu_ps_addr[i] = pmgr_psreg_by_name(0, gpu_ps_names[i]);
        if (!gpu_ps_addr[i] || (gpu_ps_addr[i] & 7)) {
            printf("HV/win gpu: no PS register for %s in the ADT: guest refused\n", gpu_ps_names[i]);
            return false;
        }
        u32 v = read32(gpu_ps_addr[i]);
        if (!gpu_ps_is_active(v)) {
            printf("HV/win gpu: %s PS 0x%x is not ACTIVE at guest start (launcher power-up missing): "
                   "guest refused\n",
                   gpu_ps_names[i], v);
            return false;
        }
    }
    /*
     * gpu-pt2: the GPU block is non-posted (Linux DT: nonposted-mmio on soc@2300000000). Windows maps device
     * memory Device-nGnRE at stage 1, so force Device-nGnRnE at stage 2 over sgx reg[0] (sgx, PMGR group 3, both
     * ASC wrappers and iop-vbars): the combined type is the more restrictive one. Before the PS hooks, which
     * replace their 16 KiB page with a software mapping afterwards.
     */
    int path[8];
    u64 sgx_base = 0, sgx_size = 0;
    if (adt_path_offset_trace(adt, "/arm-io/sgx", path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &sgx_base, &sgx_size) < 0 || !sgx_size ||
        ((sgx_base | sgx_size) & 0x3fff)) {
        printf("HV/win gpu: no usable /arm-io/sgx reg[0]: guest refused\n");
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (gpu_ps_addr[i] < sgx_base || gpu_ps_addr[i] >= sgx_base + sgx_size) {
            printf("HV/win gpu: PS %s 0x%lx outside sgx: guest refused\n", gpu_ps_names[i], gpu_ps_addr[i]);
            return false;
        }
    }
    if (hv_map_hw_dev(sgx_base, sgx_base, sgx_size) < 0) {
        printf("HV/win gpu: stage-2 Device-nGnRnE map of sgx failed: guest refused\n");
        return false;
    }
    printf("HV/win gpu: stage 2 0x%lx..0x%lx Device-nGnRnE (MemAttr 0000; sgx, PMGR group 3, GFX/GFX1 wrappers "
           "and iop-vbars)\n",
           sgx_base, sgx_base + sgx_size);
    for (int i = 0; i < 3; i++) {
        if (hv_map_hook(gpu_ps_addr[i], hv_win_gpu_ps_hook, sizeof(u64)) < 0) {
            printf("HV/win gpu: PS hook for %s failed: guest refused\n", gpu_ps_names[i]);
            return false;
        }
    }
    /* stage-2 entries changed: no guest has run yet, but drop any cached walk anyway */
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    gpu_ps_armed = true;
    printf("HV/win gpu: PS words GPX/AFR/GFX: 16 KiB page 0x%lx trapped (EL2 emulates every access, writes "
           "filtered)\n",
           gpu_ps_addr[0] & ~0x3fffUL);
    printf("HV/win gpu: PS guard armed: GPX 0x%lx (0x%x) AFR 0x%lx (0x%x) GFX 0x%lx (0x%x)\n", gpu_ps_addr[0],
           read32(gpu_ps_addr[0]), gpu_ps_addr[1], read32(gpu_ps_addr[1]), gpu_ps_addr[2],
           read32(gpu_ps_addr[2]));
    return true;
}

void hv_win_gpu_report(void)
{
    if (!gpu_ps_armed)
        return;
    printf("HV/win gpu: PS guard: %ld guest write(s), %ld forced to ACTIVE, %ld dropped; GPX 0x%x AFR 0x%x "
           "GFX 0x%x\n",
           (long)gpu_ps_writes, (long)gpu_ps_forced, (long)gpu_ps_dropped, read32(gpu_ps_addr[0]),
           read32(gpu_ps_addr[1]), read32(gpu_ps_addr[2]));
}
