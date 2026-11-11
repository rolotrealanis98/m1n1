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

#define HV_SESSION_COLLECT_S 30

extern bool hv_started_cpus[MAX_CPUS];

static struct {
    bool armed, autoreset;
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

void hv_win_session_tick(void)
{
    if (!sess.armed || hv_win.exit_reason != HV_WIN_EXIT_NONE || !guest_running())
        return;
    if (mrs(CNTPCT_EL0) - sess.last_beat < sess.timeout_ticks)
        return;
    sess.deadman++;
    sess.autoreset = true;
    printf("HV/win n7: dead-man: no host heartbeat for %lu s: ending the session (SYSTEM_OFF path, storage "
           "retire, then reset)\n", (unsigned long)(sess.timeout_ticks / mrs(CNTFRQ_EL0)));
    hv_win_system_exit(HV_WIN_EXIT_DEADMAN);
}

void hv_win_session_after_exit(void)
{
    u32 why = hv_win.exit_reason;
    bool guest_exit = why == HV_WIN_EXIT_SYSTEM_OFF || why == HV_WIN_EXIT_SYSTEM_RESET;
    if (!sess.autoreset && !(sess.armed && guest_exit))
        return; /* the host collects the session as before */
    if (!sess.autoreset) {
        printf("HV/win n7: guest exit; waiting up to %d s for the host to collect the session\n", HV_SESSION_COLLECT_S);
        iodev_console_flush();
        u64 end = mrs(CNTPCT_EL0) + HV_SESSION_COLLECT_S * mrs(CNTFRQ_EL0);
        while (mrs(CNTPCT_EL0) < end) {
            if (iodev_can_read(uartproxy_iodev) > 0)
                return; /* the launcher is there: its guest-exit path continues (it resets with p.reboot()) */
            udelay(10000);
        }
        printf("HV/win n7: no host request within %d s of the guest exit\n", HV_SESSION_COLLECT_S);
    }
    sess.self_resets++;
    hv_win_storage_status_lines("HV/win n7 FINAL"); /* counters after the retire (read only) */
    printf("HV/win n7: session ended (%s): resetting the machine\n",
           why == HV_WIN_EXIT_HOST_STOP ? "host stop" : why == HV_WIN_EXIT_DEADMAN ? "dead-man" : "guest exit");
    iodev_console_flush();
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
        default: return ~0UL;
    }
}
