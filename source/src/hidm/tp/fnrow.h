/* SPDX-License-Identifier: MIT */
/*
 * fnrow: Apple Fn layer and top row of the J716s built-in keyboard, translated for Windows.
 *
 * The keyboard reports the top row as F1..F12 (keyboard page 0x3a..0x45) and the Fn (globe)
 * key as usage 0x00ff:0003 in report 1 (byte 9, bit 1; byte 9 bit 0 is consumer 0xb8).
 * Windows ignores the Fn usage, so without this layer the top row is always F1..F12.
 * Linux does the same translation in hid-apple (apple_host_fn_keys, fnmode=3 default:
 * media keys without Fn). This is an independent implementation of that behaviour.
 *
 *   media (default, as macOS)   with Fn (or Fn-lock off)
 *   F1  display brightness -    consumer 0x70 (Windows: brightness down)  F1
 *   F2  display brightness +    consumer 0x6f                              F2
 *   F3  Mission Control         Win+Tab (Task View)                        F3
 *   F4  Spotlight               Win+S (Search)                             F4
 *   F5  Dictation               Win+H (voice typing)                       F5
 *   F6  Do Not Disturb          Win+N (notification centre, DND toggle)    F6
 *   F7/F8/F9                    consumer 0xb6 / 0xcd / 0xb5 (prev/play/next)
 *   F10/F11/F12                 consumer 0xe2 / 0xea / 0xe9 (mute/vol-/vol+)
 *   Ctrl+F1 / Ctrl+F2 (media)   keyboard backlight down / up (handled in the hypervisor,
 *                               consumed; Linux on this machine binds the same keys)
 *   Fn+Esc                      toggle Fn-lock (media <-> F-keys), consumed
 *   Fn+Backspace/Enter          Delete / Insert
 *   Fn+Up/Down/Left/Right       PageUp / PageDown / Home / End
 *
 * The consumer usages go out in a synthesized Consumer Control collection, report ID 0x53,
 * appended to the (already vendor-filtered) keyboard descriptor. The translation is only
 * enabled when report 1 has exactly the J716s layout; anything else passes through.
 *
 * Freestanding, no allocation. Own code (MIT).
 */
#ifndef FNROW_H
#define FNROW_H
#include "../abi/hidm_abi.h"

#define FNROW_KBD_RID 0x01
#define FNROW_KBD_LEN 10u
#define FNROW_CC_RID 0x53
#define FNROW_CC_LEN 2u
#define FNROW_DESC_APPEND 35u /* bytes fnrow_patch_descriptor appends */

/* Events (bitmask returned by fnrow_input). */
#define FNROW_EV_KBL_UP     (1u << 0)
#define FNROW_EV_KBL_DOWN   (1u << 1)
#define FNROW_EV_DISP_UP    (1u << 2)
#define FNROW_EV_DISP_DOWN  (1u << 3)
#define FNROW_EV_FNLOCK     (1u << 4)
#define FNROW_EV_TOPROW     (1u << 5) /* a top-row or Fn-layer key went down (for logging) */

struct fnrow {
    hm_u8 enabled;      /* descriptor matched and the consumer TLC was appended */
    hm_u8 media;        /* 1 = media keys without Fn (default, macOS); 0 = F-keys without Fn */
    hm_u8 held[256];    /* action per held key code, 0 = not held */
    hm_u16 cc_bits, cc_sent;
    hm_u8 cc_valid;     /* cc_sent has been pushed at least once */
    signed char kbl_held;     /* +1 / -1 while a keyboard-backlight key is held, else 0 */
    hm_u32 presses, consumer_reports;
};

/* Checks that d[0..*len) starts with the J716s report-1 layout and has no report 0x53, then
 * appends the Consumer Control TLC (FNROW_DESC_APPEND bytes) if cap allows. Sets f->enabled.
 * Returns 0 when appended, -1 when left unchanged (translation off, pass-through). */
int fnrow_patch_descriptor(struct fnrow *f, hm_u8 *d, hm_u32 *len, hm_u32 cap);
void fnrow_init(struct fnrow *f);
/* Translate one input report from the keyboard. in/len: the raw report (in[0] = report ID).
 * kbd_out (FNROW_KBD_LEN bytes) receives the translated report 1; *push_cc is set when the
 * consumer report cc_out (FNROW_CC_LEN bytes) must be pushed after it. Reports other than a
 * well-formed report 1 are copied unchanged (return 0, *kbd_len = len).
 * Returns the FNROW_EV_* events of this report. */
hm_u32 fnrow_input(struct fnrow *f, const hm_u8 *in, hm_u32 len, hm_u8 *kbd_out, hm_u32 *kbd_len,
                   hm_u8 *cc_out, int *push_cc);
#endif
