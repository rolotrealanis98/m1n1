/* SPDX-License-Identifier: MIT */
/*
 * kbl: J716s keyboard backlight, driven from the hypervisor.
 *
 * Linux on this machine (owner's tree, patch 0032 "keyboard backlight on fpwm0"; README: verified
 * by eye 0/32/255/0): ADT /arm-io/pwm0 ("fpwm,s5l8920x", reg 0x505040000 + 0x4000, clock-gate
 * FPWM0), child kbd-backlight on channel 0 at 25 kHz -> pwm-leds, period 40000 ns. The SMC has no
 * keyboard-light key on this machine; the PWM is the whole path. pwm-apple programs it as:
 *   ON_CYCLES (0x1c) = clk * duty, OFF_CYCLES (0x18) = clk * period - on,
 *   CTRL (0x00) = ENABLE | OUTPUT_ENABLE | UPDATE (bits 0, 14, 5); off = CTRL 0.
 * clk = clkref, 24 MHz -> 960 cycles per 40 us period.
 *
 * Behaviour (lead's option (a)): 17 levels (0..16), default level at boot, Ctrl+brightness keys
 * step it (with auto-repeat while held), off after an idle time without keyboard or trackpad
 * input, back to the last level on the next input. Only the three PWM registers are written.
 *
 * Freestanding (io callbacks), no allocation. Own code (MIT).
 */
#ifndef KBL_H
#define KBL_H
#include "../abi/hidm_abi.h"

#define KBL_PWM_BASE 0x505040000ull
#define KBL_PWM_CTRL 0x00u
#define KBL_PWM_OFF  0x18u
#define KBL_PWM_ON   0x1cu
#define KBL_CTRL_ON  ((1u << 0) | (1u << 5) | (1u << 14)) /* ENABLE | UPDATE | OUTPUT_ENABLE */
#define KBL_PERIOD_CYCLES 960u   /* 24 MHz * 40 us */
#define KBL_LEVELS 16            /* levels 0..16 */
#define KBL_DEFAULT_LEVEL 6      /* about 14 % duty; Linux's verified 32/255 is 12.5 % */
#define KBL_IDLE_US 60000000ull  /* off after 60 s without input */
#define KBL_REPEAT_DELAY_US 400000ull
#define KBL_REPEAT_US 120000ull

struct kbl_io {
    void *ctx;
    void (*write32)(void *, hm_u64 addr, hm_u32 v);
};

struct kbl {
    struct kbl_io io;
    int ready;            /* PWM checked and powered; otherwise every call is a no-op */
    int level;            /* user level 0..16 */
    int lit;              /* what the PWM shows now: level, or 0 while idle-off */
    hm_u64 last_input_us, next_repeat_us;
    int repeat_dir;
    hm_u32 writes, steps, idle_offs, wakes;
};

hm_u32 kbl_on_cycles(int level);
/* Sets the default level (writes the PWM). io must be valid; ready = 1 after this. */
void kbl_start(struct kbl *b, const struct kbl_io *io, hm_u64 now_us);
/* A Ctrl+brightness press (dir +1/-1): one step now, then auto-repeat while kbl_tick sees held. */
void kbl_step(struct kbl *b, int dir, hm_u64 now_us);
/* Any keyboard/trackpad input: restores the level after idle-off. */
void kbl_activity(struct kbl *b, hm_u64 now_us);
/* Periodic: held = +1/-1 while the key is held (auto-repeat), 0 otherwise; idle-off. */
void kbl_tick(struct kbl *b, int held, hm_u64 now_us);
/* Hypervisor teardown: light off, no further writes. */
void kbl_stop(struct kbl *b);
#endif
