/* SPDX-License-Identifier: MIT */
/* fnrow + kbl host tests (2026-11-11): descriptor gate through the HIDM v1.1 driver core,
 * top-row translation table, Fn-lock, held-key semantics, rollover, consumer-report edges,
 * random-report fuzz invariants, keyboard-backlight state machine and its exact writes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hidm/tp/fnrow.h"
#include "hidm/tp/kbl.h"
#include "hidm/tp/hid_filter.h"
#include "v11/driver/hidm_core.h"
static unsigned checks, groups;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
/* the real J716s keyboard descriptor (hardware run n3tp1, console "MTP: rdesc") */
static const hm_u8 KBD_HW[189] = {
0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,
0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x29,0xff,0x81,
0x00,0x05,0x0c,0x75,0x01,0x95,0x01,0x09,0xb8,0x15,0x00,0x25,0x01,0x81,0x02,0x05,0xff,0x09,0x03,0x75,0x07,0x95,0x01,0x81,0x02,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,
0x85,0x52,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x09,0xcd,0x81,0x02,0x09,0xb3,0x81,0x02,0x09,0xb4,0x81,0x02,0x09,0xb5,0x81,0x02,0x09,0xb6,0x81,0x02,0x81,0x01,
0x81,0x01,0x81,0x01,0x85,0x09,0x15,0x00,0x25,0x01,0x75,0x08,0x95,0x01,0x06,0x01,0xff,0x09,0x0b,0xb1,0x02,0x75,0x08,0x95,0x02,0xb1,0x01,0xc0,0x06,0x00,0xff,0x09,
0x06,0xa1,0x01,0x06,0x00,0xff,0x09,0x06,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x40,0x85,0x3f,0x81,0x22,0x76,0x00,0x40,0x95,0x02,0xb1,0x01,0xc0};

static struct fnrow F;
static hm_u8 out[256], cc[FNROW_CC_LEN];
static hm_u32 olen; static int pcc;
static hm_u8 last_cc; static int cc_pushes;
/* one report 1: mods, fn, up to 3 keys */
static hm_u32 rep(hm_u8 mods, int fn, hm_u8 a, hm_u8 b, hm_u8 c)
{
    hm_u8 in[10] = {1, mods, 0, a, b, c, 0, 0, 0, (hm_u8)(fn ? 2 : 0)};
    hm_u32 ev = fnrow_input(&F, in, 10, out, &olen, cc, &pcc);
    CHECK(olen == 10 && out[0] == 1 && out[9] == in[9] && out[2] == 0);
    if (pcc) { CHECK(cc[0] == 0x53 && cc[1] != last_cc); last_cc = cc[1]; cc_pushes++; }
    return ev;
}
static int has_key(hm_u8 k) { for (int i = 3; i < 9; i++) if (out[i] == k) return 1; return 0; }
static int nkeys(void) { int n = 0; for (int i = 3; i < 9; i++) n += out[i] != 0; return n; }
static void fresh(void)
{
    hm_u8 d[512]; hm_u32 n; struct hid_filter_result r;
    memcpy(d, KBD_HW, 189);
    CHECK(!hid_strip_vendor_tlcs(d, 189, &r) && r.len == 156);
    n = r.len;
    fnrow_init(&F);
    CHECK(!fnrow_patch_descriptor(&F, d, &n, sizeof(d)) && F.enabled && F.media == 1);
    last_cc = 0; cc_pushes = 0; groups++;
}

static void test_descriptor(void)
{
    hm_u8 d[512]; hm_u32 n; struct hid_filter_result r; hm_layout l;
    groups++;
    memcpy(d, KBD_HW, 189);
    CHECK(!hid_strip_vendor_tlcs(d, 189, &r) && r.removed_tlcs == 1 && r.len == 156);
    n = r.len;
    fnrow_init(&F);
    CHECK(!fnrow_patch_descriptor(&F, d, &n, sizeof(d)) && n == 156 + FNROW_DESC_APPEND && F.enabled);
    CHECK(!memcmp(d, KBD_HW, 156));
    /* the Windows-side gate: the hidm5w 1.1 driver core accepts it, with the expected layouts */
    CHECK(hm_parse_descriptor_ex(d, n, &l, HM_MAX_FEATURE) == HM_OK && hm_parse_descriptor(d, n, &l) == HM_OK);
    CHECK(l.ids && l.length[0][1] == 10 && l.length[1][1] == 2 && l.length[0][0x52] == 2 &&
          l.length[2][9] == 4 && l.length[0][0x53] == FNROW_CC_LEN && !l.length[0][0x3f]);
    /* second patch: 0x53 already declared -> refused, unchanged */
    { hm_u32 n2 = n; hm_u8 c[512]; memcpy(c, d, n);
      CHECK(fnrow_patch_descriptor(&F, d, &n2, sizeof(d)) == -1 && n2 == n && !memcmp(c, d, n) && !F.enabled); }
    /* no room -> refused */
    memcpy(d, KBD_HW, 189); hid_strip_vendor_tlcs(d, 189, &r); n = r.len;
    CHECK(fnrow_patch_descriptor(&F, d, &n, 156 + FNROW_DESC_APPEND - 1) == -1 && n == 156 && !F.enabled);
    /* a different keyboard (one byte of report 1 changed, or a short generic keyboard) -> refused */
    memcpy(d, KBD_HW, 189); hid_strip_vendor_tlcs(d, 189, &r); n = r.len; d[87] = 0x08;
    CHECK(fnrow_patch_descriptor(&F, d, &n, sizeof(d)) == -1 && n == 156 && !F.enabled);
    { static const hm_u8 g[] = {5,1,9,6,0xa1,1,0xc0}; hm_u8 e[64]; hm_u32 m = sizeof(g); memcpy(e, g, m);
      CHECK(fnrow_patch_descriptor(&F, e, &m, sizeof(e)) == -1 && m == sizeof(g)); }
    /* disabled: every report passes through byte for byte, no consumer report */
    { hm_u8 in[10] = {1, 0, 0, 0x3a, 0, 0, 0, 0, 0, 0};
      CHECK(fnrow_input(&F, in, 10, out, &olen, cc, &pcc) == 0 && olen == 10 && !memcmp(in, out, 10) && !pcc); }
}

static void test_table(void)
{
    /* media mode (default): F1..F12 without Fn */
    static const struct { hm_u8 key; int cc_bit; hm_u8 win; hm_u32 ev; } t[12] = {
        {0x3a, 0, 0, FNROW_EV_DISP_DOWN}, {0x3b, 1, 0, FNROW_EV_DISP_UP}, {0x3c, -1, 0x2b, 0},
        {0x3d, -1, 0x16, 0}, {0x3e, -1, 0x0b, 0}, {0x3f, -1, 0x11, 0}, {0x40, 2, 0, 0},
        {0x41, 3, 0, 0}, {0x42, 4, 0, 0}, {0x43, 5, 0, 0}, {0x44, 6, 0, 0}, {0x45, 7, 0, 0}};
    for (int i = 0; i < 12; i++) {
        fresh();
        hm_u32 ev = rep(0, 0, t[i].key, 0, 0);
        CHECK(ev & FNROW_EV_TOPROW);
        CHECK((ev & (FNROW_EV_DISP_DOWN | FNROW_EV_DISP_UP)) == t[i].ev);
        CHECK(!has_key(t[i].key));
        if (t[i].cc_bit >= 0) { CHECK(pcc && cc[1] == (1u << t[i].cc_bit) && nkeys() == 0 && out[1] == 0); }
        else { CHECK(!pcc && has_key(t[i].win) && nkeys() == 1 && out[1] == 0x08); }
        rep(0, 0, 0, 0, 0);
        CHECK(nkeys() == 0 && out[1] == 0);
        if (t[i].cc_bit >= 0) CHECK(pcc && cc[1] == 0 && cc_pushes == 2); else CHECK(!pcc && cc_pushes == 0);
        /* with Fn held: the plain F-key, nothing else */
        fresh();
        CHECK(!(rep(0, 1, t[i].key, 0, 0) & (FNROW_EV_DISP_DOWN | FNROW_EV_DISP_UP)));
        CHECK(has_key(t[i].key) && nkeys() == 1 && out[1] == 0 && !pcc);
        rep(0, 1, 0, 0, 0); CHECK(nkeys() == 0);
    }
    /* ordinary keys and modifiers pass, Fn or not */
    fresh();
    rep(0x02, 0, 0x04, 0x16, 0); CHECK(has_key(4) && has_key(0x16) && nkeys() == 2 && out[1] == 0x02 && !pcc);
    rep(0x02, 1, 0x04, 0x16, 0); CHECK(has_key(4) && has_key(0x16) && nkeys() == 2);
}

static void test_fn_layer(void)
{
    static const hm_u8 from[6] = {0x2a, 0x28, 0x52, 0x51, 0x50, 0x4f}, to[6] = {0x4c, 0x49, 0x4b, 0x4e, 0x4a, 0x4d};
    for (int i = 0; i < 6; i++) {
        fresh();
        rep(0, 1, from[i], 0, 0); CHECK(has_key(to[i]) && nkeys() == 1);
        rep(0, 0, from[i], 0, 0); CHECK(has_key(to[i]) && nkeys() == 1);   /* decided at press: Fn up, key held */
        rep(0, 0, 0, 0, 0); CHECK(nkeys() == 0);
        rep(0, 0, from[i], 0, 0); CHECK(has_key(from[i]) && nkeys() == 1); /* no Fn: itself */
        rep(0, 0, 0, 0, 0);
    }
    /* Fn-lock: Fn+Esc toggles, consumed; then F-keys are plain and Fn gives media */
    fresh();
    CHECK(rep(0, 1, 0x29, 0, 0) & FNROW_EV_FNLOCK); CHECK(nkeys() == 0 && F.media == 0);
    rep(0, 1, 0x29, 0, 0); CHECK(F.media == 0 && nkeys() == 0);          /* held: no second toggle */
    rep(0, 0, 0, 0, 0);
    rep(0, 0, 0x45, 0, 0); CHECK(has_key(0x45) && !pcc);
    rep(0, 0, 0, 0, 0);
    rep(0, 1, 0x45, 0, 0); CHECK(!has_key(0x45) && pcc && cc[1] == 0x80);
    rep(0, 1, 0, 0, 0); CHECK(pcc && cc[1] == 0);
    CHECK(rep(0, 1, 0x29, 0, 0) & FNROW_EV_FNLOCK); CHECK(F.media == 1);
    rep(0, 0, 0, 0, 0);
    rep(0, 0, 0x29, 0, 0); CHECK(has_key(0x29) && F.media == 1);          /* Esc alone is Esc */
    rep(0, 0, 0, 0, 0);
    /* Fn pressed while a media key is held: the release still clears the consumer bit */
    fresh();
    rep(0, 0, 0x44, 0, 0); CHECK(pcc && cc[1] == 0x40);
    rep(0, 1, 0x44, 0, 0); CHECK(!pcc && !has_key(0x44));
    rep(0, 1, 0, 0, 0); CHECK(pcc && cc[1] == 0);
    /* two media keys together, released one by one */
    fresh();
    rep(0, 0, 0x44, 0x45, 0); CHECK(pcc && cc[1] == 0xc0);
    rep(0, 0, 0x45, 0, 0); CHECK(pcc && cc[1] == 0x80);
    rep(0, 0, 0x45, 0x04, 0); CHECK(!pcc && has_key(4) && nkeys() == 1);
    rep(0, 0, 0, 0, 0); CHECK(pcc && cc[1] == 0 && nkeys() == 0);
    /* rollover (error code 1 in all slots): passed through, held state kept */
    fresh();
    rep(0, 0, 0x44, 0, 0);
    { hm_u8 in[10] = {1, 0, 0, 1, 1, 1, 1, 1, 1, 0}; fnrow_input(&F, in, 10, out, &olen, cc, &pcc);
      CHECK(!memcmp(in, out, 10) && !pcc); }
    rep(0, 0, 0, 0, 0); CHECK(pcc && cc[1] == 0);
    /* other reports (0x52 consumer, wrong length) pass through unchanged */
    { hm_u8 in[2] = {0x52, 0x01}; CHECK(fnrow_input(&F, in, 2, out, &olen, cc, &pcc) == 0 && olen == 2 && !memcmp(in, out, 2) && !pcc); }
    { hm_u8 in[9] = {1, 0, 0, 0x3a}; CHECK(fnrow_input(&F, in, 9, out, &olen, cc, &pcc) == 0 && olen == 9 && !memcmp(in, out, 9)); }
}

static void test_kbl_keys(void)
{
    fresh();
    CHECK(rep(0x01, 0, 0x3b, 0, 0) & FNROW_EV_KBL_UP); CHECK(F.kbl_held == 1 && nkeys() == 0 && !pcc && out[1] == 0x01);
    CHECK(!(rep(0x01, 0, 0x3b, 0, 0) & FNROW_EV_KBL_UP)); CHECK(F.kbl_held == 1);   /* held: one event */
    rep(0x00, 0, 0x3b, 0, 0); CHECK(F.kbl_held == 1 && !pcc);                       /* Ctrl up first: still backlight */
    rep(0, 0, 0, 0, 0); CHECK(F.kbl_held == 0);
    CHECK(rep(0x10, 0, 0x3a, 0, 0) & FNROW_EV_KBL_DOWN); CHECK(F.kbl_held == -1 && !(rep(0x10, 0, 0x3a, 0, 0) & FNROW_EV_DISP_DOWN));
    rep(0, 0, 0, 0, 0);
    /* Ctrl+Fn+F1 = Ctrl+F1 (a shortcut), not backlight */
    CHECK(!(rep(0x01, 1, 0x3a, 0, 0) & FNROW_EV_KBL_DOWN)); CHECK(has_key(0x3a) && out[1] == 0x01);
    rep(0, 0, 0, 0, 0);
    /* Ctrl+F3 (media) is Ctrl+Win+Tab, not swallowed */
    rep(0x01, 0, 0x3c, 0, 0); CHECK(has_key(0x2b) && out[1] == 0x09);
    rep(0, 0, 0, 0, 0);
}

static void test_fuzz(void)
{
    unsigned seed = 12345;
    fresh();
    for (unsigned it = 0; it < 300000; it++) {
        hm_u8 in[10] = {1};
        seed = seed * 1103515245u + 12345u;
        in[1] = (hm_u8)(seed >> 8);
        in[9] = (hm_u8)((seed >> 16) & 3);
        for (int i = 3; i < 9; i++) {
            seed = seed * 1103515245u + 12345u;
            unsigned r = (seed >> 16) % 10;
            /* mostly top row, Esc, arrows, Backspace; sometimes letters, sometimes empty */
            static const hm_u8 pool[] = {0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x45,0x29,0x2a,0x28,0x4f,0x50,0x51,0x52,0x04,0x16};
            in[i] = r < 4 ? 0 : pool[(seed >> 20) % sizeof(pool)];
            if (!((seed >> 24) & 255)) in[i] = 1;
        }
        int roll = 0;
        for (int i = 3; i < 9; i++) roll |= in[i] == 1;
        fnrow_input(&F, in, 10, out, &olen, cc, &pcc);
        CHECK(olen == 10 && out[0] == 1);
        if (roll) { CHECK(!memcmp(in, out, 10) && !pcc); continue; }
        if (pcc) { CHECK(cc[0] == 0x53 && cc[1] == (hm_u8)F.cc_sent); }
        /* translated reports never carry a key twice */
        int dup = 0;
        for (int i = 3; i < 9; i++) for (int j = i + 1; j < 9; j++) if (out[i] && out[i] == out[j] && out[i] > 3) dup = 1;
        CHECK(!dup);
        if (it % 1000 == 999) {
            hm_u8 z[10] = {1};
            fnrow_input(&F, z, 10, out, &olen, cc, &pcc);
            CHECK(nkeys() == 0 && out[1] == 0 && F.cc_bits == 0 && F.kbl_held == 0 && F.cc_sent == 0);
            for (int k = 0; k < 256; k++) CHECK(!F.held[k]);
        }
    }
    groups++;
}

/* kbl: exact writes */
static struct { hm_u64 a; hm_u32 v; } W[64]; static unsigned nw;
static void wr(void *c, hm_u64 a, hm_u32 v)
{
    (void)c;
    CHECK(a == 0x505040000ull || a == 0x505040018ull || a == 0x50504001cull); /* only the 3 PWM registers */
    if (nw < 64) { W[nw].a = a; W[nw].v = v; }
    nw++;
}
static void test_kbl(void)
{
    struct kbl b; const struct kbl_io io = {NULL, wr};
    hm_u64 t = 1000000;
    groups++;
    CHECK(kbl_on_cycles(0) == 0 && kbl_on_cycles(KBL_LEVELS) == KBL_PERIOD_CYCLES && kbl_on_cycles(99) == KBL_PERIOD_CYCLES);
    for (int l = 1; l <= KBL_LEVELS; l++) CHECK(kbl_on_cycles(l) > kbl_on_cycles(l - 1) && kbl_on_cycles(l) >= 6);
    nw = 0; kbl_start(&b, &io, t);
    CHECK(nw == 3 && W[0].a == 0x50504001cull && W[0].v == kbl_on_cycles(KBL_DEFAULT_LEVEL) &&
          W[1].a == 0x505040018ull && W[1].v == KBL_PERIOD_CYCLES - W[0].v &&
          W[2].a == 0x505040000ull && W[2].v == 0x4021);
    nw = 0; kbl_step(&b, 1, t); CHECK(b.level == KBL_DEFAULT_LEVEL + 1 && nw == 3);
    /* auto-repeat: nothing before 400 ms, then every 120 ms while held */
    nw = 0; kbl_tick(&b, 1, t + 399000); CHECK(nw == 0);
    kbl_tick(&b, 1, t + 400000); CHECK(b.level == KBL_DEFAULT_LEVEL + 2);
    kbl_tick(&b, 1, t + 450000); CHECK(b.level == KBL_DEFAULT_LEVEL + 2);
    kbl_tick(&b, 1, t + 520000); CHECK(b.level == KBL_DEFAULT_LEVEL + 3);
    kbl_tick(&b, 0, t + 700000); kbl_tick(&b, 1, t + 900000); CHECK(b.level == KBL_DEFAULT_LEVEL + 3); /* released: no repeat */
    for (int i = 0; i < 40; i++) kbl_step(&b, 1, t + 1000000);
    CHECK(b.level == KBL_LEVELS && b.lit == KBL_LEVELS);
    for (int i = 0; i < 40; i++) kbl_step(&b, -1, t + 1000000);
    CHECK(b.level == 0 && b.lit == 0);
    nw = 0; kbl_step(&b, -1, t + 1000000); CHECK(nw == 0);   /* already off: no write */
    kbl_step(&b, 1, t + 1000000); CHECK(nw == 3 && b.level == 1);
    /* idle-off after 60 s without input, wake on input */
    hm_u64 t2 = t + 1000000;
    nw = 0; kbl_tick(&b, 0, t2 + KBL_IDLE_US - 1); CHECK(nw == 0 && b.lit == 1);
    kbl_tick(&b, 0, t2 + KBL_IDLE_US); CHECK(nw == 1 && W[0].a == 0x505040000ull && W[0].v == 0 && b.lit == 0 && b.idle_offs == 1);
    kbl_tick(&b, 0, t2 + 2 * KBL_IDLE_US); CHECK(nw == 1 && b.idle_offs == 1);
    kbl_activity(&b, t2 + 2 * KBL_IDLE_US); CHECK(nw == 4 && b.lit == 1 && b.wakes == 1);
    kbl_activity(&b, t2 + 2 * KBL_IDLE_US + 10); CHECK(nw == 4);   /* steady input: no writes */
    /* first step after idle-off wakes then steps */
    kbl_tick(&b, 0, t2 + 4 * KBL_IDLE_US); CHECK(b.lit == 0);
    kbl_step(&b, 1, t2 + 4 * KBL_IDLE_US); CHECK(b.lit == 2 && b.level == 2);
    nw = 0; kbl_stop(&b); CHECK(nw == 1 && W[0].v == 0 && !b.ready);
    kbl_step(&b, 1, t2 + 5 * KBL_IDLE_US); kbl_activity(&b, 0); kbl_tick(&b, 1, ~0ull); kbl_stop(&b); CHECK(nw == 1);
    /* never ready: nothing */
    { struct kbl z; memset(&z, 0, sizeof(z)); nw = 0; kbl_step(&z, 1, 0); kbl_tick(&z, 1, ~0ull); kbl_activity(&z, 0); kbl_stop(&z); CHECK(nw == 0); }
}

int main(void)
{
    test_descriptor();
    test_table();
    test_fn_layer();
    test_kbl_keys();
    test_fuzz();
    test_kbl();
    printf("fnrow+kbl: %u groups, %u checks PASS\n", groups, checks);
    return 0;
}
