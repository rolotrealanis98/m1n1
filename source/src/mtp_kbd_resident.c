/* SPDX-License-Identifier: MIT */
#include "mtp_kbd.h"
#include "adt.h"
#include "asc.h"
#include "rtkit.h"
#include "dapf.h"
#include "utils.h"
#include "wdt.h"

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
int mtp_kbd_resident_start(struct mtp_kbd *k, hm_service *s)
{
    const struct mtp_io io = {NULL, mmio_read, mmio_write, now_us, control_poll, NULL};
    mtp_kbd_init(k, &io, s);
    if (attempted) goto fail;
    attempted = true;
    /* Exact J716s geometry before the first hardware access. */
    if (!geometry("/arm-io/mtp", 0, 0x294600000, 0x88000) ||
        !geometry("/arm-io/dart-mtp", 1, 0x294810000, 0x4000) ||
        !geometry("/arm-io/dockchannel-mtp", 1, 0x294b14000, 8) ||
        !geometry("/arm-io/dockchannel-mtp", 2, 0x294b30000, 8) ||
        !geometry("/arm-io/dockchannel-mtp", 3, MTP_DATA, 0x30) || !dapf_checked()) goto fail;
    u32 cpu = read32(0x294600044);
    if (cpu != 0 && cpu != 0x10) goto fail;
    /* A live/stale producer is not adopted without a platform reset. */
    if (!(read32(0x294608114) & (1u << 17)) || read32(MTP_RX_COUNT)) goto fail;
    asc_dev_t *asc = asc_init("/arm-io/mtp");
    if (!asc) goto fail;
    mtp_rtkit = rtkit_init("mtp", asc, NULL, NULL, NULL, true);
    if (!mtp_rtkit) { asc_free(asc); goto fail; }
    rtkit_mtp_mode(mtp_rtkit, 0x294c00000, 0xc4000);
    if (dapf_init("/arm-io/dart-mtp", 1) < 0) goto fail;
    /* Polled input channel (index 1); preserve other channels' interrupt bits. */
    write32(0x294b14000, read32(0x294b14000) & ~0xcu);
    write32(0x294b30000, 0);
    write32(0x294b30004, 1);
    write32(0x294600044, 0x10);
    if (!rtkit_boot(mtp_rtkit)) goto fail;
    k->deadline = now_us(NULL) + 20000000;
    while (k->phase != MTP_LIVE && k->phase != MTP_FAILED) {
        mtp_kbd_poll(k);
        wdt_kick();
        udelay(50); /* preguest only */
    }
    if (k->phase != MTP_LIVE) return -1;
    return 0;
fail:
    mtp_kbd_fail(k, MTP_E_RTKit);
    /* Never quiesce/free a running MTP. Platform reset is the recovery boundary. */
    return -1;
}
