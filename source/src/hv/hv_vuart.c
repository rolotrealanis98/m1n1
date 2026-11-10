/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "hv.h"
#include "aic.h"
#include "iodev.h"
#include "uart.h"
#include "uart_regs.h"
#include "usb.h"

#include "win/hv_win.h"

bool active = false;

u32 ucon = 0;
u32 utrstat = 0;
u32 ufstat = 0;

int vuart_irq = 0;

static void update_irq(void)
{
    ssize_t rx_queued;

    hv_win_diag_note(HV_WIN_DIAG_USB_EVENTS, NULL, 0);
    iodev_handle_events(IODEV_USB_VUART);
    hv_win_diag_note(HV_WIN_DIAG_USB_EVENTS_DONE, NULL, 0);

    utrstat |= UTRSTAT_TXBE | UTRSTAT_TXE;
    utrstat &= ~UTRSTAT_RXD;

    ufstat = 0;
    if ((rx_queued = iodev_can_read(IODEV_USB_VUART))) {
        utrstat |= UTRSTAT_RXD;
        if (rx_queued > 15)
            ufstat = FIELD_PREP(UFSTAT_RXCNT, 15) | UFSTAT_RXFULL;
        else
            ufstat = FIELD_PREP(UFSTAT_RXCNT, rx_queued);

        if (FIELD_GET(UCON_RXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_RXTO_ENA) {
            utrstat |= UTRSTAT_RXTO;
        }
    }

    if (FIELD_GET(UCON_TXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_TXTHRESH_ENA) {
        utrstat |= UTRSTAT_TXTHRESH;
    }

    if (vuart_irq) {
        uart_clear_irqs();
        if (utrstat & (UTRSTAT_TXTHRESH | UTRSTAT_RXTHRESH | UTRSTAT_RXTO)) {
            aic_set_sw(vuart_irq, true);
        } else {
            aic_set_sw(vuart_irq, false);
        }
    }

    //     printf("HV: vuart UTRSTAT=0x%x UFSTAT=0x%x UCON=0x%x\n", utrstat, ufstat, ucon);
}

static void handle_vuart_passthrough(uint8_t b)
{
    const char PREFIX[] = "HVLOG: ";
    static int state = 0;

    if (!PREFIX[state]) {
        if (b == '\r' || b == '\n') {
            printf("\n");
            state = 0;
            return;
        }
        printf("%c", b);
        return;
    }

    if (b == PREFIX[state])
        state++;
    else
        state = 0;

    if (!PREFIX[state])
        printf("%s", PREFIX);
}

static bool handle_vuart(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(width);

    hv_win_diag_note(HV_WIN_DIAG_VUART, ctx, addr);

    addr &= 0xfff;

    update_irq();

    if (write) {
        //         printf("HV: vuart W 0x%lx <- 0x%lx (%d)\n", addr, *val, width);
        switch (addr) {
            case UCON:
                ucon = *val;
                break;
            case UTXH: {
                uint8_t b = *val;
                hv_win_diag_note(HV_WIN_DIAG_CAN_WRITE, NULL, b);
                bool ready = iodev_can_write(IODEV_USB_VUART);
                hv_win_diag_note(HV_WIN_DIAG_CAN_WRITE_DONE, NULL, ready);
                if (ready) {
                    hv_win_diag_note(HV_WIN_DIAG_WRITE, NULL, b);
                    iodev_write(IODEV_USB_VUART, &b, 1);
                    hv_win_diag_note(HV_WIN_DIAG_WRITE_DONE, NULL, b);
                }
                handle_vuart_passthrough(b);
                break;
            }
            case UTRSTAT:
                utrstat &= ~(*val & (UTRSTAT_TXTHRESH | UTRSTAT_RXTHRESH | UTRSTAT_RXTO));
                break;
        }
    } else {
        switch (addr) {
            case UCON:
                *val = ucon;
                break;
            case URXH:
                if (iodev_can_read(IODEV_USB_VUART)) {
                    uint8_t c;
                    iodev_read(IODEV_USB_VUART, &c, 1);
                    *val = c;
                } else {
                    *val = 0;
                }
                break;
            case UTRSTAT:
                *val = utrstat;
                break;
            case UFSTAT:
                *val = ufstat;
                break;
            default:
                *val = 0;
                break;
        }
        //         printf("HV: vuart R 0x%lx -> 0x%lx (%d)\n", addr, *val, width);
    }

    hv_win_diag_note(HV_WIN_DIAG_VUART_DONE, NULL, addr);
    return true;
}

void hv_vuart_poll(void)
{
    if (!active)
        return;

    update_irq();
}

void hv_map_vuart(u64 base, int irq, iodev_id_t iodev)
{
    hv_map_hook(base, handle_vuart, 0x1000);
    usb_iodev_vuart_setup(iodev);
    vuart_irq = irq;
    active = true;
}
