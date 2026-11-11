/* SPDX-License-Identifier: MIT */
/* fnrow: see fnrow.h. Own code; behaviour follows Linux hid-apple's apple_host_fn_keys table. */
#include "fnrow.h"

/* Report 1 of the J716s keyboard, as captured on hardware (n3tp1): modifiers, reserved byte,
 * LED output, 6-key array (keyboard page 0..255), consumer 0xb8 (1 bit), vendor 0x00ff:0003
 * (Fn, 7 bits, value 0/1), End Collection. Translation is on only when the descriptor starts
 * with exactly these bytes, so the byte offsets below are known. */
static const hm_u8 j716s_report1[90] = {
0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,
0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x29,0xff,0x81,
0x00,0x05,0x0c,0x75,0x01,0x95,0x01,0x09,0xb8,0x15,0x00,0x25,0x01,0x81,0x02,0x05,0xff,0x09,0x03,0x75,0x07,0x95,0x01,0x81,0x02,0xc0};

/* Consumer Control, report 0x53, 8 one-bit variables in this order (bit 0 first). */
static const hm_u8 cc_tlc[FNROW_DESC_APPEND] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, FNROW_CC_RID,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x09, 0x70,   /* bit 0 Display Brightness Decrement */
    0x09, 0x6f,   /* bit 1 Display Brightness Increment */
    0x09, 0xb6,   /* bit 2 Scan Previous Track */
    0x09, 0xcd,   /* bit 3 Play/Pause */
    0x09, 0xb5,   /* bit 4 Scan Next Track */
    0x09, 0xe2,   /* bit 5 Mute */
    0x09, 0xea,   /* bit 6 Volume Decrement */
    0x09, 0xe9,   /* bit 7 Volume Increment */
    0x81, 0x02, 0xc0};

enum { CC_BRI_DN, CC_BRI_UP, CC_PREV, CC_PLAY, CC_NEXT, CC_MUTE, CC_VOL_DN, CC_VOL_UP };
/* actions stored in held[] */
#define A_PASS     0x01
#define A_EAT      0x02
#define A_KBL_UP   0x03
#define A_KBL_DN   0x04
#define A_CC(b)    (0x10 | (b))   /* consumer bit b */
#define A_KEY(i)   (0x20 | (i))   /* remap[i] */
#define A_WIN(i)   (0x30 | (i))   /* Left GUI + win_key[i] */
static const hm_u8 remap[6] = {0x4c, 0x49, 0x4b, 0x4e, 0x4a, 0x4d}; /* Del Ins PgUp PgDn Home End */
static const hm_u8 win_key[4] = {0x2b, 0x16, 0x0b, 0x11};            /* Tab S H N */
/* F1..F12 in media mode */
static const hm_u8 media_action[12] = {
    A_CC(CC_BRI_DN), A_CC(CC_BRI_UP), A_WIN(0), A_WIN(1), A_WIN(2), A_WIN(3),
    A_CC(CC_PREV), A_CC(CC_PLAY), A_CC(CC_NEXT), A_CC(CC_MUTE), A_CC(CC_VOL_DN), A_CC(CC_VOL_UP)};

#define K_ESC 0x29
#define K_F1 0x3a
#define K_F12 0x45
#define MOD_CTRL 0x11 /* left | right Control */
#define MOD_LGUI 0x08

void fnrow_init(struct fnrow *f)
{
    hm_zero(f, sizeof(*f));
    f->media = 1;
}

/* Walk short items; return 1 if a Report ID item with value rid exists, -1 if malformed. */
static int has_report_id(const hm_u8 *d, hm_u32 n, hm_u32 rid)
{
    hm_u32 p = 0;
    while (p < n) {
        hm_u32 b = d[p], sz = b & 3, v = 0, k;
        if (b == 0xfe) { if (n - p < 3 || d[p + 1] > n - p - 3) return -1; p += 3u + d[p + 1]; continue; }
        if (sz == 3) sz = 4;
        if (sz > n - p - 1) return -1;
        for (k = 0; k < sz; k++) v |= (hm_u32)d[p + 1 + k] << (8 * k);
        if (((b >> 2) & 3) == 1 && (b >> 4) == 8 && v == rid) return 1;
        p += 1 + sz;
    }
    return 0;
}

int fnrow_patch_descriptor(struct fnrow *f, hm_u8 *d, hm_u32 *len, hm_u32 cap)
{
    hm_u32 n = *len, i;
    f->enabled = 0;
    if (!d || n < sizeof(j716s_report1) || cap < n || cap - n < FNROW_DESC_APPEND) return -1;
    for (i = 0; i < sizeof(j716s_report1); i++)
        if (d[i] != j716s_report1[i]) return -1;
    if (has_report_id(d, n, FNROW_CC_RID) != 0) return -1;
    for (i = 0; i < FNROW_DESC_APPEND; i++) d[n + i] = cc_tlc[i];
    *len = n + FNROW_DESC_APPEND;
    f->enabled = 1;
    return 0;
}

static hm_u8 decide(struct fnrow *f, hm_u8 c, int fn, int ctrl, hm_u32 *ev)
{
    if (c >= K_F1 && c <= K_F12) {
        int media = f->media ? !fn : fn;
        if (!media) return A_PASS;
        *ev |= FNROW_EV_TOPROW;
        if (ctrl && c == K_F1) { *ev |= FNROW_EV_KBL_DOWN; return A_KBL_DN; }
        if (ctrl && c == K_F1 + 1) { *ev |= FNROW_EV_KBL_UP; return A_KBL_UP; }
        if (c == K_F1) *ev |= FNROW_EV_DISP_DOWN;
        if (c == K_F1 + 1) *ev |= FNROW_EV_DISP_UP;
        return media_action[c - K_F1];
    }
    if (!fn) return A_PASS;
    switch (c) {
    case K_ESC: f->media ^= 1; *ev |= FNROW_EV_FNLOCK | FNROW_EV_TOPROW; return A_EAT;
    case 0x2a: *ev |= FNROW_EV_TOPROW; return A_KEY(0); /* Backspace -> Delete */
    case 0x28: *ev |= FNROW_EV_TOPROW; return A_KEY(1); /* Enter -> Insert */
    case 0x52: *ev |= FNROW_EV_TOPROW; return A_KEY(2); /* Up -> PageUp */
    case 0x51: *ev |= FNROW_EV_TOPROW; return A_KEY(3); /* Down -> PageDown */
    case 0x50: *ev |= FNROW_EV_TOPROW; return A_KEY(4); /* Left -> Home */
    case 0x4f: *ev |= FNROW_EV_TOPROW; return A_KEY(5); /* Right -> End */
    default: return A_PASS;
    }
}

hm_u32 fnrow_input(struct fnrow *f, const hm_u8 *in, hm_u32 len, hm_u8 *kbd_out, hm_u32 *kbd_len,
                   hm_u8 *cc_out, int *push_cc)
{
    hm_u32 ev = 0, i, j, nk = 0;
    hm_u8 seen[32], mods, keys[6];
    int fn, ctrl, rollover = 0;
    *push_cc = 0;
    if (!f->enabled || len != FNROW_KBD_LEN || in[0] != FNROW_KBD_RID) {
        for (i = 0; i < len; i++) kbd_out[i] = in[i];
        *kbd_len = len;
        return 0;
    }
    fn = (in[9] >> 1) & 1;
    ctrl = (in[1] & MOD_CTRL) != 0;
    for (i = 3; i < 9; i++) if (in[i] >= 1 && in[i] <= 3) rollover = 1;
    if (rollover) {
        /* error report (too many keys): pass it on; held keys and the consumer state are kept */
        for (i = 0; i < FNROW_KBD_LEN; i++) kbd_out[i] = in[i];
        *kbd_len = FNROW_KBD_LEN;
        return 0;
    }
    hm_zero(seen, sizeof(seen));
    {
        /* presses: decide once, at key-down; releases: whatever was decided then */
        for (i = 3; i < 9; i++) {
            hm_u8 c = in[i];
            if (c < 4) continue;
            seen[c >> 3] |= (hm_u8)(1u << (c & 7));
            if (!f->held[c]) { f->held[c] = decide(f, c, fn, ctrl, &ev); f->presses++; }
        }
        for (i = 4; i < 256; i++)
            if (f->held[i] && !(seen[i >> 3] & (1u << (i & 7)))) f->held[i] = 0;
    }
    mods = in[1];
    f->cc_bits = 0;
    f->kbl_held = 0;
    hm_zero(keys, sizeof(keys));
    for (i = 3; i < 9; i++) {
        hm_u8 c = in[i], a, out = 0;
        if (c < 4) continue;
        a = f->held[c];
        if (a == A_PASS) out = c;
        else if ((a & 0xf0) == 0x20) out = remap[a & 0x0f];
        else if ((a & 0xf0) == 0x30) { out = win_key[a & 0x0f]; mods |= MOD_LGUI; }
        else if ((a & 0xf0) == 0x10) f->cc_bits |= (hm_u16)(1u << (a & 0x0f));
        else if (a == A_KBL_UP) f->kbl_held = 1;
        else if (a == A_KBL_DN) f->kbl_held = -1;
        if (!out) continue;
        for (j = 0; j < nk; j++) if (keys[j] == out) break;
        if (j == nk && nk < 6) keys[nk++] = out;
    }
    kbd_out[0] = FNROW_KBD_RID;
    kbd_out[1] = mods;
    kbd_out[2] = in[2];
    for (i = 0; i < 6; i++) kbd_out[3 + i] = keys[i];
    kbd_out[9] = in[9];
    *kbd_len = FNROW_KBD_LEN;
    if (!f->cc_valid || f->cc_bits != f->cc_sent) {
        /* first report establishes the consumer state only if something is set */
        if (f->cc_valid || f->cc_bits) {
            cc_out[0] = FNROW_CC_RID;
            cc_out[1] = (hm_u8)f->cc_bits;
            *push_cc = 1;
            f->cc_sent = f->cc_bits;
            f->cc_valid = 1;
            f->consumer_reports++;
        }
    }
    return ev;
}
