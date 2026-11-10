/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef USB_H
#define USB_H

#include "iodev.h"
#include "types.h"
#include "usb_dwc3.h"

dwc3_dev_t *usb_bringup(u32 idx);

void usb_init(void);
void usb_hpm_restore_irqs(bool force);
void usb_iodev_init(void);
void usb_iodev_shutdown(void);
void usb_iodev_vuart_setup(iodev_id_t iodev);

int usb_enable_debugusb(void);

/* Stop m1n1's own CDC-ACM gadget on a port (never the proxy's). 0 also when none is running. */
int usb_iodev_release(u32 idx);

/*
 * Host preparation of one Type-C port for a guest's xHCI driver (P_HV_WIN_USB_PREP, m5pro-windows
 * docs/USB-HOST.md): USB-PD controller in S0, m1n1's gadget stopped, both instances of the port's
 * DART translating DVA == PA over `ranges` only (or bypassing, if asked and the hardware allows),
 * USB2 PHY in host mode with the PIPE on the dummy producer, DWC3 out of reset in the host role,
 * xHC reset and halted. T6050/T6051 only. Returns a USB_HOST_INFO_* word (>= 0) or a
 * USB_HOST_E_* error; on an error after the gadget is stopped the controller is held in reset.
 */
/*
 * Flags: DART_BYPASS tries bypass first (the master then reaches all of RAM); LEGACY_PHY uses
 * m1n1's PHY values plus the host bits, no ADT tunables and no SUSPHY; NO_HCRST leaves the xHC as
 * the core reset left it; LOCK_DART sets the DARTs' TTBR/TCR protection until reset; KEEP_PD
 * does not touch the USB-PD controller; PHY_BEFORE_RESET runs the USB2 PHY sequence while DWC3 is
 * still held in reset (Linux's native order) instead of after its release.
 */
#define USB_HOST_F_DART_BYPASS      BIT(0)
#define USB_HOST_F_LEGACY_PHY       BIT(1)
#define USB_HOST_F_NO_HCRST         BIT(2)
#define USB_HOST_F_LOCK_DART        BIT(3)
#define USB_HOST_F_KEEP_PD          BIT(4)
#define USB_HOST_F_PHY_BEFORE_RESET BIT(5)
#define USB_HOST_F_MASK             GENMASK(5, 0)

#define USB_HOST_INFO_IDENTITY   BIT(0)
#define USB_HOST_INFO_BYPASS     BIT(1)
#define USB_HOST_INFO_AC64       BIT(2)
#define USB_HOST_INFO_LEGACY_PHY BIT(3)
#define USB_HOST_INFO_PORTS      GENMASK(15, 8) /* HCSPARAMS1.MaxPorts */
#define USB_HOST_INFO_HCIVERSION GENMASK(31, 16)
#define USB_HOST_INFO_TABLES     GENMASK(47, 32) /* identity tables (16 KiB each) */
#define USB_HOST_INFO_CCS        GENMASK(55, 48) /* ports 1..8 with a device after the reset */

enum usb_host_error {
    USB_HOST_E_ARGS = -1,     /* bad port number or flags */
    USB_HOST_E_CHIP = -2,     /* not a T6050/T6051 */
    USB_HOST_E_PORT = -3,     /* no usb-drdN/atc-phyN/dart-usbN in the ADT */
    USB_HOST_E_BUSY = -4,     /* the proxy's or the vuart's port, or a guest is running */
    USB_HOST_E_RANGES = -5,   /* identity ranges invalid, outside DRAM or over m1n1 */
    USB_HOST_E_PD = -6,       /* USB-PD controller not found or not in S0 */
    USB_HOST_E_TUNABLES = -7, /* an ADT tunable is missing or malformed */
    USB_HOST_E_PIPE = -8,     /* PIPE not on the dummy producer */
    USB_HOST_E_DART = -9,     /* DART locked, wrong type, too narrow, or refused the tables */
    USB_HOST_E_PHY = -10,     /* USB2 PHY did not reach the run state */
    USB_HOST_E_DWC3 = -11,    /* DWC3 core init failed */
    USB_HOST_E_XHCI = -12,    /* xHC missing or reset failed */
};

struct dart_range;
s64 usb_host_prepare(u32 port, u32 flags, const struct dart_range *ranges, u32 count);

#endif
