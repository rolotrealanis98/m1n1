/* SPDX-License-Identifier: MIT */
/*
 * Windows Precision Touchpad (PTP) presentation of the J716s trackpad.
 *
 * Builds a HID report descriptor with the PTP top-level collections Windows
 * requires (touch pad input, device capabilities, certification status,
 * configuration: input mode and selective reporting) plus a legacy mouse
 * collection, and translates Apple multi-touch frames (apple_mt.h) into those
 * reports. Feature reports are answered from local state, synchronously.
 *
 * Own code (MIT). The 256-byte certification blob is the widely used sample
 * blob, taken from paulsmir/windows-on-m1 (MIT, see PROVENANCE.md).
 * Freestanding: no libc, no allocation; caller serializes all calls.
 */
#ifndef PTP_H
#define PTP_H
#include "apple_mt.h"

#define PTP_CONTACTS 5u
#define PTP_RID_INPUT 1u
#define PTP_RID_CAPS 2u
#define PTP_RID_CERT 3u
#define PTP_RID_MODE 4u
#define PTP_RID_SELECT 5u
#define PTP_RID_MOUSE 6u
#define PTP_INPUT_LEN 35u   /* id + 5 x 6 + scan time 2 + count 1 + button 1 */
#define PTP_MOUSE_LEN 4u    /* id + buttons + dx + dy */
#define PTP_CAPS_LEN 2u
#define PTP_CERT_LEN 257u
#define PTP_MODE_LEN 2u
#define PTP_SELECT_LEN 2u
#define PTP_DESC_MAX 768u
#define PTP_MODE_MOUSE 0u
#define PTP_MODE_PTP 3u
#define PTP_PAD_DEPRESSIBLE 0u

struct ptp_slot { hm_u8 active; hm_u16 x, y; };

struct ptp {
    struct amt_dims dims;
    hm_u32 xmax, ymax;            /* logical maxima (device spans), min 0 */
    hm_u32 phys_w, phys_h;        /* physical maxima in 0.1 mm */
    hm_u32 units_per_mm;          /* for mouse-mode scaling */
    hm_u32 match_dist2;           /* contact tracking radius squared */
    hm_u8 input_mode, surface_on, button_on;
    struct ptp_slot slot[PTP_CONTACTS];
    hm_u8 last_button, last_reported;
    /* legacy mouse mode */
    hm_u8 m_have, m_slot;
    hm_u16 m_px, m_py;
    signed int m_rx, m_ry;
    /* counters (diagnostics) */
    hm_u32 frames, reports, dropped_contacts, rejected, mode_changes;
    hm_u8 desc[PTP_DESC_MAX];
    hm_u32 desc_len;
};

/* Returns 0 on success (descriptor built from the device's 0xd9 geometry). */
int ptp_init(struct ptp *p, const struct amt_dims *dims);
/* Translate one parsed frame. Writes one report (PTP input in mode 3, mouse
 * otherwise) into out[PTP_INPUT_LEN] and returns its length, or 0 if nothing
 * is to be sent. t_us: monotonic microseconds. */
hm_u32 ptp_frame(struct ptp *p, const struct amt_frame *f, hm_u64 t_us, hm_u8 *out);
/* hidm backend feature/output callback for this interface (HM_* status). */
hm_u32 ptp_report(struct ptp *p, hm_u32 op, hm_u32 rid, hm_u8 *buf, hm_u32 *len);
#endif
