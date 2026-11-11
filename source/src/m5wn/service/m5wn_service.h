/* SPDX-License-Identifier: MIT */
/* M5WN service: the device side of the mediated network transport. Freestanding C, no
 * allocation, no libc. One instance per device; every entry point and every backend push
 * must run under the same device lock (resident: bhl; QEMU: the BQL; tests: single thread). */
#ifndef M5WN_SERVICE_H
#define M5WN_SERVICE_H
#include "../abi/m5wn_abi.h"

#define WN_BACKLOG 32u

/* Guest memory accessor supplied by the platform. Each call must itself validate that
 * [ipa, ipa+len) is guest RAM the guest may share (resident: hv_storage_guest_span inside a
 * begin/end bracket; twin: inside the RAM window). Return 0 on success. */
typedef struct wn_platform {
    void *ctx;
    int (*guest_read)(void *ctx, wn_u64 ipa, void *dst, wn_u32 len);
    int (*guest_write)(void *ctx, wn_u64 ipa, const void *src, wn_u32 len);
    int (*region_ok)(void *ctx, wn_u64 ipa, wn_u64 len); /* optional, at CONFIGURE */
} wn_platform;

/* Backend: what actually moves frames (N1 core, fake, or QEMU). No callback may block. */
typedef struct wn_backend_ops {
    int (*tx)(void *ctx, const wn_u8 *frame, wn_u32 len); /* 0 = accepted */
    void (*set_filter)(void *ctx, wn_u32 filter);           /* optional */
    void (*session)(void *ctx, int open);                   /* optional: HELLO(1)/BYE(0) */
    wn_u32 (*wifi_scan)(void *ctx);
    wn_u32 (*wifi_results)(void *ctx, wn_u32 first, wn_bss *out, wn_u32 max, wn_u32 *total);
    wn_u32 (*wifi_join)(void *ctx, const wn_join *join);
    wn_u32 (*wifi_leave)(void *ctx);
    wn_u32 (*wifi_status)(void *ctx, wn_wifi_status *status);
} wn_backend_ops;

typedef struct wn_service {
    const wn_platform *plat;
    const wn_backend_ops *be;
    void *be_ctx;
    wn_u8 mac[6];
    wn_u32 features, link_mbps;
    /* session */
    wn_u32 status, session, last_seq;
    wn_u64 region;
    wn_u32 region_len, tx_slots, rx_slots;
    wn_u32 tx_prod, tx_cons, rx_prod, rx_cons;
    wn_u32 irq_enable, event_seq, event_ack;
    wn_u32 tx_drops, rx_drops, tx_bad;
    wn_u32 filter, nmcast;
    wn_u8 mcast[WN_MAX_MCAST][6];
    wn_stats stats;
    /* control page: request bytes are guest-writable, response bytes service-owned */
    wn_u32 req_words[WN_RESP_OFF / 4];
    wn_response resp;
    /* RX backlog for frames arriving while the ring is full */
    wn_u32 bl_head, bl_tail;
    wn_u16 bl_len[WN_BACKLOG];
    wn_u8 bl_data[WN_BACKLOG][WN_MAX_FRAME];
    wn_u8 scratch[WN_SLOT_BYTES];
    int in_tx;
} wn_service;

void wn_zero(void *p, wn_u32 n);
void wn_copy(void *d, const void *s, wn_u32 n);
/* Initialise; features gets WN_F_WIFI when the backend has wifi ops. */
void wn_service_init(wn_service *s, const wn_platform *plat, const wn_backend_ops *be, void *be_ctx,
                     const wn_u8 mac[6], wn_u32 features);
/* MMIO: page 0 = register page, page 1 = control page. off is the byte offset. */
wn_u32 wn_service_read(wn_service *s, unsigned page, wn_u32 off);
void wn_service_write(wn_service *s, unsigned page, wn_u32 off, wn_u32 value);
/* Interrupt level (READY and an enabled pending source). */
int wn_service_irq(const wn_service *s);
/* Backend pushes. rx returns 0 if queued/delivered, nonzero if dropped or filtered. */
int wn_service_rx(wn_service *s, const wn_u8 *frame, wn_u32 len);
void wn_service_set_link(wn_service *s, int up, wn_u32 mbps);
void wn_service_event(wn_service *s);
/* Platform teardown (guest reset / resident retire): forget the region, mask everything. */
void wn_service_reset(wn_service *s);
#endif
