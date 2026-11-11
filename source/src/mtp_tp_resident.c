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
#include "mtp_dma.h"

#define SMC_KEY_GP1C 0x67703163u    /* 'gp1c' */
#define GPIO_CMD_OUTPUT 0x01000000u /* gpio-macsmc CMD_OUTPUT */

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

/* Firmware buffer: shared dart-mtp host memory (mtp_dma.c), as Linux's
 * dchid_send_firmware uses dma_alloc_coherent on the same stream. */
static int dma_prepare(u32 len)
{
    void *cpu;
    if (mtp_dma_mode() == MTP_DMA_OFF || !mtp_dma_alloc(len, &cpu, &dma_iova)) {
        printf("MTP-TP: no dart-mtp host DMA for the firmware -> no trackpad\n");
        return -1;
    }
    dma_buf = cpu;
    dma_cap = ALIGN_UP(len, SZ_16K);
    printf("MTP-TP: firmware buffer %u bytes at DVA %#lx\n", dma_cap, dma_iova);
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
