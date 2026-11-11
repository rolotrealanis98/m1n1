/* SPDX-License-Identifier: MIT */
/* kbl: see kbl.h. Own code. */
#include "kbl.h"

hm_u32 kbl_on_cycles(int level)
{
    hm_u32 l, on;
    if (level <= 0) return 0;
    if (level > KBL_LEVELS) level = KBL_LEVELS;
    l = (hm_u32)level;
    on = (KBL_PERIOD_CYCLES * l * l + (KBL_LEVELS * KBL_LEVELS) / 2) / (KBL_LEVELS * KBL_LEVELS);
    return on < 6 ? 6 : on; /* level 1 still visible */
}

static void show(struct kbl *b, int level)
{
    hm_u32 on;
    if (!b->ready || level == b->lit) return;
    on = kbl_on_cycles(level);
    if (!on) {
        b->io.write32(b->io.ctx, KBL_PWM_BASE + KBL_PWM_CTRL, 0);
        b->writes++;
    } else {
        b->io.write32(b->io.ctx, KBL_PWM_BASE + KBL_PWM_ON, on);
        b->io.write32(b->io.ctx, KBL_PWM_BASE + KBL_PWM_OFF, KBL_PERIOD_CYCLES - on);
        b->io.write32(b->io.ctx, KBL_PWM_BASE + KBL_PWM_CTRL, KBL_CTRL_ON);
        b->writes += 3;
    }
    b->lit = level;
}

void kbl_start(struct kbl *b, const struct kbl_io *io, hm_u64 now_us)
{
    hm_zero(b, sizeof(*b));
    b->io = *io;
    b->ready = 1;
    b->lit = -1; /* unknown: force the first write */
    b->level = KBL_DEFAULT_LEVEL;
    b->last_input_us = now_us;
    show(b, b->level);
}

void kbl_step(struct kbl *b, int dir, hm_u64 now_us)
{
    if (!b->ready) return;
    b->last_input_us = now_us;
    if (b->lit != b->level) { show(b, b->level); b->wakes++; } /* first press after idle-off */
    b->level += dir > 0 ? 1 : -1;
    if (b->level < 0) b->level = 0;
    if (b->level > KBL_LEVELS) b->level = KBL_LEVELS;
    b->steps++;
    show(b, b->level);
    b->repeat_dir = dir > 0 ? 1 : -1;
    b->next_repeat_us = now_us + KBL_REPEAT_DELAY_US;
}

void kbl_activity(struct kbl *b, hm_u64 now_us)
{
    if (!b->ready) return;
    b->last_input_us = now_us;
    if (b->lit != b->level) { show(b, b->level); b->wakes++; }
}

void kbl_tick(struct kbl *b, int held, hm_u64 now_us)
{
    if (!b->ready) return;
    if (held && held == b->repeat_dir && now_us >= b->next_repeat_us) {
        int before = b->level;
        b->level += held;
        if (b->level < 0) b->level = 0;
        if (b->level > KBL_LEVELS) b->level = KBL_LEVELS;
        if (b->level != before) b->steps++;
        b->last_input_us = now_us;
        show(b, b->level);
        b->next_repeat_us = now_us + KBL_REPEAT_US;
    }
    if (!held) b->repeat_dir = 0;
    if (b->lit > 0 && now_us - b->last_input_us >= KBL_IDLE_US) {
        show(b, 0);
        b->idle_offs++;
    }
}

void kbl_stop(struct kbl *b)
{
    if (!b->ready) return;
    show(b, 0);
    b->ready = 0;
}
