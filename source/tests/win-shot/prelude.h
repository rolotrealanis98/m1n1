/* nightly-12 win-shot host test: platform mocks for the REAL hv_win_shot.c (appended after this file with its
 * #include lines stripped). Host only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef int64_t s64; typedef uint16_t u16; typedef uint8_t u8;
typedef int iodev_id_t;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HV_WIN_SHOT_BYTE 0x17
#define EVT_M5W_SHOT 0x10
#define USB_IODEV_COUNT 8
enum { IODEV_UART = 0, IODEV_USB_VUART = 3, IODEV_USB0 = 4 };
enum { CDC_ACM_PIPE_0 = 0, CDC_ACM_PIPE_1 = 1 };
enum { GUARD_OFF = 0, GUARD_SKIP = 1, GUARD_SILENT = 0x100 };

/* clock: every CNTPCT read advances by tick_step (default 0) */
static u64 now_ticks, tick_step, freq = 24000000;
#define mrs(r) mock_##r()
static u64 mock_CNTPCT_EL0(void) { u64 t = now_ticks; now_ticks += tick_step; return t; }
static u64 mock_CNTFRQ_EL0(void) { return freq; }
#define sysop(x) ((void)0)

/* EL2 mapping: reads outside [mapped_lo, mapped_hi) fault (exc_count++, value 0) as with GUARD_SKIP */
static volatile int exc_count, exc_guard;
static u64 mapped_lo, mapped_hi;
static int probe_reads;
static u32 read32(u64 a)
{
    probe_reads++;
    if (a < mapped_lo || a + 4 > mapped_hi) {
        if (!(exc_guard & GUARD_SKIP)) { fprintf(stderr, "unguarded fault at %#llx\n", (unsigned long long)a); abort(); }
        exc_count++;
        return 0;
    }
    return *(volatile u32 *)a;
}

struct boot_video { u64 base, display, stride, width, height, depth; };
static struct { struct boot_video video; } cur_boot_args;

/* shadow */
static bool shadow_on;
static u64 shadow_base, shadow_stride, shadow_w, shadow_h;
static bool hv_win_fb_shadow_src(u64 *b, u64 *s, u64 *w, u64 *h)
{
    if (!shadow_on) return false;
    *b = shadow_base; *s = shadow_stride; *w = shadow_w; *h = shadow_h;
    return true;
}

/* dcp-pt: who owns the panel (hv_win_dcp_scanout): 0 EL2 (the DFB), 1 Windows + published surface, 2 Windows, none published */
struct hv_win_scanout { u64 pa, stride; u32 w, h, fmt; };
static int scan_state; static struct hv_win_scanout scan_mock;
static int hv_win_dcp_scanout(struct hv_win_scanout *o) { if (scan_state == 1) *o = scan_mock; return scan_state; }

/* proxy link: USB ring with ring_free bytes; every event is recorded */
iodev_id_t uartproxy_iodev = IODEV_USB0;
static int lock_depth, lock_max;
static size_t ring_free = 1 << 20;
static int fake_dev;
static void iodev_lock(iodev_id_t id) { (void)id; lock_depth++; if (lock_depth > lock_max) lock_max = lock_depth; }
static void iodev_unlock(iodev_id_t id) { (void)id; lock_depth--; }
static void *iodev_get_opaque(iodev_id_t id) { return (id >= IODEV_USB0 && id < IODEV_USB0 + USB_IODEV_COUNT) ? &fake_dev : NULL; }
static size_t usb_dwc3_write_free(void *dev, int pipe) { (void)dev; (void)pipe; return ring_free; }

#define EVMAX 20000
struct ev { u16 type, len; u8 kind; u16 row, w, h, seq, scale_src; u8 fmt; u32 aux0, aux1; u8 *data; };
static struct ev evs[EVMAX];
static int nev, partial_writes;
static void uartproxy_send_event(u16 type, void *data, u16 len)
{
    if (lock_depth < 1) { fprintf(stderr, "event outside the iodev lock\n"); abort(); }
    size_t need = 8 + (size_t)len + 4;
    if (need > ring_free) { partial_writes++; return; }  /* the real ring would take part of it: a torn stream */
    ring_free -= need;
    if (nev >= EVMAX) { fprintf(stderr, "too many events\n"); abort(); }
    struct ev *e = &evs[nev++];
    const u8 *p = data;
    e->type = type; e->len = len;
    memcpy(&e->seq, p + 4, 2); e->kind = p[6]; e->fmt = p[7];
    memcpy(&e->row, p + 8, 2); memcpy(&e->w, p + 10, 2); memcpy(&e->h, p + 12, 2); memcpy(&e->scale_src, p + 14, 2);
    memcpy(&e->aux0, p + 16, 4); memcpy(&e->aux1, p + 20, 4);
    e->data = malloc(len > 24 ? len - 24 : 1);
    if (len > 24) memcpy(e->data, p + 24, len - 24);
}
static char lastline[512]; static int lines;
#define printf(...) (lines++, snprintf(lastline, sizeof(lastline), __VA_ARGS__))
