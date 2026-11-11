/* SPDX-License-Identifier: MIT */
/*
 * Host memory the MTP coprocessor may DMA (dart-mtp, stream 0), preguest only.
 *
 * Linux on this machine: the MTP helper and the DockChannel HID transport both
 * sit behind dart-mtp stream 0 (DT iommus = <&mtp_dart 0>, apple,dma-range =
 * <0x100 0x4000 0x0 0xffff0000>); RTKit buffer requests with IOVA 0 and the
 * trackpad firmware get dma_alloc_coherent memory there (rtkit-helper.c
 * shmem_setup, dockchannel-hid.c dchid_send_firmware). A two-level (non
 * four-level) T8110 stream walks the low 36 bits of the DVA; the high bits are the
 * domain window (apple-dart.c "three-level stream ... TLB is tagged with the whole
 * DVA, inside the window"). So: IOVA = window (vm-base & ~(2^36-1)) | low 36 bits.
 *
 * State decision (dart-mtp stream 0 as iBoot left it):
 *   BYPASS_DART            -> DVA = PA, no write
 *   TRANSLATE + TTBR valid -> add one L2 table under a free root slot (iBoot tables)
 *   off (TCR 0, no TTBR), unlocked -> Linux attach: own root table, TTBR0, enable
 *                             stream 0, TCR0 = TRANSLATE, flush
 *   anything else          -> refuse (the coprocessor's host-buffer requests fail)
 * Every write is listed in input/trackpad-20261111/WRITES.md (T3).
 */
#include "types.h"
#include "mtp_dma.h"
#include "rtkit.h"
#include "adt.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#ifndef TP_PTR
#define TP_PTR(pa) ((void *)(pa))
#define TP_PA(p) ((u64)(p))
#endif

#define DART_MTP 0x294800000ull
#define T8110_ERROR 0x100
#define T8110_TLB_CMD 0x80
#define T8110_TLB_BUSY BIT(31)
#define T8110_PROTECT 0x200
#define T8110_ENABLE_STREAMS 0xc00
#define T8110_TCR0 0x1000
#define T8110_TTBR0 0x1400
#define TCR_TRANSLATE BIT(0)
#define TCR_BYPASS_DART BIT(1)
#define TTBR_VALID BIT(0)
#define TTBR_ADDR GENMASK(29, 2)
#define PTE_VALID BIT(0)
#define PTE_OFFSET GENMASK(39, 10)
#define PTE_SP_END GENMASK(51, 40)
#define LOW36 ((1ull << 36) - 1)

/*
 * nightly-2 (review finding 1): which physical pages the MTP may be pointed at. A DART table page, or a page a
 * table maps, must be resident-owned (m1n1 image + heap allocated so far) or lie outside the RAM iBoot hands
 * to the OS (boot_args phys_base..+mem_size), which contains every page the launcher can give the guest
 * (m5w,memory) and Windows' memory map. Table pages must also be inside m1n1's DRAM mapping before they
 * are read. The host test provides its own versions (MTP_DMA_MEM_PROVIDED).
 */
#ifndef MTP_DMA_MEM_PROVIDED
#include "heapblock.h"
#include "memory.h"
#include "xnuboot.h"
static bool mtp_dma_resident(u64 pa, u64 len)
{
    u64 lo = (u64)_base, hi = (u64)heapblock_peek();
    return len && pa >= lo && pa <= hi && len <= hi - pa;
}
static bool mtp_dma_os_ram(u64 pa, u64 len)
{
    u64 lo = cur_boot_args.phys_base, hi = lo + cur_boot_args.mem_size;
    return pa < hi && lo < pa + len;
}
static bool mtp_dma_dram(u64 pa, u64 len)
{
    return len && pa >= ram_base && pa - ram_base <= mem_size_actual && len <= mem_size_actual - (pa - ram_base);
}
#endif
static bool page_ok(u64 pa)
{
    return mtp_dma_resident(pa, SZ_16K) || !mtp_dma_os_ram(pa, SZ_16K);
}
static u64 refused_pa;
/* Every table page and every page the existing iBoot tables map; false = refuse the iBoot-tables mode. */
static bool iboot_tables_ok(u64 root_pa)
{
    if ((root_pa & (SZ_16K - 1)) || !mtp_dma_dram(root_pa, SZ_16K) || !page_ok(root_pa)) {
        refused_pa = root_pa;
        return false;
    }
    const u64 *root = TP_PTR(root_pa);
    for (u32 i = 0; i < SZ_16K / 8; i++) {
        if (!(root[i] & PTE_VALID))
            continue;
        u64 t = (u64)FIELD_GET(PTE_OFFSET, root[i]) << 14;
        if (!mtp_dma_dram(t, SZ_16K) || !page_ok(t)) {
            refused_pa = t;
            return false;
        }
        const u64 *tbl = TP_PTR(t);
        for (u32 j = 0; j < SZ_16K / 8; j++) {
            if (!(tbl[j] & PTE_VALID))
                continue;
            u64 leaf = (u64)FIELD_GET(PTE_OFFSET, tbl[j]) << 14;
            if (!page_ok(leaf)) {
                refused_pa = leaf;
                return false;
            }
        }
    }
    return true;
}

static enum mtp_dma_mode mode = MTP_DMA_OFF;
static u64 window, slot_base;
static u64 *l2;
static u32 next_page;
static bool tried;

static bool flush_idle(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (!(read32(DART_MTP + T8110_TLB_CMD) & T8110_TLB_BUSY)) return true;
        udelay(1);
    }
    return false;
}
static bool flush(void)
{
    dma_wmb();
    if (!flush_idle()) return false;
    write32(DART_MTP + T8110_TLB_CMD, 0); /* FLUSH_ALL, as m1n1 dart_t8110_flush_all */
    return flush_idle();
}

static bool geometry(u64 *vm_base)
{
    int path[8];
    u64 base, size;
    u32 len = 0;
    int node = adt_path_offset_trace(adt, "/arm-io/dart-mtp", path);
    if (node < 0 || !adt_is_compatible(adt, node, "dart,t8110") ||
        adt_get_reg(adt, path, "reg", 0, &base, &size) || base != DART_MTP || size < 0x4000)
        return false;
    const void *p = adt_getprop(adt, node, "vm-base", &len);
    *vm_base = 0;
    if (p && len == 8) memcpy(vm_base, p, 8);
    else if (p && len == 4) { u32 v; memcpy(&v, p, 4); *vm_base = v; }
    return true;
}

enum mtp_dma_mode mtp_dma_init(void)
{
    u64 vm_base;
    if (tried) return mode;
    tried = true;
    if (!geometry(&vm_base)) { printf("MTP-DMA: dart-mtp ADT geometry unexpected -> off\n"); return mode; }
    u32 tcr = read32(DART_MTP + T8110_TCR0), ttbr = read32(DART_MTP + T8110_TTBR0);
    u32 err = read32(DART_MTP + T8110_ERROR), prot = read32(DART_MTP + T8110_PROTECT);
    u32 en = read32(DART_MTP + T8110_ENABLE_STREAMS);
    printf("MTP-DMA: dart-mtp sid0 TCR=%#x TTBR=%#x ERROR=%#x PROTECT=%#x ENABLE=%#x vm-base=%#lx\n",
           tcr, ttbr, err, prot, en, vm_base);
    window = vm_base & ~LOW36;
    if (tcr & TCR_BYPASS_DART) {
        mode = MTP_DMA_BYPASS;
        printf("MTP-DMA: decision dart-mtp: bypass -> DVA = PA\n");
        return mode;
    }
    if (err) { printf("MTP-DMA: decision dart-mtp: error pending -> off\n"); return mode; }
    u32 first = (u32)(((vm_base & LOW36) + (1ull << 25) - 1) >> 25);
    if (first < 1) first = 1;
    u64 *root;
    if (tcr == TCR_TRANSLATE && (ttbr & TTBR_VALID)) {
        u64 root_pa = (u64)FIELD_GET(TTBR_ADDR, ttbr) << 14;
        if (!iboot_tables_ok(root_pa)) {
            printf("MTP-DMA: decision dart-mtp: iBoot tables REFUSED: page %#lx is OS/guest RAM and not resident-owned "
                   "(or outside the DRAM mapping) -> off\n", refused_pa);
            return mode;
        }
        root = TP_PTR(root_pa);
        mode = MTP_DMA_IBOOT_TABLES;
    } else if (!tcr && !(ttbr & TTBR_VALID) && !(prot & 1)) {
        root = memalign(SZ_16K, SZ_16K);
        if (!root || !mtp_dma_resident(TP_PA(root), SZ_16K)) return mode;
        memset(root, 0, SZ_16K);
        mode = MTP_DMA_OWN_TABLES;
    } else {
        printf("MTP-DMA: decision dart-mtp: state not handled -> off\n");
        return mode;
    }
    /* Lowest free 32 MiB slot inside Linux's window (apple,dma-range size 0xffff0000:
     * slots up to 127), so the DVA stays within the stream's input range. */
    u32 idx;
    for (idx = first; idx <= 127; idx++)
        if (!(root[idx] & PTE_VALID)) break;
    l2 = memalign(SZ_16K, SZ_16K);
    if (idx > 127 || !l2) { printf("MTP-DMA: no free root slot -> off\n"); mode = MTP_DMA_OFF; return mode; }
    if (!mtp_dma_resident(TP_PA(l2), SZ_16K)) { printf("MTP-DMA: L2 table not resident-owned -> off\n"); mode = MTP_DMA_OFF; return mode; }
    memset(l2, 0, SZ_16K);
    dma_wmb();
    root[idx] = FIELD_PREP(PTE_OFFSET, TP_PA(l2) >> 14) | PTE_VALID;
    slot_base = (u64)idx << 25;
    if (mode == MTP_DMA_OWN_TABLES) {
        /* Linux attach order: TTBR, stream enable, TCR translate; then flush. */
        dma_wmb();
        write32(DART_MTP + T8110_TTBR0, TTBR_VALID | (u32)((TP_PA(root) >> 14) << 2));
        write32(DART_MTP + T8110_ENABLE_STREAMS, en | BIT(0));
        write32(DART_MTP + T8110_TCR0, TCR_TRANSLATE);
    }
    if (!flush()) {
        printf("MTP-DMA: dart-mtp TLB flush did not finish -> off\n");
        if (mode == MTP_DMA_IBOOT_TABLES) root[idx] = 0;
        mode = MTP_DMA_OFF;
        return mode;
    }
    printf("MTP-DMA: decision dart-mtp: %s -> root[%u] window %#lx, DVA %#lx.. (ERROR after %#x)\n",
           mode == MTP_DMA_OWN_TABLES ? "own tables (Linux attach)" : "iBoot tables", idx, window,
           window | slot_base, read32(DART_MTP + T8110_ERROR));
    return mode;
}

bool mtp_dma_alloc(size_t sz, void **cpu, u64 *iova)
{
    size_t size = ALIGN_UP(sz, SZ_16K);
    if (!sz || mode == MTP_DMA_OFF) return false;
    u8 *buf = memalign(SZ_16K, size);
    if (!buf) return false;
    if (!mtp_dma_resident(TP_PA(buf), size)) { free(buf); return false; } /* nightly-2: only resident pages */
    memset(buf, 0, size);
    if (mode == MTP_DMA_BYPASS) {
        *cpu = buf; *iova = TP_PA(buf);
        return true;
    }
    u32 pages = (u32)(size / SZ_16K);
    if (pages > 2048 - next_page) { free(buf); return false; }
    for (u32 i = 0; i < pages; i++)
        l2[next_page + i] = FIELD_PREP(PTE_OFFSET, (TP_PA(buf) + (u64)i * SZ_16K) >> 14) |
                            FIELD_PREP(PTE_SP_END, 0xfff) | PTE_VALID;
    if (!flush()) {
        for (u32 i = 0; i < pages; i++) l2[next_page + i] = 0;
        free(buf);
        return false;
    }
    *cpu = buf;
    *iova = window | slot_base | ((u64)next_page * SZ_16K);
    next_page += pages;
    return true;
}

enum mtp_dma_mode mtp_dma_mode(void) { return mode; }

/* mtp_kbd_dma_hook: host buffers for the MTP RTKit session (keyboard and trackpad). */
void mtp_dma_rtkit_hook(rtkit_dev_t *rtk)
{
    if (mtp_dma_init() == MTP_DMA_OFF) {
        printf("MTP-DMA: no host DMA: only coprocessor-placed (SRAM) RTKit buffers can be accepted\n");
        return;
    }
    rtkit_mtp_host_buffers(rtk, mtp_dma_alloc);
}
