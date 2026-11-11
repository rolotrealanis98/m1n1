/* SPDX-License-Identifier: MIT */
#include "m5wn_fake.h"

const wn_u8 wn_fake_gw_mac[6] = {0x02, 0x4d, 0x35, 0x57, 0x4e, 0x01};

static wn_u16 be16(const wn_u8 *p) { return (wn_u16)(p[0] << 8 | p[1]); }
static wn_u32 be32(const wn_u8 *p) { return (wn_u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(wn_u8 *p, wn_u32 v) { p[0] = (wn_u8)(v >> 8); p[1] = (wn_u8)v; }
static void put32(wn_u8 *p, wn_u32 v) { p[0] = (wn_u8)(v >> 24); p[1] = (wn_u8)(v >> 16); p[2] = (wn_u8)(v >> 8); p[3] = (wn_u8)v; }
static int eq(const wn_u8 *a, const wn_u8 *b, wn_u32 n)
{
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}
static wn_u16 csum(const wn_u8 *p, wn_u32 n)
{
    wn_u32 sum = 0;
    for (; n > 1; n -= 2, p += 2) sum += (wn_u32)(p[0] << 8 | p[1]);
    if (n) sum += (wn_u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (wn_u16)~sum;
}

static int link_up(const wn_fake *f) { return (f->svc->status & WN_ST_LINK) != 0; }

static void eth_hdr(wn_u8 *o, const wn_u8 *dst, wn_u16 type)
{
    wn_copy(o, dst, 6);
    wn_copy(o + 6, wn_fake_gw_mac, 6);
    put16(o + 12, type);
}

static void ip_hdr(wn_u8 *ip, wn_u32 total, wn_u8 proto, wn_u32 src, wn_u32 dst)
{
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, total);
    put16(ip + 4, 0); put16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, src); put32(ip + 16, dst);
    put16(ip + 10, csum(ip, 20));
}

static void handle_arp(wn_fake *f, const wn_u8 *fr, wn_u32 len)
{
    const wn_u8 *a = fr + 14;
    wn_u8 *o = f->frame;
    if (len < 42 || be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || be16(a + 6) != 1) return;
    if (be32(a + 24) != WN_FAKE_GW_IP) return; /* never answer for the client's own address (DAD) */
    wn_zero(o, 60);
    eth_hdr(o, a + 8, 0x0806);
    put16(o + 14, 1); put16(o + 16, 0x0800); o[18] = 6; o[19] = 4; put16(o + 20, 2);
    wn_copy(o + 22, wn_fake_gw_mac, 6); put32(o + 28, WN_FAKE_GW_IP);
    wn_copy(o + 32, a + 8, 6); wn_copy(o + 38, a + 14, 4);
    if (!wn_service_rx(f->svc, o, 60)) f->arp_replies++;
}

static void handle_icmp(wn_fake *f, const wn_u8 *fr, wn_u32 len, const wn_u8 *ip, wn_u32 ihl, wn_u32 tot)
{
    wn_u8 *o = f->frame;
    wn_u32 n = tot - ihl;
    const wn_u8 *ic = ip + ihl;
    if (n < 8 || ic[0] != 8 || ic[1] != 0 || 14 + tot > sizeof(f->frame) || 14 + tot > len) return;
    wn_zero(o, 60);
    eth_hdr(o, fr + 6, 0x0800);
    ip_hdr(o + 14, 20 + n, 1, WN_FAKE_GW_IP, be32(ip + 12));
    wn_copy(o + 34, ic, n);
    o[34] = 0; /* echo reply */
    put16(o + 36, 0);
    put16(o + 36, csum(o + 34, n));
    if (!wn_service_rx(f->svc, o, 34 + n < 60 ? 60 : 34 + n)) f->icmp_replies++;
}

static wn_u32 dhcp_type(const wn_u8 *opt, wn_u32 n)
{
    wn_u32 i = 0;
    while (i < n) {
        wn_u8 code = opt[i];
        if (code == 0) { i++; continue; }
        if (code == 255 || i + 1 >= n) break;
        if (i + 2 + opt[i + 1] > n) break;
        if (code == 53 && opt[i + 1] == 1) return opt[i + 2];
        i += 2u + opt[i + 1];
    }
    return 0;
}

static void handle_dhcp(wn_fake *f, const wn_u8 *udp, wn_u32 ulen)
{
    const wn_u8 *b = udp + 8;
    wn_u32 n = ulen - 8, type, reply;
    wn_u8 *o = f->frame, *ob, *op;
    static const wn_u8 bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (n < 240 || b[0] != 1 || b[1] != 1 || b[2] != 6 || be32(b + 236) != 0x63825363) return;
    type = dhcp_type(b + 240, n - 240);
    if (type == 1) reply = 2;      /* DISCOVER -> OFFER */
    else if (type == 3) reply = 5; /* REQUEST -> ACK */
    else return;
    wn_zero(o, sizeof(f->frame));
    eth_hdr(o, bcast, 0x0800);
    ob = o + 42;
    ob[0] = 2; ob[1] = 1; ob[2] = 6; ob[3] = 0;
    wn_copy(ob + 4, b + 4, 4);    /* xid */
    wn_copy(ob + 10, b + 10, 2);  /* flags */
    put32(ob + 16, WN_FAKE_CLIENT_IP);
    put32(ob + 20, WN_FAKE_GW_IP);
    wn_copy(ob + 28, b + 28, 16); /* chaddr */
    put32(ob + 236, 0x63825363);
    op = ob + 240;
    *op++ = 53; *op++ = 1; *op++ = (wn_u8)reply;
    *op++ = 54; *op++ = 4; put32(op, WN_FAKE_GW_IP); op += 4;
    *op++ = 51; *op++ = 4; put32(op, 86400); op += 4;
    *op++ = 1; *op++ = 4; put32(op, 0xffffff00u); op += 4;
    *op++ = 3; *op++ = 4; put32(op, WN_FAKE_GW_IP); op += 4;
    *op++ = 6; *op++ = 4; put32(op, WN_FAKE_GW_IP); op += 4;
    *op++ = 255;
    {
        wn_u32 blen = (wn_u32)(op - ob);
        if (blen < 300) blen = 300; /* BOOTP minimum */
        put16(o + 34, 67); put16(o + 36, 68); put16(o + 38, 8 + blen); put16(o + 40, 0);
        ip_hdr(o + 14, 28 + blen, 17, WN_FAKE_GW_IP, 0xffffffffu);
        if (!wn_service_rx(f->svc, o, 42 + blen)) {
            if (reply == 2) f->dhcp_offers++;
            else f->dhcp_acks++;
            wn_copy(f->client_mac, b + 28, 6);
        }
    }
}

static void handle_ipv4(wn_fake *f, const wn_u8 *fr, wn_u32 len)
{
    const wn_u8 *ip = fr + 14;
    wn_u32 ihl, tot;
    if (len < 34 || (ip[0] >> 4) != 4) return;
    ihl = (ip[0] & 15u) * 4u;
    tot = be16(ip + 2);
    if (ihl < 20 || tot < ihl || 14 + tot > len || (be16(ip + 6) & 0x3fff)) return;
    if (ip[9] == 1 && be32(ip + 16) == WN_FAKE_GW_IP) handle_icmp(f, fr, len, ip, ihl, tot);
    else if (ip[9] == 17 && tot >= ihl + 8) {
        const wn_u8 *u = ip + ihl;
        wn_u32 ulen = be16(u + 4);
        if (be16(u) == 68 && be16(u + 2) == 67 && ulen >= 8 && ulen <= tot - ihl) handle_dhcp(f, u, ulen);
    }
}

static int fake_tx(void *ctx, const wn_u8 *fr, wn_u32 len)
{
    wn_fake *f = (wn_fake *)ctx;
    if (!link_up(f)) return -1; /* not associated: the radio would drop it */
    f->tx_seen++;
    if (!f->gateway) return f->fwd ? f->fwd(f->fwd_ctx, fr, len) : -1;
    if (len < 14) return 0;
    if (be16(fr + 12) == 0x0806) handle_arp(f, fr, len);
    else if (be16(fr + 12) == 0x0800) handle_ipv4(f, fr, len);
    return 0;
}

static void fake_session(void *ctx, int open)
{
    wn_fake *f = (wn_fake *)ctx;
    if (open && f->autolink) {
        f->state = WN_WIFI_CONNECTED;
        f->joined = 0;
        wn_service_set_link(f->svc, 1, 1000);
    }
}

static wn_u32 fake_scan(void *ctx)
{
    wn_fake *f = (wn_fake *)ctx;
    f->scans++;
    wn_service_event(f->svc); /* the synthetic scan completes at once */
    return WN_OK;
}

static wn_u32 fake_results(void *ctx, wn_u32 first, wn_bss *out, wn_u32 max, wn_u32 *total)
{
    wn_fake *f = (wn_fake *)ctx;
    wn_u32 n = 0;
    *total = f->scans ? WN_FAKE_NBSS : 0;
    while (first + n < *total && n < max) {
        out[n] = f->bss[first + n];
        n++;
    }
    return n;
}

static wn_u32 fake_join(void *ctx, const wn_join *j)
{
    wn_fake *f = (wn_fake *)ctx;
    wn_u32 i;
    for (i = 0; i < WN_FAKE_NBSS; i++)
        if (f->bss[i].ssid_len == j->ssid_len && eq(f->bss[i].ssid, j->ssid, j->ssid_len)) break;
    if (i == WN_FAKE_NBSS) {
        f->state = WN_WIFI_FAILED; f->last_error = 1;
        wn_service_event(f->svc);
        return WN_OK; /* accepted; the result arrives as an event, like the radio's */
    }
    if (!(f->bss[i].security_mask & (1u << j->security))) {
        f->state = WN_WIFI_FAILED; f->last_error = 2;
        wn_service_event(f->svc);
        return WN_OK;
    }
    f->state = WN_WIFI_CONNECTED; f->last_error = 0; f->joined = i;
    wn_service_set_link(f->svc, 1, 300);
    wn_service_event(f->svc);
    return WN_OK;
}

static wn_u32 fake_leave(void *ctx)
{
    wn_fake *f = (wn_fake *)ctx;
    f->state = WN_WIFI_IDLE;
    wn_service_set_link(f->svc, 0, 0);
    return WN_OK;
}

static wn_u32 fake_status(void *ctx, wn_wifi_status *st)
{
    wn_fake *f = (wn_fake *)ctx;
    st->state = f->state;
    st->last_error = f->last_error;
    st->scan_done_count = f->scans;
    if (f->state == WN_WIFI_CONNECTED) st->current = f->bss[f->joined];
    return WN_OK;
}

const wn_backend_ops wn_fake_ops = {
    fake_tx, 0, fake_session, fake_scan, fake_results, fake_join, fake_leave, fake_status,
};

static void set_bss(wn_bss *b, const char *ssid, wn_u8 last, wn_u16 ch, wn_s16 rssi, wn_u16 mask)
{
    wn_u32 n = 0;
    wn_zero(b, sizeof(*b));
    while (ssid[n] && n < WN_SSID_MAX) { b->ssid[n] = (wn_u8)ssid[n]; n++; }
    b->ssid_len = (wn_u8)n;
    wn_copy(b->bssid, wn_fake_gw_mac, 6);
    b->bssid[5] = last;
    b->channel = ch;
    b->rssi_dbm = rssi;
    b->security_mask = mask;
}

void wn_fake_init(wn_fake *f, wn_service *svc, int gateway, int autolink)
{
    wn_zero(f, sizeof(*f));
    f->svc = svc;
    f->gateway = gateway;
    f->autolink = autolink;
    set_bss(&f->bss[0], "m5wn-open", 0x10, 6, -40, 1u << WN_SEC_OPEN);
    set_bss(&f->bss[1], "m5wn-wpa2", 0x11, 36, -55, 1u << WN_SEC_WPA2_PSK | 1u << WN_SEC_WPA3_SAE);
}
