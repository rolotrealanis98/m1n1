/* SPDX-License-Identifier: MIT */
/*
 * Resident adapter for the J716s trackpad layer (hidm/tp/mtp_tp.c).
 * Preguest only: firmware DMA buffer + dart-mtp mapping, SMC gp1c AFE reset.
 * Every register write is listed in input/trackpad-20261111/WRITES.md.
 */
#include "mtp_kbd.h"
#include "mtp_tp_resident.h"
#include "hidm/tp/mtp_tp.h"
#include "adt.h"
#include "malloc.h"
#include "rtkit.h"
#include "smc.h"
#include "string.h"
#include "utils.h"
#include "wdt.h"
#include "vsprintf.h"
#include <stdarg.h>

#define DART_MTP 0x294800000ull
#define DART_T8110_ERROR 0x100
#define DART_T8110_TLB_CMD 0x80
#define DART_T8110_TLB_CMD_BUSY BIT(31)
#define DART_T8110_PROTECT 0x200
#define DART_T8110_TCR0 0x1000      /* stream 0 */
#define DART_T8110_TTBR0 0x1400     /* stream 0 */
#define TCR_TRANSLATE BIT(0)
#define TCR_BYPASS_DART BIT(1)
#define TTBR_VALID BIT(0)
#define TTBR_ADDR GENMASK(29, 2)
#define PTE_VALID BIT(0)
#define PTE_OFFSET GENMASK(39, 10)  /* t8110 (dart.c DART_T6000_PTE_OFFSET) */
#define PTE_SP_END GENMASK(51, 40)
#define SMC_KEY_GP1C 0x67703163u    /* 'gp1c' */
#define GPIO_CMD_OUTPUT 0x01000000u /* gpio-macsmc CMD_OUTPUT */

/* Physical address <-> pointer (identity in m1n1; host tests remap to an arena). */
#ifndef TP_PTR
#define TP_PTR(pa) ((void *)(pa))
#define TP_PA(p) ((u64)(p))
#endif

static struct mtp_tp tp;
static smc_dev_t *smc;
static bool smc_tried, preguest = true;
static u64 fw_addr, fw_len;
static u8 *dma_buf;
static u64 dma_iova;
static u32 dma_cap;

static int tp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int tp_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    return n;
}
static hm_u64 now_us(void *ctx)
{
    (void)ctx;
    u64 ticks = mrs(CNTPCT_EL0), hz = mrs(CNTFRQ_EL0);
    return ticks / hz * 1000000 + ticks % hz * 1000000 / hz;
}

/* AFE reset = SMC GPIO gp1c, active low (Linux DT apple,afe-reset-gpios =
 * <&smc_gpio_low 28 GPIO_ACTIVE_LOW>; gpio-macsmc writes CMD_OUTPUT | level).
 * asserted -> level 0, released -> level 1. Preguest only: the SMC RTKit call is
 * synchronous; at runtime the request is refused (GPIO NACK path). */
static int afe_reset(void *ctx, int asserted)
{
    (void)ctx;
    if (!preguest) return -1;
    if (!smc_tried) {
        smc_tried = true;
        rtkit_set_boot_timeout(10 * USEC_PER_SEC);
        smc = smc_init();
        rtkit_set_boot_timeout(0);
        printf("MTP-TP: SMC %s\n", smc ? "up" : "NOT available");
    }
    if (!smc) return -1;
    u32 v = GPIO_CMD_OUTPUT | (asserted ? 0 : 1);
    int ret = smc_write_u32(smc, SMC_KEY_GP1C, v);
    printf("MTP-TP: SMC gp1c <- %#x: %d\n", v, ret);
    return ret ? -1 : 0;
}

static bool dart_geometry_ok(u64 *vm_base)
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

static bool flush_idle(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (!(read32(DART_MTP + DART_T8110_TLB_CMD) & DART_T8110_TLB_CMD_BUSY)) return true;
        udelay(1);
    }
    return false;
}

/*
 * The firmware buffer the MTP DMAs from. Decision by the state iBoot left in
 * dart-mtp stream 0 (read only, then at most: one L1 entry, one new L2 table,
 * one TLB flush):
 *   TCR BYPASS_DART       -> IOVA = PA, no DART write;
 *   TCR == TRANSLATE only, TTBR valid, no error -> map into a free 32 MiB L1 slot;
 *   anything else          -> refuse: no trackpad, keyboard unaffected.
 */
static int dma_prepare(u32 len)
{
    u64 vm_base;
    if (!dart_geometry_ok(&vm_base)) { printf("MTP-TP: dart-mtp ADT geometry unexpected\n"); return -1; }
    u32 tcr = read32(DART_MTP + DART_T8110_TCR0), ttbr = read32(DART_MTP + DART_T8110_TTBR0);
    u32 err = read32(DART_MTP + DART_T8110_ERROR), prot = read32(DART_MTP + DART_T8110_PROTECT);
    printf("MTP-TP: dart-mtp sid0 TCR=%#x TTBR=%#x ERROR=%#x PROTECT=%#x vm-base=%#lx\n", tcr, ttbr,
           err, prot, vm_base);
    dma_cap = ALIGN_UP(len, SZ_16K);
    dma_buf = memalign(SZ_16K, dma_cap);
    if (!dma_buf) return -1;
    memset(dma_buf, 0, dma_cap);
    if (tcr & TCR_BYPASS_DART) {
        dma_iova = TP_PA(dma_buf);
        printf("MTP-TP: decision dart-mtp: bypass -> firmware IOVA = PA %#lx\n", dma_iova);
        return 0;
    }
    if (tcr != TCR_TRANSLATE || !(ttbr & TTBR_VALID) || err) {
        printf("MTP-TP: decision dart-mtp: state not handled (TCR/TTBR/ERROR) -> no trackpad\n");
        return -1;
    }
    u64 *l1 = TP_PTR((u64)FIELD_GET(TTBR_ADDR, ttbr) << 14);
    u32 first = (u32)(((vm_base & ((1ull << 36) - 1)) + (1ull << 25) - 1) >> 25), idx;
    for (idx = 2047; idx >= first && idx > 0; idx--)
        if (!(l1[idx] & PTE_VALID)) break;
    if (idx < first || idx == 0 || dma_cap > (1u << 25)) {
        printf("MTP-TP: decision dart-mtp: no free L1 slot -> no trackpad\n");
        return -1;
    }
    u64 *l2 = memalign(SZ_16K, SZ_16K);
    if (!l2) return -1;
    memset(l2, 0, SZ_16K);
    for (u32 off = 0, i = 0; off < dma_cap; off += SZ_16K, i++)
        l2[i] = FIELD_PREP(PTE_OFFSET, (TP_PA(dma_buf) + off) >> 14) | FIELD_PREP(PTE_SP_END, 0xfff) |
                PTE_VALID;
    dma_wmb();
    l1[idx] = FIELD_PREP(PTE_OFFSET, TP_PA(l2) >> 14) | PTE_VALID;
    dma_wmb();
    if (!flush_idle()) { printf("MTP-TP: dart-mtp TLB busy before flush\n"); l1[idx] = 0; return -1; }
    write32(DART_MTP + DART_T8110_TLB_CMD, 0); /* FLUSH_ALL, as dart.c dart_t8110_flush_all */
    if (!flush_idle()) { printf("MTP-TP: dart-mtp TLB flush did not finish\n"); return -1; }
    dma_iova = (vm_base & ~((1ull << 36) - 1)) | ((u64)idx << 25);
    printf("MTP-TP: decision dart-mtp: translate -> L1[%u] new L2 %p, firmware IOVA %#lx "
           "(ERROR after %#x)\n", idx, l2, dma_iova, read32(DART_MTP + DART_T8110_ERROR));
    return 0;
}

static void attach(struct mtp_kbd *k)
{
    const u8 *fw = (const u8 *)fw_addr;
    if (!fw_addr || fw_len < TP_FW_HDR || fw_len > TP_FW_MAX) return;
    u32 dlen = hm_le32(fw + 12);
    if (dlen == 0 || dlen > fw_len) { printf("MTP-TP: firmware header bad\n"); return; }
    if (dma_prepare(dlen) < 0) return;
    const struct mtp_tp_io io = {k, mtp_kbd_tp_send, afe_reset, now_us, tp_log,
                                 dma_buf, dma_iova, dma_cap};
    if (mtp_tp_init(&tp, &io, fw, (u32)fw_len) < 0) return;
    k->tp = &tp;
    printf("MTP-TP: attached (firmware %u bytes, data %u)\n", (unsigned)fw_len, dlen);
}

void mtp_tp_resident_arm(hm_u64 addr, hm_u64 len)
{
    fw_addr = addr;
    fw_len = len;
    mtp_kbd_attach_hook = addr ? attach : NULL;
}

/* After keyboard LIVE: finish the trackpad (bounded 8 s), then release the SMC
 * client. Returns the PTP interface to publish, or NULL. */
const hm_backend_interface *mtp_tp_resident_finish(struct mtp_kbd *k)
{
    if (!k->tp) return NULL;
    u64 deadline = now_us(NULL) + 8000000;
    while (k->phase == MTP_LIVE && tp.phase != TP_LIVE && tp.phase != TP_FAILED &&
           now_us(NULL) < deadline) {
        mtp_kbd_poll(k);
        wdt_kick();
        udelay(50);
    }
    preguest = false;
    if (smc) { smc_shutdown(smc); smc = NULL; }
    if (tp.phase != TP_LIVE) {
        printf("MTP-TP: trackpad not available (phase %s)\n", mtp_tp_phase_name(tp.phase));
        k->tp = NULL; /* keyboard keeps working exactly as without the trackpad */
        return NULL;
    }
    printf("MTP-TP: bring-up OK: PTP iface=%u descriptor=%u bytes\n", tp.mt,
           (unsigned)tp.ptp.desc_len);
    return &tp.iface;
}
