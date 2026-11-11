/* nightly-7 win-session: every trigger of the cable-independent session end */
static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL line %d: %s (last: %s)\n", __LINE__, #x, lastline); } } while (0)
static void reset_world(void)
{
    memset(&sess, 0, sizeof(sess));
    memset(&pw, 0, sizeof(pw));
    memset(hv_cpu_rec, 0, sizeof(hv_cpu_rec));
    wdt_kicks = wdt_sets = hang_reports = 0; wdt_alarm = 0; wdt_old = 3600000000u;
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    hv_win.exit_reason = HV_WIN_EXIT_NONE;
    nexits = resets = final_lines = 0;
    host_bytes = 0; now_ticks = 1000; enabled = true;
    hv_started_cpus[0] = hv_started_cpus[12] = true; /* guest running on 2 CPUs */
}
/* after_exit either returns (host collects) or resets (longjmp); returns 1 if it reset */
static int run_after_exit(void)
{
    if (setjmp(reset_jmp))
        return 1;
    hv_win_session_after_exit();
    return 0;
}
static void guest_stops(void) { memset(hv_started_cpus, 0, sizeof(hv_started_cpus)); }
int main(void)
{
    /* 1. host stop with self-reset: the SYSTEM_OFF path is requested, then the resident resets after it */
    reset_world();
    CHECK(hv_win_session_end(7, 1) == 0 && nexits == 1 && exits[0] == HV_WIN_EXIT_HOST_STOP);
    CHECK(hv_win_session_end(7, 1) == -1);                 /* an exit is already pending */
    guest_stops();
    CHECK(run_after_exit() == 1 && resets == 1 && final_lines == 1 && strstr(lastline, "host stop"));
    /* host stop without the reset flag: the host collects (no reset) */
    reset_world();
    CHECK(hv_win_session_end(1, 0) == 0);
    guest_stops();
    CHECK(run_after_exit() == 0 && resets == 0);
    /* refusals: no guest, bad flags, Windows personality off */
    reset_world(); guest_stops();
    CHECK(hv_win_session_end(1, 1) == -1 && nexits == 0);
    reset_world();
    CHECK(hv_win_session_end(1, 2) == -1 && nexits == 0);
    reset_world(); enabled = false;
    CHECK(hv_win_session_end(1, 1) == -1 && nexits == 0);

    /* 2. dead-man: armed by a heartbeat; fires only after the timeout without a heartbeat */
    reset_world();
    for (int i = 0; i < 1000; i++) { now_ticks += freq / 1000; hv_win_session_tick(); }
    CHECK(nexits == 0);                                    /* never armed: never fires */
    CHECK(hv_win_session_heartbeat(5) == -1 && hv_win_session_heartbeat(3601) == -1);
    CHECK(hv_win_session_heartbeat(120) == 0 && hv_win_session_stat(4) == 120);
    for (int s = 0; s < 119; s++) { now_ticks += freq; hv_win_session_tick(); }
    CHECK(nexits == 0);
    CHECK(hv_win_session_heartbeat(0) == 0);              /* a refresh restarts the window */
    for (int s = 0; s < 119; s++) { now_ticks += freq; hv_win_session_tick(); }
    CHECK(nexits == 0);
    now_ticks += 2 * freq; hv_win_session_tick();
    CHECK(nexits == 1 && exits[0] == HV_WIN_EXIT_DEADMAN && hv_win_session_stat(3) == 1);
    hv_win_session_tick();
    CHECK(nexits == 1);                                    /* once */
    guest_stops();
    CHECK(run_after_exit() == 1 && resets == 1 && strstr(lastline, "dead-man"));
    /* the dead-man does not fire after the guest stopped on its own */
    reset_world();
    hv_win_session_heartbeat(60);
    hv_win.exit_reason = HV_WIN_EXIT_SYSTEM_OFF;
    now_ticks += 600 * freq; hv_win_session_tick();
    CHECK(nexits == 0);

    /* 3. nightly-7b (changed from nightly-7, which waited 30 s here and so reset before the launcher could read the
     * P_HV_START reply): a guest exit with the dead-man armed returns at once and leaves the SoC watchdog at 30 s
     * (every host request pets it; without a host it resets the machine) */
    reset_world();
    hv_win_session_heartbeat(120);
    hv_win.exit_reason = HV_WIN_EXIT_SYSTEM_RESET; guest_stops();
    u64 t0 = now_ticks;
    CHECK(run_after_exit() == 0 && resets == 0 && now_ticks == t0);
    CHECK(wdt_sets == 1 && wdt_alarm == 30u * 24000000u);
    /* ... with the progress watchdog armed, its alarm is kept for the collection */
    reset_world();
    CHECK(hv_win_progress_wdt_arm(20) == 0);
    hv_win.exit_reason = HV_WIN_EXIT_SYSTEM_OFF; guest_stops();
    CHECK(run_after_exit() == 0 && resets == 0 && wdt_alarm == 20u * 24000000u && !pw.on);
    /* guest exit without the dead-man armed: unchanged (the host collects; no wait, no reset) */
    reset_world();
    hv_win.exit_reason = HV_WIN_EXIT_SYSTEM_OFF; guest_stops();
    t0 = now_ticks;
    CHECK(run_after_exit() == 0 && resets == 0 && now_ticks == t0);

    /* 4. nightly-7b progress watchdog: range; applied at the first tick; pets only when every other started CPU took
     * a timer FIQ in the last second; reports the stuck CPU; off when stage 1 left no watchdog armed */
    reset_world();
    CHECK(hv_win_progress_wdt_arm(4) == -1 && hv_win_progress_wdt_arm(121) == -1 && !pw.on);
    CHECK(!hv_win_progress_wdt_tick() && wdt_kicks == 0);           /* not armed: hv_tick pets as before */
    CHECK(hv_win_progress_wdt_arm(20) == 0 && wdt_sets == 0);       /* deferred to the guest's first tick */
    CHECK(hv_win_progress_wdt_tick() && wdt_sets == 1 && wdt_alarm == 20u * 24000000u && wdt_kicks == 1);
    for (int s = 0; s < 10; s++) {                                  /* all CPUs advance: one pet per second */
        for (int ms = 0; ms < 1000; ms++) {
            now_ticks += freq / 1000;
            hv_cpu_rec[0].heartbeat++; hv_cpu_rec[12].heartbeat++;
            hv_win_progress_wdt_tick();
        }
    }
    CHECK(wdt_kicks == 1 + 10 && hang_reports == 0);
    for (int s = 0; s < 10; s++) {                                  /* CPU 12 stops taking FIQs: no pet, reported */
        for (int ms = 0; ms < 1000; ms++) {
            now_ticks += freq / 1000;
            hv_cpu_rec[0].heartbeat++;
            hv_win_progress_wdt_tick();
        }
    }
    CHECK(wdt_kicks <= 1 + 10 + 1 && hang_reports >= 2 && hang_cpu == 12);
    /* m1n1 audit: a CPU Windows switched off (PSCI CPU_OFF: the core leaves the guest and hv.c clears its
     * hv_started_cpus flag) is not counted as stuck; petting resumes */
    hv_started_cpus[12] = false;
    int k0 = wdt_kicks, r0 = hang_reports;
    for (int s = 0; s < 5; s++) {
        for (int ms = 0; ms < 1000; ms++) {
            now_ticks += freq / 1000;
            hv_cpu_rec[0].heartbeat++;
            hv_win_progress_wdt_tick();
        }
    }
    CHECK(wdt_kicks >= k0 + 4 && hang_reports == r0);
    reset_world(); wdt_old = 0;                                     /* no watchdog left armed by stage 1 */
    CHECK(hv_win_progress_wdt_arm(20) == 0 && !hv_win_progress_wdt_tick() && !pw.on);

    printf("win-session: %d checks, %d failed\n", checks, fails);
    fprintf(stdout, "%s", lastline);
    return fails != 0;
}
