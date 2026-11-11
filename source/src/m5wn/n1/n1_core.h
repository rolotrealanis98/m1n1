/* SPDX-License-Identifier: MIT */
/* N1 bring-up core for the EL2 resident: PCIe function setup, ROM firmware boot, link cycle,
 * control/Alpha IPC, Wi-Fi protocol 27 control and Ethernet data path, exposed as an M5WN
 * backend. Clean-room from LINUX-MAP.md (sections 2-5, 9); every hardware access is listed with
 * its source in WRITES.md. Polling only (no MSI): the resident tick calls n1_poll(). */
#ifndef N1_CORE_H
#define N1_CORE_H
#include "n1_hal.h"
#include "../abi/m5wn_abi.h"

enum n1_stage {
    N1_ST_NONE = 0,
    N1_ST_TRAINED,  /* port 0 up, endpoint 106b:1900 (ROM personality) */
    N1_ST_ROM_DONE, /* ROM accepted the FTAB (0x8040 == 1, 0x8000 == 4 or 1) */
    N1_ST_CYCLED,   /* link retrained after the ROM boot; Control stage 1 */
    N1_ST_IPC,      /* control IPC, memswaps, CCHI, Alpha handshake */
    N1_ST_RADIO,    /* Wi-Fi rings open, radio started, station configured, data rings open */
    N1_ST_FAILED = 100,
};

/* Failure codes (n1.err) */
enum {
    N1E_OK, N1E_ARGS, N1E_TRAIN, N1E_ENDPOINT, N1E_BAR, N1E_FW_FORMAT, N1E_DMA, N1E_ROM_BUSY,
    N1E_ROM_RESPONSE, N1E_ROM_STAGE, N1E_PM, N1E_CYCLE, N1E_FUNCTIONS, N1E_SECONDARY, N1E_IPC_STAGE,
    N1E_HANDSHAKE, N1E_MEMSWAP, N1E_MESSAGE, N1E_CCHI, N1E_ALPHA, N1E_COMMAND, N1E_SCAN, N1E_CONNECT,
    N1E_STORAGE_LOCK, N1E_LINK,
};

#define N1_SCAN_MAX 32
#define N1_WIN_SIZE (32u << 20)
#define N1_TX_SLOTS 2048u
#define N1_RX_SLOTS 2048u

struct n1_buf { n1_u8 *cpu; n1_u64 iova; size_t size; };

struct n1_ctx {
    struct n1_buf page, msgs;
    n1_u32 index[4], mtr, mcr;
    n1_u16 tr, cr, msg_head;
};

struct n1_bss_entry {
    n1_u8 bssid[6], ssid[32], ssid_len, channel, band, has_rsn;
    n1_u16 rsn_caps, akm_mask; /* akm_mask: bit 2 = PSK (00-0F-AC:2), bit 8 = SAE (00-0F-AC:8) */
    int rssi;
};

struct n1 {
    const struct n1_hal *h;
    const struct n1_addrs *a;
    enum n1_stage stage;
    int err;
    n1_u32 err_detail;
    /* PCIe */
    n1_u64 win_cpu, win_pci, win_size, win_next;
    n1_u64 bar_cpu[3], bar_pci[3];
    n1_u32 bar_size[3], pm_cap0;
    /* firmware */
    const n1_u8 *fw;
    size_t fw_size;
    struct n1_buf fwbuf, desc, secondary, msw[3];
    struct n1_ctx control, alpha;
    struct n1_buf cchi_rx, cchi_tx, debug_rx, window;
    /* Wi-Fi control */
    n1_u16 rx_next[5], rx_head[5], cmd_head, seq;
    int reply_valid;
    n1_u16 reply_seq, reply_status;
    n1_u32 reply_cmd;
    n1_u32 scans_done, beacons, events;
    int scanning, connecting, connected, data_open, wifi_failed;
    n1_u16 connect_status;
    n1_u64 connect_deadline, scan_deadline;
    n1_u32 scan_done_at_start;
    struct n1_bss_entry scan[N1_SCAN_MAX];
    n1_u32 nscan;
    int last_error; /* M5WN wifi last_error */
    struct n1_bss_entry cur;
    n1_u8 mac[6];
    /* data path */
    n1_u16 tx_post;
    n1_u8 tx_busy[N1_TX_SLOTS];
    n1_u32 rx_frames, rx_bad, tx_frames, tx_done, tx_fail;
    /* consumer (the M5WN service) */
    void *sink;
    int (*sink_rx)(void *sink, const n1_u8 *frame, n1_u32 len);
    void (*sink_link)(void *sink, int up, n1_u32 mbps);
    void (*sink_event)(void *sink);
};

/* Window defaults: apcie0 32-bit memory range, PCI 0x80000000 -> CPU 0xb_8000_0000, 1 GiB (P0001:1351). */
#define N1_WIN_PCI 0x80000000ull
#define N1_WIN_CPU 0xb80000000ull
#define N1_WIN_SIZE_DEFAULT 0x40000000ull

void n1_init(struct n1 *n, const struct n1_hal *h, const struct n1_addrs *a, const n1_u8 *fw, size_t fw_size);
/* Run stages in order up to `stop` (N1_ST_TRAINED .. N1_ST_RADIO). 0 on success, else -1 with n->err. */
int n1_bringup(struct n1 *n, enum n1_stage stop);
/* Tick: drain rings, deliver RX frames, finish scans/connects. Never blocks. */
void n1_poll(struct n1 *n);
/* FTAB lookup (exported for tests). 0 on success. */
int n1_ftab_entry(const n1_u8 *img, size_t size, const char tag[4], n1_u32 *off, n1_u32 *len, int sparse);

/* M5WN backend: wn_backend_ops-compatible functions (ctx = struct n1 *). */
int n1_wn_tx(void *ctx, const n1_u8 *frame, n1_u32 len);
n1_u32 n1_wn_scan(void *ctx);
n1_u32 n1_wn_results(void *ctx, n1_u32 first, wn_bss *out, n1_u32 max, n1_u32 *total);
n1_u32 n1_wn_join(void *ctx, const wn_join *j);
n1_u32 n1_wn_leave(void *ctx);
n1_u32 n1_wn_status(void *ctx, wn_wifi_status *st);
#endif
