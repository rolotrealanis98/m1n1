/* SPDX-License-Identifier: MIT */
/*
 * nightly-12: framebuffer snapshot ("eyes on the panel"). Read-only. The host sends a request on the proxy link,
 * and the resident streams a box-downscaled RGB image back as proxy events. It works while the guest runs and
 * after the storage select / host-link lockdown, without a proxy session and without stopping any vCPU.
 *
 * Request: HV_WIN_SHOT_BYTE (0x17), then 18 ASCII hex digits: src(1) scale(1) x(4) y(4) w(4) h(4).
 *   src 0 = the panel: the boot (POST) framebuffer cur_boot_args.video, which is what the panel scans out.
 *           The shadow converter writes into it, and the native display driver presents into the same surface
 *           (AcquirePostDisplayOwnership: same pa, pitch and format). Format by video.depth: 30 = X2R10G10B10
 *           (B in bits 9:0), 24/32 = X8R8G8B8.
 *   src 1 = the EL2 shadow (the guest's BGRA8 surface), only while the shadow is active.
 *   src 2 = the DFB regardless of who owns the panel (stale while Windows owns it).
 *   dcp-pt: src 0 follows the glass. EL2 owns the panel: the DFB. Windows owns it (display-owner register = 1): the surface
 *   the guest published in the owner frame (hv_win_dcp_scanout; resolved source 3 in BEGIN), or FAIL SHOT_E_STALE when it
 *   published none. Never the paused DFB.
 *   scale 1, 2, 4 or 8 (box average of scale x scale source pixels).
 *   x, y, w, h: a region in source pixels; w == 0 or h == 0 means the whole surface. It is clamped to the surface.
 * The host never supplies an address. Every read is computed from the resident's own surface description and
 * stays inside [base, base + stride * height); host tests check this under ASan.
 * hv_tick reads the bytes (hv_win_shot_rx) the same way as the heartbeat byte. A byte that cannot continue a
 * request is handed back unconsumed, so '!' and the heartbeat are never lost.
 *
 * Reply: EVT_M5W_SHOT proxy events (struct shot_evt + data): BEGIN, one ROW per output row (w * 3 bytes RGB),
 * END, or FAIL. hv_win_shot_tick runs on the tick CPU and does one bounded slice per tick: at least one row,
 * then rows until SHOT_BUDGET_US is spent. An event is queued only when the whole event fits in the USB ring
 * (all or nothing, under the iodev lock), so the stream is never torn and the tick never waits for the host.
 * No proxy entry, no rendezvous, no all-vCPU stop. Each slice is bounded and counted (max slice in END and on
 * the console).
 * The allow-list for the locked link (native_link.c) is unchanged: this is not a proxy opcode and not a memory
 * read by the host.
 */
#include "hv_win.h"
#include "../hv.h"
#include "exception.h"
#include "iodev.h"
#include "uartproxy.h"
#include "usb_dwc3.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define SHOT_REQ_DIGITS   18
#define SHOT_W_MAX        4096          /* output row pixels */
#define SHOT_DIM_MAX      16384
#define SHOT_BUDGET_US    150           /* per tick, after the first row */
#define SHOT_RX_TIMEOUT_MS 500          /* an incomplete request is dropped */
#define SHOT_TIMEOUT_MS   30000         /* a host that stops reading: abort */
#define SHOT_FAIL_GRACE_MS 5000         /* a FAIL event waits this long for room, then the capture ends silently */
#define SHOT_EVT_MAGIC    0x544f4853u   /* "SHOT" */
#define SHOT_EVT_HDR      8             /* uartproxy.c UartEventHdr: u32 type, u16 len, u16 event_type */

enum { SHOT_BEGIN = 0, SHOT_ROW = 1, SHOT_END = 2, SHOT_FAIL = 3 };
enum { SHOT_FMT_X2R10G10B10 = 0, SHOT_FMT_X8R8G8B8 = 1 };
enum {
    SHOT_E_BUSY = 1, SHOT_E_SRC = 2, SHOT_E_FMT = 3, SHOT_E_SCALE = 4, SHOT_E_REGION = 5, SHOT_E_MAP = 6,
    SHOT_E_TIMEOUT = 7, SHOT_E_LINK = 8,
    SHOT_E_STALE = 9, /* dcp-pt: Windows owns the panel and published no valid scanout surface: the DFB is not on glass */
};

struct shot_evt {
    u32 magic;
    u16 seq;
    u8 kind;
    u8 fmt;
    u16 row;
    u16 w;
    u16 h;
    u16 scale_src; /* scale | src << 8 */
    u32 aux0;      /* BEGIN: x | y << 16; END: elapsed ms; FAIL: reason */
    u32 aux1;      /* BEGIN: source w | h << 16; END: longest slice in us; FAIL: detail */
} __attribute__((packed));

struct shot_cap {
    bool active, begun, failed_pending;
    u32 fail_reason, fail_detail;
    u64 base, stride;
    u32 fmt, src, scale, sw, sh, x, y, w, h, ow, oh, row;
    u64 t0, slice_max, fail_t0;
};

static struct {
    /* request receiver */
    u32 rx_n;
    u8 rx[SHOT_REQ_DIGITS];
    u64 rx_t0;
    u16 seq;
    /* totals */
    u64 requests, done, failed, rows, bytes, slice_max_all, dropped_rx;
} shot;
static struct shot_cap cap; /* the capture in progress */

static u32 shot_acc[3][SHOT_W_MAX];
static u8 shot_buf[sizeof(struct shot_evt) + SHOT_W_MAX * 3];

static u64 shot_us(void)
{
    u64 f = mrs(CNTFRQ_EL0) / 1000000;
    return f ? f : 1;
}

static int shot_hex(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static u32 shot_field(unsigned int at, unsigned int n)
{
    u32 v = 0;
    for (unsigned int i = 0; i < n; i++)
        v = (v << 4) | (u32)shot_hex(shot.rx[at + i]);
    return v;
}

/* true: the whole event was queued; false: no room now (nothing was written) or no USB proxy link */
static bool shot_send(const struct shot_evt *e, const void *data, u32 len)
{
    iodev_id_t id = uartproxy_iodev;
    if (id < IODEV_USB0 || id >= IODEV_USB0 + USB_IODEV_COUNT)
        return false;
    u32 total = sizeof(*e) + len;
    if (total > sizeof(shot_buf) || total > 0xffff)
        return false;
    iodev_lock(id);
    void *dev = iodev_get_opaque(id);
    size_t need = SHOT_EVT_HDR + total + 4;
    if (!dev || usb_dwc3_write_free(dev, CDC_ACM_PIPE_0) < need) {
        iodev_unlock(id);
        return false;
    }
    if ((const u8 *)e != shot_buf) {
        memcpy(shot_buf, e, sizeof(*e));
        if (len)
            memcpy(shot_buf + sizeof(*e), data, len);
    }
    uartproxy_send_event(EVT_M5W_SHOT, shot_buf, (u16)total);
    iodev_unlock(id);
    shot.bytes += need;
    return true;
}

static void shot_evt_init(struct shot_evt *e, u8 kind)
{
    memset(e, 0, sizeof(*e));
    e->magic = SHOT_EVT_MAGIC;
    e->seq = shot.seq;
    e->kind = kind;
    e->fmt = (u8)cap.fmt;
    e->w = (u16)cap.ow;
    e->h = (u16)cap.oh;
    e->scale_src = (u16)(cap.scale | cap.src << 8);
}

static void shot_fail_set(u32 reason, u32 detail)
{
    cap.active = true; /* until the FAIL event is out (or the timeout) */
    cap.failed_pending = true;
    cap.fail_reason = reason;
    cap.fail_detail = detail;
    cap.fail_t0 = mrs(CNTPCT_EL0);
}

/* One read probe of an address inside the surface; false when it faults (not mapped at EL2). */
static bool shot_probe(u64 addr)
{
    exc_count = 0;
    exc_guard = GUARD_SKIP | GUARD_SILENT;
    (void)read32(addr);
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == 0;
}

static void shot_begin(void)
{
    u32 src = (u32)shot_hex(shot.rx[0]), scale = (u32)shot_hex(shot.rx[1]);
    u32 x = shot_field(2, 4), y = shot_field(6, 4), w = shot_field(10, 4), h = shot_field(14, 4);
    shot.requests++;
    shot.seq++;
    memset(&cap, 0, sizeof(cap));
    cap.src = src;
    cap.scale = scale;
    cap.t0 = mrs(CNTPCT_EL0);

    u64 base = 0, stride = 0, sw = 0, sh = 0;
    u32 fmt = SHOT_FMT_X8R8G8B8;
    struct hv_win_scanout sc = {0};
    int own = src == 0 ? hv_win_dcp_scanout(&sc) : 0;
    if (own == 2) {
        shot_fail_set(SHOT_E_STALE, 1);
        return;
    }
    if (own == 1) {
        base = sc.pa;
        stride = sc.stride;
        sw = sc.w;
        sh = sc.h;
        fmt = sc.fmt ? SHOT_FMT_X8R8G8B8 : SHOT_FMT_X2R10G10B10;
        cap.src = 3;
    } else if (src == 0 || src == 2) {
        const struct boot_video *v = &cur_boot_args.video;
        base = v->base;
        stride = v->stride;
        sw = v->width;
        sh = v->height;
        switch (v->depth & 0xff) {
            case 30:
                fmt = SHOT_FMT_X2R10G10B10;
                break;
            case 24:
            case 32:
                fmt = SHOT_FMT_X8R8G8B8;
                break;
            default:
                { shot_fail_set(SHOT_E_FMT, (u32)(v->depth & 0xff)); return; }
        }
    } else if (src == 1) {
        if (!hv_win_fb_shadow_src(&base, &stride, &sw, &sh))
            { shot_fail_set(SHOT_E_SRC, 1); return; }
    } else {
        { shot_fail_set(SHOT_E_SRC, src); return; }
    }
    cap.fmt = fmt;
    if (!base || !sw || !sh || sw > SHOT_DIM_MAX || sh > SHOT_DIM_MAX || stride < sw * 4 || (stride & 3) ||
        base > UINT64_MAX - stride * sh)
        { shot_fail_set(SHOT_E_SRC, 2); return; }
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
        { shot_fail_set(SHOT_E_SCALE, scale); return; }
    if (!w || !h) {
        x = y = 0;
        w = (u32)sw;
        h = (u32)sh;
    }
    if (x >= sw || y >= sh)
        { shot_fail_set(SHOT_E_REGION, x | y << 16); return; }
    if (w > sw - x)
        w = (u32)(sw - x);
    if (h > sh - y)
        h = (u32)(sh - y);
    u32 ow = w / scale, oh = h / scale;
    if (!ow || !oh || ow > SHOT_W_MAX)
        { shot_fail_set(SHOT_E_REGION, ow | oh << 16); return; }
    cap.base = base;
    cap.stride = stride;
    cap.sw = (u32)sw;
    cap.sh = (u32)sh;
    cap.x = x;
    cap.y = y;
    cap.w = ow * scale;
    cap.h = oh * scale;
    cap.ow = ow;
    cap.oh = oh;
    /* first and last pixel the capture will read */
    u64 first = base + (u64)y * stride + (u64)x * 4;
    u64 last = base + (u64)(y + cap.h - 1) * stride + (u64)(x + cap.w - 1) * 4;
    if (!shot_probe(first) || !shot_probe(last))
        { shot_fail_set(SHOT_E_MAP, 0); return; }
    cap.active = true;
}

/* Box-average output row r into shot_buf + header as RGB8. Reads rows y + r*s .. +s-1, columns x .. x+w-1. */
static void shot_row(u32 r)
{
    u32 s = cap.scale, ow = cap.ow, n = s * s;
    memset(shot_acc, 0, sizeof(shot_acc[0][0]) * 3 * SHOT_W_MAX);
    for (u32 dy = 0; dy < s; dy++) {
        const u32 *p = (const u32 *)(cap.base + (u64)(cap.y + r * s + dy) * cap.stride + (u64)cap.x * 4);
        u32 i = 0;
        for (u32 ox = 0; ox < ow; ox++) {
            u32 ar = 0, ag = 0, ab = 0;
            for (u32 dx = 0; dx < s; dx++, i++) {
                u32 v = p[i];
                if (cap.fmt == SHOT_FMT_X2R10G10B10) {
                    ar += (v >> 20) & 0x3ff;
                    ag += (v >> 10) & 0x3ff;
                    ab += v & 0x3ff;
                } else {
                    ar += (v >> 16) & 0xff;
                    ag += (v >> 8) & 0xff;
                    ab += v & 0xff;
                }
            }
            shot_acc[0][ox] += ar;
            shot_acc[1][ox] += ag;
            shot_acc[2][ox] += ab;
        }
    }
    u8 *o = shot_buf + sizeof(struct shot_evt);
    for (u32 ox = 0; ox < ow; ox++) {
        for (u32 c = 0; c < 3; c++) {
            u32 avg = (shot_acc[c][ox] + n / 2) / n;
            *o++ = (u8)(cap.fmt == SHOT_FMT_X2R10G10B10 ? (avg * 255 + 511) / 1023 : avg);
        }
    }
}

/* hv_tick: one byte from the proxy link; true when it belongs to a snapshot request */
bool hv_win_shot_rx(u8 b)
{
    if (b == HV_WIN_SHOT_BYTE) {
        if (shot.rx_n)
            shot.dropped_rx++;
        shot.rx_n = 0;
        shot.rx_t0 = mrs(CNTPCT_EL0) | 1;
        return true;
    }
    if (!shot.rx_t0)
        return false;
    if (shot_hex(b) < 0) {          /* not part of a request: drop the partial one, the caller handles b */
        shot.rx_t0 = 0;
        shot.rx_n = 0;
        shot.dropped_rx++;
        return false;
    }
    shot.rx[shot.rx_n++] = b;
    if (shot.rx_n < SHOT_REQ_DIGITS)
        return true;
    shot.rx_t0 = 0;
    shot.rx_n = 0;
    if (cap.active) {              /* one capture at a time; the running one goes on */
        shot.failed++;
        return true;
    }
    shot_begin();
    return true;
}

/* hv_tick, after the framebuffer converter: one bounded slice of the running capture */
void hv_win_shot_tick(void)
{
    u64 now = mrs(CNTPCT_EL0), us = shot_us();
    if (shot.rx_t0 && now - shot.rx_t0 > SHOT_RX_TIMEOUT_MS * 1000 * us) {
        shot.rx_t0 = 0;
        shot.rx_n = 0;
        shot.dropped_rx++;
    }
    if (!cap.active)
        return;
    struct shot_evt *e = (struct shot_evt *)shot_buf;
    if (cap.failed_pending) {
        struct shot_evt f;
        shot_evt_init(&f, SHOT_FAIL);
        f.aux0 = cap.fail_reason;
        f.aux1 = cap.fail_detail;
        if (shot_send(&f, NULL, 0) || now - cap.fail_t0 > SHOT_FAIL_GRACE_MS * 1000 * us) {
            cap.active = false;
            cap.failed_pending = false;
            shot.failed++;
            printf("HV/win n12 shot %u failed: reason %u detail 0x%x\n", shot.seq, cap.fail_reason,
                   cap.fail_detail);
        }
        return;
    }
    if (now - cap.t0 > SHOT_TIMEOUT_MS * 1000 * us) {
        shot_fail_set(SHOT_E_TIMEOUT, cap.row);
        return;
    }
    u64 t0 = now, budget = SHOT_BUDGET_US * us;
    if (!cap.begun) {
        struct shot_evt b;
        shot_evt_init(&b, SHOT_BEGIN);
        b.aux0 = cap.x | cap.y << 16;
        b.aux1 = cap.sw | cap.sh << 16;
        if (!shot_send(&b, NULL, 0))
            return;
        cap.begun = true;
    }
    u32 rowbytes = cap.ow * 3;
    size_t need = SHOT_EVT_HDR + sizeof(struct shot_evt) + rowbytes + 4;
    while (cap.row < cap.oh) {
        iodev_id_t id = uartproxy_iodev;
        void *dev = (id >= IODEV_USB0 && id < IODEV_USB0 + USB_IODEV_COUNT) ? iodev_get_opaque(id) : NULL;
        if (!dev || usb_dwc3_write_free(dev, CDC_ACM_PIPE_0) < need)
            break;                  /* the host has not read enough yet: next tick */
        shot_row(cap.row);
        shot_evt_init(e, SHOT_ROW);
        e->row = (u16)cap.row;
        if (!shot_send(e, NULL, rowbytes))
            break;                  /* the room went to the console in between: this row again next tick */
        cap.row++;
        shot.rows++;
        if (mrs(CNTPCT_EL0) - t0 >= budget)
            break;
    }
    u64 slice = (mrs(CNTPCT_EL0) - t0) / us;
    if (slice > cap.slice_max)
        cap.slice_max = slice;
    if (slice > shot.slice_max_all)
        shot.slice_max_all = slice;
    if (cap.row < cap.oh)
        return;
    struct shot_evt d;
    shot_evt_init(&d, SHOT_END);
    d.row = (u16)cap.row;
    d.aux0 = (u32)((mrs(CNTPCT_EL0) - cap.t0) / (1000 * us));
    d.aux1 = (u32)cap.slice_max;
    if (!shot_send(&d, NULL, 0))
        return;
    cap.active = false;
    shot.done++;
    printf("HV/win n12 shot %u: %ux%u from %s %ux%u scale %u in %u ms, longest tick slice %lu us (no vCPU stop)\n",
           shot.seq, cap.ow, cap.oh, cap.src == 3 ? "guest scanout" : cap.src == 2 ? "DFB (forced)" : cap.src == 1 ? "shadow" : "panel DFB", cap.sw, cap.sh, cap.scale, d.aux0,
           (unsigned long)cap.slice_max);
}

u64 hv_win_shot_stat(u64 idx)
{
    const u64 v[] = {shot.requests, shot.done, shot.failed, shot.rows, shot.bytes, shot.slice_max_all,
                     shot.dropped_rx, cap.active};
    return idx < ARRAY_SIZE(v) ? v[idx] : 0;
}
