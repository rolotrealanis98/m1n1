/* SPDX-License-Identifier: MIT */
#include "m5wn_service.h"

void wn_zero(void *p, wn_u32 n)
{
    volatile wn_u8 *b = (volatile wn_u8 *)p;
    while (n--) *b++ = 0;
}
void wn_copy(void *d, const void *s, wn_u32 n)
{
    wn_u8 *a = (wn_u8 *)d;
    const wn_u8 *b = (const wn_u8 *)s;
    while (n--) *a++ = *b++;
}
static wn_u32 rd32(const wn_u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (wn_u32)p[3] << 24; }

static void session_clear(wn_service *s)
{
    s->status &= WN_ST_LINK; /* carrier is backend state, not session state */
    s->session = 0;
    s->region = 0;
    s->region_len = s->tx_slots = s->rx_slots = 0;
    s->tx_prod = s->tx_cons = s->rx_prod = s->rx_cons = 0;
    s->irq_enable = 0;
    s->event_ack = s->event_seq;
    s->filter = s->nmcast = 0;
    s->bl_head = s->bl_tail = 0;
    s->last_seq = 0;
}

void wn_service_init(wn_service *s, const wn_platform *plat, const wn_backend_ops *be, void *be_ctx,
                     const wn_u8 mac[6], wn_u32 features)
{
    wn_zero(s, sizeof(*s));
    s->plat = plat;
    s->be = be;
    s->be_ctx = be_ctx;
    wn_copy(s->mac, mac, 6);
    s->features = features;
    if (be && be->wifi_scan && be->wifi_results && be->wifi_join && be->wifi_leave && be->wifi_status)
        s->features |= WN_F_WIFI;
    else
        s->features &= ~WN_F_WIFI;
}

void wn_service_reset(wn_service *s)
{
    int had = s->session;
    session_clear(s);
    s->status &= ~WN_ST_FAULT;
    if (had && s->be && s->be->session) s->be->session(s->be_ctx, 0);
}

static void fault(wn_service *s)
{
    s->status |= WN_ST_FAULT;
    s->status &= ~WN_ST_READY;
    s->irq_enable = 0;
    s->stats.faults++;
}

static int ready(const wn_service *s) { return (s->status & (WN_ST_READY | WN_ST_FAULT)) == WN_ST_READY; }

int wn_service_irq(const wn_service *s)
{
    if (!ready(s)) return 0;
    if ((s->irq_enable & WN_IRQ_RX) && s->rx_prod != s->rx_cons) return 1;
    if ((s->irq_enable & WN_IRQ_EVENT) && s->event_seq != s->event_ack) return 1;
    return 0;
}

static wn_u64 slot_ipa(const wn_service *s, int rx, wn_u32 counter)
{
    wn_u32 idx = rx ? s->tx_slots + (counter & (s->rx_slots - 1)) : (counter & (s->tx_slots - 1));
    return s->region + (wn_u64)idx * WN_SLOT_BYTES;
}

static int accept_frame(const wn_service *s, const wn_u8 *f)
{
    static const wn_u8 bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    wn_u32 i, j;
    if (s->filter & WN_FILTER_PROMISCUOUS) return 1;
    if (!(f[0] & 1)) {
        for (i = 0; i < 6 && f[i] == s->mac[i]; i++) {}
        return i == 6 && (s->filter & WN_FILTER_DIRECTED);
    }
    for (i = 0; i < 6 && f[i] == bcast[i]; i++) {}
    if (i == 6) return (s->filter & WN_FILTER_BROADCAST) != 0;
    if (s->filter & WN_FILTER_ALL_MULTICAST) return 1;
    if (!(s->filter & WN_FILTER_MULTICAST)) return 0;
    for (j = 0; j < s->nmcast; j++) {
        for (i = 0; i < 6 && f[i] == s->mcast[j][i]; i++) {}
        if (i == 6) return 1;
    }
    return 0;
}

/* Write one frame into the next RX slot: payload first, header last, then publish. */
static int deliver(wn_service *s, const wn_u8 *frame, wn_u32 len)
{
    wn_slot_hdr h;
    wn_u64 ipa = slot_ipa(s, 1, s->rx_prod);
    if (s->plat->guest_write(s->plat->ctx, ipa + WN_SLOT_HDR, frame, len)) return -1;
    h.length = len;
    h.flags = h.reserved0 = h.reserved1 = 0;
    if (s->plat->guest_write(s->plat->ctx, ipa, &h, sizeof(h))) return -1;
    /* The platform's accessor/trap return provides the release ordering (ABI.md). */
    s->rx_prod++;
    s->stats.rx_packets++;
    s->stats.rx_bytes += len;
    return 0;
}

static void drain_backlog(wn_service *s)
{
    while (ready(s) && s->bl_head != s->bl_tail && s->rx_prod - s->rx_cons < s->rx_slots) {
        wn_u32 i = s->bl_tail % WN_BACKLOG;
        if (deliver(s, s->bl_data[i], s->bl_len[i])) { fault(s); return; }
        s->bl_tail++;
    }
}

int wn_service_rx(wn_service *s, const wn_u8 *frame, wn_u32 len)
{
    if (!ready(s) || !frame || len < WN_MIN_FRAME || len > WN_MAX_FRAME) return -1;
    if (!accept_frame(s, frame)) return -1;
    if (s->bl_head == s->bl_tail && s->rx_prod - s->rx_cons < s->rx_slots) {
        if (deliver(s, frame, len)) { fault(s); return -1; }
        return 0;
    }
    if (s->bl_head - s->bl_tail >= WN_BACKLOG) {
        s->rx_drops++;
        s->stats.rx_drops++;
        return -1;
    }
    wn_copy(s->bl_data[s->bl_head % WN_BACKLOG], frame, len);
    s->bl_len[s->bl_head % WN_BACKLOG] = (wn_u16)len;
    s->bl_head++;
    return 0;
}

void wn_service_set_link(wn_service *s, int up, wn_u32 mbps)
{
    wn_u32 old = s->status & WN_ST_LINK;
    if (up) s->status |= WN_ST_LINK;
    else s->status &= ~WN_ST_LINK;
    s->link_mbps = up ? mbps : 0;
    if (old != (s->status & WN_ST_LINK)) s->event_seq++;
}

void wn_service_event(wn_service *s) { s->event_seq++; }

static void tx_doorbell(wn_service *s, wn_u32 prod)
{
    wn_u32 adv = prod - s->tx_cons;
    if (!ready(s)) return;
    if (adv > s->tx_slots) { fault(s); return; }
    s->tx_prod = prod;
    s->in_tx = 1;
    while (s->tx_cons != prod) {
        wn_slot_hdr h;
        wn_u64 ipa = slot_ipa(s, 0, s->tx_cons);
        if (s->plat->guest_read(s->plat->ctx, ipa, &h, sizeof(h))) { fault(s); break; }
        /* snapshot: the length is read exactly once and validated before use */
        if (h.length < WN_MIN_FRAME || h.length > WN_MAX_FRAME || h.flags || h.reserved0 || h.reserved1) {
            s->tx_bad++;
            s->stats.tx_bad++;
        } else if (s->plat->guest_read(s->plat->ctx, ipa + WN_SLOT_HDR, s->scratch, h.length)) {
            fault(s);
            break;
        } else if (!s->be || !s->be->tx || s->be->tx(s->be_ctx, s->scratch, h.length)) {
            s->tx_drops++;
            s->stats.tx_drops++;
        } else {
            s->stats.tx_packets++;
            s->stats.tx_bytes += h.length;
        }
        if (!ready(s)) break; /* a reentrant push may have faulted the session */
        s->tx_cons++;
    }
    s->in_tx = 0;
}

static void rx_ack(wn_service *s, wn_u32 cons)
{
    if (!ready(s)) return;
    if (cons - s->rx_cons > s->rx_prod - s->rx_cons) { fault(s); return; }
    s->rx_cons = cons;
    drain_backlog(s);
}

static wn_u32 do_configure(wn_service *s, const wn_request *r)
{
    wn_u64 base = (wn_u64)r->arg0 | (wn_u64)r->arg1 << 32;
    wn_u32 tx, rx, need;
    if (!s->session || (s->status & WN_ST_READY)) return WN_NOT_READY;
    if (r->length != 8) return WN_BAD_REQUEST;
    tx = rd32(r->payload);
    rx = rd32(r->payload + 4);
    if (tx < WN_MIN_SLOTS || tx > WN_MAX_SLOTS || (tx & (tx - 1)) || rx < WN_MIN_SLOTS ||
        rx > WN_MAX_SLOTS || (rx & (rx - 1)))
        return WN_RANGE;
    need = (tx + rx) * WN_SLOT_BYTES;
    if (r->arg2 != need || (base & 0xfff) || base == 0 || base + need < base) return WN_RANGE;
    if (s->plat->region_ok && s->plat->region_ok(s->plat->ctx, base, need)) return WN_DENIED;
    s->region = base;
    s->region_len = need;
    s->tx_slots = tx;
    s->rx_slots = rx;
    s->tx_prod = s->tx_cons = s->rx_prod = s->rx_cons = 0;
    s->status |= WN_ST_READY;
    return WN_OK;
}

static wn_u32 do_request(wn_service *s, const wn_request *r, wn_response *o)
{
    wn_u32 i, st;
    if (r->abi != WN_VERSION) return WN_BAD_VERSION;
    if (r->reserved || r->length > WN_PAYLOAD) return WN_BAD_REQUEST;
    if (r->op == WN_OP_HELLO) {
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        wn_service_reset(s);
        s->session = 1;
        if (s->be && s->be->session) s->be->session(s->be_ctx, 1);
        return WN_OK;
    }
    if (!s->session || (s->status & WN_ST_FAULT)) return WN_NOT_READY;
    switch (r->op) {
    case WN_OP_CONFIGURE:
        return do_configure(s, r);
    case WN_OP_SET_FILTER:
        if (r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        if (r->arg0 & ~(WN_FILTER_DIRECTED | WN_FILTER_MULTICAST | WN_FILTER_ALL_MULTICAST |
                        WN_FILTER_BROADCAST | WN_FILTER_PROMISCUOUS))
            return WN_RANGE;
        s->filter = r->arg0;
        if (s->be && s->be->set_filter) s->be->set_filter(s->be_ctx, s->filter);
        return WN_OK;
    case WN_OP_SET_MCAST:
        if (r->arg0 || r->arg1 || r->arg2 || r->length % 6 || r->length / 6 > WN_MAX_MCAST) return WN_BAD_REQUEST;
        for (i = 0; i < r->length / 6; i++) {
            if (!(r->payload[i * 6] & 1)) return WN_RANGE;
        }
        s->nmcast = r->length / 6;
        wn_copy(s->mcast, r->payload, r->length);
        return WN_OK;
    case WN_OP_GET_STATS:
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        wn_copy(o->payload, &s->stats, sizeof(s->stats));
        o->length = sizeof(s->stats);
        return WN_OK;
    case WN_OP_BYE:
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        wn_service_reset(s);
        return WN_OK;
    default:
        break;
    }
    if (!(s->features & WN_F_WIFI)) return WN_UNSUPPORTED;
    switch (r->op) {
    case WN_OP_WIFI_SCAN:
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        return s->be->wifi_scan(s->be_ctx);
    case WN_OP_WIFI_RESULTS: {
        wn_bss tmp[WN_RESULTS_MAX];
        wn_u32 total = 0, n;
        if (r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        wn_zero(tmp, sizeof(tmp));
        n = s->be->wifi_results(s->be_ctx, r->arg0, tmp, WN_RESULTS_MAX, &total);
        if (n > WN_RESULTS_MAX) n = WN_RESULTS_MAX;
        for (i = 0; i < n; i++) {
            if (tmp[i].ssid_len > WN_SSID_MAX) tmp[i].ssid_len = WN_SSID_MAX;
            tmp[i].reserved = 0;
            tmp[i].reserved2 = 0;
        }
        wn_copy(o->payload, &total, 4);
        wn_copy(o->payload + 4, &n, 4);
        wn_copy(o->payload + 8, tmp, n * sizeof(wn_bss));
        o->length = 8 + n * (wn_u32)sizeof(wn_bss);
        return WN_OK;
    }
    case WN_OP_WIFI_JOIN: {
        wn_join j;
        if (r->arg0 || r->arg1 || r->arg2 || r->length != sizeof(wn_join)) return WN_BAD_REQUEST;
        wn_copy(&j, r->payload, sizeof(j)); /* snapshot */
        if (j.reserved || j.ssid_len == 0 || j.ssid_len > WN_SSID_MAX || j.security > WN_SEC_WPA3_SAE ||
            j.key_len > WN_KEY_MAX)
            return WN_BAD_REQUEST;
        if (j.security == WN_SEC_OPEN && j.key_len) return WN_BAD_REQUEST;
        if (j.security == WN_SEC_WPA2_PSK && j.key_len != 32) return WN_BAD_REQUEST;
        if (j.security == WN_SEC_WPA3_SAE && (j.key_len < 8 || j.key_len > 63)) return WN_BAD_REQUEST;
        for (i = j.ssid_len; i < WN_SSID_MAX; i++)
            if (j.ssid[i]) return WN_BAD_REQUEST;
        for (i = j.key_len; i < WN_KEY_MAX; i++)
            if (j.key[i]) return WN_BAD_REQUEST;
        st = s->be->wifi_join(s->be_ctx, &j);
        wn_zero(&j, sizeof(j)); /* do not keep key material on the stack */
        return st;
    }
    case WN_OP_WIFI_LEAVE:
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        return s->be->wifi_leave(s->be_ctx);
    case WN_OP_WIFI_STATUS: {
        wn_wifi_status w;
        if (r->arg0 || r->arg1 || r->arg2 || r->length) return WN_BAD_REQUEST;
        wn_zero(&w, sizeof(w));
        st = s->be->wifi_status(s->be_ctx, &w);
        if (st != WN_OK) return st;
        if (w.current.ssid_len > WN_SSID_MAX) w.current.ssid_len = WN_SSID_MAX;
        w.reserved = 0;
        wn_copy(o->payload, &w, sizeof(w));
        o->length = sizeof(w);
        return WN_OK;
    }
    default:
        return WN_BAD_REQUEST;
    }
}

static void doorbell(wn_service *s, wn_u32 seq)
{
    wn_request r;
    wn_copy(&r, s->req_words, sizeof(r)); /* one snapshot; the guest cannot change it mid-call */
    wn_zero(&s->resp, sizeof(s->resp));
    s->resp.seq = seq;
    if (seq == 0 || r.seq != seq || (seq == s->last_seq && r.op != WN_OP_HELLO)) {
        s->resp.status = WN_BAD_REQUEST;
        return;
    }
    s->resp.status = do_request(s, &r, &s->resp);
    if (s->resp.status != WN_OK) s->resp.length = 0;
    s->last_seq = seq;
    wn_zero(&r, sizeof(r));
}

wn_u32 wn_service_read(wn_service *s, unsigned page, wn_u32 off)
{
    if (off & 3 || off >= WN_PAGE) return 0;
    if (page == 1) {
        if (off < WN_RESP_OFF) return s->req_words[off / 4];
        if (off < WN_RESP_OFF + sizeof(wn_response)) return rd32((const wn_u8 *)&s->resp + (off - WN_RESP_OFF));
        return 0;
    }
    if (page != 0) return 0;
    switch (off) {
    case WN_R_MAGIC: return WN_MAGIC;
    case WN_R_VERSION: return WN_VERSION;
    case WN_R_STATUS: return s->status;
    case WN_R_FEATURES: return s->features;
    case WN_R_MAC_LO: return rd32(s->mac);
    case WN_R_MAC_HI: return s->mac[4] | s->mac[5] << 8;
    case WN_R_MTU: return 1500;
    case WN_R_SLOT_SIZE: return WN_SLOT_BYTES;
    case WN_R_TX_PROD: return s->tx_prod;
    case WN_R_TX_CONS: return s->tx_cons;
    case WN_R_RX_PROD: return s->rx_prod;
    case WN_R_RX_CONS: return s->rx_cons;
    case WN_R_IRQ_ENABLE: return s->irq_enable;
    case WN_R_IRQ_STATUS:
        return (ready(s) && s->rx_prod != s->rx_cons ? WN_IRQ_RX : 0) |
               (s->event_seq != s->event_ack ? WN_IRQ_EVENT : 0);
    case WN_R_EVENT_SEQ: return s->event_seq;
    case WN_R_EVENT_ACK: return s->event_ack;
    case WN_R_LINK_MBPS: return s->link_mbps;
    case WN_R_TX_DROPS: return s->tx_drops;
    case WN_R_RX_DROPS: return s->rx_drops;
    case WN_R_TX_BAD: return s->tx_bad;
    default: return 0;
    }
}

void wn_service_write(wn_service *s, unsigned page, wn_u32 off, wn_u32 value)
{
    if (off & 3 || off >= WN_PAGE) return;
    if (page == 1) {
        if (off < WN_RESP_OFF) s->req_words[off / 4] = value;
        return;
    }
    if (page != 0) return;
    switch (off) {
    case WN_R_TX_PROD: tx_doorbell(s, value); break;
    case WN_R_RX_CONS: rx_ack(s, value); break;
    case WN_R_IRQ_ENABLE:
        if (ready(s)) s->irq_enable = value & (WN_IRQ_RX | WN_IRQ_EVENT);
        break;
    case WN_R_EVENT_ACK:
        if (value - s->event_ack <= s->event_seq - s->event_ack) s->event_ack = value;
        break;
    case WN_R_DOORBELL: doorbell(s, value); break;
    default: break;
    }
}
