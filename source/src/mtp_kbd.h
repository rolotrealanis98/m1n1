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
/* MTP_STM: Linux order (dockchannel-hid enables "stm" first, others after its READY). */
enum mtp_phase { MTP_ENUM, MTP_STM, MTP_ENABLE, MTP_LIVE, MTP_FAILED };
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
    int (*log)(const char *, ...); /* optional console line; NULL = silent */
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
    /* stm (Linux enables it first); never multi-touch / any other interface */
    hm_u8 stm_iface, stm_seq;
    int stm_found, stm_complete, stm_sent, stm_ack, stm_ready, stm_state;
    hm_u64 stm_deadline, rx_bytes;
};
#define MTP_STM_ANNOUNCE_US 1000000u /* wait for stm INIT after keyboard INIT */
#define MTP_STM_READY_US 2000000u    /* wait for stm ACK + READY */
#define MTP_LOG_REPORTS 32u          /* hex-dump only the first N input reports */
void mtp_kbd_init(struct mtp_kbd *, const struct mtp_io *, hm_service *);
void mtp_kbd_poll(struct mtp_kbd *);
void mtp_kbd_fail(struct mtp_kbd *, enum mtp_error);
hm_u32 mtp_kbd_report(void *, hm_u32, hm_u32, hm_u32, hm_u8 *, hm_u32 *);
/* Resident adapter: called once before guest entry, tick only thereafter. */
int mtp_kbd_resident_start(struct mtp_kbd *, hm_service *);
#endif
