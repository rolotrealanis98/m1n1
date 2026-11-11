/* SPDX-License-Identifier: MIT */
/*
 * SMCP core. See smcp_core.h. Linux citations (pinned tree a0f2f30c, "GPL-2.0-only OR MIT"
 * files; behaviour ported, no code copied) are given per decision; LINUX-MAP.md has the list.
 */
#include "smcp_core.h"

/* ---- key table ------------------------------------------------------------------------- */

enum { SET_SLOW = 1, SET_FAST = 2 };

struct smcp_keydef {
    uint32_t key;
    uint8_t size;   /* expected size; 0 = BCF0 special (1 or 4) */
    uint8_t set;
};

#define K4(s) SMCP_KEY((s)[0], (s)[1], (s)[2], (s)[3])

/* Every key here is read by Linux on this machine (LINUX-MAP.md, "SMC keys"). Order = poll order. */
static const struct smcp_keydef keydefs[SMCP_NKEYS] = {
    [K_BUIC] = {K4("BUIC"), 1, SET_SLOW}, /* macsmc-power.c:587 */
    [K_B0AV] = {K4("B0AV"), 2, SET_SLOW}, /* :595 */
    [K_B0AC] = {K4("B0AC"), 2, SET_SLOW}, /* :599 */
    [K_B0RM] = {K4("B0RM"), 2, SET_SLOW}, /* :646 */
    [K_B0FC] = {K4("B0FC"), 2, SET_SLOW}, /* :642 */
    [K_B0DC] = {K4("B0DC"), 2, SET_SLOW}, /* :638 */
    [K_B0CT] = {K4("B0CT"), 2, SET_SLOW}, /* :670 */
    [K_B0AT] = {K4("B0AT"), 2, SET_SLOW}, /* :662 */
    [K_B0TE] = {K4("B0TE"), 2, SET_SLOW}, /* :571 */
    [K_B0TF] = {K4("B0TF"), 2, SET_SLOW}, /* :579 */
    [K_BNCB] = {K4("BNCB"), 1, SET_SLOW}, /* :1165 */
    [K_BBAD] = {K4("BBAD"), 1, SET_SLOW}, /* :678 */
    [K_BSFC] = {K4("BSFC"), 1, SET_SLOW}, /* :277,530 */
    [K_BCF0] = {K4("BCF0"), 0, SET_SLOW}, /* :497-508, size picked at :1108-1121 */
    [K_CHIS] = {K4("CHIS"), 4, SET_SLOW}, /* :824 */
    [K_CHCE] = {K4("CHCE"), 1, SET_SLOW}, /* :258 */
    [K_CHCC] = {K4("CHCC"), 1, SET_SLOW}, /* :265 */
    [K_CHSC] = {K4("CHSC"), 1, SET_SLOW}, /* :319 */
    [K_CH0R] = {K4("CH0R"), 4, SET_SLOW}, /* :253 */
    [K_CHNC] = {K4("CHNC"), 8, SET_SLOW}, /* :305 */
    [K_ACi] = {K4("AC-i"), 2, SET_SLOW},  /* :272 (absent on this machine per t1b) */
    [K_CHWA] = {K4("CHWA"), 1, SET_SLOW}, /* :291,1128 */
    [K_CHLS] = {K4("CHLS"), 2, SET_SLOW}, /* :287,1130 (only when CHWA does not read) */
    [K_ACSt] = {K4("ACSt"), 4, SET_SLOW}, /* :521 */
    [K_ACPW] = {K4("ACPW"), 4, SET_SLOW}, /* :836 */
    [K_TCMb] = {K4("TCMb"), 4, SET_SLOW}, /* t6050-die0.dtsi:112-113 (macsmc-hwmon) */
    [K_TB0T] = {K4("TB0T"), 4, SET_SLOW}, /* hwmon-laptop.dtsi:15 */
    [K_TCHP] = {K4("TCHP"), 4, SET_SLOW}, /* hwmon-laptop.dtsi:20 */
    [K_TW0P] = {K4("TW0P"), 4, SET_SLOW}, /* hwmon-laptop.dtsi:25 */
    [K_TH0x] = {K4("TH0x"), 4, SET_SLOW}, /* hwmon-common.dtsi:25 */
    [K_F0Ac] = {K4("F0Ac"), 4, SET_SLOW}, /* hwmon-fan.dtsi:10, t6050-j716s.dts fan section */
    [K_F1Ac] = {K4("F1Ac"), 4, SET_SLOW}, /* hwmon-fan-dual.dtsi:16 */
    [K_F0Mx] = {K4("F0Mx"), 4, SET_SLOW}, /* hwmon-fan.dtsi (apple,fan-maximum) */
    [K_F1Mx] = {K4("F1Mx"), 4, SET_SLOW}, /* hwmon-fan-dual.dtsi */
    [K_MSLD] = {K4("MSLD"), 1, SET_FAST}, /* macsmc-input.c:111,185,217 */
    [K_bHLD] = {K4("bHLD"), 4, SET_FAST}, /* macsmc-input.c:186,227 */
};

/* ---- small helpers (freestanding) --------------------------------------------------------- */

static void zero(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

static uint32_t le32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint64_t le64(const uint8_t *b) { return le32(b) | ((uint64_t)le32(b + 4) << 32); }

static uint64_t now(smcp *s) { return s->ops.now_us(s->ops.ctx); }

static bool vok(const smcp *s, int k) { return s->v[k].present && s->v[k].ok; }
static uint8_t vu8(const smcp *s, int k) { return s->v[k].b[0]; }
static uint16_t vu16(const smcp *s, int k) { return le16(s->v[k].b); }
static uint32_t vu32(const smcp *s, int k) { return le32(s->v[k].b); }
static uint64_t vu64(const smcp *s, int k) { return le64(s->v[k].b); }

#define R(off) (s->regs[(off) / 4])

int32_t smcp_flt_to_milli(uint32_t bits)
{
    uint32_t exp = (bits >> 23) & 0xff, mant = bits & 0x7fffff;
    bool neg = bits >> 31;
    uint64_t x;
    if (exp == 0)
        return 0; /* zero and denormals (< 1.2e-38) */
    if (exp == 0xff)
        return neg ? INT32_MIN : INT32_MAX;
    x = (uint64_t)(mant | 0x800000u) * 1000u; /* < 2^34 */
    int e = (int)exp - 150;
    if (e >= 0) {
        if (e > 30 || (x << e) >> e != x || (x << e) > (uint64_t)INT32_MAX)
            return neg ? INT32_MIN : INT32_MAX;
        x <<= e;
    } else {
        x = (-e >= 63) ? 0 : x >> -e;
        if (x > (uint64_t)INT32_MAX)
            return neg ? INT32_MIN : INT32_MAX;
    }
    return neg ? -(int32_t)x : (int32_t)x;
}

static uint32_t milli_c_to_dk(int32_t m)
{
    int64_t mm = m;
    int64_t dk = (mm >= 0 ? (mm + 50) / 100 : (mm - 50) / 100) + 2732;
    return dk > 0 ? (uint32_t)dk : 0;
}

/* ---- battery logic (behaviour of macsmc-power.c) ----------------------------------------- */

#define CHNC_BATTERY_FULL (1ull << 0)  /* macsmc-power.c:39 */
#define CHNC_BMS_BUSY     (1ull << 23) /* :44 */
#define CH0R_LOWER_FLAGS  0xffffu      /* :49 */
#define CH0R_BMS_BUSY     (1u << 8)    /* :53 */
#define ACSt_CAN_BOOT_AP    (1u << 2)  /* :60 */
#define ACSt_CAN_BOOT_IBOOT (1u << 1)  /* :61 */
#define CHWA_CHLS_FIXED_START_OFFSET 5 /* :63 */
#define CHLS_MIN_END_THRESHOLD 10      /* :64 */
#define CHWA_FIXED_END_THRESHOLD 80    /* :66 */
#define NOMINAL_CELL_MV 3800u          /* :36 MACSMC_NOMINAL_CELL_VOLTAGE_MV */

/* macsmc_battery_charging_status (:215-226): status_from_current is J700-only, so CHARGING. */
static int charging_status(void) { return SMCP_CHG_CHARGING; }

/* macsmc_battery_get_status (:234-327). A failed read of a key Linux treats as fundamental
 * (CHCE, CHCC, BSFC, CHSC) makes Linux return an error; here that is UNKNOWN. */
int smcp_batt_status(const smcp *s)
{
    int limit = 0;
    bool limited = false;

    if (vok(s, K_CH0R) && (vu32(s, K_CH0R) & CH0R_LOWER_FLAGS & ~CH0R_BMS_BUSY))
        return SMCP_CHG_DISCHARGING;
    if (!vok(s, K_CHCE))
        return SMCP_CHG_UNKNOWN;
    if (!vu8(s, K_CHCE))
        return SMCP_CHG_DISCHARGING;
    if (!vok(s, K_CHCC))
        return SMCP_CHG_UNKNOWN;
    if (!vu8(s, K_CHCC))
        return SMCP_CHG_DISCHARGING;
    if (vok(s, K_ACi) && vu16(s, K_ACi) < 100)
        return SMCP_CHG_DISCHARGING;
    if (!vok(s, K_BSFC))
        return SMCP_CHG_UNKNOWN;
    if (vu8(s, K_BSFC))
        return SMCP_CHG_FULL;

    bool has_chwa = s->v[K_CHWA].ever;               /* :1128-1131 */
    bool has_chls = !has_chwa && s->v[K_CHLS].ever;
    if (has_chls) {
        if (vok(s, K_CHLS) && (vu16(s, K_CHLS) & 0xff) >= CHLS_MIN_END_THRESHOLD)
            limit = (vu16(s, K_CHLS) & 0xff) - CHWA_CHLS_FIXED_START_OFFSET;
    } else if (has_chwa) {
        if (vok(s, K_CHWA) && vu8(s, K_CHWA))
            limit = CHWA_FIXED_END_THRESHOLD - CHWA_CHLS_FIXED_START_OFFSET;
    }
    if (limit > 0 && vok(s, K_BUIC) && vu8(s, K_BUIC) >= limit)
        limited = true;

    if (vok(s, K_CHNC)) {
        uint64_t f = vu64(s, K_CHNC);
        if (f & CHNC_BATTERY_FULL)
            return SMCP_CHG_FULL;
        if (f == CHNC_BMS_BUSY && !limited)
            return charging_status();
        if (f)
            return SMCP_CHG_NOT_CHARGING;
        return charging_status();
    }
    if (!vok(s, K_CHSC))
        return SMCP_CHG_UNKNOWN;
    if (!vu8(s, K_CHSC))
        return SMCP_CHG_NOT_CHARGING;
    return charging_status();
}

static bool bcf0_read(const smcp *s, uint32_t *val)
{
    if (!vok(s, K_BCF0))
        return false;
    *val = s->fw_ge_27 ? vu8(s, K_BCF0) : vu32(s, K_BCF0);
    return true;
}

/* macsmc_battery_get_capacity_level (:510-537) */
int smcp_cap_level(const smcp *s)
{
    uint32_t v;
    if (bcf0_read(s, &v) && v)
        return SMCP_CAP_CRITICAL;
    if (vok(s, K_ACSt)) {
        v = vu32(s, K_ACSt);
        if (!(v & ACSt_CAN_BOOT_IBOOT))
            return SMCP_CAP_CRITICAL;
        if (!(v & ACSt_CAN_BOOT_AP))
            return SMCP_CAP_LOW;
    }
    if (!vok(s, K_BSFC))
        return SMCP_CAP_UNKNOWN;
    return vu8(s, K_BSFC) ? SMCP_CAP_FULL : SMCP_CAP_NORMAL;
}

/* ---- register file ----------------------------------------------------------------------- */

static void raise(smcp *s, uint32_t ev) { s->evt_pending |= ev & SMCP_EV_ALL; }

static void update_status(smcp *s)
{
    uint32_t st = R(SMCP_R_STATUS) & (SMCP_ST_VALID | SMCP_ST_BATT);
    if (s->phase == SMCP_PH_FAILED)
        st |= SMCP_ST_FAILED;
    else
        st |= SMCP_ST_BACKEND;
    if (s->phase == SMCP_PH_RUN || s->phase == SMCP_PH_FAILED) {
        if (s->v[K_CHIS].present)
            st |= SMCP_ST_AC;
        if (s->v[K_MSLD].present)
            st |= SMCP_ST_LIDKEY;
        if (s->v[K_bHLD].present)
            st |= SMCP_ST_PWRBKEY;
    }
    if ((st & SMCP_ST_VALID) && (s->phase == SMCP_PH_FAILED || now(s) - s->last_slow_done > SMCP_STALE_US))
        st |= SMCP_ST_STALE;
    if (s->flag_power_actions)
        st |= SMCP_ST_POWERACT;
    R(SMCP_R_STATUS) = st;
    R(SMCP_R_ERR_COUNT) = s->err_count;
    R(SMCP_R_LAST_ERR) = s->last_err_key;
    R(SMCP_R_TIMEOUTS) = s->timeouts;
}

static uint32_t u16_or_unknown(const smcp *s, int k)
{
    return vok(s, k) ? vu16(s, k) : 0xffffffffu;
}

static void fast_done(smcp *s)
{
    if (s->v[K_MSLD].present && s->v[K_MSLD].ok) {
        uint32_t lid = vu8(s, K_MSLD) ? 0 : 1; /* MSLD 1 = closed (macsmc-input.c:111-121) */
        if (s->prev_valid && lid != s->prev_lid)
            raise(s, SMCP_EV_LID);
        s->prev_lid = lid;
        R(SMCP_R_LID) = lid;
    }
    if (s->v[K_bHLD].present && s->v[K_bHLD].ok) {
        uint32_t held = vu32(s, K_bHLD) & 1; /* macsmc-input.c:227-233 */
        if (held && !s->prev_bhld) {
            R(SMCP_R_PWRB_COUNT)++;
            raise(s, SMCP_EV_PWRB);
        }
        s->prev_bhld = held;
    }
    update_status(s);
}

static void slow_done(smcp *s)
{
    int status = smcp_batt_status(s);
    int level = smcp_cap_level(s);
    bool batt = vok(s, K_B0AV) && status > SMCP_CHG_UNKNOWN; /* probe :1036-1038 */
    uint32_t st;

    st = 0;
    if (status == SMCP_CHG_CHARGING)
        st = SMCP_BST_CHARGING;
    else if (status == SMCP_CHG_DISCHARGING)
        st = SMCP_BST_DISCHARGING;
    if (level == SMCP_CAP_CRITICAL)
        st |= SMCP_BST_CRITICAL;

    R(SMCP_R_CHG_STATUS) = (uint32_t)status;
    R(SMCP_R_CAP_LEVEL) = (uint32_t)level;
    R(SMCP_R_BST_STATE) = st;
    if (vok(s, K_B0AC)) {
        int16_t ma = (int16_t)vu16(s, K_B0AC);
        R(SMCP_R_BST_RATE) = ma < 0 ? (uint32_t)(-(int32_t)ma) : (uint32_t)ma;
    } else {
        R(SMCP_R_BST_RATE) = 0xffffffffu;
    }
    if (vok(s, K_B0RM)) {
        uint16_t rm = vu16(s, K_B0RM);
        if (!s->fw_ge_27) /* macsmc_b0rm_to_mah (:540-543): swapped before firmware 27 */
            rm = (uint16_t)((rm >> 8) | (rm << 8));
        R(SMCP_R_BST_REMAIN) = rm;
    } else {
        R(SMCP_R_BST_REMAIN) = 0xffffffffu;
    }
    R(SMCP_R_BST_VOLTAGE) = u16_or_unknown(s, K_B0AV);
    R(SMCP_R_BIX_DESIGN) = u16_or_unknown(s, K_B0DC);
    R(SMCP_R_BIX_FULL) = u16_or_unknown(s, K_B0FC);
    R(SMCP_R_BIX_CYCLES) = u16_or_unknown(s, K_B0CT);
    if (vok(s, K_BNCB) && (int8_t)vu8(s, K_BNCB) > 0)
        R(SMCP_R_BIX_VOLTAGE) = (uint32_t)(int8_t)vu8(s, K_BNCB) * NOMINAL_CELL_MV; /* :1166 */
    else
        R(SMCP_R_BIX_VOLTAGE) = 0xffffffffu;
    R(SMCP_R_BATT_PERCENT) = vok(s, K_BUIC) ? vu8(s, K_BUIC) : 0xffffffffu;
    R(SMCP_R_BATT_TEMP) = u16_or_unknown(s, K_B0AT);
    R(SMCP_R_BATT_TTE) = (vok(s, K_B0TE) && vu16(s, K_B0TE) != 0xffff) ? vu16(s, K_B0TE) : 0xffffffffu;
    R(SMCP_R_BATT_TTF) = (vok(s, K_B0TF) && vu16(s, K_B0TF) != 0xffff) ? vu16(s, K_B0TF) : 0xffffffffu;
    R(SMCP_R_PSR) = (vok(s, K_CHIS) && vu32(s, K_CHIS)) ? 1 : 0; /* :824-826 */
    R(SMCP_R_BATT_HEALTH) = (vok(s, K_BBAD) && vu8(s, K_BBAD)) ? 1 : 0;
    R(SMCP_R_AC_POWER) = vok(s, K_ACPW) ? vu32(s, K_ACPW) : 0;

    static const int temps[SMCP_NTEMPS] = {K_TCMb, K_TB0T, K_TCHP, K_TW0P, K_TH0x};
    for (int i = 0; i < SMCP_NTEMPS; i++)
        R(SMCP_R_TEMP0 + 4 * i) = vok(s, temps[i]) ? milli_c_to_dk(smcp_flt_to_milli(vu32(s, temps[i]))) : 0;
    static const int fans[4] = {K_F0Ac, K_F1Ac, K_F0Mx, K_F1Mx};
    for (int i = 0; i < 4; i++) {
        int32_t m = vok(s, fans[i]) ? smcp_flt_to_milli(vu32(s, fans[i])) : 0;
        R(SMCP_R_FAN0_RPM + 4 * i) = m > 0 ? (uint32_t)(m / 1000) : 0;
    }

    uint32_t full = R(SMCP_R_BIX_FULL), pct = R(SMCP_R_BATT_PERCENT), psr = R(SMCP_R_PSR);
    if (!s->prev_valid) {
        /* first data: tell Windows to re-read everything it may have read while not VALID */
        raise(s, SMCP_EV_BATT | SMCP_EV_AC | SMCP_EV_LID);
    } else {
        if (st != s->prev_bst_state || pct != s->prev_percent || full != s->prev_full || psr != s->prev_psr)
            raise(s, SMCP_EV_BATT);
        if (psr != s->prev_psr)
            raise(s, SMCP_EV_AC);
    }
    s->prev_bst_state = st;
    s->prev_percent = pct;
    s->prev_full = full;
    s->prev_psr = psr;
    s->prev_valid = true;

    uint32_t stbits = R(SMCP_R_STATUS) | SMCP_ST_VALID;
    stbits = batt ? (stbits | SMCP_ST_BATT) : (stbits & ~SMCP_ST_BATT);
    R(SMCP_R_STATUS) = stbits;
    R(SMCP_R_SEQ)++;
    s->last_slow_done = now(s);
    update_status(s);
}

/* ---- client ------------------------------------------------------------------------------ */

static void fail_backend(smcp *s)
{
    s->phase = SMCP_PH_FAILED;
    s->busy = false;
    update_status(s);
}

static void record_err(smcp *s, uint32_t key)
{
    s->err_count++;
    s->last_err_key = key;
}

static bool send_cmd(smcp *s, uint8_t kind, uint32_t key, const uint8_t *wbuf, uint8_t len, int slot,
                     uint64_t timeout_us)
{
    uint8_t type = kind == SMCP_K_INFO ? SMCP_MSG_GET_KEY_INFO
                   : kind == SMCP_K_WRITE ? SMCP_MSG_WRITE_KEY : SMCP_MSG_READ_KEY;
    s->msgid = (s->msgid + 1) & 0xf;
    if (kind == SMCP_K_WRITE)
        s->ops.shmem_write(s->ops.ctx, wbuf, len); /* macsmc.c:161-162 */
    /* macsmc.c:98-102: MSG 7:0, ID 15:12, SIZE 23:16, WSIZE 31:24 (0 here), DATA 63:32 */
    uint64_t msg = (uint64_t)type | ((uint64_t)(s->msgid & 0xf) << 12) |
                   ((uint64_t)(kind == SMCP_K_INFO ? 0 : len) << 16) | ((uint64_t)key << 32);
    if (s->ops.send(s->ops.ctx, msg) < 0) {
        /* treated like a timeout: back off, count towards FAILED, retry the same step */
        s->last_result = -2;
        record_err(s, key);
        s->quarantine_until = now(s) + SMCP_QUARANTINE_US;
        if (++s->consecutive_timeouts >= SMCP_MAX_TIMEOUTS)
            fail_backend(s);
        return false;
    }
    s->busy = true;
    s->cur_id = s->msgid;
    s->cur_kind = kind;
    s->cur_len = len;
    s->cur_slot = slot;
    s->cur_key = key;
    s->t_sent = now(s);
    s->timeout_us = timeout_us;
    return true;
}

static void handle_notification(smcp *s, uint32_t ev)
{
    s->notifications++;
    s->last_notification = ev;
    uint16_t type = ev >> 16;
    if (type == 0x7201) { /* macsmc-input.c:47,55-72: button events */
        uint8_t button = (ev >> 8) & 0xff, state = ev & 0xff;
        if ((button == 0x01 || button == 0x06) && state) {
            R(SMCP_R_PWRB_COUNT)++;
            raise(s, SMCP_EV_PWRB);
        }
    } else if (type == 0x7203) { /* lid: re-read MSLD now */
        s->next_fast = 0;
    } else if ((ev & 0xff000000u) == 0x71000000u) { /* macsmc-power.c:940-951: power events */
        s->next_slow = 0;
    }
}

static void complete(smcp *s, uint64_t m)
{
    uint8_t result = m & 0xff;
    uint8_t size = (m >> 16) & 0xff;
    uint32_t data = (uint32_t)(m >> 32);
    smcp_val *v = s->cur_slot >= 0 ? &s->v[s->cur_slot] : NULL;

    s->busy = false;
    s->consecutive_timeouts = 0;
    s->last_result = result;
    s->last_reply_len = size;
    s->last_reply_data = data;

    if (s->cur_kind == SMCP_K_INFO) {
        if (!v)
            return;
        if (result) {
            v->present = false; /* absent key (e.g. AC-i: result 0x84 on this machine) */
            record_err(s, s->cur_key);
            return;
        }
        uint8_t info[6];
        s->ops.shmem_read(s->ops.ctx, info, 6); /* macsmc.c:255-260 */
        uint8_t want = keydefs[s->cur_slot].size;
        bool good = want ? info[0] == want : (info[0] == 1 || info[0] == 4);
        v->present = good;
        v->len = info[0];
        if (!good)
            record_err(s, s->cur_key);
        return;
    }
    if (s->cur_kind == SMCP_K_READ && v) {
        if (result || size > s->cur_len || size == 0) { /* macsmc.c:110-115,177-178 */
            v->ok = false;
            record_err(s, s->cur_key);
            return;
        }
        zero(v->b, sizeof(v->b));
        if (s->cur_len <= 4) { /* macsmc.c:180-189 */
            for (unsigned i = 0; i < size; i++)
                v->b[i] = (data >> (8 * i)) & 0xff;
        } else {
            s->ops.shmem_read(s->ops.ctx, v->b, size);
        }
        v->ok = true;
        v->ever = true;
        return;
    }
    if (result)
        record_err(s, s->cur_key);
}

static void pump(smcp *s)
{
    for (unsigned n = 0; n < SMCP_RECV_BUDGET; n++) {
        uint64_t m;
        int r = s->ops.recv(s->ops.ctx, &m);
        if (r < 0) {
            fail_backend(s);
            return;
        }
        if (r == 0)
            break;
        if ((m & 0xff) == SMCP_MSG_NOTIFICATION) { /* macsmc.c:429,452 */
            handle_notification(s, (uint32_t)(m >> 32));
            continue;
        }
        if (s->busy && ((m >> 12) & 0xf) == s->cur_id)
            complete(s, m);
        /* else: a late reply to a timed-out command; dropped (quarantine covers its SRAM) */
    }
    if (s->busy && now(s) - s->t_sent > s->timeout_us) {
        s->busy = false;
        s->last_result = -1;
        s->timeouts++;
        s->consecutive_timeouts++;
        record_err(s, s->cur_key);
        /* a timed-out read keeps its last good value (STALE/FAILED say how old it is); an
         * SMC error result clears it, as Linux's read would fail */
        s->quarantine_until = now(s) + SMCP_QUARANTINE_US;
        if (s->consecutive_timeouts >= SMCP_MAX_TIMEOUTS)
            fail_backend(s);
    }
}

/* ---- schedule ---------------------------------------------------------------------------- */

static bool wanted(const smcp *s, int k, uint8_t set)
{
    if (!(keydefs[k].set & set) || !s->v[k].present)
        return false;
    if (k == K_CHLS && s->v[K_CHWA].ever) /* Linux reads CHLS only without a readable CHWA */
        return false;
    return true;
}

static int next_in(const smcp *s, int from, uint8_t set)
{
    for (int k = from; k < SMCP_NKEYS; k++)
        if (wanted(s, k, set))
            return k;
    return -1;
}

static void finish_info(smcp *s)
{
    s->fw_ge_27 = s->v[K_BCF0].present && s->v[K_BCF0].len == 1; /* :1108-1121 */
    s->phase = SMCP_PH_RUN;
    s->slow_idx = s->fast_idx = -1;
    s->next_slow = s->next_fast = 0;
    update_status(s);
}

void smcp_tick(smcp *s)
{
    if (s->phase == SMCP_PH_FAILED)
        return;
    pump(s);
    if (s->phase == SMCP_PH_FAILED)
        return;
    if (s->busy || now(s) < s->quarantine_until) {
        update_status(s);
        return;
    }

    if (s->phase == SMCP_PH_INFO) {
        if (s->info_idx < SMCP_NKEYS) {
            int k = s->info_idx++;
            if (!send_cmd(s, SMCP_K_INFO, keydefs[k].key, NULL, 0, k, SMCP_TIMEOUT_US))
                s->info_idx = k;
            return;
        }
        finish_info(s);
    }

    uint64_t t = now(s);
    if (s->fast_idx < 0 && s->next_fast <= t) {
        s->fast_idx = 0;
        s->next_fast = t + SMCP_FAST_PERIOD_US;
    }
    if (s->fast_idx >= 0) {
        int k = next_in(s, s->fast_idx, SET_FAST);
        if (k >= 0) {
            s->fast_idx = k + 1;
            if (!send_cmd(s, SMCP_K_READ, keydefs[k].key, NULL, s->v[k].len, k, SMCP_TIMEOUT_US))
                s->fast_idx = k;
            return;
        }
        s->fast_idx = -1;
        fast_done(s);
    }
    if (s->slow_idx < 0 && s->next_slow <= t)
        s->slow_idx = 0;
    if (s->slow_idx >= 0) {
        int k = next_in(s, s->slow_idx, SET_SLOW);
        if (k >= 0) {
            s->slow_idx = k + 1;
            if (!send_cmd(s, SMCP_K_READ, keydefs[k].key, NULL, s->v[k].len, k, SMCP_TIMEOUT_US))
                s->slow_idx = k;
            return;
        }
        s->slow_idx = -1;
        s->next_slow = t + SMCP_SLOW_PERIOD_US;
        slow_done(s);
    }
    update_status(s);
}

void smcp_init(smcp *s, const smcp_ops *ops, uint32_t record_flags)
{
    zero(s, sizeof(*s));
    s->ops = *ops;
    s->phase = SMCP_PH_INFO;
    s->slow_idx = s->fast_idx = -1;
    s->flag_power_actions = (record_flags & SMCP_REC_F_POWER_ACTIONS) != 0;
    R(SMCP_R_MAGIC) = SMCP_MAGIC;
    R(SMCP_R_VERSION) = SMCP_VERSION;
    R(SMCP_R_BST_RATE) = R(SMCP_R_BST_REMAIN) = R(SMCP_R_BST_VOLTAGE) = 0xffffffffu;
    R(SMCP_R_BIX_DESIGN) = R(SMCP_R_BIX_FULL) = R(SMCP_R_BIX_VOLTAGE) = R(SMCP_R_BIX_CYCLES) = 0xffffffffu;
    R(SMCP_R_BATT_PERCENT) = R(SMCP_R_BATT_TEMP) = R(SMCP_R_BATT_TTE) = R(SMCP_R_BATT_TTF) = 0xffffffffu;
    R(SMCP_R_LID) = 1; /* open until MSLD says otherwise */
    s->prev_lid = 1;
    update_status(s);
}

uint32_t smcp_reg_read(smcp *s, uint32_t off)
{
    if ((off & 3) || off >= SMCP_R_REG_END)
        return 0;
    if (off == SMCP_R_EVT_PENDING)
        return s->evt_pending;
    if (off == SMCP_R_EVT_ENABLE)
        return s->evt_enable;
    if (off == SMCP_R_EVT_ACK)
        return 0;
    return s->regs[off / 4];
}

void smcp_reg_write(smcp *s, uint32_t off, uint32_t value)
{
    if (off == SMCP_R_EVT_ACK)
        s->evt_pending &= ~value;
    else if (off == SMCP_R_EVT_ENABLE)
        s->evt_enable = value & SMCP_EV_ALL;
    /* everything else: write ignored */
}

bool smcp_irq(const smcp *s) { return (s->evt_pending & s->evt_enable) != 0; }
bool smcp_failed(const smcp *s) { return s->phase == SMCP_PH_FAILED; }

int smcp_cmd_blocking(smcp *s, uint8_t kind, uint32_t key, const uint8_t *wbuf, uint8_t len,
                      uint64_t timeout_us)
{
    uint64_t t0 = now(s);
    /* a command still outstanding (or quarantined) gets up to its own timeout first */
    while ((s->busy || now(s) < s->quarantine_until) && s->phase != SMCP_PH_FAILED) {
        pump(s);
        if (now(s) - t0 > SMCP_ATOMIC_TIMEOUT_US + SMCP_QUARANTINE_US)
            return -3;
    }
    if (s->phase == SMCP_PH_FAILED)
        return -4;
    if (!send_cmd(s, kind, key, wbuf, len, -1, timeout_us))
        return -2;
    while (s->busy && s->phase != SMCP_PH_FAILED)
        pump(s);
    if (s->phase == SMCP_PH_FAILED && s->busy)
        return -4;
    return s->last_result;
}
