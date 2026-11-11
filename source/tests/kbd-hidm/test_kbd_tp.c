/* SPDX-License-Identifier: MIT */
/* Keyboard backend + trackpad layer against a byte-level mock MTP FIFO.
 * INIT packets are the T2b synthetic vectors (NOT hardware captures). The mock
 * checks every host frame's checksum, header, sequence and the J716s trackpad
 * order (LINUX-MAP.md in input/trackpad-20261111). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtp_kbd.h"
#include "hidm/tp/mtp_tp.h"
#include "vectors.h"
static unsigned checks, groups;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
struct mock {
    unsigned char rx[65536], tx[8192]; unsigned head, tail, tx_n, tx_done; hm_u64 us;
    int afe, afe_writes; hm_u64 afe_t; int pm[4], fw, act_mode, dims, frames_seen;
    unsigned comm_seq_expect; hm_u8 dma[96 * 1024];
};
static struct mock m; static struct mtp_kbd k; static struct mtp_tp t; static hm_service s;
static hm_u8 FW[28 + 1024];
static hm_u32 rd(void *ctx, hm_u64 a)
{
    struct mock *p = ctx;
    if (a == MTP_RX_COUNT) return p->head - p->tail > 2048 ? 2048 : p->head - p->tail;
    if (a == MTP_TX_FREE) return 2048;
    CHECK(a == MTP_RX32 && p->head - p->tail >= 4);
    hm_u32 v = hm_le32(p->rx + p->tail); p->tail += 4; return v;
}
static void wr(void *ctx, hm_u64 a, hm_u32 v)
{ struct mock *p = ctx; CHECK(a == MTP_TX32 && p->tx_n + 4 <= sizeof(p->tx)); hm_put32(p->tx + p->tx_n, v); p->tx_n += 4; }
static hm_u64 now(void *ctx) { return ((struct mock *)ctx)->us; }
static hm_u64 tp_now(void *ctx) { (void)ctx; return m.us; } /* tp io ctx is the keyboard */
static int ctl(void *ctx) { (void)ctx; return 0; }
static unsigned frame(unsigned char *f, unsigned ch, unsigned seq, unsigned iface, unsigned flags, const void *msg, unsigned n, hm_u32 rc)
{
    unsigned body = (8 + n + 3) & ~3u; memset(f, 0, body + 12); f[0] = 8; f[1] = ch; f[2] = body; f[3] = body >> 8;
    f[4] = seq; f[5] = iface; f[8] = flags; f[10] = n; f[11] = n >> 8; hm_put32(f + 12, rc); memcpy(f + 16, msg, n);
    hm_u32 sum = 0; for (unsigned i = 0; i < body + 8; i += 4) sum += hm_le32(f + i); hm_put32(f + body + 8, ~sum); return body + 12;
}
static void feed(const void *p, unsigned n) { CHECK(m.head + n <= sizeof(m.rx)); memcpy(m.rx + m.head, p, n); m.head += n; }
static void reply(unsigned ch, unsigned seq, unsigned iface, unsigned flags, const void *msg, unsigned n)
{ unsigned char f[128]; feed(f, frame(f, ch, seq, iface, flags, msg, n, 0)); }
static const hm_u8 DIMS[17] = {0xd9, 0x80, 0x3e, 0, 0, 0x10, 0x27, 0, 0, 0xa8, 0xe4, 0xd4, 0xfe, 0x58, 0x1b, 0x34, 0x21};
/* The mock coprocessor: consume complete host frames, verify, answer. */
static void mtp_side(void)
{
    while (m.tx_n - m.tx_done >= 8) {
        unsigned char *f = m.tx + m.tx_done; unsigned body = f[2] | f[3] << 8;
        if (m.tx_n - m.tx_done < body + 12) return;
        hm_u32 sum = 0; for (unsigned i = 0; i < body + 12; i += 4) sum += hm_le32(f + i);
        CHECK(sum == 0xffffffffu && f[0] == 8 && f[1] == 0x11 && !(body & 3));
        unsigned len = f[10] | f[11] << 8, iface = f[5], seq = f[4], flags = f[8]; const hm_u8 *msg = f + 16;
        CHECK(len && len <= body - 8);
        m.frames_seen++;
        if (!iface) { CHECK(seq == (m.comm_seq_expect & 0xff)); m.comm_seq_expect++; }
        if (!iface && msg[0] == 0xb4) {
            reply(0x11, seq, 0, flags, msg, 1);
            if (msg[1] != 1) { hm_u8 r[2] = {0xf1, msg[1]}; reply(0x12, 0, 0, 0, r, 2); }  /* MT: READY after power */
        } else if (iface == 4 && msg[0] == 0x21) {
            CHECK(len == 2 && !msg[1] && flags == 0x80 && seq == 0); m.act_mode = 1; reply(0x11, seq, 4, flags, msg, 1);
        } else if (!iface && msg[0] == 0x95) {
            hm_u64 iova = 0; for (int i = 0; i < 8; i++) iova |= (hm_u64)msg[4 + i] << (8 * i);
            CHECK(len == 16 && msg[1] == 2 && msg[3] == 1 && iova == 0x2000000 && hm_le32(msg + 12) == 1024);
            CHECK(m.dma[10] == 1 && m.dma[11] == FW[28 + 11] && m.act_mode);
            m.fw = 1; reply(0x11, seq, 0, flags, msg, 1);
        } else if (!iface && msg[0] == 0x40) {
            int idx = (msg[3] == 2) * 2 + msg[4];
            CHECK(m.fw && len == 9 && msg[2] == 1 && !m.pm[idx]);
            for (int i = 0; i < idx; i++) CHECK(m.pm[i]);
            if (idx == 1) CHECK(m.afe == 1 && m.us - m.afe_t >= 50000);
            if (idx == 3) { CHECK(m.afe == 0 && m.us - m.afe_t >= 50000); }
            m.pm[idx] = 1; reply(0x11, seq, 0, flags, msg, 1);
            if (idx == 3) { hm_u8 r[2] = {0xf1, 1}; reply(0x12, 0, 0, 0, r, 2); }
        } else if (iface == 1 && msg[0] == 0xd9) {
            CHECK(flags == 0x81 && len == 1); m.dims = 1; reply(0x11, seq, 1, flags, DIMS, 17);
        } else if (!iface && msg[0] == 0xa1) {
            reply(0x11, seq, 0, flags, msg, 1);
        } else CHECK(!"unexpected host frame");
        m.tx_done += body + 12;
    }
}
static int afe(void *ctx, int a) { (void)ctx; m.afe = a; m.afe_t = m.us; m.afe_writes++; return 0; }
static void pump(unsigned ms) { for (unsigned i = 0; i < ms * 20 && k.phase != MTP_FAILED; i++) { mtp_kbd_poll(&k); mtp_side(); m.us += 50; } }
static void reset(int with_tp)
{
    memset(&m, 0, sizeof(m)); memset(&s, 0, sizeof(s));
    struct mtp_io io = {&m, rd, wr, now, ctl, NULL, getenv("KBD_LOG") ? printf : NULL};
    mtp_kbd_init(&k, &io, &s);
    if (with_tp) {
        struct mtp_tp_io tio = {&k, mtp_kbd_tp_send, afe, tp_now, getenv("KBD_LOG") ? printf : NULL, m.dma, 0x2000000, sizeof(m.dma)};
        CHECK(mtp_tp_init(&t, &tio, FW, sizeof(FW)) == 0);
        k.tp = &t;
    }
    groups++;
}
/* real J716s keyboard descriptor (hardware run n3tp1, console "MTP: rdesc") */
static const hm_u8 KBD_HW[189] = {
0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,
0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x29,0xff,0x81,
0x00,0x05,0x0c,0x75,0x01,0x95,0x01,0x09,0xb8,0x15,0x00,0x25,0x01,0x81,0x02,0x05,0xff,0x09,0x03,0x75,0x07,0x95,0x01,0x81,0x02,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,
0x85,0x52,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x09,0xcd,0x81,0x02,0x09,0xb3,0x81,0x02,0x09,0xb4,0x81,0x02,0x09,0xb5,0x81,0x02,0x09,0xb6,0x81,0x02,0x81,0x01,
0x81,0x01,0x81,0x01,0x85,0x09,0x15,0x00,0x25,0x01,0x75,0x08,0x95,0x01,0x06,0x01,0xff,0x09,0x0b,0xb1,0x02,0x75,0x08,0x95,0x02,0xb1,0x01,0xc0,0x06,0x00,0xff,0x09,
0x06,0xa1,0x01,0x06,0x00,0xff,0x09,0x06,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x40,0x85,0x3f,0x81,0x22,0x76,0x00,0x40,0x95,0x02,0xb1,0x01,0xc0};
int main(void)
{
    memset(FW, 0, sizeof(FW)); hm_put32(FW, TP_FW_MAGIC); hm_put32(FW + 4, 1); hm_put32(FW + 8, 28);
    hm_put32(FW + 12, 1024); hm_put32(FW + 16, 10); for (unsigned i = 0; i < 1024; i++) FW[28 + i] = (hm_u8)(i ^ 0x5a);
    /* full bring-up: keyboard first (stm, keyboard), then the trackpad, all on one comm counter */
    reset(1);
    for (unsigned i = 0; i < 7; i++) feed(vectors[i].p, vectors[i].n);
    pump(10);
    CHECK(k.phase == MTP_LIVE && k.iface == 2);
    pump(2000);
    if (t.phase != TP_LIVE) fprintf(stderr, "tp phase %s err %d waiting %d w_report %#x tx_len %u\n", mtp_tp_phase_name(t.phase), t.error, t.waiting, t.w_report, k.tx_len);
    CHECK(t.phase == TP_LIVE && t.mt == 1 && t.act == 4 && m.dims && m.act_mode && m.afe_writes == 2 && m.afe == 0);
    CHECK(m.frames_seen == 2 + 2 + 1 + 1 + 4 + 1);   /* stm, kbd, b4 mt, b4 act, 21, 95, 4 x 40, d9 */
    hm_backend_interface ifs[2] = {k.keyboard, t.iface};
    CHECK(hm_service_init(&s, ifs, 2, mtp_kbd_report_all, &k) == HM_OK);
    s.ready = s.enabled = 1; s.active = 3;
    /* keyboard report still flows */
    feed(vectors[9].p, vectors[9].n); pump(1);
    CHECK(s.head == 1); hm_slot slot; hm_copy(&slot, s.data + HM_RING_OFF, sizeof(slot)); CHECK(slot.iface == 2);
    /* trackpad: 0x75 frames on iface 1 (518-byte frame accepted: not the keyboard's 256 limit) */
    hm_u8 rep[38 + 16 * 30], f[700]; memset(rep, 0, sizeof(rep)); rep[0] = 0x75;
    rep[22] = 1; rep[38 + 18] = 0x40; rep[38 + 6] = 0x00; rep[38 + 7] = 0x10;
    feed(f, frame(f, 0x12, 1, 1, 0, rep, 38 + 30, 0)); pump(1);
    rep[38 + 4] = 0xf0; feed(f, frame(f, 0x12, 2, 1, 0, rep, 38 + 30, 0)); pump(1);
    CHECK(t.translated == 1 && s.head == 2); hm_copy(&slot, s.data + HM_RING_OFF + HM_SLOT_BYTES, sizeof(slot));
    CHECK(slot.iface == 1 && slot.length == PTP_MOUSE_LEN && slot.payload[0] == PTP_RID_MOUSE && slot.payload[2] == 22);
    rep[22] = 16; for (unsigned i = 1; i < 16; i++) rep[38 + i * 30 + 18] = 0x40;
    feed(f, frame(f, 0x12, 3, 1, 0, rep, sizeof(rep), 0)); pump(1);
    CHECK(k.phase == MTP_LIVE && t.reports == 3);
    /* feature through the service callback: PTP caps, keyboard still denied */
    hm_u8 b[300]; hm_u32 l = sizeof(b);
    CHECK(mtp_kbd_report_all(&k, 1, HM_GET_FEATURE, PTP_RID_CAPS, b, &l) == HM_OK && b[1] == 5);
    l = 2; CHECK(mtp_kbd_report_all(&k, 2, HM_GET_FEATURE, 1, b, &l) == HM_DENIED);
    /* a firmware GPIO request for the multi-touch interface is pulsed and ACKed 0 */
    feed(f, frame(f, 0x12, 4, 0, 0, "\xa0\x01\x1c\x00\x03", 5, 0)); pump(30);
    CHECK(m.afe_writes == 4 && m.afe == 0 && k.phase == MTP_LIVE);
    /* one for stm is NACKed as before */
    int before = m.frames_seen;
    feed(f, frame(f, 0x12, 5, 0, 0, "\xa0\x03\x1d\x00\x03", 5, 0)); pump(2);
    CHECK(m.frames_seen == before + 1 && m.afe_writes == 4);
    /* trackpad absent from INIT: keyboard comes up, trackpad fails cleanly */
    reset(1);
    for (unsigned i = 2; i < 4; i++) feed(vectors[i].p, vectors[i].n);
    pump(2000);
    CHECK(k.phase == MTP_LIVE && t.phase == TP_FAILED && t.error == TP_E_TOPOLOGY && m.frames_seen == 2);
    /* without the layer: identical to the keyboard-only backend (only stm + keyboard enabled) */
    reset(0);
    for (unsigned i = 0; i < 7; i++) feed(vectors[i].p, vectors[i].n);
    pump(2000);
    CHECK(k.phase == MTP_LIVE && m.frames_seen == 2);
    /* real keyboard descriptor: vendor TLC removed (189 -> 156), its report 0x3f dropped */
    reset(0);
    { hm_u8 msg[300]; unsigned n = 22; memset(msg, 0, sizeof(msg)); msg[0] = 0xf0; msg[3] = 2; memcpy(msg + 4, "keyboard", 8);
      msg[n++] = 0; msg[n++] = 0; msg[n++] = 189; msg[n++] = 0; memcpy(msg + n, KBD_HW, 189); n += 189;
      msg[n++] = 2; msg[n++] = 0; msg[n++] = 0; msg[n++] = 0;
      feed(f, frame(f, 0x12, 0, 0, 0, msg, n, 0)); }
    pump(1100); pump(50);
    /* fnrow (2026-11-11): the filtered 156 bytes are unchanged and the 35-byte Consumer Control
     * TLC (report 0x53) follows them: 156 + 35 = 191 */
    CHECK(k.phase == MTP_LIVE && k.kbd_filtered && k.fn.enabled && k.desc_len == 156 + FNROW_DESC_APPEND &&
          k.keyboard.info.descriptor_length == 156 + FNROW_DESC_APPEND && !memcmp(k.descriptor, KBD_HW, 156) &&
          k.descriptor[156] == 0x05 && k.descriptor[157] == 0x0c && k.descriptor[163] == FNROW_CC_RID &&
          k.descriptor[156 + FNROW_DESC_APPEND - 1] == 0xc0);
    CHECK(hm_service_init(&s, &k.keyboard, 1, mtp_kbd_report_all, &k) == HM_OK);
    s.ready = s.enabled = s.active = 1;
    { hm_u8 r1[10] = {1, 0, 0, 4}, r3f[65] = {0x3f}; feed(f, frame(f, 0x12, 1, 2, 0, r3f, 65, 0)); feed(f, frame(f, 0x12, 2, 2, 0, r1, 10, 0)); pump(2);
      CHECK(k.kbd_dropped == 1 && s.head == 1 && k.reports == 1); }
    /* fnrow end to end: F10 (media mode) -> report 1 without it + consumer report 0x53 bit 5 (mute);
     * Ctrl+F2 -> backlight counter, nothing for Windows but report 1 with Ctrl; release clears 0x53 */
    { hm_u8 f10[10] = {1, 0, 0, 0x43}, none[10] = {1}, cf2[10] = {1, 0x01, 0, 0x3b};
      feed(f, frame(f, 0x12, 3, 2, 0, f10, 10, 0)); pump(2);
      CHECK(s.head == 3 && k.reports == 2 && k.fn.cc_sent == 0x20 && k.input_seen == 2);
      feed(f, frame(f, 0x12, 4, 2, 0, none, 10, 0)); pump(2);
      CHECK(s.head == 5 && k.fn.cc_sent == 0);
      feed(f, frame(f, 0x12, 5, 2, 0, cf2, 10, 0)); pump(2);
      CHECK(s.head == 6 && k.kbl_up == 1 && k.kbl_down == 0 && k.fn.kbl_held == 1);
      feed(f, frame(f, 0x12, 6, 2, 0, none, 10, 0)); pump(2);
      CHECK(s.head == 7 && k.kbl_up == 1 && k.fn.kbl_held == 0 && k.fn_logged == 2); }
    printf("kbd+tp: %u groups, %u checks PASS\n", groups, checks);
    return 0;
}
