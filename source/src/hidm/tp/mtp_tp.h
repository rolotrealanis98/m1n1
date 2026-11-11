/* SPDX-License-Identifier: MIT */
/*
 * J716s built-in trackpad bring-up on the MTP coprocessor (DockChannel HID),
 * layered on the keyboard backend (mtp_kbd.c), which owns the FIFO, the comm
 * sequence counter and frame reception.
 *
 * Sequence (Linux dockchannel-hid.c + hid-apple-mtp-haptic.c + hid-magicmouse.c
 * behaviour on this machine, see LINUX-MAP.md):
 *   enable multi-touch {b4,mt} -> enable actuator {b4,act} -> actuator feature
 *   SET {21,00} (device-controlled click) -> firmware {95,02,00,mt,iova64,size32}
 *   -> power method 2: {40,02,mt,00,00} AFE assert 50 ms {40,02,mt,00,01}
 *                       {40,02,mt,02,00} AFE release 50 ms {40,02,mt,02,01}
 *   -> READY {f1,mt} -> feature GET 0xd9 (dimensions) -> LIVE.
 * Every wait is a deadline checked from poll; nothing here blocks.
 * Own code (MIT); behaviour from GPL-2.0 OR MIT dockchannel-hid.c, no code copied.
 */
#ifndef MTP_TP_H
#define MTP_TP_H
#include "ptp.h"
#include "../service/hidm_service.h"

#define TP_FW_MAGIC 0x46444948u   /* "HIDF", dchid fw_header */
#define TP_FW_HDR 20u
#define TP_FW_MAX (256u * 1024u)
#define TP_CMD_TIMEOUT_US 1000000u      /* dockchannel-hid COMMAND_TIMEOUT_MS */
#define TP_READY_TIMEOUT_US 2000000u    /* START_TIMEOUT_MS */
#define TP_AFE_HOLD_US 50000u           /* assert hold, and settle after release */
#define TP_GPIO_PULSE_US 10000u         /* FIFO-mode 0xa0 pulse (dchid_handle_gpio) */
#define TP_LOG_REPORTS 16u

enum tp_phase {
    TP_WAIT_INIT, TP_EN_MT, TP_EN_ACT, TP_ACT_MODE, TP_FW,
    TP_OFF_WILL, TP_OFF_AFE, TP_OFF_HAS, TP_ON_WILL, TP_ON_AFE, TP_ON_HAS,
    TP_READY, TP_DIMS, TP_LIVE, TP_FAILED
};
enum tp_error { TP_E_NONE, TP_E_TIMEOUT, TP_E_ACK, TP_E_FW, TP_E_AFE, TP_E_DIMS,
                TP_E_TOPOLOGY, TP_E_INIT, TP_E_PTP };

struct mtp_tp_io {
    void *ctx;
    /* Queue one command frame (channel 0x11). Returns 0 when queued, -1 when the
     * single TX slot is busy (retry next poll). iface 0 = comm; the transport
     * assigns the comm sequence; other interfaces pass their own seq. */
    int (*send)(void *ctx, hm_u8 iface, hm_u8 flags, hm_u8 seq, const hm_u8 *msg,
                unsigned len, hm_u8 *seq_used);
    /* Drive the AFE reset line: 1 = asserted (reset), 0 = released. On J716s
     * this is SMC key gp1c, active low (resident: smc_write_u32(gp1c,
     * 0x01000000 | !asserted)). Returns 0 on success; -1 = unavailable. */
    int (*afe_reset)(void *ctx, int asserted);
    hm_u64 (*now_us)(void *ctx);
    int (*log)(const char *, ...);
    /* DMA buffer the coprocessor reads the firmware from, already mapped by the
     * hypervisor at dma_iova (dart-mtp stream 0 or bypass PA). */
    hm_u8 *dma_buf;
    hm_u64 dma_iova;
    hm_u32 dma_cap;
};

struct mtp_tp {
    struct mtp_tp_io io;
    enum tp_phase phase;
    enum tp_error error;
    hm_u8 mt, act, mt_seq, act_seq;
    int mt_found, mt_complete, act_found, act_complete, mt_gpio_announced;
    int mt_ready, act_ready;
    /* outstanding command */
    int waiting;
    hm_u8 w_iface, w_flags, w_report, w_seq;
    hm_u32 w_rc;
    int w_done;
    hm_u8 w_resp[64];
    hm_u32 w_resp_len;
    hm_u64 deadline, afe_until;
    /* firmware (HIDF): data part after the header */
    const hm_u8 *fw;
    hm_u32 fw_len, fw_iface_off;
    /* pending GPIO pulse from a 0xa0 request (FIFO mode) */
    int pulse;
    hm_u64 pulse_until;
    hm_u8 pulse_evt[5];
    /* result */
    struct amt_dims dims;
    struct ptp ptp;
    hm_backend_interface iface;
    hm_u32 reports, translated, bad_reports, other_reports, acks_ignored;
    hm_u8 seen_ids;
};

/* fw: complete HIDF file (header + data). Returns 0 if it validates. */
int mtp_tp_init(struct mtp_tp *t, const struct mtp_tp_io *io, const hm_u8 *fw, hm_u32 fw_len);
/* Hooks called by the keyboard backend's receive path. */
void mtp_tp_on_init(struct mtp_tp *t, const hm_u8 *m, unsigned n);  /* INIT 0xf0, any name */
void mtp_tp_on_ready(struct mtp_tp *t, hm_u8 iface);
/* ACK frame on channel 0x11. Returns 1 if it matched the outstanding command. */
int mtp_tp_on_ack(struct mtp_tp *t, hm_u8 iface, hm_u8 seq, hm_u8 flags, hm_u32 rc,
                  const hm_u8 *m, unsigned n);
/* GPIO request 0xa0 for the multi-touch interface. Returns 1 if taken (the
 * ACK {a1, rc, event} is then sent by mtp_tp), 0 to let the caller NACK. */
int mtp_tp_on_gpio(struct mtp_tp *t, const hm_u8 *m, unsigned n);
/* Input report on any non-comm interface. Returns 1 if it was the trackpad's.
 * In TP_LIVE it is translated and pushed to the service as interface t->mt. */
int mtp_tp_on_report(struct mtp_tp *t, hm_u8 iface, const hm_u8 *m, unsigned n,
                     hm_service *s, hm_u64 t_us);
/* Advance the state machine; call from the keyboard backend's poll. */
void mtp_tp_step(struct mtp_tp *t);
/* Feature/output callback for the PTP interface. */
hm_u32 mtp_tp_report(struct mtp_tp *t, hm_u32 op, hm_u32 rid, hm_u8 *buf, hm_u32 *len);
const char *mtp_tp_phase_name(enum tp_phase p);
#endif
