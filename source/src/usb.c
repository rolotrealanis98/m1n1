/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "usb.h"
#include "adt.h"
#include "dart.h"
#include "heapblock.h"
#include "i2c.h"
#include "iodev.h"
#include "malloc.h"
#include "pmgr.h"
#include "string.h"
#include "tps6598x.h"
#include "types.h"
#include "uartproxy.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "utils.h"
#include "vsprintf.h"
#include "xnuboot.h"

struct usb_drd_regs {
    uintptr_t drd_regs;
    uintptr_t drd_regs_unk3;
    uintptr_t atc;
};

#if USB_IODEV_COUNT > 100
#error "USB_IODEV_COUNT is limited to 100 to prevent overflow in ADT path names"
#endif

static int first_usb_iodev = !!USE_DEBUG_USB;

// length of the format string is is used as buffer size
// limits the USB instance numbers to reasonable 2 digits
#define FMT_DART_PATH        "/arm-io/dart-usb%u"
#define FMT_DART_MAPPER_PATH "/arm-io/dart-usb%u/mapper-usb%u"
#define FMT_ATC_PATH         "/arm-io/atc-phy%u"
#define FMT_DRD_PATH         "/arm-io/usb-drd%u"
// HPM_PATH string is at most
// "/arm-io/i2cX" (12) + "/" + hpmBusManagerX (14) + "/" + "hpmX" (4) + '\0'
#define MAX_HPM_PATH_LEN 40

static tps6598x_irq_state_t tps6598x_irq_state[USB_IODEV_COUNT];
static bool usb_is_initialized = false;

/* USB2 PHY regs */
#define USB2PHY_USBCTL           0x00
#define USB2PHY_USBCTL_USB_MODE  0x7
#define USB2PHY_USBCTL_RUN       2
#define USB2PHY_USBCTL_ISOLATION 4

#define USB2PHY_CTL             0x04
#define USB2PHY_CTL_RESET       BIT(0)
#define USB2PHY_CTL_PORT_RESET  BIT(1)
#define USB2PHY_CTL_APB_RESET_N BIT(2)
#define USB2PHY_CTL_SIDDQ       BIT(3)

#define USB2PHY_SIG                      0x08
#define USB2PHY_SIG_VBUSDET_FORCE_VAL    BIT(0)
#define USB2PHY_SIG_VBUSDET_FORCE_EN     BIT(1)
#define USB2PHY_SIG_VBUSVLDEXT_FORCE_VAL BIT(2)
#define USB2PHY_SIG_VBUSVLDEXT_FORCE_EN  BIT(3)
#define USB2PHY_SIG_HOST                 (7 << 12)

#define USB2PHY_MISCTUNE                 0x1c
#define USB2PHY_MISCTUNE_APBCLK_GATE_OFF BIT(29)
#define USB2PHY_MISCTUNE_REFCLK_GATE_OFF BIT(30)

#define PIPEHANDLER_MUX_CTRL             0x0c
#define PIPEHANDLER_MUX_CTRL_USB3        0x08
#define PIPEHANDLER_MUX_CTRL_USB4_TUNNEL 0x11
#define PIPEHANDLER_MUX_CTRL_DUMMY       0x22

/*
 * T6050 moved the MUX_CTRL fields: clock select in bits 6:4, data select in bits 1:0
 * (T8103: 5:3 and 2:0). The same dummy/dummy selection is 0x42 there; 0x22 reads as the
 * USB4 clock with dummy data, a state the kernel's T6050 combo PHY refuses to park from.
 */
#define PIPEHANDLER_MUX_CTRL_T6050_CLK   GENMASK(6, 4)
#define PIPEHANDLER_MUX_CTRL_T6050_DATA  GENMASK(1, 0)
#define PIPEHANDLER_MUX_CTRL_T6050_DUMMY 0x42

#define PIPEHANDLER_LOCK_REQ 0x10
#define PIPEHANDLER_LOCK_ACK 0x14
#define PIPEHANDLER_LOCK_EN  BIT(0)

#define PIPEHANDLER_AON_GEN                     0x1C
#define PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN BIT(4)
#define PIPEHANDLER_AON_GEN_DWC3_RESET_N        BIT(0)

#define PIPEHANDLER_NONSELECTED_OVERRIDE 0x20
#define PIPEHANDLER_NATIVE_RESET         BIT(12)
#define PIPEHANDLER_DUMMY_PHY_EN         BIT(15)
#define PIPEHANDLER_NATIVE_POWER_DOWN    GENMASK(3, 0)

static dart_dev_t *usb_dart_init(u32 idx)
{
    int mapper_offset;
    char path[sizeof(FMT_DART_MAPPER_PATH)];

    snprintf(path, sizeof(path), FMT_DART_MAPPER_PATH, idx, idx);
    mapper_offset = adt_path_offset(adt, path);
    if (mapper_offset < 0) {
        // Device not present
        return NULL;
    }

    u32 dart_idx;
    if (ADT_GETPROP(adt, mapper_offset, "reg", &dart_idx) < 0) {
        printf("usb: Error getting DART %s device index/\n", path);
        return NULL;
    }

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    return dart_init_adt(path, 1, dart_idx, false);
}

static int usb_drd_get_regs(u32 idx, struct usb_drd_regs *regs)
{
    int adt_drd_path[8];
    int adt_drd_offset;
    int adt_phy_path[8];
    int adt_phy_offset;
    char phy_path[sizeof(FMT_ATC_PATH)];
    char drd_path[sizeof(FMT_DRD_PATH)];

    snprintf(drd_path, sizeof(drd_path), FMT_DRD_PATH, idx);
    adt_drd_offset = adt_path_offset_trace(adt, drd_path, adt_drd_path);
    if (adt_drd_offset < 0) {
        // Nonexistent device
        return -1;
    }

    snprintf(phy_path, sizeof(phy_path), FMT_ATC_PATH, idx);
    adt_phy_offset = adt_path_offset_trace(adt, phy_path, adt_phy_path);
    if (adt_phy_offset < 0) {
        printf("usb: Error getting phy node %s\n", phy_path);
        return -1;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 0, &regs->atc, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", phy_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 0, &regs->drd_regs, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", drd_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 3, &regs->drd_regs_unk3, NULL) < 0) {
        printf("usb: Error getting reg with index 3 for %s.\n", drd_path);
        return -1;
    }

    return 0;
}

/*
 * Select the dummy PIPE producer in the T6050 field layout, as the kernel's combo PHY does
 * (combo_reset_assert(), apple_t6050_usb3_pipe_cold_dummy()): controller in reset and clamped,
 * then clock off, dummy data, dummy clock. The kernel PHY accepts only 0, 2 or 0x42 here when
 * dwc3-apple first asserts the controller reset. The caller releases the reset afterwards.
 */
static void usb_pipehandler_park_t6050(u32 idx, uintptr_t pipehandler)
{
    u32 before = read32(pipehandler + PIPEHANDLER_MUX_CTRL);

    /* An earlier stage (the chainloading m1n1's proxy) may have left the controller running. */
    clear32(pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    set32(pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    mask32(pipehandler + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_T6050_CLK, 0);
    udelay(10);
    mask32(pipehandler + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_T6050_DATA,
           FIELD_PREP(PIPEHANDLER_MUX_CTRL_T6050_DATA, 2));
    udelay(10);
    mask32(pipehandler + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_T6050_CLK,
           FIELD_PREP(PIPEHANDLER_MUX_CTRL_T6050_CLK, 4));
    udelay(10);

    u32 after = read32(pipehandler + PIPEHANDLER_MUX_CTRL);
    bool parked = (after & (PIPEHANDLER_MUX_CTRL_T6050_CLK | PIPEHANDLER_MUX_CTRL_T6050_DATA)) ==
                  PIPEHANDLER_MUX_CTRL_T6050_DUMMY;
    printf("usb: usb-drd%u: PIPE %s the dummy producer, MUX_CTRL %#x -> %#x\n", idx,
           parked ? "on" : "NOT on", before, after);
}

int usb_phy_bringup(u32 idx)
{
    char path[24];

    if (idx >= USB_IODEV_COUNT)
        return -1;

    struct usb_drd_regs usb_regs;
    if (usb_drd_get_regs(idx, &usb_regs) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_ATC_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DRD_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    write32(usb_regs.atc + USB2PHY_SIG, 0x01c1000f);
    write32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET);
    clear32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_RESET);
    clear32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
    write32(usb_regs.atc + USB2PHY_MISCTUNE, 0x008c0813);
    write32(usb_regs.atc + USB2PHY_USBCTL, USB2PHY_USBCTL_RUN);

    if (chip_id == T6050 || chip_id == T6051)
        usb_pipehandler_park_t6050(idx, usb_regs.drd_regs_unk3);
    else
        write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_DUMMY);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_NONSELECTED_OVERRIDE, 0x9332);

    return 0;
}

dwc3_dev_t *usb_iodev_bringup(u32 idx)
{
    dart_dev_t *usb_dart = usb_dart_init(idx);
    if (!usb_dart)
        return NULL;

    struct usb_drd_regs usb_reg;
    if (usb_drd_get_regs(idx, &usb_reg) < 0)
        return NULL;

    return usb_dwc3_init(usb_reg.drd_regs, usb_dart);
}

#define USB_IODEV_WRAPPER(name, pipe)                                                              \
    static ssize_t usb_##name##_can_read(void *dev)                                                \
    {                                                                                              \
        return usb_dwc3_can_read(dev, pipe);                                                       \
    }                                                                                              \
                                                                                                   \
    static bool usb_##name##_can_write(void *dev)                                                  \
    {                                                                                              \
        return usb_dwc3_can_write(dev, pipe);                                                      \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_read(void *dev, void *buf, size_t count)                           \
    {                                                                                              \
        return usb_dwc3_read(dev, pipe, buf, count);                                               \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_write(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_write(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_queue(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_queue(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_handle_events(void *dev)                                              \
    {                                                                                              \
        usb_dwc3_handle_events(dev);                                                               \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_flush(void *dev)                                                      \
    {                                                                                              \
        usb_dwc3_flush(dev, pipe);                                                                 \
    }

USB_IODEV_WRAPPER(0, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(1, CDC_ACM_PIPE_1)

static struct iodev_ops iodev_usb_ops = {
    .can_read = usb_0_can_read,
    .can_write = usb_0_can_write,
    .read = usb_0_read,
    .write = usb_0_write,
    .queue = usb_0_queue,
    .flush = usb_0_flush,
    .handle_events = usb_0_handle_events,
};

static struct iodev_ops iodev_usb_sec_ops = {
    .can_read = usb_1_can_read,
    .can_write = usb_1_can_write,
    .read = usb_1_read,
    .write = usb_1_write,
    .queue = usb_1_queue,
    .flush = usb_1_flush,
    .handle_events = usb_1_handle_events,
};

struct iodev iodev_usb_vuart = {
    .ops = &iodev_usb_sec_ops,
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

static int hpm_idx(char *hpm_path)
{
    const char *name = strrchr(hpm_path, '/');
    if (!name)
        return -1;
    if (strncmp(name + 1, "hpm", 3) != 0)
        return -1; // unexpected hpm node name
    u8 idx = name[4] - '0';
    if (idx > 9)
        return -2; // unexpected hpm index
    return idx;
}

static bool usb_init_match(char *hpm_path, void *)
{
    int idx = hpm_idx(hpm_path);
    // skip hpm0 depending on whether DEBUG_USB is in use or not
    if (idx < first_usb_iodev)
        return false;
    if (idx >= USB_IODEV_COUNT)
        return false;
    return true;
}

static int usb_init_one(char *hpm_path, tps6598x_dev_t *tps, void *)
{
    int idx = hpm_idx(hpm_path);

    bool ok = true;

    if (tps6598x_powerup(tps) < 0) {
        printf("usb: tps6598x_powerup failed for %s.\n", hpm_path);
        ok = false;
    }

    if (tps6598x_disable_irqs(tps, &tps6598x_irq_state[idx])) {
        printf("usb: unable to disable IRQ masks for %s.\n", hpm_path);
        ok = false;
    }

    if (ok && chip_id == T6050)
        printf("usb: %s: USB-PD controller in S0, IRQs masked\n", hpm_path);

    return 0;
}

void usb_init(void)
{
    if (usb_is_initialized)
        return;

    /*
     * A7-A11 uses a custom internal otg controller with the peripheral part
     * being dwc2.
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0) {
        /* We do not support the custom controller and dwc2 (yet). */
        return;
    }

    /*
     * T6050 (M5 Pro/Max) too: the sn201202x USB-PD controllers on the gen-4 nub-spmi buses
     * are put in S0 like on every other machine, so a port in host mode gets VBUS. SPMI
     * register selection, stale RX draining and every HPM command are bounded (spmi.c,
     * tps6598x.c), so an unresponsive controller costs time instead of hanging usb_init.
     */
    tps6598x_foreach_hpm(usb_init_match, usb_init_one, NULL, true);

    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

static bool usb_hpm_restore_irqs_match(char *hpm_path, void *state)
{
    int idx = hpm_idx(hpm_path);
    // skip hpm0 depending on whether DEBUG_USB is in use or not
    if (idx < first_usb_iodev)
        return false;

    bool force = *(bool *)state;
    if (iodev_get_usage(IODEV_USB0 + idx) && !force)
        return false;

    if (!tps6598x_irq_state[idx].valid)
        return false;

    return true;
}

static int usb_hpm_restore_irqs_one(char *hpm_path, tps6598x_dev_t *tps, void *)
{
    int idx = hpm_idx(hpm_path);

    if (tps6598x_restore_irqs(tps, &tps6598x_irq_state[idx]))
        printf("usb: unable to restore IRQ masks for %s\n", hpm_path);

    return 0;
}

void usb_hpm_restore_irqs(bool force)
{
    /*
     * Do not try to restore irqs on A7-A11 which don't use i2c
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0)
        return;

    tps6598x_foreach_hpm(usb_hpm_restore_irqs_match, usb_hpm_restore_irqs_one, &force, true);
}

void usb_iodev_init(void)
{
    for (int i = first_usb_iodev; i < USB_IODEV_COUNT; i++) {
        dwc3_dev_t *opaque;
        struct iodev *usb_iodev;

        opaque = usb_iodev_bringup(i);
        if (!opaque)
            continue;

        usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
        if (!usb_iodev)
            continue;

        usb_iodev->ops = &iodev_usb_ops;
        usb_iodev->opaque = opaque;
        usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
        spin_init(&usb_iodev->lock);

        iodev_register_device(IODEV_USB0 + i, usb_iodev);
        printf("USB%d: initialized at %p\n", i, opaque);
    }
}

int usb_iodev_release(u32 idx)
{
    if (idx >= USB_IODEV_COUNT || IODEV_USB0 + idx == uartproxy_iodev)
        return -1;

    void *opaque = iodev_get_opaque(IODEV_USB0 + idx);
    if (opaque && iodev_usb_vuart.opaque == opaque)
        return -1;

    struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0 + idx);
    if (!usb_iodev)
        return 0;

    printf("USB%d: released for a guest host controller\n", idx);
    usb_dwc3_shutdown(usb_iodev->opaque);
    free(usb_iodev);
    return 0;
}

void usb_iodev_shutdown(void)
{
    for (int i = first_usb_iodev; i < USB_IODEV_COUNT; i++) {
        struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0 + i);
        if (!usb_iodev)
            continue;

        printf("USB%d: shutdown\n", i);
        usb_dwc3_shutdown(usb_iodev->opaque);
        free(usb_iodev);
    }
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}

int usb_enable_debugusb(void)
{
    if (usb_is_initialized) {
        printf("usb: debugusb can not be enabled after USB is initialized\n");
        return -1;
    }

    first_usb_iodev = 1;
    tps6598x_enable_debugusb();
    printf("usb: waiting 1000ms after enabling debugusb\n");
    mdelay(1000);

    // reset console read pointer to replay the console log
    iodev_reset_rp(IODEV_DOCKCHANNEL_UART);

    return 0;
}

/*
 * ---- Host preparation for a guest's xHCI driver ----------------------------------------------
 *
 * m5pro-windows docs/USB-HOST.md. Two USB2 PHY recipes, both running on this hardware under Linux:
 *   native (default): Aurora's T6050 combo PHY (drivers/phy/apple/usb-t6050/apple_t6050_usb_phy.h,
 *     apple_t6050_usb2_stop() then apple_t6050_usb2_host[]), with the ADT's USB2PHY default and
 *     host tunables, plus the dwc3-apple CIO values and the controller's ADT tunables, SUSPHY on;
 *     what the J716s daily kernel does in host mode on the left-front port.
 *   legacy (USB_HOST_F_LEGACY_PHY): usb_phy_bringup()'s values with the host bits
 * USB2PHY_SIG[14:12] (m5pro-linux contrib/validated/legacy-7.2/usb, phy-apple-usb2), no controller
 * tunables, SUSPHY off; the 7.2-kernel USB 2.0 host of 2026-11-06 (R2-B). Both leave USB3 off: the
 * PIPE stays on the dummy producer, the port runs at high speed.
 */

#define USB2_DIGITAL_CFG      0x00 /* atc-phyN reg[1] ("usb2-digital"), bits 0 and 3 set in host */
#define USB2_DIGITAL_CFG_HOST (BIT(0) | BIT(3))

#define USB_HOST_DRD_TUNABLE_SPAN 0x11800 /* dwc3 core window, ADT usb-drdN reg[0] */

struct usb_host_tunable {
    const char *prop;
    bool on_drd; /* usb-drdN, else atc-phyN */
    u32 reg_idx; /* "reg" entry of that node holding the bank */
    u32 offset;  /* bank start inside that entry */
    u32 span;    /* bytes from the bank start that records may touch */
};

/* atc-phyN reg[0]: the USB2 PHY bank (bank 0 of the T6050 combo PHY) */
static const struct usb_host_tunable usb_host_usb2_tunables[] = {
    {"tunable_USB2PHY_DFLT", false, 0, 0, 0x4000},
    {"tunable_USB2PHY_HOST", false, 0, 0, 0x4000},
};

/*
 * dwc3-apple.c dwc3_apple_tunable_list in application order, as the J716s daily DT carries them
 * (gbl-default, gbl-host, cio, dbg, cfg-host, link; bulk and ausbc are not described there).
 */
static const struct usb_host_tunable usb_host_drd_tunables[] = {
    {"tunable_DRD_USB31_GBL_DEFAULT", true, 0, 0xc100, USB_HOST_DRD_TUNABLE_SPAN - 0xc100},
    {"tunable_DRD_USB31_GBL_HOST", true, 0, 0xc100, USB_HOST_DRD_TUNABLE_SPAN - 0xc100},
    {"tunable_DRD_CIO_REGS_DEFAULT", true, 0, 0xcd20, USB_HOST_DRD_TUNABLE_SPAN - 0xcd20},
    {"tunable_DRD_USB31_DBG_DEFAULT", true, 0, 0xd800, 0x800},
    {"tunable_DRD_USB31_CFG_HOST", true, 1, 0, 0x4000},
    {"tunable_LINK_REGS_DEFAULT", true, 0, 0xd000, 0x800},
};

/* ADT ATC/DRD tunable record: offset in bits 23:0, access size in bits (32) in 31:24 */
struct usb_host_tunable_rec {
    u32 tag;
    u32 mask;
    u32 value;
} PACKED;

struct usb_host_port {
    u32 idx;
    char drd_path[sizeof(FMT_DRD_PATH)];
    char atc_path[sizeof(FMT_ATC_PATH)];
    char dart_path[sizeof(FMT_DART_PATH)];
    int drd_trace[8], atc_trace[8], dart_trace[8];
    int drd_node, atc_node, dart_node;
    u64 core, core_size; /* usb-drdN reg[0]: DWC3 + xHCI */
    u64 pipehandler;     /* usb-drdN reg[3] */
    u64 usb2phy;         /* atc-phyN reg[0] */
    u64 usb2dig;         /* atc-phyN reg[1] */
    u64 dart[2];         /* dart-usbN reg[0] (LLT) and reg[1] (BLK) */
};

static struct dart_identity usb_host_ident[USB_IODEV_COUNT];

static int usb_host_tunable_bank(const struct usb_host_port *p, const struct usb_host_tunable *t,
                                 u64 *bank)
{
    u64 base, size;
    int *trace = (int *)(t->on_drd ? p->drd_trace : p->atc_trace);

    if (adt_get_reg(adt, trace, "reg", t->reg_idx, &base, &size) < 0 || size < t->offset + t->span)
        return -1;
    *bank = base + t->offset;
    return 0;
}

/* Check every record (apply = false) or write them (apply = true). */
static int usb_host_tunables(const struct usb_host_port *p, const struct usb_host_tunable *list,
                             size_t n, bool apply)
{
    for (size_t i = 0; i < n; i++) {
        const struct usb_host_tunable *t = &list[i];
        int node = t->on_drd ? p->drd_node : p->atc_node;
        u32 len;
        const struct usb_host_tunable_rec *rec = adt_getprop(adt, node, t->prop, &len);
        u64 bank;

        if (!rec || !len || len % sizeof(*rec) || usb_host_tunable_bank(p, t, &bank)) {
            printf("usb: %s: %s missing or malformed\n", t->on_drd ? p->drd_path : p->atc_path,
                   t->prop);
            return -1;
        }
        for (u32 j = 0; j < len / sizeof(*rec); j++) {
            u32 off = rec[j].tag & 0xffffff;
            if ((rec[j].tag >> 24) != 32 || off & 3 || off + 4 > t->span ||
                rec[j].value & ~rec[j].mask) {
                printf("usb: %s: %s record %u is malformed\n",
                       t->on_drd ? p->drd_path : p->atc_path, t->prop, j);
                return -1;
            }
            if (apply)
                mask32(bank + off, rec[j].mask, rec[j].value);
        }
    }
    return 0;
}

static int usb_host_get_port(u32 idx, struct usb_host_port *p)
{
    memset(p, 0, sizeof(*p));
    p->idx = idx;
    snprintf(p->drd_path, sizeof(p->drd_path), FMT_DRD_PATH, idx);
    snprintf(p->atc_path, sizeof(p->atc_path), FMT_ATC_PATH, idx);
    snprintf(p->dart_path, sizeof(p->dart_path), FMT_DART_PATH, idx);

    p->drd_node = adt_path_offset_trace(adt, p->drd_path, p->drd_trace);
    p->atc_node = adt_path_offset_trace(adt, p->atc_path, p->atc_trace);
    p->dart_node = adt_path_offset_trace(adt, p->dart_path, p->dart_trace);
    if (p->drd_node < 0 || p->atc_node < 0 || p->dart_node < 0)
        return -1;
    if (!adt_is_compatible(adt, p->dart_node, "dart,t8110"))
        return -1;

    u64 dart_size[2];
    if (adt_get_reg(adt, p->drd_trace, "reg", 0, &p->core, &p->core_size) < 0 ||
        adt_get_reg(adt, p->drd_trace, "reg", 3, &p->pipehandler, NULL) < 0 ||
        adt_get_reg(adt, p->atc_trace, "reg", 0, &p->usb2phy, NULL) < 0 ||
        adt_get_reg(adt, p->atc_trace, "reg", 1, &p->usb2dig, NULL) < 0 ||
        adt_get_reg(adt, p->dart_trace, "reg", 0, &p->dart[0], &dart_size[0]) < 0 ||
        adt_get_reg(adt, p->dart_trace, "reg", 1, &p->dart[1], &dart_size[1]) < 0)
        return -1;
    /* T6050/T6051 exposes full v2 exception banks in each 0xc000 region. */
    if (p->core_size < 0xd800 + 0x800 || dart_size[0] < 0xc000 || dart_size[1] < 0xc000)
        return -1;
    return 0;
}

/* Identity ranges: valid for dart_identity_build, inside DRAM, clear of m1n1 and its heap. */
static int usb_host_check_ranges(const struct dart_range *r, u32 count)
{
    int node = adt_path_offset(adt, "/chosen");
    u64 dram_base = 0, dram_size = 0;
    u64 m1n1_lo = cur_boot_args.phys_base, m1n1_hi = (u64)heapblock_alloc(0);

    if (node < 0 || ADT_GETPROP(adt, node, "dram-base", &dram_base) < 0 ||
        ADT_GETPROP(adt, node, "dram-size", &dram_size) < 0 || !dram_size)
        return -1;

    for (u32 i = 0; i < count; i++) {
        u64 b = r[i].base, e = r[i].base + r[i].size;
        if (e < b || b < dram_base || e > dram_base + dram_size) {
            printf("usb: identity range 0x%lx..0x%lx is outside DRAM 0x%lx+0x%lx\n", b, e,
                   dram_base, dram_size);
            return -1;
        }
        if (b < m1n1_hi && m1n1_lo < e) {
            printf("usb: identity range 0x%lx..0x%lx overlaps m1n1 and its heap 0x%lx..0x%lx\n", b,
                   e, m1n1_lo, m1n1_hi);
            return -1;
        }
        u64 pb, ps, pu;
        heapblock_pool_info(&pb, &ps, &pu);
        if (ps && b < pb + ps && pb < e) { /* n5c: the whole pool, allocated or not */
            printf("usb: identity range 0x%lx..0x%lx overlaps the resident pool 0x%lx+0x%lx\n", b, e, pb, ps);
            return -1;
        }
    }
    return 0;
}

struct usb_host_pd {
    u32 idx;
    bool found;
    bool s0;
};

/* hpmN serves usb-drdN, as usb_init() assumes (J716s ADT: hpm1 port-number 2 = usb-drd1) */
static bool usb_host_pd_match(char *hpm_path, void *data)
{
    const struct usb_host_pd *pd = data;
    const char *name = strrchr(hpm_path, '/');

    return name && !strncmp(name + 1, "hpm", 3) && name[4] == (char)('0' + pd->idx) && !name[5];
}

static int usb_host_pd_s0(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    struct usb_host_pd *pd = data;

    pd->found = true;
    pd->s0 = tps6598x_powerup(tps) == 0;
    printf("usb: %s: USB-PD controller %s\n", hpm_path, pd->s0 ? "in S0" : "NOT in S0");
    return HPM_ACTION_STOP;
}

static void usb_host_hold_reset(const struct usb_host_port *p)
{
    clear32(p->pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    set32(p->pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
}

/* Asahi atcphy_dwc3_reset_deassert(): unclamp first, then release the reset */
static void usb_host_release_reset(const struct usb_host_port *p)
{
    clear32(p->pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    set32(p->pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
}

/* apple_t6050_usb2_stop() */
static void usb_host_usb2_stop(const struct usb_host_port *p)
{
    if (!(read32(p->usb2phy + USB2PHY_CTL) & USB2PHY_CTL_PORT_RESET)) {
        set32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
        mdelay(5);
    }
    mask32(p->usb2phy + USB2PHY_USBCTL, USB2PHY_USBCTL_USB_MODE, USB2PHY_USBCTL_ISOLATION);
    set32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
    set32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET);
    set32(p->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
    set32(p->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
    udelay(1);
}

/* apple_t6050_usb2_host[]: default and host tunables, out of reset, run */
static void usb_host_usb2_native(const struct usb_host_port *p)
{
    usb_host_tunables(p, usb_host_usb2_tunables, ARRAY_SIZE(usb_host_usb2_tunables), true);
    mdelay(10);
    clear32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
    udelay(10);
    clear32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET);
    clear32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
    set32(p->usb2dig + USB2_DIGITAL_CFG, USB2_DIGITAL_CFG_HOST);
    set32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
    clear32(p->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
    clear32(p->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
    udelay(30);
    mdelay(5);
    mask32(p->usb2phy + USB2PHY_USBCTL, USB2PHY_USBCTL_USB_MODE, USB2PHY_USBCTL_RUN);
}

/* usb_phy_bringup()'s USB2 values with the host signalling bits (R2-B, 7.2 kernel) */
static void usb_host_usb2_legacy(const struct usb_host_port *p)
{
    write32(p->usb2phy + USB2PHY_SIG, 0x01c1000f | USB2PHY_SIG_HOST);
    write32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET);
    udelay(10);
    clear32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET);
    udelay(10);
    clear32(p->usb2phy + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
    udelay(10);
    write32(p->usb2phy + USB2PHY_MISCTUNE, 0x008c0813);
    write32(p->usb2phy + USB2PHY_USBCTL, USB2PHY_USBCTL_RUN);
    udelay(10);
}

static int usb_host_usb2_check(const struct usb_host_port *p)
{
    u32 usbctl = read32(p->usb2phy + USB2PHY_USBCTL), ctl = read32(p->usb2phy + USB2PHY_CTL);
    u32 sig = read32(p->usb2phy + USB2PHY_SIG);
    u32 resets = USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET | USB2PHY_CTL_SIDDQ;

    printf("usb: %s: USB2 PHY USBCTL %08x CTL %08x SIG %08x MISCTUNE %08x\n", p->atc_path, usbctl,
           ctl, sig, read32(p->usb2phy + USB2PHY_MISCTUNE));
    if ((usbctl & USB2PHY_USBCTL_USB_MODE) != USB2PHY_USBCTL_RUN || (ctl & resets) ||
        (sig & USB2PHY_SIG_HOST) != USB2PHY_SIG_HOST)
        return -1;
    return 0;
}

static int usb_host_attach_darts(const struct usb_host_port *p, u32 flags,
                                 const struct dart_range *ranges, u32 count, u64 *info)
{
    struct dart_identity *id = &usb_host_ident[p->idx];
    u64 top = ranges[count - 1].base + ranges[count - 1].size;

    for (int i = 0; i < 2; i++) {
        if (dart_t8110_locked(p->dart[i])) {
            printf("usb: %s reg[%d] is locked\n", p->dart_path, i);
            return -1;
        }
        if (dart_t8110_block(p->dart[i]))
            return -1;
    }

    if (flags & USB_HOST_F_DART_BYPASS) {
        int bypass = dart_t8110_attach_bypass(p->dart[0]);
        if (!bypass)
            bypass = dart_t8110_attach_bypass(p->dart[1]);
        if (!bypass) {
            printf("usb: %s: both instances bypass (the controller reaches all of RAM)\n",
                   p->dart_path);
            *info |= USB_HOST_INFO_BYPASS;
            return 0;
        }
        if (bypass < -1)
            return -1; /* A flush timeout is not a refusal of bypass bits. */
        int blocked0 = dart_t8110_block(p->dart[0]);
        int blocked1 = dart_t8110_block(p->dart[1]);
        if (blocked0 || blocked1)
            return -1;
        printf("usb: %s: bypass refused, falling back to the identity map\n", p->dart_path);
    }

    for (int i = 0; i < 2; i++) {
        u32 va = dart_t8110_va_width(p->dart[i]);
        if (va <= 36 || (va < 64 && top > (1ULL << va))) {
            printf("usb: %s reg[%d]: VA width %u cannot hold DVA == PA up to 0x%lx\n", p->dart_path,
                   i, va, top);
            return -1;
        }
    }

    dart_identity_free(id);
    if (dart_identity_build(id, ranges, count))
        return -1;
    for (int i = 0; i < 2; i++) {
        if (dart_t8110_attach_identity(p->dart[i], id)) {
            int blocked0 = dart_t8110_block(p->dart[0]);
            int blocked1 = dart_t8110_block(p->dart[1]);
            /* A stalled invalidation may retain table references: keep them allocated. */
            if (!blocked0 && !blocked1)
                dart_identity_free(id);
            return -1;
        }
    }
    /* Do not lock the first instance before the second has attached successfully. */
    if (flags & USB_HOST_F_LOCK_DART)
        for (int i = 0; i < 2; i++)
            dart_t8110_lock(p->dart[i]);
    printf("usb: %s: identity map on every stream of both instances, %u tables at 0x%lx, %u "
           "range(s) up to 0x%lx, ERROR %08x/%08x\n",
           p->dart_path, id->ntables, id->root, count, top, dart_t8110_error(p->dart[0]),
           dart_t8110_error(p->dart[1]));
    *info |= USB_HOST_INFO_IDENTITY | FIELD_PREP(USB_HOST_INFO_TABLES, min(id->ntables, 0xffffU));
    return 0;
}

static void usb_host_usb2(const struct usb_host_port *p, bool legacy)
{
    usb_host_usb2_stop(p);
    if (legacy)
        usb_host_usb2_legacy(p);
    else
        usb_host_usb2_native(p);
}

s64 usb_host_prepare(u32 port, u32 flags, const struct dart_range *ranges, u32 count)
{
    struct usb_host_port p;
    struct usb_xhci_info xhci;
    bool legacy = flags & USB_HOST_F_LEGACY_PHY;
    u64 info = legacy ? USB_HOST_INFO_LEGACY_PHY : 0;
    s64 err;

    if (port >= USB_IODEV_COUNT || (flags & ~USB_HOST_F_MASK))
        return USB_HOST_E_ARGS;
    if (chip_id != T6050 && chip_id != T6051)
        return USB_HOST_E_CHIP;
    if (usb_host_get_port(port, &p) < 0) {
        printf("usb: port %u: no usb-drd/atc-phy/dart-usb (t8110) set in the ADT\n", port);
        return USB_HOST_E_PORT;
    }
    void *opaque = iodev_get_opaque(IODEV_USB0 + port);
    if (IODEV_USB0 + port == uartproxy_iodev || (opaque && iodev_usb_vuart.opaque == opaque)) {
        printf("usb: port %u carries the proxy or the vuart; refusing\n", port);
        return USB_HOST_E_BUSY;
    }
    if (!ranges || !count || count > 64 || usb_host_check_ranges(ranges, count))
        return USB_HOST_E_RANGES;
    if (!legacy &&
        (usb_host_tunables(&p, usb_host_usb2_tunables, ARRAY_SIZE(usb_host_usb2_tunables), false) ||
         usb_host_tunables(&p, usb_host_drd_tunables, ARRAY_SIZE(usb_host_drd_tunables), false)))
        return USB_HOST_E_TUNABLES;

    printf("usb: preparing %s (core 0x%lx) for a guest xHCI driver, %s USB2 PHY, flags 0x%x\n",
           p.drd_path, p.core, legacy ? "legacy" : "native", flags);

    if (pmgr_adt_power_enable(p.atc_path) < 0 || pmgr_adt_power_enable(p.dart_path) < 0 ||
        pmgr_adt_power_enable(p.drd_path) < 0)
        return USB_HOST_E_PORT;

    /* usb_init() put every non-debug HPM in S0 at boot; make sure this one still is */
    if (!(flags & USB_HOST_F_KEEP_PD)) {
        struct usb_host_pd pd = {.idx = port};
        tps6598x_foreach_hpm(usb_host_pd_match, usb_host_pd_s0, &pd, true);
        if (!pd.found || !pd.s0) {
            printf("usb: hpm%u %s\n", port, pd.found ? "did not reach S0" : "not found");
            return USB_HOST_E_PD;
        }
    }

    /* From here the controller is ours: stop m1n1's gadget (and its DMA) on this port */
    if (usb_iodev_release(port) < 0)
        return USB_HOST_E_BUSY;

    usb_host_hold_reset(&p);
    usb_pipehandler_park_t6050(port, p.pipehandler);
    if ((read32(p.pipehandler + PIPEHANDLER_MUX_CTRL) &
         (PIPEHANDLER_MUX_CTRL_T6050_CLK | PIPEHANDLER_MUX_CTRL_T6050_DATA)) !=
        PIPEHANDLER_MUX_CTRL_T6050_DUMMY) {
        err = USB_HOST_E_PIPE;
        goto fail;
    }

    if (usb_host_attach_darts(&p, flags, ranges, count, &info)) {
        err = USB_HOST_E_DART;
        goto fail;
    }

    if (flags & USB_HOST_F_PHY_BEFORE_RESET)
        usb_host_usb2(&p, legacy);
    usb_host_release_reset(&p);
    if (!(flags & USB_HOST_F_PHY_BEFORE_RESET))
        usb_host_usb2(&p, legacy);
    if (usb_host_usb2_check(&p)) {
        err = USB_HOST_E_PHY;
        goto fail;
    }

    if (usb_dwc3_host_core_init(p.core)) {
        err = USB_HOST_E_DWC3;
        goto fail;
    }
    if (!legacy) {
        usb_dwc3_apple_setup_cio(p.core);
        usb_host_tunables(&p, usb_host_drd_tunables, ARRAY_SIZE(usb_host_drd_tunables), true);
    }
    usb_dwc3_host_select(p.core, !legacy);
    printf("usb: %s: DWC3 host role, GCTL %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x, PIPE %08x, "
           "AON %08x\n",
           p.drd_path, read32(p.core + DWC3_GCTL), read32(p.core + DWC3_GUSB2PHYCFG(0)),
           read32(p.core + DWC3_GUSB3PIPECTL(0)), read32(p.pipehandler + PIPEHANDLER_MUX_CTRL),
           read32(p.pipehandler + PIPEHANDLER_AON_GEN));

    if (!(flags & USB_HOST_F_NO_HCRST)) {
        if (usb_xhci_reset(p.core, &xhci)) {
            err = USB_HOST_E_XHCI;
            goto fail;
        }
        info |= FIELD_PREP(USB_HOST_INFO_PORTS, xhci.max_ports) |
                FIELD_PREP(USB_HOST_INFO_HCIVERSION, xhci.hciversion) |
                FIELD_PREP(USB_HOST_INFO_CCS, xhci.ccs_ports & 0xff) |
                (xhci.ac64 ? USB_HOST_INFO_AC64 : 0);
        printf("usb: %s: xHCI %x.%02x, %u ports, %u slots, %s, halted and reset\n", p.drd_path,
               xhci.hciversion >> 8, xhci.hciversion & 0xff, xhci.max_ports, xhci.max_slots,
               xhci.ac64 ? "64-bit DMA" : "32-bit DMA only");
    }

    printf("usb: %s ready for the guest: xHCI window 0x%lx+0x%lx, info 0x%lx\n", p.drd_path, p.core,
           p.core_size, info);
    return info;

fail:
    usb_host_hold_reset(&p);
    printf("usb: %s: host preparation failed (%ld); controller held in reset\n", p.drd_path, err);
    return err;
}
