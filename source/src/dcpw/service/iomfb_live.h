/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iomfb_live.h -- the live DCP session in the resident (PLAN D3), behind the resident flag
 * dcpw_live (default OFF). Follows the owner's Linux (drm/apple + the 27.3 patches) exactly where it
 * is known, and the macOS 27.3 capture where Linux is wrong or silent (each case marked in the .c).
 * Reference: CALLBACKS.md (the callback table with citations) and LINUX-MAP.md.
 *
 * Stages (one flag value each, hardware plan LIVE-PLAN.md):
 *   LIVE_ATTACH  RTKit adopt (the caller runs m1n1 rtkit_boot), endpoints 0x2d + 0x37, SET_SHMEM,
 *                answer every callback, A451 start chain incl. the D120 boot chain; read the modes
 *                (DCPAV TimingElements, D414 DPTimingModeId). No client open, no power, no mode set,
 *                no swap: the panel keeps showing iBoot's framebuffer.
 *   LIVE_FLIP    + client open, A535 set_power_state (OOBCMD), swaps of the guest surface at the
 *                ALREADY ACTIVE timing (no A463), constant 120 deadline timestamps.
 *   LIVE_FULL    + A463 mode set, A500 adaptive sync, brightness in the swap.
 */
#ifndef IOMFB_LIVE_H
#define IOMFB_LIVE_H
#include <stdint.h>
#include "iomfb_codec.h"
#include "iomfb_session.h"   /* IOMFB_PROP_MAX, iomfb_timings_to_modes */
#include "dart4.h"
#include "dcpw_core.h"

struct live;

enum live_stage { LIVE_OFF = 0, LIVE_ATTACH = 1, LIVE_FLIP = 2, LIVE_FULL = 3 };
enum live_state { LS_IDLE, LS_WAIT_INIT, LS_STARTING, LS_STARTED, LS_OPENING, LS_POWERING, LS_READY, LS_FAILED };

struct live_io {
	void *ctx;
	void (*send)(void *ctx, uint8_t ep, uint64_t msg);       /* RTKit message to an endpoint */
	int (*pump)(void *ctx);                                   /* deliver inbound via live_rx(); 0 = nothing */
	uint64_t (*now_ns)(void *ctx);
	uint64_t (*alloc)(void *ctx, uint64_t len);               /* resident pages, zeroed, 16K aligned, not guest-mapped */
	void *(*ptr)(void *ctx, uint64_t pa);                      /* resident view of a resident PA */
	void (*log)(void *ctx, const char *s);
};

#define LIVE_MAX_MEMDESC 64
#define LIVE_TXDEPTH 8
#define LIVE_MAX_REGS 8

struct live_cmd {           /* one outstanding AP command on a tx stack */
	char tag[5];
	uint32_t off, end, in_len, out_len;
	void (*done)(struct live *, struct live_cmd *, const uint8_t *out);
	uint32_t cookie;
};

struct live_cb {            /* one deferred DCP callback awaiting our nested commands */
	uint8_t ctx;
	uint32_t off, in_len, out_len;
	char tag[5];
	uint32_t step;          /* boot-chain position for D120 */
	int ok;
};

struct live_memdesc { uint64_t pa, len, dva, piodma_dva; int used; };

struct live {
	struct live_io io;
	enum live_stage stage;
	enum live_state state;
	uint8_t *shmem;
	uint64_t shmem_pa, shmem_dva;
	struct dart4 *dart_dcp, *dart_piodma, *dart_disp;
	uint64_t disp_regs[LIVE_MAX_REGS], disp_reg_len[LIVE_MAX_REGS];
	uint32_t ndisp_regs;
	uint64_t bw_scratch;        /* 0x286720000 on J716s (D003) */
	uint64_t clock0_hz;         /* 810 MHz (D408 PROV 0, macOS) */
	struct dcpw_dev *dev;

	/* AP command stacks: [0] CMD, [1] OOBCMD (top level, from offset 0 of tx area 0x0 / 0x8000),
	 * [2] nested on CB, [3] nested on OOBCB. A nested command is placed at the end of the innermost
	 * record on its context's channel stack (callback records and nested commands share it:
	 * REF/iomfb.c:785,803,1473-1487), in the tx area of that context (0x0 / 0x8000). */
	struct live_cmd tx[4][LIVE_TXDEPTH];
	int txn[4];
	uint32_t chend[2][2 * LIVE_TXDEPTH];   /* channel stack ends: [0] CB, [1] OOBCB */
	int chn[2];
	uint64_t alloc_total;
	/* deferred callbacks (LIFO) */
	struct live_cb cbs[LIVE_TXDEPTH];
	int ncb;

	struct live_memdesc md[LIVE_MAX_MEMDESC];

	/* DCPAV and state read back */
	uint8_t prop[IOMFB_PROP_MAX];
	uint32_t prop_len, prop_recv;
	int prop_active, prop_contig;
	struct iomfb_timing timings[IOMFB_MAX_TIMINGS];
	int ntimings;
	uint32_t cur_timing;        /* D414 DPTimingModeId */
	uint32_t brightness_scale;
	uint32_t dfb_w, dfb_h, dfb_format;
	int main_display;
	uint32_t open_step;

	/* swaps */
	uint32_t swap_inflight, swaps_done;
	uint32_t bl_nits, bl_pending;
	struct dcpw_present last;
	int have_last;

	/* bookkeeping */
	uint32_t cb_count, cb_unknown, cmd_count, timeouts;
	char last_unknown[5];
	char fail_why[48];
	uint32_t timeout_polls;
};

void live_init(struct live *l, const struct live_io *io, enum live_stage stage, struct dart4 *dart_dcp,
               struct dart4 *dart_piodma, struct dart4 *dart_disp);
int live_attach(struct live *l);                           /* SET_SHMEM .. A451 acked; 0 = ok */
int live_open(struct live *l);                             /* client open + power (LIVE_FLIP+) */
void live_rx(struct live *l, uint8_t ep, uint64_t msg);
void live_backend(struct live *l, struct dcpw_backend *be);
int live_wait_idle(struct live *l, uint32_t polls);        /* pump until no command is outstanding */

#endif
