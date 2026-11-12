/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef USB_DWC3_H
#define USB_DWC3_H

#include "dart.h"
#include "types.h"

typedef struct dwc3_dev dwc3_dev_t;

typedef enum _cdc_acm_pipe_id_t {
    CDC_ACM_PIPE_0,
    CDC_ACM_PIPE_1,
    CDC_ACM_PIPE_MAX
} cdc_acm_pipe_id_t;

dwc3_dev_t *usb_dwc3_init(uintptr_t regs, dart_dev_t *dart);
void usb_dwc3_shutdown(dwc3_dev_t *dev);

void usb_dwc3_handle_events(dwc3_dev_t *dev);

ssize_t usb_dwc3_can_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc3_can_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
size_t usb_dwc3_write_free(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe); /* nightly-12 */

u8 usb_dwc3_getbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc3_putbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);

size_t usb_dwc3_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc3_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc3_queue(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc3_flush(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);

/*
 * Host role for a controller a guest's xHCI driver takes over (usb.c usb_host_prepare). No DMA
 * is set up here: the guest's driver allocates every xHCI structure itself.
 */
int usb_dwc3_host_core_init(uintptr_t regs);
void usb_dwc3_apple_setup_cio(uintptr_t regs);
/* host PRTCAP; susphy: PHY suspend on (Apple glue), else off with SLEEPM off (legacy quirks) */
void usb_dwc3_host_select(uintptr_t regs, bool susphy);

struct usb_xhci_info {
    u32 hciversion;
    u32 caplength;
    u32 max_ports;
    u32 max_slots;
    bool ac64;
    u32 ccs_ports; /* bitmap of ports 1..32 with CurrentConnectStatus after the reset */
};
/* Halt and reset the xHC (USBCMD.HCRST), leave it halted; 0 or -1 */
int usb_xhci_reset(uintptr_t regs, struct usb_xhci_info *info);

#endif
