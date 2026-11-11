/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HV_WIN_DCPW_H
#define HV_WIN_DCPW_H
#include "types.h"

#define DCPW_LIVE_REPORT_MAGIC 0x564c5044u /* "DPLV" */
struct dcpw_live_report {
	u32 magic, stage, step, result, state;
	u32 callbacks, unknown, commands, ntimings, cur_timing;
	u32 timing_id[8], timing_rate_q16[8], timing_wh[8];
	u32 dfb_w, dfb_h, bl_nits, ep2d, dart_tables, dart_flushes;
	u32 swaps, swap_us, full_rc;
	char fail[48];
	char last_unknown[4];
};

int dcpw_live_run(u64 stage, u64 nflips, u64 report_pa, u64 flags);

#endif
