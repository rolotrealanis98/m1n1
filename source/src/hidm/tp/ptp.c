/* SPDX-License-Identifier: MIT */
#include "ptp.h"

/* Sample certification-status blob used by open PTP implementations
 * (bytes from paulsmir/windows-on-m1 apple_precision_touchpad.c, MIT). It is
 * not a Microsoft certification of this device; see WINDOWS-ARCH.md. */
static const hm_u8 cert_blob[256] = {
    0xfc, 0x28, 0xfe, 0x84, 0x40, 0xcb, 0x9a, 0x87, 0x0d, 0xbe, 0x57, 0x3c,
    0xb6, 0x70, 0x09, 0x88, 0x07, 0x97, 0x2d, 0x2b, 0xe3, 0x38, 0x34, 0xb6,
    0x6c, 0xed, 0xb0, 0xf7, 0xe5, 0x9c, 0xf6, 0xc2, 0x2e, 0x84, 0x1b, 0xe8,
    0xb4, 0x51, 0x78, 0x43, 0x1f, 0x28, 0x4b, 0x7c, 0x2d, 0x53, 0xaf, 0xfc,
    0x47, 0x70, 0x1b, 0x59, 0x6f, 0x74, 0x43, 0xc4, 0xf3, 0x47, 0x18, 0x53,
    0x1a, 0xa2, 0xa1, 0x71, 0xc7, 0x95, 0x0e, 0x31, 0x55, 0x21, 0xd3, 0xb5,
    0x1e, 0xe9, 0x0c, 0xba, 0xec, 0xb8, 0x89, 0x19, 0x3e, 0xb3, 0xaf, 0x75,
    0x81, 0x9d, 0x53, 0xb9, 0x41, 0x57, 0xf4, 0x6d, 0x39, 0x25, 0x29, 0x7c,
    0x87, 0xd9, 0xb4, 0x98, 0x45, 0x7d, 0xa7, 0x26, 0x9c, 0x65, 0x3b, 0x85,
    0x68, 0x89, 0xd7, 0x3b, 0xbd, 0xff, 0x14, 0x67, 0xf2, 0x2b, 0xf0, 0x2a,
    0x41, 0x54, 0xf0, 0xfd, 0x2c, 0x66, 0x7c, 0xf8, 0xc0, 0x8f, 0x33, 0x13,
    0x03, 0xf1, 0xd3, 0xc1, 0x0b, 0x89, 0xd9, 0x1b, 0x62, 0xcd, 0x51, 0xb7,
    0x80, 0xb8, 0xaf, 0x3a, 0x10, 0xc1, 0x8a, 0x5b, 0xe8, 0x8a, 0x56, 0xf0,
    0x8c, 0xaa, 0xfa, 0x35, 0xe9, 0x42, 0xc4, 0xd8, 0x55, 0xc3, 0x38, 0xcc,
    0x2b, 0x53, 0x5c, 0x69, 0x52, 0xd5, 0xc8, 0x73, 0x02, 0x38, 0x7c, 0x73,
    0xb6, 0x41, 0xe7, 0xff, 0x05, 0xd8, 0x2b, 0x79, 0x9a, 0xe2, 0x34, 0x60,
    0x8f, 0xa3, 0x32, 0x1f, 0x09, 0x78, 0x62, 0xbc, 0x80, 0xe3, 0x0f, 0xbd,
    0x65, 0x20, 0x08, 0x13, 0xc1, 0xe2, 0xee, 0x53, 0x2d, 0x86, 0x7e, 0xa7,
    0x5a, 0xc5, 0xd3, 0x7d, 0x98, 0xbe, 0x31, 0x48, 0x1f, 0xfb, 0xda, 0xaf,
    0xa2, 0xa8, 0x6a, 0x89, 0xd6, 0xbf, 0xf2, 0xd3, 0x32, 0x2a, 0x9a, 0xe4,
    0xcf, 0x17, 0xb7, 0xb8, 0xf4, 0xe1, 0x33, 0x08, 0x24, 0x8b, 0xc4, 0x43,
    0xa5, 0xe5, 0x24, 0xc2,
};

/* ---- descriptor builder: short items only ---- */
struct db { hm_u8 *d; hm_u32 n, cap; int err; };
static void item(struct db *b, hm_u8 prefix, hm_u32 v, unsigned bytes)
{
    static const hm_u8 code[5] = {0, 1, 2, 0, 3};
    unsigned i;
    if (b->n + 1 + bytes > b->cap) { b->err = 1; return; }
    b->d[b->n++] = (hm_u8)(prefix | code[bytes]);
    for (i = 0; i < bytes; i++) b->d[b->n++] = (hm_u8)(v >> (8 * i));
}
#define I0(p) item(b, p, 0, 0)
#define I1(p, v) item(b, p, v, 1)
#define I2(p, v) item(b, p, v, 2)
#define I4(p, v) item(b, p, v, 4)
/* prefixes (size bits zero) */
enum { UPAGE = 0x04, USAGE = 0x08, COLL = 0xa0, ENDC = 0xc0, RID = 0x84, RSIZE = 0x74, RCOUNT = 0x94,
       LMIN = 0x14, LMAX = 0x24, PMIN = 0x34, PMAX = 0x44, UEXP = 0x54, UNIT = 0x64,
       INPUT = 0x80, FEATURE = 0xb0, UMIN = 0x18, UMAX = 0x28 };

static void finger(struct db *b, const struct ptp *p)
{
    I1(UPAGE, 0x0d); I1(USAGE, 0x22); I1(COLL, 0x02);          /* Finger, logical */
    I1(LMIN, 0); I1(LMAX, 1); I1(RSIZE, 1); I1(RCOUNT, 1);
    I1(USAGE, 0x47); I1(INPUT, 0x02);                          /* Confidence */
    I1(USAGE, 0x42); I1(INPUT, 0x02);                          /* Tip switch */
    I1(RCOUNT, 6); I1(INPUT, 0x03);                            /* padding */
    I1(RSIZE, 8); I1(RCOUNT, 1); I1(LMAX, PTP_CONTACTS - 1);
    I1(USAGE, 0x51); I1(INPUT, 0x02);                          /* Contact ID */
    I1(UPAGE, 0x01); I1(LMIN, 0); I1(RSIZE, 16); I1(RCOUNT, 1);
    I1(UEXP, 0x0e); I1(UNIT, 0x11); I1(PMIN, 0);               /* cm x 10^-2 */
    I4(LMAX, p->xmax); I4(PMAX, p->phys_w); I1(USAGE, 0x30); I1(INPUT, 0x02);   /* X */
    I4(LMAX, p->ymax); I4(PMAX, p->phys_h); I1(USAGE, 0x31); I1(INPUT, 0x02);   /* Y */
    I0(ENDC);
}

static int build(struct ptp *p)
{
    struct db bb = {p->desc, 0, PTP_DESC_MAX, 0}, *b = &bb;
    unsigned i;
    /* Touch pad TLC */
    I1(UPAGE, 0x0d); I1(USAGE, 0x05); I1(COLL, 0x01);
    I1(RID, PTP_RID_INPUT);
    for (i = 0; i < PTP_CONTACTS; i++) finger(b, p);
    I1(UPAGE, 0x0d); I1(UEXP, 0x0c); I2(UNIT, 0x1001);          /* seconds x 10^-4 */
    I1(PMIN, 0); I4(PMAX, 0xffff); I1(LMIN, 0); I4(LMAX, 0xffff);
    I1(RSIZE, 16); I1(RCOUNT, 1); I1(USAGE, 0x56); I1(INPUT, 0x02);  /* Scan time */
    I1(UEXP, 0); I1(UNIT, 0); I1(PMIN, 0); I1(PMAX, 0);
    I1(USAGE, 0x54); I1(LMAX, 0x7f); I1(RSIZE, 8); I1(RCOUNT, 1); I1(INPUT, 0x02); /* count */
    I1(UPAGE, 0x09); I1(USAGE, 0x01); I1(LMAX, 1); I1(RSIZE, 1); I1(RCOUNT, 1);
    I1(INPUT, 0x02);                                           /* Button 1 */
    I1(RCOUNT, 7); I1(INPUT, 0x03);
    I1(UPAGE, 0x0d); I1(RID, PTP_RID_CAPS);
    I1(USAGE, 0x55); I1(USAGE, 0x59); I1(RSIZE, 4); I1(RCOUNT, 2); I1(LMAX, 0x0f);
    I1(FEATURE, 0x02);                                         /* max contacts, pad type */
    I2(UPAGE, 0xff00); I1(RID, PTP_RID_CERT); I1(USAGE, 0xc5);
    I1(LMIN, 0); I2(LMAX, 0xff); I1(RSIZE, 8); I2(RCOUNT, 0x100);
    I1(FEATURE, 0x02);                                         /* certification blob */
    I0(ENDC);
    /* Configuration TLC */
    I1(UPAGE, 0x0d); I1(USAGE, 0x0e); I1(COLL, 0x01);
    I1(RID, PTP_RID_MODE); I1(USAGE, 0x22); I1(COLL, 0x02);
    I1(USAGE, 0x52); I1(LMIN, 0); I1(LMAX, 0x0a); I1(RSIZE, 8); I1(RCOUNT, 1);
    I1(FEATURE, 0x02); I0(ENDC);                               /* Input mode */
    I1(USAGE, 0x22); I1(COLL, 0x00); I1(RID, PTP_RID_SELECT);
    I1(USAGE, 0x57); I1(USAGE, 0x58); I1(RSIZE, 1); I1(RCOUNT, 2); I1(LMAX, 1);
    I1(FEATURE, 0x02); I1(RCOUNT, 6); I1(FEATURE, 0x03); I0(ENDC); /* surface/button switch */
    I0(ENDC);
    /* Legacy mouse TLC (input mode 0) */
    I1(UPAGE, 0x01); I1(USAGE, 0x02); I1(COLL, 0x01);
    I1(RID, PTP_RID_MOUSE); I1(USAGE, 0x01); I1(COLL, 0x00);
    I1(UPAGE, 0x09); I1(UMIN, 1); I1(UMAX, 2); I1(LMIN, 0); I1(LMAX, 1);
    I1(RSIZE, 1); I1(RCOUNT, 2); I1(INPUT, 0x02); I1(RCOUNT, 6); I1(INPUT, 0x03);
    I1(UPAGE, 0x01); I1(USAGE, 0x30); I1(USAGE, 0x31); I1(LMIN, 0x81); I1(LMAX, 0x7f);
    I1(RSIZE, 8); I1(RCOUNT, 2); I1(INPUT, 0x06);
    I0(ENDC); I0(ENDC);
    if (b->err) return -1;
    p->desc_len = b->n;
    return 0;
}

int ptp_init(struct ptp *p, const struct amt_dims *d)
{
    hm_u32 span;
    hm_zero(p, sizeof(*p));
    if (!d) return -1;
    p->dims = *d;
    p->xmax = (hm_u32)((signed int)d->max_x - d->min_x);
    p->ymax = (hm_u32)((signed int)d->max_y - d->min_y);
    p->phys_w = d->width / 10;   /* 0.01 mm -> 0.1 mm */
    p->phys_h = d->height / 10;
    if (!p->xmax || !p->ymax || p->xmax > 0xffff || p->ymax > 0xffff || !p->phys_w || !p->phys_h)
        return -1;
    p->units_per_mm = (hm_u32)((hm_u64)100 * p->xmax / d->width);
    if (!p->units_per_mm) p->units_per_mm = 1;
    span = p->xmax > p->ymax ? p->xmax : p->ymax;
    p->match_dist2 = (span / 6) * (span / 6);   /* ~2.5 cm on a 16 cm pad */
    p->input_mode = PTP_MODE_MOUSE;             /* PTP spec: start in mouse mode */
    p->surface_on = p->button_on = 1;
    return build(p);
}

static hm_u16 clampu(signed int v, hm_u32 max)
{
    if (v < 0) return 0;
    if ((hm_u32)v > max) return (hm_u16)max;
    return (hm_u16)v;
}
static hm_u32 dist2(hm_u16 ax, hm_u16 ay, hm_u16 bx, hm_u16 by)
{
    signed int dx = (signed int)ax - bx, dy = (signed int)ay - by;
    return (hm_u32)(dx * dx) + (hm_u32)(dy * dy);
}

/* Greedy nearest-pair tracking (slot <- contact). Apple's record carries a
 * possible contact ID, but its meaning is unverified on J716s, so the stable
 * Windows contact ID is the slot index chosen by position continuity. */
static void track(struct ptp *p, const struct amt_frame *f, hm_u8 *lifting)
{
    hm_u16 lx[AMT_MAX_CONTACTS], ly[AMT_MAX_CONTACTS];
    hm_u8 used[AMT_MAX_CONTACTS], matched[PTP_CONTACTS];
    unsigned i, s;
    hm_zero(used, sizeof(used)); hm_zero(matched, sizeof(matched));
    for (i = 0; i < f->count; i++) {
        lx[i] = clampu((signed int)f->c[i].x - p->dims.min_x, p->xmax);
        ly[i] = clampu((signed int)p->dims.max_y - f->c[i].y, p->ymax);  /* Y down */
    }
    for (;;) {
        hm_u32 best = 0xffffffffu; unsigned bs = 0, bi = 0;
        for (s = 0; s < PTP_CONTACTS; s++) {
            if (!p->slot[s].active || matched[s]) continue;
            for (i = 0; i < f->count; i++) {
                hm_u32 d;
                if (used[i]) continue;
                d = dist2(p->slot[s].x, p->slot[s].y, lx[i], ly[i]);
                if (d < best) { best = d; bs = s; bi = i; }
            }
        }
        if (best == 0xffffffffu || best > p->match_dist2) break;
        matched[bs] = 1; used[bi] = 1;
        p->slot[bs].x = lx[bi]; p->slot[bs].y = ly[bi];
    }
    for (s = 0; s < PTP_CONTACTS; s++) {
        lifting[s] = p->slot[s].active && !matched[s];
        if (lifting[s]) p->slot[s].active = 0;     /* reported once with tip 0 */
    }
    for (i = 0; i < f->count; i++) {
        if (used[i]) continue;
        for (s = 0; s < PTP_CONTACTS; s++)
            if (!p->slot[s].active && !lifting[s]) break;
        if (s == PTP_CONTACTS) { p->dropped_contacts++; continue; }
        p->slot[s].active = 1; p->slot[s].x = lx[i]; p->slot[s].y = ly[i];
    }
}

static hm_u32 mouse(struct ptp *p, hm_u8 button, hm_u8 *out)
{
    unsigned s;
    signed int dx = 0, dy = 0;
    for (s = 0; s < PTP_CONTACTS && !p->slot[s].active; s++) {}
    if (s < PTP_CONTACTS && p->surface_on) {
        if (p->m_have && p->m_slot == s) {
            /* 8 mickeys per millimetre, remainder carried. */
            signed int ax = ((signed int)p->slot[s].x - p->m_px) * 8 + p->m_rx;
            signed int ay = ((signed int)p->slot[s].y - p->m_py) * 8 + p->m_ry;
            dx = ax / (signed int)p->units_per_mm; p->m_rx = ax - dx * (signed int)p->units_per_mm;
            dy = ay / (signed int)p->units_per_mm; p->m_ry = ay - dy * (signed int)p->units_per_mm;
            if (dx > 127) dx = 127;
            if (dx < -127) dx = -127;
            if (dy > 127) dy = 127;
            if (dy < -127) dy = -127;
        } else { p->m_rx = p->m_ry = 0; }
        p->m_have = 1; p->m_slot = (hm_u8)s;
        p->m_px = p->slot[s].x; p->m_py = p->slot[s].y;
    } else p->m_have = 0;
    if (!dx && !dy && button == p->last_button) return 0;
    out[0] = PTP_RID_MOUSE; out[1] = button; out[2] = (hm_u8)dx; out[3] = (hm_u8)dy;
    return PTP_MOUSE_LEN;
}

hm_u32 ptp_frame(struct ptp *p, const struct amt_frame *f, hm_u64 t_us, hm_u8 *out)
{
    hm_u8 lifting[PTP_CONTACTS];
    hm_u8 button;
    hm_u32 len = 0;
    unsigned s, n = 0;
    p->frames++;
    track(p, f, lifting);
    button = (hm_u8)(p->button_on && f->button);
    if (p->input_mode != PTP_MODE_PTP) {
        len = mouse(p, button, out);
    } else {
        hm_zero(out, PTP_INPUT_LEN);
        out[0] = PTP_RID_INPUT;
        for (s = 0; s < PTP_CONTACTS && p->surface_on; s++) {
            hm_u8 *c;
            if (!p->slot[s].active && !lifting[s]) continue;
            c = out + 1 + n * 6;
            c[0] = (hm_u8)(1u | (p->slot[s].active ? 2u : 0u));   /* confidence, tip */
            c[1] = (hm_u8)s;
            c[2] = (hm_u8)p->slot[s].x; c[3] = (hm_u8)(p->slot[s].x >> 8);
            c[4] = (hm_u8)p->slot[s].y; c[5] = (hm_u8)(p->slot[s].y >> 8);
            n++;
        }
        /* Nothing touching, nothing lifting, button unchanged: stay silent. */
        if (n || button != p->last_button || p->last_reported) {
            hm_u32 scan = (hm_u32)(t_us / 100);
            out[31] = (hm_u8)scan; out[32] = (hm_u8)(scan >> 8);
            out[33] = (hm_u8)n; out[34] = button;
            len = PTP_INPUT_LEN;
            p->last_reported = (hm_u8)(n != 0);
        }
    }
    p->last_button = button;
    if (len) p->reports++;
    return len;
}

static hm_u32 put(const hm_u8 *src, hm_u32 n, hm_u8 *buf, hm_u32 *len)
{
    hm_u32 k = n < *len ? n : *len;
    hm_copy(buf, src, k);
    *len = k;
    return HM_OK;
}

hm_u32 ptp_report(struct ptp *p, hm_u32 op, hm_u32 rid, hm_u8 *buf, hm_u32 *len)
{
    hm_u8 r[PTP_CERT_LEN];
    unsigned s;
    if (!buf || !len || !*len) return HM_RANGE;
    if (op == HM_GET_FEATURE) {
        r[0] = (hm_u8)rid;
        switch (rid) {
        case PTP_RID_CAPS:
            r[1] = (hm_u8)(PTP_CONTACTS | (PTP_PAD_DEPRESSIBLE << 4));
            return put(r, PTP_CAPS_LEN, buf, len);
        case PTP_RID_CERT:
            hm_copy(r + 1, cert_blob, sizeof(cert_blob));
            return put(r, PTP_CERT_LEN, buf, len);
        case PTP_RID_MODE:
            r[1] = p->input_mode;
            return put(r, PTP_MODE_LEN, buf, len);
        case PTP_RID_SELECT:
            r[1] = (hm_u8)(p->surface_on | (p->button_on << 1));
            return put(r, PTP_SELECT_LEN, buf, len);
        default:
            return HM_DENIED;
        }
    }
    if (op != HM_SET_FEATURE || *len != 2 || buf[0] != rid) return HM_DENIED;
    if (rid == PTP_RID_MODE) {
        if (buf[1] != PTP_MODE_MOUSE && buf[1] != PTP_MODE_PTP) return HM_DENIED;
        if (buf[1] != p->input_mode) {
            p->input_mode = buf[1];
            p->mode_changes++;
            for (s = 0; s < PTP_CONTACTS; s++) p->slot[s].active = 0;  /* resync */
            p->m_have = 0; p->last_reported = 0;
        }
        *len = 0;
        return HM_OK;
    }
    if (rid == PTP_RID_SELECT) {
        if (buf[1] & 0xfc) return HM_DENIED;
        p->surface_on = buf[1] & 1;
        p->button_on = (buf[1] >> 1) & 1;
        *len = 0;
        return HM_OK;
    }
    return HM_DENIED;
}
