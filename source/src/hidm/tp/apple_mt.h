/* SPDX-License-Identifier: MIT */
/*
 * J716s (M5 Pro) built-in trackpad: parser for the MTP multi-touch interface's
 * raw input report and its sensor-dimensions feature report.
 *
 * Independent implementation. The layout is taken from the behaviour of Linux
 * hid-magicmouse.c (magicmouse_raw_event_mtp, struct tp_header / tp_finger,
 * magicmouse_open's 0xd9 query) and dockchannel-hid.c (MT_REPORT_ID 0x75,
 * button byte 23); see LINUX-MAP.md for file:line. No Linux code is copied.
 *
 * Freestanding: no libc, no allocation. Every byte is untrusted.
 */
#ifndef APPLE_MT_H
#define APPLE_MT_H
#include "../abi/hidm_abi.h"

typedef signed short amt_s16;
typedef signed int amt_s32;

#define AMT_REPORT_ID 0x75u       /* dockchannel-hid.c MT_REPORT_ID */
#define AMT_HEADER 38u            /* struct tp_header: 22 + 1 + 1 + 14 */
#define AMT_COUNT_OFF 22u         /* tp_header.num_fingers */
#define AMT_BUTTON_OFF 23u        /* tp_header.buttons, bit 0 = firmware click */
#define AMT_RECORD 30u            /* struct tp_finger, 15 x le16 */
#define AMT_MAX_CONTACTS 16u      /* hid-magicmouse MAX_CONTACTS */
#define AMT_MAX_REPORT (AMT_HEADER + AMT_MAX_CONTACTS * AMT_RECORD) /* 518 */
#define AMT_DIM_REPORT_ID 0xd9u   /* SENSOR_DIMENSIONS_REPORT_ID */
#define AMT_DIM_LEN 17u           /* report ID + le32 w, le32 h, 4 x le16 */

/* Record field offsets inside one 30-byte record (struct tp_finger order). */
#define AMT_R_ID 0u        /* unknown1: low byte is the contact ID on T6041 (unverified J716s) */
#define AMT_R_CLASS 2u     /* unknown2: low byte is a class on T6041 (unverified J716s) */
#define AMT_R_X 4u
#define AMT_R_Y 6u
#define AMT_R_TOOL_MAJOR 12u
#define AMT_R_TOOL_MINOR 14u
#define AMT_R_ORIENT 16u
#define AMT_R_TOUCH_MAJOR 18u
#define AMT_R_TOUCH_MINOR 20u
#define AMT_R_PRESSURE 26u

struct amt_contact {
    hm_u8 raw_id, raw_class;
    amt_s16 x, y;                 /* device units, Y up (Linux negates it) */
    hm_u16 touch_major, touch_minor, tool_major, tool_minor, pressure;
    amt_s16 orientation;
};

struct amt_frame {
    hm_u8 button;                 /* firmware click state (force touch + haptic) */
    hm_u8 count;                  /* contacts kept (touch_major != 0) */
    hm_u8 records;                /* num_fingers announced in the header */
    hm_u32 pressure_sum;          /* sum over kept contacts, as Linux computes it */
    struct amt_contact c[AMT_MAX_CONTACTS];
};

struct amt_dims {
    hm_u32 width, height;         /* hundredths of a millimetre */
    amt_s16 min_x, min_y, max_x, max_y;
};

enum amt_result {
    AMT_OK = 0,
    AMT_E_ID = -1,                /* not report 0x75 */
    AMT_E_SHORT = -2,             /* shorter than the header */
    AMT_E_STRIDE = -3,            /* (len - 38) not a multiple of 30 */
    AMT_E_COUNT = -4,             /* records < num_fingers, or > 16 */
    AMT_E_DIMS = -5,              /* bad 0xd9 report */
};

int amt_parse(const hm_u8 *report, hm_u32 len, struct amt_frame *out);
int amt_parse_dims(const hm_u8 *report, hm_u32 len, struct amt_dims *out);
#endif
