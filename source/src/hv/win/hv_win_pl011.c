/* SPDX-License-Identifier: MIT */
/*
 * Emulated ARM PL011 UART for the Windows guest (m5pro-windows EL2-RUNTIME items 9/10, GAPS G11).
 *
 * Windows' kernel debugger and its serial console only speak the DBG2/SPCR subtypes it ships
 * drivers for; the Samsung UART on Apple silicon is not one of them, the PL011 (DBG2 subtype
 * 0x0003) is. EL2 therefore emulates a PL011 in the synthetic IPA window (CONTRACT.md s1,
 * /chosen/m5w,pl011, default 0xF_2000_0000) and carries its bytes over the same USB CDC pipe the
 * Samsung vuart uses (IODEV_USB_VUART, the m1n1 USB device's second CDC ACM interface).
 *
 * Model: an always-ready transmitter (TX completes on the DR write, so TXFE = 1, TXFF = BUSY = 0)
 * and a receiver fed by whatever the host writes into the CDC pipe. Baud rate, line control and
 * FIFO settings are stored and read back but have no effect. DMA is not implemented (DMACR reads
 * as zero, writes are ignored). The 4 KiB register file repeats nothing: every other offset in
 * the 64 KiB frame reads as zero and ignores writes.
 *
 * The CDC pipe is shared with hv_vuart.c: whichever UART the guest polls receives the next host
 * byte. Mu uses only the Samsung UART and Windows only the PL011, so in practice one side is idle.
 *
 * Interrupts are optional: with a GSIV configured, a 0 -> 1 transition of the masked interrupt
 * status calls hv_win_pl011_raise(gsiv), a weak no-op here that the vGIC implements; the vGIC can
 * re-sample the level with hv_win_pl011_irq_level() (for example on EOI) and
 * hv_win_pl011_poll() should run from the hypervisor tick so host input raises RX without a
 * guest access. Windows KD polls the UART, so none of this is needed for a debugger session.
 */

#include "hv_win.h"
#include "hv_win_hidm.h"
#include "hv_win_storage.h"
#include "iodev.h"
#include "utils.h"

#include "../hv.h"

#define PL011_FRAME_SIZE 0x10000
#define PL011_REGS_SIZE  0x1000
#define PL011_FIFO_DEPTH 32

#define UARTDR        0x000
#define UARTRSR       0x004 /* read: RSR, write: ECR */
#define UARTFR        0x018
#define UARTILPR      0x020
#define UARTIBRD      0x024
#define UARTFBRD      0x028
#define UARTLCR_H     0x02c
#define UARTCR        0x030
#define UARTIFLS      0x034
#define UARTIMSC      0x038
#define UARTRIS       0x03c
#define UARTMIS       0x040
#define UARTICR       0x044
#define UARTDMACR     0x048
#define UARTPERIPHID0 0xfe0
#define UARTPCELLID0  0xff0

#define FR_CTS  BIT(0)
#define FR_DSR  BIT(1)
#define FR_DCD  BIT(2)
#define FR_BUSY BIT(3)
#define FR_RXFE BIT(4)
#define FR_TXFF BIT(5)
#define FR_RXFF BIT(6)
#define FR_TXFE BIT(7)

#define LCR_H_FEN BIT(4)

#define CR_UARTEN BIT(0)
#define CR_TXE    BIT(8)
#define CR_RXE    BIT(9)

#define INT_RX  BIT(4)
#define INT_TX  BIT(5)
#define INT_RT  BIT(6)
#define INT_ALL GENMASK(10, 0)

#define IFLS_RX GENMASK(5, 3)

#define CR_RESET   (CR_TXE | CR_RXE)
#define IFLS_RESET 0x12

/* PrimeCell identification: UART PL011 r1p5 (PeriphID0..3), PrimeCell ID 0xb105f00d */
static const u8 pl011_periph_id[4] = {0x11, 0x10, 0x14, 0x00};
static const u8 pl011_pcell_id[4] = {0x0d, 0xf0, 0x05, 0xb1};

static struct {
    bool configured;
    u64 base;
    u32 gsiv;

    u32 ibrd;
    u32 fbrd;
    u32 lcr_h;
    u32 cr;
    u32 ifls;
    u32 imsc;
    u32 ris; /* latched sources (TX); RX/RT are recomputed from the pipe on every update */

    bool line;
    u64 tx_bytes;
    u64 tx_dropped;
    u64 rx_bytes;
} pl011;

/*
 * smp2g: kernel-debugger break/continue log (hv_win_kdlog.h). Passive: the bytes are forwarded exactly
 * as before; this only notes when a state-change packet left the guest, which CPU sent it, and when
 * and on which CPU the host's continue was read. Printed by the SMP dump (boot CPU only). Lock-free:
 * a slot index comes from an atomic counter, so a concurrent writer can at worst garble one record.
 */
#include "hv_win_kdlog.h"
#include "smp.h"
#define KDLOG_N 16
static struct kdlog_parser kd_tx, kd_rx;
static struct kdlog_event kd_ring[KDLOG_N];
static u64 kd_seq, kd_nr_change, kd_nr_continue;

static void kd_note(struct kdlog_parser *p, u8 b)
{
    struct kdlog_event ev = {0};

    if (!kdlog_feed(p, b, &ev))
        return;
    ev.t = mrs(CNTPCT_EL0);
    ev.cpu = smp_id();
    if (ev.type == 7)
        kd_nr_change++;
    else
        kd_nr_continue++;
    kd_ring[__atomic_fetch_add(&kd_seq, 1, __ATOMIC_RELAXED) % KDLOG_N] = ev;
}

void hv_win_pl011_kd_dump(void)
{
    u64 now = mrs(CNTPCT_EL0), tk_ms = mrs(CNTFRQ_EL0) / 1000, seq = kd_seq;

    if (!tk_ms)
        tk_ms = 1;
    printf("HV/win smp2g KD: %lu state changes sent, %lu continues read (UART tx %lu rx %lu bytes); "
           "last %d, oldest first:\n",
           kd_nr_change, kd_nr_continue, pl011.tx_bytes, pl011.rx_bytes, KDLOG_N);
    for (u64 i = seq > KDLOG_N ? seq - KDLOG_N : 0; i < seq; i++) {
        struct kdlog_event ev = kd_ring[i % KDLOG_N];
        if (ev.type == 7)
            printf("HV/win smp2g KD: %ld ms ago: state change 0x%x (%s) reported for processor %u, "
                   "sent by CPU %u, pc 0x%lx\n",
                   (s64)(now - ev.t) / (s64)tk_ms, ev.code,
                   ev.code == 0x3030 ? "exception" : ev.code == 0x3031 ? "load symbols" : "command",
                   ev.proc, ev.cpu, ev.pc);
        else if (ev.type == 2)
            printf("HV/win smp2g KD: %ld ms ago: continue (api 0x%x, status 0x%lx) for processor %u, "
                   "read by CPU %u\n",
                   (s64)(now - ev.t) / (s64)tk_ms, ev.code, ev.pc, ev.proc, ev.cpu);
    }
}

void __attribute__((weak)) hv_win_pl011_raise(u32 gsiv)
{
    UNUSED(gsiv);
}

static bool pl011_pipe_ready(void)
{
    /* hv_map_vuart() binds the CDC pipe to the proxy's USB device; without it there is no pipe */
    return iodev_get_opaque(IODEV_USB_VUART) != NULL;
}

static ssize_t pl011_rx_queued(void)
{
    if (!pl011_pipe_ready())
        return 0;

    iodev_handle_events(IODEV_USB_VUART);
    return iodev_can_read(IODEV_USB_VUART);
}

static u32 pl011_rx_trigger(void)
{
    static const u32 levels[8] = {4, 8, 16, 24, 28, 28, 28, 28}; /* 1/8 .. 7/8 of 32 entries */

    if (!(pl011.lcr_h & LCR_H_FEN))
        return 1;
    return levels[FIELD_GET(IFLS_RX, pl011.ifls)];
}

static u32 pl011_ris(ssize_t queued)
{
    u32 ris = pl011.ris;

    if (queued > 0) {
        /* No receive timer: data that waits in the FIFO is reported as timed out right away */
        ris |= INT_RT;
        if ((u32)queued >= pl011_rx_trigger())
            ris |= INT_RX;
    }

    return ris;
}

static void pl011_update(ssize_t queued)
{
    bool line = (pl011_ris(queued) & pl011.imsc) != 0;

    if (line && !pl011.line && pl011.gsiv)
        hv_win_pl011_raise(pl011.gsiv);
    pl011.line = line;
}

static u32 pl011_fr(ssize_t queued)
{
    u32 fr = FR_CTS | FR_DSR | FR_DCD | FR_TXFE;

    if (queued <= 0)
        fr |= FR_RXFE;
    else if ((u32)queued >= ((pl011.lcr_h & LCR_H_FEN) ? PL011_FIFO_DEPTH : 1))
        fr |= FR_RXFF;

    return fr;
}

static void pl011_tx(u8 b)
{
    if (pl011_pipe_ready() && iodev_can_write(IODEV_USB_VUART)) {
        iodev_write(IODEV_USB_VUART, &b, 1);
        pl011.tx_bytes++;
        kd_note(&kd_tx, b); /* smp2g */
    } else {
        /* Nobody listening: drop rather than stall the guest's polling loop */
        pl011.tx_dropped++;
    }
    pl011.ris |= INT_TX;
}

static u32 pl011_rx(void)
{
    u8 c;

    if (pl011_rx_queued() <= 0)
        return 0;
    if (iodev_read(IODEV_USB_VUART, &c, 1) != 1)
        return 0;

    pl011.rx_bytes++;
    kd_note(&kd_rx, c); /* smp2g */
    return c; /* no framing, parity, break or overrun errors on a USB pipe */
}

static u32 pl011_read(u64 off, ssize_t queued)
{
    if (off >= UARTPERIPHID0 && off < UARTPERIPHID0 + 0x10)
        return pl011_periph_id[(off - UARTPERIPHID0) >> 2];
    if (off >= UARTPCELLID0 && off < UARTPCELLID0 + 0x10)
        return pl011_pcell_id[(off - UARTPCELLID0) >> 2];

    switch (off) {
        case UARTDR:
            return pl011_rx();
        case UARTRSR:
            return 0;
        case UARTFR:
            return pl011_fr(queued);
        case UARTIBRD:
            return pl011.ibrd;
        case UARTFBRD:
            return pl011.fbrd;
        case UARTLCR_H:
            return pl011.lcr_h;
        case UARTCR:
            return pl011.cr;
        case UARTIFLS:
            return pl011.ifls;
        case UARTIMSC:
            return pl011.imsc;
        case UARTRIS:
            return pl011_ris(queued);
        case UARTMIS:
            return pl011_ris(queued) & pl011.imsc;
        default:
            /* ILPR, DMACR, ICR (write-only) and everything unimplemented */
            return 0;
    }
}

static void pl011_write(u64 off, u32 val)
{
    switch (off) {
        case UARTDR:
            pl011_tx(val & 0xff);
            break;
        case UARTRSR:
            /* ECR: clears the (always zero) error flags */
            break;
        case UARTIBRD:
            pl011.ibrd = val & 0xffff;
            break;
        case UARTFBRD:
            pl011.fbrd = val & 0x3f;
            break;
        case UARTLCR_H:
            pl011.lcr_h = val & 0xff;
            break;
        case UARTCR:
            /* The transmitter is always ready; enabling it reports an empty FIFO */
            if ((val & CR_TXE) && !(pl011.cr & CR_TXE))
                pl011.ris |= INT_TX;
            pl011.cr = val & 0xff87;
            break;
        case UARTIFLS:
            pl011.ifls = val & 0x3f;
            break;
        case UARTIMSC:
            pl011.imsc = val & INT_ALL;
            break;
        case UARTICR:
            pl011.ris &= ~(val & INT_ALL);
            break;
        default:
            /* FR, RIS, MIS and the ID registers are read-only; ILPR and DMACR are ignored */
            break;
    }
}

static bool pl011_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);

    u64 off = addr - pl011.base;

    /* Registers are 32 bits wide; byte/halfword accesses hit the low lane, 64-bit ones are split */
    if (width > 3)
        return false;

    if (off >= PL011_REGS_SIZE || (off & 3)) {
        if (!write)
            *val = 0;
        return true;
    }

    ssize_t queued = pl011_rx_queued();

    if (write) {
        pl011_write(off, (u32)*val);
    } else {
        u64 mask = width == 3 ? 0xffffffffUL : (BIT(8 << width) - 1);
        *val = pl011_read(off, queued) & mask;
        if (off == UARTDR)
            queued = pl011_rx_queued();
    }

    pl011_update(queued);
    return true;
}

int hv_win_pl011_configure(u64 base, u64 gsiv)
{
    if (hv_win_hidm_claims_spi(gsiv)) return -1;
    if (gsiv <= UINT32_MAX && hv_win_storage_claims_spi((u32)gsiv))
        return -1;
    if (!base) {
        printf("HV/win: PL011 base must be non-zero\n");
        return -1;
    }

    if (base & (PL011_FRAME_SIZE - 1)) {
        printf("HV/win: PL011 base 0x%lx is not 64 KiB aligned\n", base);
        return -1;
    }

    if (gsiv > 0xffffffff) {
        printf("HV/win: PL011 GSIV 0x%lx out of range\n", gsiv);
        return -1;
    }

    if (pl011.configured) {
        if (pl011.base != base) {
            printf("HV/win: PL011 already at 0x%lx\n", pl011.base);
            return -1;
        }
        pl011.gsiv = gsiv;
        return 0;
    }

    if (!pl011_pipe_ready()) {
        printf("HV/win: PL011 needs the vuart CDC pipe (hv_map_vuart) first\n");
        return -1;
    }

    if (hv_pt_is_ram(base)) {
        printf("HV/win: PL011 base 0x%lx is guest RAM\n", base);
        return -1;
    }

    pl011.base = base;
    pl011.gsiv = gsiv;
    pl011.ibrd = 0;
    pl011.fbrd = 0;
    pl011.lcr_h = 0;
    pl011.cr = CR_RESET;
    pl011.ifls = IFLS_RESET;
    pl011.imsc = 0;
    pl011.ris = INT_TX;
    pl011.line = false;
    pl011.tx_bytes = pl011.tx_dropped = pl011.rx_bytes = 0;

    if (hv_map_hook(base, pl011_hook, PL011_FRAME_SIZE) < 0) {
        printf("HV/win: PL011 hook at 0x%lx failed\n", base);
        return -1;
    }

    pl011.configured = true;
    printf("HV/win: PL011 at 0x%lx (64 KiB frame), GSIV %ld, shares the vuart CDC pipe\n", base,
           gsiv);
    return 0;
}

void hv_win_pl011_poll(void)
{
    if (!pl011.configured)
        return;

    pl011_update(pl011_rx_queued());
}

bool hv_win_pl011_irq_level(void)
{
    return pl011.configured && pl011.line;
}

u64 hv_win_pl011_stats(void)
{
    return (pl011.tx_bytes & 0xffffffff) | ((pl011.rx_bytes & 0xffff) << 32) |
           ((pl011.tx_dropped & 0xffff) << 48);
}

u32 hv_win_pl011_gsiv(void)
{
    return pl011.configured ? pl011.gsiv : 0;
}
