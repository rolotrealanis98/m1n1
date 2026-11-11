/* nightly-7 win-session host test: platform mocks for the REAL hv_win_session.c (appended after this file with its
 * #include lines stripped). Host only. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef int64_t s64; typedef uint8_t u8;
typedef int iodev_id_t;
#define MAX_CPUS 24
enum { HV_WIN_EXIT_NONE = 0, HV_WIN_EXIT_SYSTEM_OFF = 1, HV_WIN_EXIT_SYSTEM_RESET = 2, HV_WIN_EXIT_HOST_STOP = 3,
       HV_WIN_EXIT_DEADMAN = 4 };
struct { u32 exit_reason; } hv_win;
static bool enabled = true;
static bool hv_win_enabled(void) { return enabled; }
bool hv_started_cpus[MAX_CPUS];
static u64 now_ticks, freq = 24000000;
#define mrs(r) mock_##r()
static u64 mock_CNTPCT_EL0(void) { return now_ticks; }
static u64 mock_CNTFRQ_EL0(void) { return freq; }
static int host_bytes;      /* bytes waiting on the proxy iodev */
static u64 host_bytes_at;   /* ... from this counter value on */
iodev_id_t uartproxy_iodev;
static long iodev_can_read(iodev_id_t id) { (void)id; return host_bytes && now_ticks >= host_bytes_at ? host_bytes : 0; }
static void iodev_console_flush(void) {}
static void udelay(u32 us) { now_ticks += (u64)us * freq / 1000000; }
static jmp_buf reset_jmp;
static int resets;
static void reboot(void) { resets++; longjmp(reset_jmp, 1); }
static int final_lines;
static void hv_win_storage_status_lines(const char *prefix) { (void)prefix; final_lines++; }
static u32 exits[8]; static int nexits;
static void hv_win_system_exit(u32 reason)
{
    exits[nexits++ & 7] = reason;
    hv_win.exit_reason = reason;
}
static char lastline[512]; static int lines;
#define printf(...) (lines++, snprintf(lastline, sizeof(lastline), __VA_ARGS__))
s64 hv_win_session_end(u64 code, u64 flags);
s64 hv_win_session_heartbeat(u64 seconds);
void hv_win_session_tick(void);
void hv_win_session_after_exit(void);
u64 hv_win_session_stat(u64 index);
