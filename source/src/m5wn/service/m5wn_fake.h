/* SPDX-License-Identifier: MIT */
/* Synthetic backend for M5WN: a fake Wi-Fi control plane (two networks, scan/join/leave)
 * and, optionally, a fake gateway (ARP, ICMP echo, DHCP server) on 192.168.77.0/24.
 * Used by the host tests, the QEMU twin (Wi-Fi part only, frames go to QEMU's network)
 * and the first hardware run (both parts; no Apple hardware is touched). */
#ifndef M5WN_FAKE_H
#define M5WN_FAKE_H
#include "m5wn_service.h"

#define WN_FAKE_NBSS 2u
typedef struct wn_fake {
    wn_service *svc;          /* service to push frames/events into */
    int gateway;              /* 1: answer ARP/ICMP/DHCP locally; 0: tx goes to fwd */
    int autolink;             /* 1: link up at HELLO without a join */
    int (*fwd)(void *ctx, const wn_u8 *frame, wn_u32 len); /* gateway == 0 */
    void *fwd_ctx;
    wn_u32 state, last_error, scans, joined;
    wn_bss bss[WN_FAKE_NBSS];
    wn_u8 client_mac[6];
    wn_u32 dhcp_offers, dhcp_acks, arp_replies, icmp_replies, tx_seen;
    wn_u8 frame[1536];
} wn_fake;

extern const wn_backend_ops wn_fake_ops;
void wn_fake_init(wn_fake *f, wn_service *svc, int gateway, int autolink);
/* addresses of the fake network */
#define WN_FAKE_GW_IP 0xc0a84d01u     /* 192.168.77.1 */
#define WN_FAKE_CLIENT_IP 0xc0a84d02u /* 192.168.77.2 */
extern const wn_u8 wn_fake_gw_mac[6];
#endif
