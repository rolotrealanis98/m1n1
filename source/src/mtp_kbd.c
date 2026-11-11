/* SPDX-License-Identifier: MIT */
/* Independent implementation from PROTOCOL.md; no Linux driver code copied. */
#include "mtp_kbd.h"
#include "hidm/tp/mtp_tp.h"
#define LOG(k, ...) do { if ((k)->io.log) (k)->io.log(__VA_ARGS__); } while (0)
static hm_u32 le16(const hm_u8 *p) { return p[0] | ((hm_u32)p[1] << 8); }
static int name_is(const hm_u8 *p, const char *want)
{
    /* INIT name is 16 bytes, not necessarily NUL-terminated; exact match, NUL padded. */
    unsigned i = 0;
    for (; want[i]; i++)
        if (p[i] != (hm_u8)want[i]) return 0;
    for (; i < 16; i++)
        if (p[i]) return 0;
    return 1;
}
static const char *phase_name(enum mtp_phase p)
{
    static const char *const n[] = {"enum", "stm", "enable", "live", "failed"};
    return (unsigned)p < 5 ? n[p] : "?";
}
static const char *error_name(enum mtp_error e)
{
    static const char *const n[] = {"none", "timeout", "frame", "overrun", "descriptor",
                                    "topology", "rtkit", "ack", "tx"};
    return (unsigned)e < 9 ? n[e] : "?";
}
/* One console line of up to 32 bytes as hex (no snprintf in this freestanding file). */
static void hexline(struct mtp_kbd *k, const char *tag, unsigned a, unsigned b,
                    const hm_u8 *p, unsigned n)
{
    static const char hx[] = "0123456789abcdef";
    char buf[32 * 3 + 1];
    unsigned o = 0;
    for (unsigned i = 0; i < n && i < 32; i++) {
        buf[o++] = hx[p[i] >> 4]; buf[o++] = hx[p[i] & 15]; buf[o++] = ' ';
    }
    buf[o] = 0;
    LOG(k, "MTP: %s %u/%u: %s\n", tag, a, b, buf);
}
void mtp_kbd_fail(struct mtp_kbd *k, enum mtp_error e)
{
    if (k->phase == MTP_FAILED) return;
    LOG(k, "MTP: FAILED error=%s in phase=%s (frames=%u reports=%u rx_bytes=%u stm=%s)\n",
        error_name(e), phase_name(k->phase), (unsigned)k->frames, (unsigned)k->reports,
        (unsigned)k->rx_bytes, k->stm_found ? "announced" : "absent");
    if (e == MTP_E_TIMEOUT && k->phase == MTP_ENUM && !k->rx_bytes)
        LOG(k, "MTP: decision fifo-polled: no FIFO byte ever arrived -> polled-FIFO assumption "
               "(or MTP never started) is the suspect\n");
    k->phase = MTP_FAILED;
    k->error = e;
    k->tx_len = 0;
    /* Keep descriptor storage immutable after publication; stop input and IRQ. */
    k->service->ready = k->service->enabled = k->service->active = 0;
    k->service->pending = 0;
    if (k->io.input_changed) k->io.input_changed(k->io.ctx);
}
void mtp_kbd_init(struct mtp_kbd *k, const struct mtp_io *io, hm_service *s)
{
    hm_zero(k, sizeof(*k));
    k->io = *io;
    k->service = s;
    k->phase = MTP_ENUM;
    k->deadline = io->now_us(io->ctx) + 20000000;
}
hm_u32 mtp_kbd_report(void *ctx, hm_u32 id, hm_u32 op, hm_u32 rid,
                     hm_u8 *data, hm_u32 *length)
{
    (void)ctx; (void)id; (void)op; (void)rid; (void)data; (void)length;
    return HM_DENIED;
}
/* One command frame (channel 0x11) into the single TX slot. */
static void frame_out(struct mtp_kbd *k, hm_u8 iface, hm_u8 flags, hm_u8 seq,
                      const hm_u8 *msg, unsigned len)
{
    unsigned body = (8 + len + 3) & ~3u;
    hm_zero(k->tx, sizeof(k->tx));
    k->tx[0] = 8; k->tx[1] = 0x11; k->tx[2] = body;
    k->tx[4] = seq; k->tx[5] = iface; k->tx[8] = flags; k->tx[10] = len;
    hm_copy(k->tx + 16, msg, len);
    hm_u32 sum = 0;
    for (unsigned i = 0; i < 8 + body; i += 4) sum += hm_le32(k->tx + i);
    hm_put32(k->tx + 8 + body, ~sum);
    k->tx_len = body + 12;
    k->tx_deadline = k->io.now_us(k->io.ctx) + 500000;
}
static void queue(struct mtp_kbd *k, const hm_u8 *msg, unsigned len)
{
    if (k->tx_len || len > 10) { mtp_kbd_fail(k, MTP_E_TX); return; }
    k->tx_kind = 0;
    frame_out(k, 0, 0x80, k->seq++, msg, len);
}
int mtp_kbd_tp_send(void *ctx, hm_u8 iface, hm_u8 flags, hm_u8 seq, const hm_u8 *msg,
                    unsigned len, hm_u8 *seq_used)
{
    struct mtp_kbd *k = ctx;
    if (k->phase == MTP_FAILED || k->tx_len || !len || len > 24) return -1;
    if (!iface) seq = k->seq++; /* comm: one counter shared with the keyboard's commands */
    *seq_used = seq;
    k->tx_kind = 0;
    frame_out(k, iface, flags, seq, msg, len);
    return 0;
}
hm_u32 mtp_kbd_report_all(void *ctx, hm_u32 id, hm_u32 op, hm_u32 rid, hm_u8 *data,
                          hm_u32 *length)
{
    struct mtp_kbd *k = ctx;
    if (k->tp && k->tp->phase == TP_LIVE && id == k->tp->mt)
        return mtp_tp_report(k->tp, op, rid, data, length);
    return HM_DENIED;
}
static void init_event(struct mtp_kbd *k, const hm_u8 *m, unsigned n)
{
    if (n < 22 || !m[3]) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
    unsigned id = m[3];
    char nm[17];
    for (unsigned i = 0; i < 16; i++) nm[i] = (m[4 + i] >= 0x20 && m[4 + i] < 0x7f) ? m[4 + i] : 0;
    nm[16] = 0;
    LOG(k, "MTP: INIT iface=%u name=\"%s\" more_packets=%u len=%u\n", id, nm, m[20], n);
    k->seen[id / 32] |= 1u << (id % 32);
    if (name_is(m + 4, "stm")) {
        /* Only enabled (never configured further) to follow Linux's order. */
        if (k->found && id == k->iface) { mtp_kbd_fail(k, MTP_E_TOPOLOGY); return; }
        if (!k->stm_found) { k->stm_found = 1; k->stm_iface = id; }
        if (id == k->stm_iface && !m[20]) k->stm_complete = 1;
        return;
    }
    if (!name_is(m + 4, "keyboard")) {
        if ((k->found && id == k->iface) || (k->stm_found && id == k->stm_iface))
            mtp_kbd_fail(k, MTP_E_TOPOLOGY);
        else if (k->tp)
            mtp_tp_on_init(k->tp, m, n);
        return;
    }
    if ((k->found && id != k->iface) || k->complete || (k->stm_found && id == k->stm_iface)) {
        mtp_kbd_fail(k, MTP_E_TOPOLOGY); return;
    }
    k->found = 1; k->iface = id;
    unsigned p = 22;
    while (p < n) {
        if (n - p < 4) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        unsigned type = le16(m + p), len = le16(m + p + 2);
        p += 4;
        if (len > n - p) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        if (type == 0) {
            /* A block is complete, even when INIT metadata spans several packets.
             * Duplicate descriptors are ambiguous; never silently concatenate. */
            if (!len || len > HM_MAX_DESC || k->desc_len) {
                mtp_kbd_fail(k, MTP_E_DESCRIPTOR); return;
            }
            hm_copy(k->descriptor, m + p, len); k->desc_len = len;
        }
        if (type == 1) { /* Keyboard announced a GPIO dependency we cannot satisfy. */
            LOG(k, "MTP: keyboard INIT announces a GPIO (block type 1): unsupported\n");
            mtp_kbd_fail(k, MTP_E_DESCRIPTOR); return;
        }
        p += len;
        if (type == 2) break;
    }
    if (!m[20]) {
        if (!k->desc_len) { mtp_kbd_fail(k, MTP_E_DESCRIPTOR); return; }
        k->complete = 1;
        /* The J716s keyboard descriptor ends in a vendor TLC with a 16384-bit feature
         * field that Windows' HID stack and the HIDM driver reject (n3tp1); publish the
         * descriptor without vendor TLCs. The raw descriptor is logged below first. */
        LOG(k, "MTP: keyboard iface=%u raw report descriptor %u bytes\n", id, (unsigned)k->desc_len);
        for (unsigned o = 0; o < k->desc_len; o += 32)
            hexline(k, "rdesc", o, (unsigned)k->desc_len, k->descriptor + o,
                    k->desc_len - o > 32 ? 32 : k->desc_len - o);
        if (!hid_strip_vendor_tlcs(k->descriptor, k->desc_len, &k->kbd_filter) &&
            k->kbd_filter.removed_tlcs) {
            LOG(k, "MTP: keyboard descriptor: %u vendor TLC(s) removed for Windows, %u -> %u bytes\n",
                (unsigned)k->kbd_filter.removed_tlcs, (unsigned)k->desc_len, (unsigned)k->kbd_filter.len);
            k->desc_len = k->kbd_filter.len;
            k->kbd_filtered = 1;
        }
        k->keyboard.info.id = id; k->keyboard.info.flags = HM_PRESENT;
        k->keyboard.info.vendor = 0x05ac; k->keyboard.info.product = 0x035a;
        k->keyboard.info.version = 0x0510;
        k->keyboard.info.descriptor_length = k->desc_len;
        k->keyboard.descriptor = k->descriptor;
        LOG(k, "MTP: keyboard iface=%u report descriptor %u bytes\n", id, (unsigned)k->desc_len);
        /* Linux order: stm first. Sent from poll(), after any queued GPIO NACK drained. */
        k->phase = MTP_STM;
        k->stm_state = 0;
        k->stm_deadline = k->io.now_us(k->io.ctx) + MTP_STM_ANNOUNCE_US;
        k->deadline = k->io.now_us(k->io.ctx) + 10000000;
    }
}
static void receive_frame(struct mtp_kbd *k)
{
    const hm_u8 *f = k->frame;
    hm_u32 sum = 0;
    for (unsigned i = 0; i < k->expected; i += 4) sum += hm_le32(f + i);
    if (sum != 0xffffffffu) { k->rejected++; mtp_kbd_fail(k, MTP_E_FRAME); return; }
    unsigned body = le16(f + 2), len = le16(f + 10);
    if (len > body - 8) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
    const hm_u8 *m = f + 16;
    k->frames++;
    if (f[1] == 0x11) {
        hm_u32 rc = hm_le32(f + 12);
        if (f[5] == 0 && f[8] == 0x80 && len && m[0] == 0xb4 && !k->ack && k->enable_sent &&
            f[4] == k->enable_seq) {
            LOG(k, "MTP: keyboard enable ACK seq=%u retcode=%#x\n", f[4], rc);
            if (rc) mtp_kbd_fail(k, MTP_E_ACK);
            else k->ack = 1;
        } else if (f[5] == 0 && f[8] == 0x80 && len && m[0] == 0xb4 && !k->stm_ack &&
                   k->stm_sent && f[4] == k->stm_seq) {
            LOG(k, "MTP: stm enable ACK seq=%u retcode=%#x\n", f[4], rc);
            k->stm_ack = rc ? -1 : 1;
        } else if (k->tp) {
            mtp_tp_on_ack(k->tp, f[5], f[4], f[8], rc, m, len);
        }
        return;
    }
    if (f[1] != 0x12 || (f[8] >> 6) != 0 || hm_le32(f + 12)) return;
    if (!f[5]) {
        if (!len) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        if (m[0] == 0xf0) init_event(k, m, len);
        else if (m[0] == 0xf1 && len >= 2) {
            if (k->enable_sent && m[1] == k->iface) {
                LOG(k, "MTP: keyboard READY iface=%u\n", m[1]);
                k->ready = 1;
            } else if (k->stm_sent && m[1] == k->stm_iface) {
                LOG(k, "MTP: stm READY iface=%u\n", m[1]);
                k->stm_ready = 1;
            } else if (k->tp) {
                mtp_tp_on_ready(k->tp, m[1]);
            }
        } else if (m[0] == 0xa0) {
            if (len != 5) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
            if (k->tp && mtp_tp_on_gpio(k->tp, m, len)) return;
            LOG(k, "MTP: GPIO request iface=%u gpio=%u cmd=%u -> NACK 0xe000f00d (as Linux here)\n",
                m[1], m[2], m[4]);
            hm_u8 nack[10] = {0xa1, 0x0d, 0xf0, 0x00, 0xe0};
            hm_copy(nack + 5, m, 5); queue(k, nack, sizeof(nack));
        }
    } else if (k->tp && f[5] != k->iface &&
               mtp_tp_on_report(k->tp, f[5], m, len, k->service, k->io.now_us(k->io.ctx))) {
        if (k->io.input_changed) k->io.input_changed(k->io.ctx);
    } else if (k->phase == MTP_LIVE && f[5] == k->iface) {
        if (!len || len > HM_PAYLOAD) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        if (k->kbd_filtered && !hid_filter_keeps_input(&k->kbd_filter, m[0])) {
            k->kbd_dropped++; /* report of a removed vendor TLC: Windows has no layout for it */
            return;
        }
        hm_service_push(k->service, k->iface, m, len, k->io.now_us(k->io.ctx) * 1000);
        if (k->io.input_changed) k->io.input_changed(k->io.ctx);
        k->reports++;
        if (k->reports <= MTP_LOG_REPORTS)
            hexline(k, "report", (unsigned)k->reports, len, m, len);
        if (k->reports == MTP_LOG_REPORTS)
            LOG(k, "MTP: report log limit reached; further reports are counted only\n");
    }
}
static void tx_poll(struct mtp_kbd *k)
{
    if (!k->tx_len) return;
    hm_u32 space = k->io.read32(k->io.ctx, MTP_TX_FREE);
    if (space > MTP_FIFO_SIZE) { mtp_kbd_fail(k, MTP_E_OVERRUN); return; }
    if (space < k->tx_len) {
        if (k->io.now_us(k->io.ctx) >= k->tx_deadline) mtp_kbd_fail(k, MTP_E_TIMEOUT);
        return;
    }
    for (unsigned i = 0; i < k->tx_len; i += 4)
        k->io.write32(k->io.ctx, MTP_TX32, hm_le32(k->tx + i));
    if (k->tx_kind == 1) k->enable_sent = 1;
    else if (k->tx_kind == 2) k->stm_sent = 1;
    k->tx_kind = 0;
    k->tx_len = 0;
}
/* Only the keyboard and stm interfaces can ever be enabled. */
static void enable(struct mtp_kbd *k, hm_u8 iface, int stm)
{
    if (stm ? !(k->stm_found && iface == k->stm_iface && iface != k->iface)
            : !(k->complete && iface == k->iface)) {
        mtp_kbd_fail(k, MTP_E_TOPOLOGY); return;
    }
    hm_u8 msg[2] = {0xb4, iface};
    if (stm) k->stm_seq = k->seq; else k->enable_seq = k->seq;
    LOG(k, "MTP: enable %s iface=%u seq=%u\n", stm ? "stm" : "keyboard", iface, k->seq);
    queue(k, msg, sizeof(msg));
    k->tx_kind = stm ? 2 : 1;
    tx_poll(k);
}
static void go_keyboard(struct mtp_kbd *k, hm_u64 now)
{
    k->phase = MTP_ENABLE;
    k->deadline = now + 5000000;
}
static void stm_step(struct mtp_kbd *k, hm_u64 now)
{
    if (k->tx_len) return;
    if (k->stm_state == 0) {
        if (k->stm_complete) {
            LOG(k, "MTP: decision stm-first: stm announced as iface=%u -> enabling stm first "
                   "(Linux order)\n", k->stm_iface);
            k->stm_state = 1;
            k->stm_deadline = now + MTP_STM_READY_US;
            enable(k, k->stm_iface, 1);
        } else if (now >= k->stm_deadline) {
            LOG(k, "MTP: decision stm-first: stm %s within %u ms -> keyboard alone (deviation)\n",
                k->stm_found ? "INIT incomplete" : "not announced", MTP_STM_ANNOUNCE_US / 1000);
            go_keyboard(k, now);
        }
    } else if (k->stm_ack && k->stm_ready) {
        LOG(k, "MTP: decision stm-first: stm ACK %s + READY -> enabling keyboard (Linux order)\n",
            k->stm_ack > 0 ? "ok" : "ERROR");
        go_keyboard(k, now);
    } else if (now >= k->stm_deadline) {
        LOG(k, "MTP: decision stm-first: after %u ms stm ACK=%d READY=%d -> enabling keyboard "
               "anyway (deviation)\n", MTP_STM_READY_US / 1000, k->stm_ack, k->stm_ready);
        go_keyboard(k, now);
    }
}
void mtp_kbd_poll(struct mtp_kbd *k)
{
    if (k->phase == MTP_FAILED) return;
    hm_u64 start = k->io.now_us(k->io.ctx);
    if (k->io.poll_control(k->io.ctx) < 0) { mtp_kbd_fail(k, MTP_E_RTKit); return; }
    if ((k->phase != MTP_LIVE && start >= k->deadline) ||
        (k->used && start >= k->partial_deadline)) {
        if (k->phase == MTP_ENABLE)
            LOG(k, "MTP: keyboard enable timeout: ACK=%d READY=%d (stm-first=%s)\n", k->ack,
                k->ready, k->stm_sent ? "yes" : "no");
        mtp_kbd_fail(k, MTP_E_TIMEOUT); return;
    }
    tx_poll(k);
    if (k->phase == MTP_LIVE && k->tp && !k->tx_len) { mtp_tp_step(k->tp); tx_poll(k); }
    if (k->phase == MTP_STM) stm_step(k, start);
    if (k->phase == MTP_ENABLE && !k->enable_sent && !k->tx_len)
        enable(k, k->iface, 0);
    /* 128 words / 512 bytes and 100 us per tick, no wait for any FIFO byte.
     * The deadline bounds batches, not an individual stalled MMIO transaction. */
    for (unsigned words = 0; words < 128 && k->phase != MTP_FAILED; words++) {
        if (k->io.now_us(k->io.ctx) - start >= 100) break;
        hm_u32 count = k->io.read32(k->io.ctx, MTP_RX_COUNT);
        if (count > MTP_FIFO_SIZE) { mtp_kbd_fail(k, MTP_E_OVERRUN); break; }
        if (count < 4) break;
        if (!k->used) k->partial_deadline = start + 500000;
        hm_put32(k->frame + k->used, k->io.read32(k->io.ctx, MTP_RX32));
        k->used += 4;
        k->rx_bytes += 4;
        if (k->used == 8) {
            unsigned body = le16(k->frame + 2);
            if (k->frame[0] != 8 || body < 8 || (body & 3) || body > MTP_FRAME_MAX - 12) {
                mtp_kbd_fail(k, MTP_E_FRAME); break;
            }
            k->expected = body + 12;
        }
        if (k->expected && k->used == k->expected) {
            receive_frame(k); k->used = k->expected = 0;
            tx_poll(k);
            if (k->phase == MTP_ENABLE && k->ack && k->ready) {
                k->phase = MTP_LIVE;
                LOG(k, "MTP: keyboard LIVE (iface=%u, stm-first=%s)\n", k->iface,
                    k->stm_sent ? "yes" : "no");
            }
        }
    }
}
