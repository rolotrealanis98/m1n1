/* SPDX-License-Identifier: MIT */
/*
 * dcp-pt: host test of the shadow converter's pause/resume (hv_win_fb_set_paused), built on the disp3 harness model
 * (harness/sim.c: the real hv_win_fb.c against stubs and a stage-2/TLB model). Scenarios, DIRTY and sweep modes:
 *   - paused: guest drawing converts nothing (native keeps the old picture, pass count frozen);
 *   - resumed: the whole shadow is reconverted, including pages dirtied while paused (native == shadow everywhere);
 *   - after the resume the dirty tracking works again (a later store reaches the native framebuffer);
 *   - pause/resume are idempotent and a resume with nothing drawn still ends with native == shadow.
 */
#define main sim_main
#include "../../../harness/sim.c"
#undef main
void hv_win_fb_set_paused(bool paused);
bool hv_win_fb_paused(void);
bool hv_win_fb_active(void);
u64 hv_win_fb_passes(void);

#ifndef NEW
#error "fbpause needs the NEW (current tree) converter"
#endif

static void scenario(const char *name, u64 flags)
{
    rd_ns_per_b = 0.0; wr_ns_per_b = 0.0;
    int rc = setup(1000, 600, 4160, flags, 0);
    CHECK(rc > 0, "%s: setup %d", name, rc);
    if (rc <= 0)
        return;
    CHECK(!hv_win_fb_paused() && hv_win_fb_active(), "%s: initial state", name);
    guest_rect(0, 0, GW, GH, 0xff102030u);
    run_until_idle(2e9);
    CHECK(verify_full() == 0, "%s: first frame not converted", name);
    u64 passes0 = hv_win_fb_passes();

    hv_win_fb_set_paused(true);
    hv_win_fb_set_paused(true); /* idempotent */
    CHECK(hv_win_fb_paused(), "%s: not paused", name);
    guest_rect(100, 100, 300, 200, 0xff405060u);
    guest_rect(0, 580, 1000, 20, 0xff708090u);
    for (int i = 0; i < 300; i++) {
        sim_now = next_tick;
        do_tick();
    }
    CHECK(hv_win_fb_passes() == passes0, "%s: converter ran while paused (%lu vs %lu)", name,
          (unsigned long)hv_win_fb_passes(), (unsigned long)passes0);
    u32 px = *(u32 *)(native + 150ul * S + 150ul * 4);
    CHECK(px == ref_px(0xff102030u), "%s: native changed while paused (0x%x)", name, px);

    hv_win_fb_set_paused(false);
    CHECK(!hv_win_fb_paused(), "%s: still paused", name);
    run_until_idle(4e9);
    for (int i = 0; i < 200; i++) {
        sim_now = next_tick;
        do_tick();
    }
    CHECK(verify_full() == 0, "%s: native != shadow after resume (%lu bad)", name, (unsigned long)verify_full());
    CHECK(hv_win_fb_passes() > passes0, "%s: no pass after resume", name);

    /* dirty tracking works again */
    guest_rect(500, 300, 100, 100, 0xff0a0b0cu);
    run_until_idle(2e9);
    for (int i = 0; i < 100; i++) {
        sim_now = next_tick;
        do_tick();
    }
    CHECK(verify_full() == 0, "%s: store after resume not converted", name);

    /* resume with nothing pending, twice */
    hv_win_fb_set_paused(true);
    hv_win_fb_set_paused(false);
    hv_win_fb_set_paused(false);
    run_until_idle(4e9);
    for (int i = 0; i < 200; i++) {
        sim_now = next_tick;
        do_tick();
    }
    CHECK(verify_full() == 0, "%s: idle pause/resume left a stale frame", name);
    teardown();
}

int main(void)
{
    CHILD(scenario("dirty", 2));
    CHILD(scenario("sweep", 0));
    CHILD(scenario("dirty+scale2", 6));
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
