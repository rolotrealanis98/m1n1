/* SPDX-License-Identifier: MIT */
/* Independent implementation from PROTOCOL.md; no Linux driver code copied. */
#include "mtp_kbd.h"
static hm_u32 le16(const hm_u8 *p) { return p[0] | ((hm_u32)p[1] << 8); }
static int keyboard_name(const hm_u8 *p)
{
    static const hm_u8 name[16] = "keyboard";
    for (unsigned i = 0; i < sizeof(name); i++)
        if (p[i] != name[i]) return 0;
    return 1;
}
void mtp_kbd_fail(struct mtp_kbd *k, enum mtp_error e)
{
    if (k->phase == MTP_FAILED) return;
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
static void queue(struct mtp_kbd *k, const hm_u8 *msg, unsigned len)
{
    if (k->tx_len || len > 10) { mtp_kbd_fail(k, MTP_E_TX); return; }
    unsigned body = (8 + len + 3) & ~3u;
    hm_zero(k->tx, sizeof(k->tx));
    k->tx[0] = 8; k->tx[1] = 0x11; k->tx[2] = body;
    k->tx[4] = k->seq++; k->tx[8] = 0x80; k->tx[10] = len;
    hm_copy(k->tx + 16, msg, len);
    hm_u32 sum = 0;
    for (unsigned i = 0; i < 8 + body; i += 4) sum += hm_le32(k->tx + i);
    hm_put32(k->tx + 8 + body, ~sum);
    k->tx_len = body + 12;
    k->tx_deadline = k->io.now_us(k->io.ctx) + 500000;
}
static void init_event(struct mtp_kbd *k, const hm_u8 *m, unsigned n)
{
    if (n < 22 || !m[3]) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
    unsigned id = m[3];
    k->seen[id / 32] |= 1u << (id % 32);
    if (!keyboard_name(m + 4)) {
        if (k->found && id == k->iface) mtp_kbd_fail(k, MTP_E_TOPOLOGY);
        return;
    }
    if ((k->found && id != k->iface) || k->complete) {
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
            mtp_kbd_fail(k, MTP_E_DESCRIPTOR); return;
        }
        p += len;
        if (type == 2) break;
    }
    if (!m[20]) {
        if (!k->desc_len) { mtp_kbd_fail(k, MTP_E_DESCRIPTOR); return; }
        k->complete = 1;
        k->keyboard.info.id = id; k->keyboard.info.flags = HM_PRESENT;
        k->keyboard.info.vendor = 0x05ac; k->keyboard.info.product = 0x035a;
        k->keyboard.info.version = 0x0510;
        k->keyboard.info.descriptor_length = k->desc_len;
        k->keyboard.descriptor = k->descriptor;
        /* Send in poll(), after any queued GPIO NACK has drained. */
        k->phase = MTP_ENABLE;
        k->deadline = k->io.now_us(k->io.ctx) + 5000000;
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
        if (k->enable_sent && f[5] == 0 && f[4] == k->enable_seq &&
            f[8] == 0x80 && len && m[0] == 0xb4) {
            if (hm_le32(f + 12)) mtp_kbd_fail(k, MTP_E_ACK);
            else k->ack = 1;
        }
        return;
    }
    if (f[1] != 0x12 || (f[8] >> 6) != 0 || hm_le32(f + 12)) return;
    if (!f[5]) {
        if (!len) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        if (m[0] == 0xf0) init_event(k, m, len);
        else if (m[0] == 0xf1 && len >= 2 && k->enable_sent && m[1] == k->iface)
            k->ready = 1;
        else if (m[0] == 0xa0) {
            if (len != 5) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
            hm_u8 nack[10] = {0xa1, 0x0d, 0xf0, 0x00, 0xe0};
            hm_copy(nack + 5, m, 5); queue(k, nack, sizeof(nack));
        }
    } else if (k->phase == MTP_LIVE && f[5] == k->iface) {
        if (!len || len > HM_PAYLOAD) { mtp_kbd_fail(k, MTP_E_FRAME); return; }
        hm_service_push(k->service, k->iface, m, len, k->io.now_us(k->io.ctx) * 1000);
        if (k->io.input_changed) k->io.input_changed(k->io.ctx);
        k->reports++;
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
    if (k->tx[16] == 0xb4) k->enable_sent = 1;
    k->tx_len = 0;
}
void mtp_kbd_poll(struct mtp_kbd *k)
{
    if (k->phase == MTP_FAILED) return;
    hm_u64 start = k->io.now_us(k->io.ctx);
    if (k->io.poll_control(k->io.ctx) < 0) { mtp_kbd_fail(k, MTP_E_RTKit); return; }
    if ((k->phase != MTP_LIVE && start >= k->deadline) ||
        (k->used && start >= k->partial_deadline)) {
        mtp_kbd_fail(k, MTP_E_TIMEOUT); return;
    }
    tx_poll(k);
    if (k->phase == MTP_ENABLE && !k->enable_sent && !k->tx_len) {
        hm_u8 msg[2] = {0xb4, k->iface};
        k->enable_seq = k->seq; queue(k, msg, sizeof(msg)); tx_poll(k);
    }
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
            if (k->phase == MTP_ENABLE && k->ack && k->ready) k->phase = MTP_LIVE;
        }
    }
}
