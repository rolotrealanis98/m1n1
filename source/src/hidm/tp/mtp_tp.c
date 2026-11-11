/* SPDX-License-Identifier: MIT */
#include "mtp_tp.h"
#define LOG(t, ...) do { if ((t)->io.log) (t)->io.log(__VA_ARGS__); } while (0)

static hm_u32 le16(const hm_u8 *p) { return p[0] | ((hm_u32)p[1] << 8); }
static int name_is(const hm_u8 *p, const char *want)
{
    unsigned i = 0;
    for (; want[i]; i++)
        if (p[i] != (hm_u8)want[i]) return 0;
    for (; i < 16; i++)
        if (p[i]) return 0;
    return 1;
}
const char *mtp_tp_phase_name(enum tp_phase p)
{
    static const char *const n[] = {"wait-init", "enable-mt", "enable-actuator", "actuator-mode",
        "firmware", "off-will", "off-afe", "off-has", "on-will", "on-afe", "on-has",
        "ready", "dims", "live", "failed"};
    return (unsigned)p <= TP_FAILED ? n[p] : "?";
}
static const char *error_name(enum tp_error e)
{
    static const char *const n[] = {"none", "timeout", "ack", "firmware", "afe", "dims",
                                    "topology", "init", "ptp"};
    return (unsigned)e <= TP_E_PTP ? n[e] : "?";
}
static void fail(struct mtp_tp *t, enum tp_error e)
{
    if (t->phase == TP_FAILED) return;
    LOG(t, "MTP-TP: FAILED error=%s in phase=%s (keyboard unaffected)\n", error_name(e),
        mtp_tp_phase_name(t->phase));
    t->error = e;
    t->phase = TP_FAILED;
    t->waiting = 0;
}

int mtp_tp_init(struct mtp_tp *t, const struct mtp_tp_io *io, const hm_u8 *fw, hm_u32 len)
{
    hm_u32 hlen, dlen, ioff;
    hm_zero(t, sizeof(*t));
    t->io = *io;
    t->phase = TP_WAIT_INIT;
    /* dchid_get_firmware: magic, version 1, header length >= 20 and inside the
     * file, data inside the file, iface byte offset inside the data. */
    if (!fw || len < TP_FW_HDR || len > TP_FW_MAX || hm_le32(fw) != TP_FW_MAGIC ||
        hm_le32(fw + 4) != 1) { fail(t, TP_E_FW); return -1; }
    hlen = hm_le32(fw + 8); dlen = hm_le32(fw + 12); ioff = hm_le32(fw + 16);
    if (hlen < TP_FW_HDR || hlen > len || dlen > len - hlen || !dlen || ioff >= dlen ||
        !io->dma_buf || dlen > io->dma_cap) { fail(t, TP_E_FW); return -1; }
    t->fw = fw + hlen; t->fw_len = dlen; t->fw_iface_off = ioff;
    return 0;
}

void mtp_tp_on_init(struct mtp_tp *t, const hm_u8 *m, unsigned n)
{
    unsigned id, p = 22;
    int mt;
    if (t->phase == TP_FAILED || n < 22) return;
    mt = name_is(m + 4, "multi-touch");
    if (!mt && !name_is(m + 4, "actuator")) return;
    id = m[3];
    if (!id) { fail(t, TP_E_TOPOLOGY); return; }
    if (mt) {
        if ((t->mt_found && id != t->mt) || (t->act_found && id == t->act)) {
            fail(t, TP_E_TOPOLOGY); return;
        }
        t->mt_found = 1; t->mt = (hm_u8)id;
        while (p < n) {
            unsigned type, blen;
            if (n - p < 4) { fail(t, TP_E_INIT); return; }
            type = le16(m + p); blen = le16(m + p + 2); p += 4;
            if (blen > n - p) { fail(t, TP_E_INIT); return; }
            if (type == 0)
                LOG(t, "MTP-TP: multi-touch iface=%u HID descriptor block %u bytes (kept by "
                       "firmware; Windows sees the synthesized PTP descriptor)\n", id, blen);
            if (type == 1) {
                t->mt_gpio_announced = 1;
                LOG(t, "MTP-TP: multi-touch announces a GPIO request block (%u bytes)\n", blen);
            }
            p += blen;
            if (type == 2) break;
        }
        if (!m[20]) t->mt_complete = 1;
    } else {
        if ((t->act_found && id != t->act) || (t->mt_found && id == t->mt)) {
            fail(t, TP_E_TOPOLOGY); return;
        }
        t->act_found = 1; t->act = (hm_u8)id;
        if (!m[20]) t->act_complete = 1;
    }
}

void mtp_tp_on_ready(struct mtp_tp *t, hm_u8 iface)
{
    if (t->mt_found && iface == t->mt) {
        if (!t->mt_ready) LOG(t, "MTP-TP: multi-touch READY iface=%u (phase %s)\n", iface,
                              mtp_tp_phase_name(t->phase));
        t->mt_ready = 1;
    } else if (t->act_found && iface == t->act) {
        if (!t->act_ready) LOG(t, "MTP-TP: actuator READY iface=%u\n", iface);
        t->act_ready = 1;
    }
}

int mtp_tp_on_ack(struct mtp_tp *t, hm_u8 iface, hm_u8 seq, hm_u8 flags, hm_u32 rc,
                  const hm_u8 *m, unsigned n)
{
    /* dchid_handle_ack: same iface, flags, sequence and first byte; length >= 1. */
    if (!t->waiting || t->w_done || iface != t->w_iface || flags != t->w_flags ||
        seq != t->w_seq || !n || m[0] != t->w_report) {
        t->acks_ignored++;
        return 0;
    }
    t->w_rc = rc;
    t->w_resp_len = n < sizeof(t->w_resp) ? n : sizeof(t->w_resp);
    hm_copy(t->w_resp, m, t->w_resp_len);
    t->w_done = 1;
    return 1;
}

static int command(struct mtp_tp *t, hm_u8 iface, hm_u8 flags, const hm_u8 *msg, unsigned len)
{
    hm_u8 seq = 0, used = 0;
    if (iface == t->mt && iface) seq = t->mt_seq;
    else if (iface == t->act && iface) seq = t->act_seq;
    if (t->io.send(t->io.ctx, iface, flags, seq, msg, len, &used) < 0) return 0; /* busy */
    /* Linux increments the per-interface counter after every command. */
    if (iface && iface == t->mt) t->mt_seq++;
    else if (iface && iface == t->act) t->act_seq++;
    t->waiting = 1; t->w_done = 0; t->w_iface = iface; t->w_flags = flags;
    t->w_report = msg[0]; t->w_seq = used; t->w_rc = 0; t->w_resp_len = 0;
    t->deadline = t->io.now_us(t->io.ctx) + TP_CMD_TIMEOUT_US;
    return 1;
}

/* Returns 1 when the outstanding command finished OK, 0 while waiting, -1 on error. */
static int finished(struct mtp_tp *t, hm_u64 now, int fatal)
{
    if (!t->waiting) return 0;
    if (t->w_done) {
        t->waiting = 0;
        if (t->w_rc) {
            LOG(t, "MTP-TP: command %#x iface=%u retcode=%#x in %s\n", t->w_report, t->w_iface,
                t->w_rc, mtp_tp_phase_name(t->phase));
            if (fatal) fail(t, TP_E_ACK);
            return -1;
        }
        return 1;
    }
    if (now >= t->deadline) {
        t->waiting = 0;
        LOG(t, "MTP-TP: command %#x iface=%u timed out in %s\n", t->w_report, t->w_iface,
            mtp_tp_phase_name(t->phase));
        if (fatal) fail(t, TP_E_TIMEOUT);
        return -1;
    }
    return 0;
}

static void pm2(hm_u8 *m, hm_u8 iface, hm_u8 state, hm_u8 has)
{
    hm_zero(m, 9);
    m[0] = 0x40; m[1] = 2; m[2] = iface; m[3] = state; m[4] = has;
}

static void pulse_step(struct mtp_tp *t, hm_u64 now)
{
    hm_u8 ack[10], used;
    if (!t->pulse || now < t->pulse_until) return;
    if (t->pulse == 1) {
        if (t->io.afe_reset(t->io.ctx, 0) < 0) LOG(t, "MTP-TP: GPIO pulse release failed\n");
        t->pulse = 2;
    }
    ack[0] = 0xa1; ack[1] = ack[2] = ack[3] = ack[4] = 0;     /* retcode 0, packed */
    hm_copy(ack + 5, t->pulse_evt, 5);
    if (t->io.send(t->io.ctx, 0, 0x80, 0, ack, sizeof(ack), &used) < 0) return; /* retry */
    t->pulse = 0;
}

int mtp_tp_on_gpio(struct mtp_tp *t, const hm_u8 *m, unsigned n)
{
    /* dchid_handle_gpio (FIFO mode): cmd 3 = pulse: set 1, 10 ms, set 0, ACK 0.
     * Only for the multi-touch interface; anything else is NACKed by the caller. */
    if (n != 5 || !t->mt_found || m[1] != t->mt || m[4] != 3 || t->pulse ||
        t->phase == TP_FAILED)
        return 0;
    if (t->io.afe_reset(t->io.ctx, 1) < 0) return 0;
    LOG(t, "MTP-TP: firmware GPIO pulse request gpio=%u -> AFE asserted 10 ms\n", m[2]);
    hm_copy(t->pulse_evt, m, 5);
    t->pulse = 1;
    t->pulse_until = t->io.now_us(t->io.ctx) + TP_GPIO_PULSE_US;
    return 1;
}

static void afe(struct mtp_tp *t, hm_u64 now, int asserted, enum tp_phase next)
{
    if (!t->afe_until) {
        if (t->io.afe_reset(t->io.ctx, asserted) < 0) { fail(t, TP_E_AFE); return; }
        LOG(t, "MTP-TP: AFE reset %s (gp1c), holding 50 ms\n", asserted ? "asserted" : "released");
        t->afe_until = now + TP_AFE_HOLD_US;
        return;
    }
    if (now >= t->afe_until) { t->afe_until = 0; t->phase = next; }
}

void mtp_tp_step(struct mtp_tp *t)
{
    hm_u64 now;
    hm_u8 m[16];
    int r;
    if (t->phase == TP_FAILED) return;
    now = t->io.now_us(t->io.ctx);
    pulse_step(t, now);
    switch (t->phase) {
    case TP_WAIT_INIT:
        if (!t->mt_complete) {
            LOG(t, "MTP-TP: multi-touch %s -> no trackpad\n",
                t->mt_found ? "INIT incomplete" : "not announced");
            fail(t, TP_E_TOPOLOGY); return;
        }
        LOG(t, "MTP-TP: multi-touch iface=%u actuator %s%u\n", t->mt,
            t->act_found ? "iface=" : "absent ", t->act_found ? t->act : 0);
        t->phase = TP_EN_MT;
        /* fall through */
    case TP_EN_MT:
        if (!t->waiting) { m[0] = 0xb4; m[1] = t->mt; command(t, 0, 0x80, m, 2); return; }
        if ((r = finished(t, now, 1)) > 0) t->phase = t->act_complete ? TP_EN_ACT : TP_FW;
        if (r > 0 && !t->act_complete) LOG(t, "MTP-TP: no actuator: no click haptics\n");
        return;
    case TP_EN_ACT:
        if (!t->waiting) { m[0] = 0xb4; m[1] = t->act; command(t, 0, 0x80, m, 2); return; }
        /* Haptics never block the pointer: a failed actuator step only logs. */
        if ((r = finished(t, now, 0)) != 0) t->phase = r > 0 ? TP_ACT_MODE : TP_FW;
        return;
    case TP_ACT_MODE:
        /* hid-apple-mtp-haptic probe: feature SET {0x21, 0} = device-controlled:
         * the actuator firmware plays the click itself on press and release. */
        if (!t->waiting) { m[0] = 0x21; m[1] = 0; command(t, t->act, 0x80, m, 2); return; }
        if ((r = finished(t, now, 0)) != 0) {
            LOG(t, "MTP-TP: actuator device-controlled click mode %s\n", r > 0 ? "set" : "NOT set");
            t->phase = TP_FW;
        }
        return;
    case TP_FW:
        if (!t->waiting) {
            hm_copy(t->io.dma_buf, t->fw, t->fw_len);
            if (t->fw_iface_off) t->io.dma_buf[t->fw_iface_off] = t->mt;
            m[0] = 0x95; m[1] = 2; m[2] = 0; m[3] = t->mt;
            for (r = 0; r < 8; r++) m[4 + r] = (hm_u8)(t->io.dma_iova >> (8 * r));
            hm_put32(m + 12, t->fw_len);
            if (command(t, 0, 0x80, m, 16))
                LOG(t, "MTP-TP: firmware %u bytes at iova %#llx\n", (unsigned)t->fw_len,
                    (unsigned long long)t->io.dma_iova);
            return;
        }
        if (finished(t, now, 1) > 0) t->phase = TP_OFF_WILL;
        return;
    case TP_OFF_WILL: case TP_OFF_HAS: case TP_ON_WILL: case TP_ON_HAS: {
        hm_u8 state = (t->phase == TP_ON_WILL || t->phase == TP_ON_HAS) ? 2 : 0;
        hm_u8 has = (t->phase == TP_OFF_HAS || t->phase == TP_ON_HAS);
        if (!t->waiting) { pm2(m, t->mt, state, has); command(t, 0, 0x80, m, 9); return; }
        if (finished(t, now, 1) > 0) {
            t->phase = t->phase == TP_OFF_WILL ? TP_OFF_AFE : t->phase == TP_OFF_HAS ? TP_ON_WILL :
                       t->phase == TP_ON_WILL ? TP_ON_AFE : TP_READY;
            if (t->phase == TP_READY) t->deadline = now + TP_READY_TIMEOUT_US;
        }
        return;
    }
    case TP_OFF_AFE: afe(t, now, 1, TP_OFF_HAS); return;
    case TP_ON_AFE: afe(t, now, 0, TP_ON_HAS); return;
    case TP_READY:
        if (t->mt_ready) { t->phase = TP_DIMS; return; }
        if (now >= t->deadline) {
            LOG(t, "MTP-TP: multi-touch READY timeout (AFE bootload failed? see RTKit syslog)\n");
            fail(t, TP_E_TIMEOUT);
        }
        return;
    case TP_DIMS:
        /* hid-magicmouse open: feature GET 0xd9 (flags group 2 | GET). */
        if (!t->waiting) { m[0] = 0xd9; command(t, t->mt, 0x81, m, 1); return; }
        if (finished(t, now, 1) > 0) {
            if (amt_parse_dims(t->w_resp, t->w_resp_len, &t->dims) != AMT_OK ||
                ptp_init(&t->ptp, &t->dims)) {
                LOG(t, "MTP-TP: bad dimensions report (%u bytes)\n", (unsigned)t->w_resp_len);
                fail(t, TP_E_DIMS); return;
            }
            LOG(t, "MTP-TP: dimensions %ux%u (0.01 mm) x %d..%d y %d..%d; PTP descriptor %u bytes\n",
                (unsigned)t->dims.width, (unsigned)t->dims.height, t->dims.min_x, t->dims.max_x,
                t->dims.min_y, t->dims.max_y, (unsigned)t->ptp.desc_len);
            t->iface.info.id = t->mt; t->iface.info.flags = HM_PRESENT;
            t->iface.info.vendor = 0x05ac; t->iface.info.product = 0x035a;
            t->iface.info.version = 0x0510;
            t->iface.info.descriptor_length = t->ptp.desc_len;
            t->iface.descriptor = t->ptp.desc;
            t->phase = TP_LIVE;
            LOG(t, "MTP-TP: trackpad LIVE (iface=%u)\n", t->mt);
        }
        return;
    default:
        return;
    }
}

int mtp_tp_on_report(struct mtp_tp *t, hm_u8 iface, const hm_u8 *m, unsigned n,
                     hm_service *s, hm_u64 t_us)
{
    struct amt_frame f;
    hm_u8 out[PTP_INPUT_LEN];
    hm_u32 len;
    if (!t->mt_found || iface != t->mt) return 0;
    t->reports++;
    if (t->reports <= TP_LOG_REPORTS && n) {
        LOG(t, "MTP-TP: raw report %u len=%u id=%#x count=%u button=%u\n", (unsigned)t->reports, n,
            m[0], n > AMT_COUNT_OFF ? m[AMT_COUNT_OFF] : 0, n > AMT_BUTTON_OFF ? m[AMT_BUTTON_OFF] : 0);
    }
    if (t->phase != TP_LIVE) return 1;
    if (amt_parse(m, n, &f) != AMT_OK) {
        if (n && m[0] != AMT_REPORT_ID) t->other_reports++;
        else t->bad_reports++;
        return 1;
    }
    len = ptp_frame(&t->ptp, &f, t_us, out);
    if (len && s) {
        hm_service_push(s, t->mt, out, len, t_us * 1000);
        t->translated++;
    }
    return 1;
}

hm_u32 mtp_tp_report(struct mtp_tp *t, hm_u32 op, hm_u32 rid, hm_u8 *buf, hm_u32 *len)
{
    if (t->phase != TP_LIVE) return HM_DENIED;
    return ptp_report(&t->ptp, op, rid, buf, len);
}
