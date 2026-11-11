/* SPDX-License-Identifier: MIT */
/*
 * nightly-7: cable-independent end of a Windows session (the owner's debug link moved to a USB-A cable, so the
 * launcher's USB-PD reset is gone).
 *
 *   P_HV_WIN_SESSION_END(code, flags)  host request, admitted by the locked link only in a host-interrupt session:
 *       ends the guest exactly as a guest SYSTEM_OFF does (hv_win_system_exit: every started vCPU exits, then the
 *       boot CPU runs hv.c's unchanged exit path, which retires HIDM/SMCP/M5WN/N1 and calls the reviewed
 *       hv_win_storage_retire()). flags bit 0: the resident resets the machine itself after that path.
 *   P_HV_WIN_HEARTBEAT(seconds)  host heartbeat; a non-zero value arms the dead-man with that timeout (10..3600 s).
 *   dead-man (hv_tick, boot CPU, under bhl): armed, guest running, no heartbeat for the timeout -> the same exit
 *       path with a self-reset.
 *   after the exit path (hv.c, boot CPU, every vCPU stopped, storage retired): reset now after a host stop with
 *       flags bit 0 or a dead-man; after a guest SYSTEM_OFF/RESET with the dead-man armed, wait up to
 *       HV_SESSION_COLLECT_S for the host to talk (the launcher's normal guest-exit path); if it does not, reset.
 * Nothing here touches storage state: the retire is called by hv.c as before, and the final status lines only
 * read counters.
 */
#include "../hv.h"
#include "hv_win.h"
#include "hv_win_storage.h"
#include "iodev.h"
#include "smp.h"
#include "uartproxy.h"
#include "utils.h"
#include "wdt.h"

#define HV_SESSION_COLLECT_S 30

extern bool hv_started_cpus[MAX_CPUS];

static struct {
    bool armed, autoreset, running;
    u64 timeout_ticks, last_beat;
    u64 ends, heartbeats, deadman, self_resets, last_code;
} sess;

static bool guest_running(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (hv_started_cpus[cpu])
            return true;
    return false;
}

s64 hv_win_session_end(u64 code, u64 flags)
{
    if (!hv_win_enabled() || (flags & ~1UL) || !guest_running() || hv_win.exit_reason != HV_WIN_EXIT_NONE) {
        printf("HV/win n7: session end (code %lu) refused (no running guest, a pending exit or bad flags)\n",
               (unsigned long)code);
        return -1;
    }
    sess.autoreset = flags & 1;
    sess.last_code = code;
    sess.ends++;
    printf("HV/win n7: host requested session end (code %lu)%s\n", (unsigned long)code,
           sess.autoreset ? "; the resident resets the machine after the exit path" : "");
    hv_win_system_exit(HV_WIN_EXIT_HOST_STOP);
    return 0;
}

s64 hv_win_session_heartbeat(u64 seconds)
{
    if (seconds && (seconds < 10 || seconds > 3600))
        return -1;
    sess.last_beat = mrs(CNTPCT_EL0);
    if (seconds) {
        if (!sess.armed)
            printf("HV/win n7: dead-man armed: %lu s without a host heartbeat ends the session and resets\n",
                   (unsigned long)seconds);
        sess.timeout_ticks = seconds * mrs(CNTFRQ_EL0);
        sess.armed = true;
    }
    sess.heartbeats++;
    return 0;
}

/* nightly-10: the launcher's heartbeat is one byte on the proxy link (HV_WIN_HEARTBEAT_BYTE), read by hv_tick's
 * normal USB poll: no proxy entry, no rendezvous, no vCPU stops (P_HV_WIN_HEARTBEAT through a host interrupt held
 * bhl inside the proxy for a whole host round trip: 3 of 17 beats stalled a core ~3 s, n9bcpu runs). */
void hv_win_session_heartbeat_byte(void)
{
    sess.last_beat = mrs(CNTPCT_EL0);
    sess.heartbeats++;
}

void hv_win_session_tick(void)
{
    if (!sess.armed || hv_win.exit_reason != HV_WIN_EXIT_NONE || !guest_running())
        return;
    if (!sess.running) { /* nightly-10: armed before select (pre-guest): the window starts with the guest */
        sess.running = true;
        sess.last_beat = mrs(CNTPCT_EL0);
        return;
    }
    if (mrs(CNTPCT_EL0) - sess.last_beat < sess.timeout_ticks)
        return;
    sess.deadman++;
    sess.autoreset = true;
    printf("HV/win n7: dead-man: no host heartbeat for %lu s: ending the session (SYSTEM_OFF path, storage "
           "retire, then reset)\n", (unsigned long)(sess.timeout_ticks / mrs(CNTFRQ_EL0)));
    hv_win_system_exit(HV_WIN_EXIT_DEADMAN);
}

/* nightly-7b progress watchdog: the SoC watchdog (left armed by stage 1, m1n1.wdt=keep) gets a short alarm at the
 * first hypervisor tick of the guest and is petted from hv_tick only when every other started CPU took a timer FIQ
 * since the previous check (1 s): a CPU spinning in EL2 with interrupts masked, or a boot CPU that never reaches its
 * tick, lets it bite. The proxy loop still pets it on every host request (proxy_process). */
#define HV_WDT_HZ 24000000ULL /* the SoC watchdog counts the 24 MHz reference clock */
static struct {
    bool on, applied;
    u32 secs;
    u64 next, period, misses, pets;
    u64 last[MAX_CPUS];
} pw;

s64 hv_win_progress_wdt_arm(u64 seconds)
{
    if (seconds < 5 || seconds > 120)
        return -1;
    pw.on = true;
    pw.applied = false;
    pw.secs = (u32)seconds;
    printf("HV/win n7b: progress watchdog: %lu s, applied at the first guest tick\n", (unsigned long)seconds);
    return 0;
}

bool hv_win_progress_wdt_tick(void)
{
    if (!pw.on)
        return false;
    u64 now = mrs(CNTPCT_EL0);
    if (!pw.applied) {
        pw.applied = true;
        pw.period = mrs(CNTFRQ_EL0);
        pw.next = now + pw.period;
        u32 old = wdt_set_alarm((u32)(pw.secs * HV_WDT_HZ));
        if (!old) {
            printf("HV/win n7b: progress watchdog NOT armed (no watchdog left armed by stage 1)\n");
            pw.on = false;
            return false;
        }
        printf("HV/win n7b: progress watchdog armed: SoC watchdog alarm %u -> %lu ticks (%u s at 24 MHz; the old "
               "alarm is %lu s at 24 MHz)\n", old, (unsigned long)(pw.secs * HV_WDT_HZ), pw.secs,
               (unsigned long)(old / HV_WDT_HZ));
        for (int c = 0; c < MAX_CPUS; c++)
            pw.last[c] = hv_cpu_rec[c].heartbeat;
        wdt_kick();
        return true;
    }
    if (now < pw.next)
        return true;
    pw.next = now + pw.period;
    int stuck = -1, me = smp_id();
    for (int c = 0; c < MAX_CPUS; c++) {
        u64 hb = __atomic_load_n(&hv_cpu_rec[c].heartbeat, __ATOMIC_RELAXED);
        if (hv_started_cpus[c] && c != me && hb == pw.last[c] && stuck < 0)
            stuck = c;
        pw.last[c] = hb;
    }
    if (stuck < 0) {
        wdt_kick();
        pw.pets++;
        pw.misses = 0;
        return true;
    }
    if (!pw.misses++ || !(pw.misses % 5))
        hv_hang_report("progress watchdog: a CPU took no timer FIQ for 1 s (not petting)", stuck);
    return true;
}

void hv_win_session_after_exit(void)
{
    u32 why = hv_win.exit_reason;
    bool guest_exit = why == HV_WIN_EXIT_SYSTEM_OFF || why == HV_WIN_EXIT_SYSTEM_RESET;
    if (!sess.autoreset && !((sess.armed || pw.on) && guest_exit))
        return; /* the host collects the session as before */
    if (!sess.autoreset) {
        /* nightly-7b: never wait here. The host cannot talk before the P_HV_START reply that follows this path
         * (nightly-7 waited 30 s for it, so the resident reset before the launcher read the guest exit). Leave a
         * short SoC watchdog instead: every host request pets it (proxy_process), so the launcher's guest-exit path
         * and p.reboot() run as before; without a host the watchdog resets the machine. */
        u32 secs = pw.on ? pw.secs : HV_SESSION_COLLECT_S;
        u32 old = wdt_set_alarm((u32)(secs * HV_WDT_HZ));
        pw.on = false; /* the tick no longer runs; the proxy loop pets */
        printf("HV/win n7: guest exit; the host has %u s per request to collect the session (watchdog %s)\n", secs,
               old ? "armed" : "NOT armed");
        iodev_console_flush();
        return;
    }
    sess.self_resets++;
    hv_win_storage_status_lines("HV/win n7 FINAL"); /* counters after the retire (read only) */
    printf("HV/win n7: session ended (%s): resetting the machine\n",
           why == HV_WIN_EXIT_HOST_STOP ? "host stop" : why == HV_WIN_EXIT_DEADMAN ? "dead-man" : "guest exit");
    iodev_console_flush();
    wdt_kick();
    udelay(300000); /* let the host read the last lines */
    reboot();
}

u64 hv_win_session_stat(u64 index)
{
    switch (index) {
        case 0: return sess.armed | (sess.autoreset << 1);
        case 1: return sess.ends;
        case 2: return sess.heartbeats;
        case 3: return sess.deadman;
        case 4: return sess.timeout_ticks / mrs(CNTFRQ_EL0);
        case 5: return pw.on | (pw.applied << 1); /* nightly-7b progress watchdog */
        case 6: return pw.pets;
        case 7: return pw.misses;
        default: return ~0UL;
    }
}
