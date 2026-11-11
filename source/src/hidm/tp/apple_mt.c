/* SPDX-License-Identifier: MIT */
#include "apple_mt.h"

static hm_u16 rd16(const hm_u8 *p) { return (hm_u16)(p[0] | (p[1] << 8)); }

/*
 * Linux behaviour reproduced (hid-magicmouse.c magicmouse_raw_event_mtp):
 *  - size >= 38 and (size - 38) % 30 == 0, else the report is ignored;
 *  - npoints = (size - 38) / 30 must be >= num_fingers and <= 16;
 *  - records with touch_major == 0 are skipped;
 *  - Y is reported negated (here: kept raw, the translator flips it);
 *  - the click is header byte 23 bit 0 (the firmware's button);
 *  - pressure is summed over the kept records (used by Linux only in host
 *    haptic mode; here for diagnostics and the optional host-click path).
 * Added: the report ID must be 0x75 (dockchannel-hid.c MT_REPORT_ID).
 */
int amt_parse(const hm_u8 *r, hm_u32 len, struct amt_frame *f)
{
    hm_u32 n, i;
    hm_zero(f, sizeof(*f));
    if (!r || len < 1 || r[0] != AMT_REPORT_ID) return AMT_E_ID;
    if (len < AMT_HEADER) return AMT_E_SHORT;
    if ((len - AMT_HEADER) % AMT_RECORD) return AMT_E_STRIDE;
    n = (len - AMT_HEADER) / AMT_RECORD;
    if (n > AMT_MAX_CONTACTS || n < r[AMT_COUNT_OFF]) return AMT_E_COUNT;
    f->records = r[AMT_COUNT_OFF];
    f->button = r[AMT_BUTTON_OFF] & 1;
    for (i = 0; i < f->records; i++) {
        const hm_u8 *p = r + AMT_HEADER + i * AMT_RECORD;
        struct amt_contact *c;
        if (!rd16(p + AMT_R_TOUCH_MAJOR)) continue;
        c = &f->c[f->count++];
        c->raw_id = p[AMT_R_ID];
        c->raw_class = p[AMT_R_CLASS];
        c->x = (amt_s16)rd16(p + AMT_R_X);
        c->y = (amt_s16)rd16(p + AMT_R_Y);
        c->tool_major = rd16(p + AMT_R_TOOL_MAJOR);
        c->tool_minor = rd16(p + AMT_R_TOOL_MINOR);
        c->orientation = (amt_s16)rd16(p + AMT_R_ORIENT);
        c->touch_major = rd16(p + AMT_R_TOUCH_MAJOR);
        c->touch_minor = rd16(p + AMT_R_TOUCH_MINOR);
        c->pressure = rd16(p + AMT_R_PRESSURE);
        /* Linux le16_to_int: signed 16-bit; negative pressure counts as is.
         * Clamp at zero so the sum cannot wrap. */
        if ((amt_s16)c->pressure > 0) f->pressure_sum += (amt_s16)c->pressure;
    }
    return AMT_OK;
}

/*
 * magicmouse_open: GET_FEATURE 0xd9 into a 32-byte buffer; at least
 * 1 + 16 bytes; buf[0] == 0xd9; struct { le32 width, height; le16 min_x,
 * min_y, max_x, max_y } at buf + 1; both spans > 0, width/height != 0 and
 * 100 * span / size != 0.
 */
int amt_parse_dims(const hm_u8 *r, hm_u32 len, struct amt_dims *d)
{
    amt_s32 xs, ys;
    hm_zero(d, sizeof(*d));
    if (!r || len < AMT_DIM_LEN || r[0] != AMT_DIM_REPORT_ID) return AMT_E_DIMS;
    d->width = hm_le32(r + 1);
    d->height = hm_le32(r + 5);
    d->min_x = (amt_s16)rd16(r + 9);
    d->min_y = (amt_s16)rd16(r + 11);
    d->max_x = (amt_s16)rd16(r + 13);
    d->max_y = (amt_s16)rd16(r + 15);
    xs = (amt_s32)d->max_x - d->min_x;
    ys = (amt_s32)d->max_y - d->min_y;
    if (xs <= 0 || ys <= 0 || !d->width || !d->height) return AMT_E_DIMS;
    /* Physical size must fit the PTP descriptor's 16-bit 0.1 mm physical field;
     * resolution check as Linux (non-zero units per mm). */
    if (d->width > 6553500u || d->height > 6553500u) return AMT_E_DIMS;
    if ((hm_u64)100 * (hm_u32)xs / d->width == 0 || (hm_u64)100 * (hm_u32)ys / d->height == 0)
        return AMT_E_DIMS;
    return AMT_OK;
}
