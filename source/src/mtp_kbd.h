/* SPDX-License-Identifier: MIT */
#ifndef MTP_KBD_H
#define MTP_KBD_H
#include "hidm/service/hidm_service.h"
#define MTP_FIFO_SIZE 2048u
#define MTP_FRAME_MAX (HM_MAX_DESC + 512u)
#define MTP_DATA 0x294b34000ull
#define MTP_TX32 (MTP_DATA + 0x10)
#define MTP_TX_FREE (MTP_DATA + 0x14)
#define MTP_RX32 (MTP_DATA + 0x28)
#define MTP_RX_COUNT (MTP_DATA + 0x2c)
enum mtp_phase { MTP_ENUM, MTP_ENABLE, MTP_LIVE, MTP_FAILED };
enum mtp_error { MTP_E_NONE, MTP_E_TIMEOUT, MTP_E_FRAME, MTP_E_OVERRUN,
                 MTP_E_DESCRIPTOR, MTP_E_TOPOLOGY, MTP_E_RTKit, MTP_E_ACK, MTP_E_TX };
/* All callbacks must be nonblocking. poll_control handles bounded RTKit work. */
struct mtp_io {
    void *ctx;
    hm_u32 (*read32)(void *, hm_u64);
    void (*write32)(void *, hm_u64, hm_u32);
    hm_u64 (*now_us)(void *);
    int (*poll_control)(void *);
    void (*input_changed)(void *); /* recompute virtual IRQ after each push */
};
struct mtp_kbd {
    struct mtp_io io;
    hm_service *service;
    hm_backend_interface keyboard;
    hm_u8 descriptor[HM_MAX_DESC], frame[MTP_FRAME_MAX], tx[32];
    hm_u32 used, expected, tx_len, desc_len, seen[8];
    hm_u64 deadline, partial_deadline, tx_deadline;
    enum mtp_phase phase;
    enum mtp_error error;
    hm_u8 iface, seq, enable_seq;
    int found, complete, ack, ready, enable_sent;
    hm_u32 frames, reports, rejected;
};
void mtp_kbd_init(struct mtp_kbd *, const struct mtp_io *, hm_service *);
void mtp_kbd_poll(struct mtp_kbd *);
void mtp_kbd_fail(struct mtp_kbd *, enum mtp_error);
hm_u32 mtp_kbd_report(void *, hm_u32, hm_u32, hm_u32, hm_u8 *, hm_u32 *);
/* Resident adapter: called once before guest entry, tick only thereafter. */
int mtp_kbd_resident_start(struct mtp_kbd *, hm_service *);
#endif
