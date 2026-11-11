/* SPDX-License-Identifier: MIT */
/* M5WN version 1: mediated network transport between the EL2 resident (or the QEMU twin)
 * and the Windows NDIS miniport m5wn.sys. Normative; ABI.md explains it. New code, no
 * Linux or Apple source in it. */
#ifndef M5WN_ABI_H
#define M5WN_ABI_H
#if defined(_MSC_VER)
typedef unsigned __int8 wn_u8;
typedef unsigned __int16 wn_u16;
typedef unsigned __int32 wn_u32;
typedef unsigned __int64 wn_u64;
typedef signed __int16 wn_s16;
#else
typedef unsigned char wn_u8;
typedef unsigned short wn_u16;
typedef unsigned int wn_u32;
typedef unsigned long long wn_u64;
typedef signed short wn_s16;
#endif

#define WN_MAGIC 0x4e57354du /* "M5WN" little-endian */
#define WN_VERSION 1u
#define WN_PAGE 4096u

/* Fixed platform assignment (twin and resident use the same addresses, as HIDM does):
 * each page in its own 16 KiB stage-2 granule, next to HIDM (0xF38100000/0xF38104000). */
#define WN_REG_BASE 0xf38108000ull
#define WN_CTRL_BASE 0xf3810c000ull
#define WN_GSIV 69u
#define WN_SPI 37u

/* ---- Register page: aligned 32-bit little-endian accesses only; others RAZ/WI ---- */
#define WN_R_MAGIC 0x00u      /* RO */
#define WN_R_VERSION 0x04u    /* RO */
#define WN_R_STATUS 0x08u     /* RO: WN_ST_* */
#define WN_R_FEATURES 0x0cu   /* RO: WN_F_* */
#define WN_R_MAC_LO 0x10u     /* RO: MAC bytes 0..3 (byte 0 in bits 7:0) */
#define WN_R_MAC_HI 0x14u     /* RO: MAC bytes 4..5 in bits 15:0 */
#define WN_R_MTU 0x18u        /* RO: 1500 */
#define WN_R_SLOT_SIZE 0x1cu  /* RO: WN_SLOT_BYTES */
#define WN_R_TX_PROD 0x20u    /* RW: driver's monotonic produced count; a write is the TX doorbell */
#define WN_R_TX_CONS 0x24u    /* RO: service's monotonic consumed count */
#define WN_R_RX_PROD 0x28u    /* RO: service's monotonic produced count */
#define WN_R_RX_CONS 0x2cu    /* RW: driver's monotonic consumed count (frees slots) */
#define WN_R_IRQ_ENABLE 0x30u /* RW: WN_IRQ_* mask */
#define WN_R_IRQ_STATUS 0x34u /* RO: WN_IRQ_* pending, independent of the mask */
#define WN_R_EVENT_SEQ 0x38u  /* RO: bumped on link/scan/join changes */
#define WN_R_EVENT_ACK 0x3cu  /* RW: driver writes the EVENT_SEQ it has handled */
#define WN_R_DOORBELL 0x40u   /* WO: control request sequence; synchronous */
#define WN_R_LINK_MBPS 0x44u  /* RO */
#define WN_R_TX_DROPS 0x48u   /* RO: frames the backend refused */
#define WN_R_RX_DROPS 0x4cu   /* RO: frames dropped because ring and backlog were full */
#define WN_R_TX_BAD 0x50u     /* RO: TX slots with an invalid length */

#define WN_ST_READY 1u  /* HELLO + CONFIGURE done, region valid */
#define WN_ST_LINK 2u   /* carrier up (Wi-Fi associated / fake link up) */
#define WN_ST_FAULT 4u  /* peer protocol violation; session stopped until HELLO */

#define WN_F_WIFI 1u    /* WIFI_* control operations supported */
#define WN_F_FAKE 2u    /* backend is synthetic (twin or hardware fake) */

#define WN_IRQ_RX 1u
#define WN_IRQ_EVENT 2u

/* ---- Control page: request at 0, response at 512; aligned 32-bit accesses only ---- */
#define WN_REQ_OFF 0u
#define WN_RESP_OFF 512u
#define WN_PAYLOAD 480u

typedef struct wn_request {
    wn_u32 seq, abi, op, arg0;
    wn_u32 arg1, arg2, length, reserved;
    wn_u8 payload[WN_PAYLOAD];
} wn_request;
typedef struct wn_response {
    wn_u32 seq, status, length, reserved;
    wn_u8 payload[WN_PAYLOAD];
    wn_u32 pad[4];
} wn_response;

/* Operations. 1-15 belong to the driver; 16+ may be relayed from the user tool. */
#define WN_OP_HELLO 1u         /* abi=1, args 0 */
#define WN_OP_CONFIGURE 2u     /* arg0/arg1 = region IPA lo/hi, arg2 = region bytes; payload u32 tx_slots, u32 rx_slots */
#define WN_OP_SET_FILTER 3u    /* arg0 = WN_FILTER_* */
#define WN_OP_SET_MCAST 4u     /* length = 6*n, n <= WN_MAX_MCAST */
#define WN_OP_GET_STATS 5u     /* response wn_stats */
#define WN_OP_BYE 9u
#define WN_OP_WIFI_SCAN 16u    /* start; completion bumps EVENT_SEQ */
#define WN_OP_WIFI_RESULTS 17u /* arg0 = first index; response u32 total, u32 count, wn_bss[count] */
#define WN_OP_WIFI_JOIN 18u    /* payload wn_join; result bumps EVENT_SEQ and sets LINK */
#define WN_OP_WIFI_LEAVE 19u
#define WN_OP_WIFI_STATUS 20u  /* response wn_wifi_status */
#define WN_FIRST_USER_OP 16u

#define WN_OK 0u
#define WN_BAD_VERSION 1u
#define WN_BAD_REQUEST 2u
#define WN_NOT_READY 3u
#define WN_RANGE 4u
#define WN_DENIED 5u
#define WN_BUSY 6u
#define WN_IO_ERROR 7u
#define WN_UNSUPPORTED 8u

#define WN_FILTER_DIRECTED 1u
#define WN_FILTER_MULTICAST 2u
#define WN_FILTER_ALL_MULTICAST 4u
#define WN_FILTER_BROADCAST 8u
#define WN_FILTER_PROMISCUOUS 0x20u
#define WN_MAX_MCAST 32u

/* ---- Guest region (driver-allocated, physically contiguous guest RAM) ----
 * [tx_slots x WN_SLOT_BYTES][rx_slots x WN_SLOT_BYTES]; base 4 KiB aligned. */
#define WN_SLOT_BYTES 2048u
#define WN_SLOT_HDR 16u
#define WN_MIN_FRAME 14u
#define WN_MAX_FRAME 1518u /* 1514 + 802.1Q tag; no FCS */
#define WN_MIN_SLOTS 4u
#define WN_MAX_SLOTS 256u
typedef struct wn_slot_hdr {
    wn_u32 length, flags, reserved0, reserved1;
} wn_slot_hdr;

typedef struct wn_stats {
    wn_u64 tx_packets, tx_bytes, rx_packets, rx_bytes;
    wn_u64 tx_drops, rx_drops, tx_bad, faults;
} wn_stats;

#define WN_SEC_OPEN 0u
#define WN_SEC_WPA2_PSK 1u /* key = 32-byte PMK (the tool derives it from the passphrase) */
#define WN_SEC_WPA3_SAE 2u /* key = password bytes, 8..63 */
#define WN_SSID_MAX 32u
#define WN_KEY_MAX 64u
typedef struct wn_join {
    wn_u32 ssid_len, security, key_len, reserved;
    wn_u8 ssid[WN_SSID_MAX];
    wn_u8 key[WN_KEY_MAX];
    wn_u8 bssid[6]; /* all-zero = any */
    wn_u16 channel; /* 0 = any */
} wn_join;
typedef struct wn_bss {
    wn_u8 ssid[WN_SSID_MAX];
    wn_u8 ssid_len, reserved;
    wn_u8 bssid[6];
    wn_u16 channel;
    wn_s16 rssi_dbm;
    wn_u16 security_mask; /* 1<<WN_SEC_* */
    wn_u16 reserved2;
} wn_bss;
#define WN_RESULTS_MAX ((WN_PAYLOAD - 8u) / sizeof(wn_bss))

#define WN_WIFI_IDLE 0u
#define WN_WIFI_SCANNING 1u
#define WN_WIFI_JOINING 2u
#define WN_WIFI_CONNECTED 3u
#define WN_WIFI_FAILED 4u
typedef struct wn_wifi_status {
    wn_u32 state, last_error, scan_done_count, reserved;
    wn_bss current;
} wn_wifi_status;

#if defined(__cplusplus)
#define WN_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define WN_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
WN_STATIC_ASSERT(sizeof(wn_request) == 512, "request");
WN_STATIC_ASSERT(sizeof(wn_response) == 512, "response");
WN_STATIC_ASSERT(sizeof(wn_slot_hdr) == WN_SLOT_HDR, "slot header");
WN_STATIC_ASSERT(sizeof(wn_stats) == 64, "stats");
WN_STATIC_ASSERT(sizeof(wn_join) == 120, "join");
WN_STATIC_ASSERT(sizeof(wn_bss) == 48, "bss");
WN_STATIC_ASSERT(sizeof(wn_wifi_status) == 64, "status");
WN_STATIC_ASSERT(WN_SLOT_HDR + WN_MAX_FRAME <= WN_SLOT_BYTES, "slot");
#endif
