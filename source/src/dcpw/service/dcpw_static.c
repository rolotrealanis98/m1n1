/* SPDX-License-Identifier: MIT */
/*
 * dcpw_static.c -- the "static" DCPW backend: no DCP traffic at all.
 *
 * It reports the J716s panel's six firmware timings (macOS 27.3 firmware, captured
 * by the owner: m5pro-linux contrib/validated/display/dcp120/firmware-timings.txt:1-6)
 * with timing ID 2 (3456x2234 @ 120 Hz) active, because the firmware reports
 * DPTimingModeId 2 before any mode set (DECODE-DCP-RPC-20261109.md:221) and the
 * Linux stage-C mode set found "Already set to timingID: 2" (stage-c-analysis.md:66).
 * INFERENCE: iBoot leaves the panel scanning at 120 Hz; RUN-SPEC D0 measures it.
 *
 * Mode set, VRR and brightness are refused (no caps). PRESENT hands the guest
 * surface to the resident's existing converter through present_hook.
 */
#include "dcpw_static.h"

static const struct dcpw_mode j716s_modes[] = {
	/* flags, id, w, h, htotal, vtotal, mHz, vrr min/max (24-120 Hz: vrr/0002 synthesis for the 120 Hz timing) */
	{ DCPW_MODE_PREFERRED | DCPW_MODE_VRR, 2, 3456, 2234, 3536, 2456, 120000, 24000, 120000, {0} },
	{ 0, 3, 3456, 2234, 3536, 2456, 60000, 0, 0, {0} },
	{ 0, 5, 3456, 2234, 3536, 2456, 59940, 0, 0, {0} },
	{ 0, 7, 3456, 2234, 3536, 2946, 50000, 0, 0, {0} },
	{ 0, 4, 3456, 2234, 3536, 2456, 48000, 0, 0, {0} },
	{ 0, 6, 3456, 2234, 3536, 2456, 47952, 0, 0, {0} },
};

const struct dcpw_mode *dcpw_j716s_modes(uint32_t *n)
{
	*n = sizeof(j716s_modes) / sizeof(j716s_modes[0]);
	return j716s_modes;
}

static int st_get_modes(void *ctx, struct dcpw_mode *out, uint32_t max, uint32_t *cur)
{
	(void)ctx;
	uint32_t n;
	const struct dcpw_mode *m = dcpw_j716s_modes(&n);
	if (n > max)
		n = max;
	for (uint32_t i = 0; i < n; i++)
		out[i] = m[i];
	*cur = 0;
	return (int)n;
}

static int st_present(void *ctx, const struct dcpw_present *p)
{
	struct dcpw_static *s = ctx;
	s->last = *p;
	s->have_last = 1;
	return s->present_hook ? s->present_hook(s->hook_ctx, p) : 0;
}

void dcpw_static_backend(struct dcpw_static *s, struct dcpw_backend *be)
{
	be->ctx = s;
	be->caps = 0;
	be->get_modes = st_get_modes;
	be->set_mode = 0;
	be->set_brightness = 0;
	be->present = st_present;
}
