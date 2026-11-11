/* SPDX-License-Identifier: MIT */
#include "mtp_kbd.h"
#include "adt.h"
#include "asc.h"
#include "rtkit.h"
#include "dapf.h"
#include "utils.h"
#include "wdt.h"
#include "vsprintf.h"
#include <stdarg.h>

static rtkit_dev_t *mtp_rtkit;
static bool attempted;
static hm_u32 mmio_read(void *ctx, hm_u64 addr) { (void)ctx; return read32(addr); }
static void mmio_write(void *ctx, hm_u64 addr, hm_u32 value)
{ (void)ctx; write32(addr, value); }
static hm_u64 now_us(void *ctx)
{
    (void)ctx;
    u64 ticks = mrs(CNTPCT_EL0), hz = mrs(CNTFRQ_EL0);
    return ticks / hz * 1000000 + ticks % hz * 1000000 / hz;
}
static int control_poll(void *ctx)
{
    (void)ctx;
    struct rtkit_message msg;
    return rtkit_recv(mtp_rtkit, &msg);
}
static bool geometry(const char *name, unsigned idx, u64 expected, u64 minimum)
{
    int path[8]; u64 base, size;
    return adt_path_offset_trace(adt, name, path) >= 0 &&
        adt_get_reg(adt, path, "reg", idx, &base, &size) == 0 &&
        base == expected && size >= minimum;
}
static bool dapf_checked(void)
{
    static const u64 starts[9] = {0x28830c000, 0x288320900, 0x2881e4050,
        0x288360100, 0x288f00014, 0x288380020, 0x288200324, 0x2881e4064, 0x2881e4068};
    static const u64 ends[9] = {0x28830c07f, 0x288320b1b, 0x2881e4053,
        0x28836010b, 0x288f00017, 0x288380023, 0x288200327, 0x2881e4067, 0x2881e406b};
    int path[8], node = adt_path_offset_trace(adt, "/arm-io/dart-mtp", path);
    u32 len = 0;
    if (node < 0 || !adt_is_compatible(adt, node, "dart,t8110") ||
        adt_getprop(adt, node, "clock-gates", &len)) return false;
    const u8 *p = adt_getprop(adt, node, "dapf-instance-0", &len);
    if (!p || len != 9 * 52) return false;
    for (unsigned i = 0; i < 9; i++, p += 52) {
        u64 start = hm_le32(p) | ((u64)hm_le32(p + 4) << 32);
        u64 end = hm_le32(p + 8) | ((u64)hm_le32(p + 12) << 32);
        if (start != starts[i] || end != ends[i] || hm_le32(p + 16) != 1 ||
            hm_le32(p + 24) != 0 || p[49] != 3 || p[50] != 1) return false;
    }
    return true;
}
#define MTP_CPU_CONTROL 0x294600044ull
#define MTP_I2A_CONTROL 0x294608114ull
#define MTP_I2A_RECV0 0x294608830ull
#define MTP_I2A_RECV1 0x294608838ull
#define MTP_DC_IRQ_MASK 0x294b14000ull
#define MTP_DC_IRQ_FLAG 0x294b14004ull
static int mtp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int mtp_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    return n;
}
int mtp_kbd_resident_start(struct mtp_kbd *k, hm_service *s)
{
    const struct mtp_io io = {NULL, mmio_read, mmio_write, now_us, control_poll, NULL, mtp_log};
    mtp_kbd_init(k, &io, s);
    if (attempted) goto fail;
    attempted = true;
    printf("MTP: stage 1/7 geometry+DAPF table check (ADT only, no MMIO)\n");
    /* Exact J716s geometry before the first hardware access. */
    if (!geometry("/arm-io/mtp", 0, 0x294600000, 0x88000) ||
        !geometry("/arm-io/dart-mtp", 1, 0x294810000, 0x4000) ||
        !geometry("/arm-io/dockchannel-mtp", 1, MTP_DC_IRQ_MASK, 8) ||
        !geometry("/arm-io/dockchannel-mtp", 2, 0x294b30000, 8) ||
        !geometry("/arm-io/dockchannel-mtp", 3, MTP_DATA, 0x30)) {
        printf("MTP: stage 1 FAILED: ADT geometry is not J716s\n"); goto fail;
    }
    if (!dapf_checked()) { printf("MTP: stage 1 FAILED: dart-mtp dapf-instance-0 differs\n"); goto fail; }
    /* Reads below: the registers Linux rtkit-helper/mailbox/dockchannel read on this SoC. */
    u32 cpu = read32(MTP_CPU_CONTROL);
    bool i2a_empty = read32(MTP_I2A_CONTROL) & (1u << 17);
    u32 rx = read32(MTP_RX_COUNT);
    printf("MTP: stage 2/7 entry state CPU_CONTROL=%#x mbox_i2a=%s fifo_rx_count=%u\n", cpu,
           i2a_empty ? "empty" : "PENDING", rx);
    if (cpu != 0 && cpu != 0x10) { printf("MTP: stage 2 FAILED: unknown CPU_CONTROL\n"); goto fail; }
    if (rx > MTP_FIFO_SIZE) { printf("MTP: stage 2 FAILED: RX_COUNT out of range\n"); goto fail; }
    if (cpu == 0 && i2a_empty && !rx) {
        printf("MTP: decision reset-state: clean (stopped, nothing pending) -> reset restored MTP\n");
    } else {
        /* Linux (rtkit-helper probe) starts and wakes the MTP whatever state it is in and
         * its receive paths drop stale data. Follow it: drain, then RUN + wake as usual. */
        unsigned msgs = 0, words = 0;
        while (msgs < 64 && !(read32(MTP_I2A_CONTROL) & (1u << 17))) {
            (void)read64(MTP_I2A_RECV0);
            (void)read64(MTP_I2A_RECV1);
            msgs++;
        }
        while (words < MTP_FIFO_SIZE / 4 && read32(MTP_RX_COUNT) >= 4) {
            (void)read32(MTP_RX32);
            words++;
        }
        bool clean = (read32(MTP_I2A_CONTROL) & (1u << 17)) && read32(MTP_RX_COUNT) < 4;
        printf("MTP: decision reset-state: NOT clean (CPU %s); drained %u mailbox msgs, %u FIFO "
               "words -> %s, continuing as Linux does\n", cpu ? "already running" : "stopped",
               msgs, words, clean ? "drained" : "STILL PENDING");
        if (!clean) goto fail;
    }
    printf("MTP: stage 3/7 RTKit instance (SRAM-only buffers 0x294c00000+0xc4000)\n");
    asc_dev_t *asc = asc_init("/arm-io/mtp");
    if (!asc) goto fail;
    mtp_rtkit = rtkit_init("mtp", asc, NULL, NULL, NULL, true);
    if (!mtp_rtkit) { asc_free(asc); goto fail; }
    rtkit_mtp_mode(mtp_rtkit, 0x294c00000, 0xc4000);
    printf("MTP: stage 4/7 DAPF init /arm-io/dart-mtp (as m1n1 kboot before Linux)\n");
    if (dapf_init("/arm-io/dart-mtp", 1) < 0) { printf("MTP: stage 4 FAILED: dapf_init\n"); goto fail; }
    /* Linux dockchannel probe: IRQ_MASK = 0, IRQ_FLAG = ~0 (write-only, no read-modify-write).
     * Thresholds are only programmed by Linux when arming an IRQ wait; polling never arms one
     * (m1n1 dockchannel_uart polls RX_COUNT the same way). */
    write32(MTP_DC_IRQ_MASK, 0);
    write32(MTP_DC_IRQ_FLAG, 0xffffffffu);
    printf("MTP: stage 5/7 FIFO: IRQ_MASK=0 IRQ_FLAG=~0 (Linux probe); decision fifo-polled: "
           "RX_COUNT/RX32 polled from the 1 ms tick, no IRQ armed\n");
    write32(MTP_CPU_CONTROL, 0x10);
    printf("MTP: stage 6/7 CPU_CONTROL=RUN; RTKit wake/HELLO/EPMAP/power (bounded ~16 s)\n");
    if (!rtkit_boot(mtp_rtkit)) { printf("MTP: stage 6 FAILED: RTKit boot\n"); goto fail; }
    printf("MTP: stage 7/7 waiting for interface INIT / enable (bounded 20 s + 10 s)\n");
    k->deadline = now_us(NULL) + 20000000;
    while (k->phase != MTP_LIVE && k->phase != MTP_FAILED) {
        mtp_kbd_poll(k);
        wdt_kick();
        udelay(50); /* preguest only */
    }
    if (k->phase != MTP_LIVE) return -1;
    printf("MTP: bring-up OK: keyboard iface=%u descriptor=%u bytes\n", k->iface,
           (unsigned)k->desc_len);
    return 0;
fail:
    mtp_kbd_fail(k, MTP_E_RTKit);
    /* Never quiesce/free a running MTP. Platform reset is the recovery boundary. */
    return -1;
}
