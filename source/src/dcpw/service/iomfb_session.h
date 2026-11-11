/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iomfb_session.h -- the "iomfb" DCPW backend: drives an IOMFB session that is
 * already up (SET_SHMEM acked, INIT_COMPLETE seen, DCPAV props delivered).
 *
 * M1 scope (offline, against tests/fake_dcp.c): callback dispatch for the DCPAV
 * property stream (D126-D128) and swap_complete (D594); commands set_parameter_dcp
 * (A500, adaptive sync), set_digital_out_mode (A463), swap_start (A456) and
 * swap_submit (A458). One command outstanding at a time (CMD context, depth 0).
 * An unknown callback marks the session failed and is NOT acked: the full callback
 * table (about 120 D tags on 27.3) is M3 work, and a wrong answer can crash the DCP
 * (CoreAnalytics precedent, kernel-dcp/0003).
 */
#ifndef IOMFB_SESSION_H
#define IOMFB_SESSION_H

#include "iomfb_codec.h"
#include "dcpw_core.h"

#define IOMFB_PROP_MAX   0x4000u /* TimingElements is 9513 B on J716s (DECODE:241) */
#define IOMFB_MAX_TIMINGS 16

struct iomfb_transport {
	void *ctx;
	void (*send)(void *ctx, uint8_t ep, uint64_t msg);
	/* Deliver pending inbound messages via iomfb_sess_rx(); return 0 when nothing arrived. */
	int (*pump)(void *ctx);
	uint64_t (*now_ns)(void *ctx);
	/* GPA -> DVA in dart-disp0 SID 0; 0 = unmapped. */
	uint64_t (*iova_for_gpa)(void *ctx, uint64_t gpa, uint64_t len);
};

struct iomfb_sess {
	uint8_t *shmem;
	struct iomfb_transport tp;
	struct dcpw_dev *dev; /* vblank / completion sink, may be 0 */
	uint32_t color_id;    /* the single SDR colour element, ID 1 (DECODE:250-261) */

	/* one outstanding command */
	int cmd_busy, cmd_acked;
	char cmd_tag[5];
	uint32_t cmd_in, cmd_out;
	uint8_t cmd_resp[16];
	uint32_t timeout_polls;

	/* DCPAV property assembly */
	uint8_t prop[IOMFB_PROP_MAX];
	uint32_t prop_len, prop_recv;
	int prop_active, prop_contig;
	struct iomfb_timing timings[IOMFB_MAX_TIMINGS];
	int ntimings;
	uint32_t cur_timing; /* timing ID */
	int vrr;

	/* swaps */
	uint32_t swap_inflight; /* swap id, 0 = none */
	uint32_t swaps_done;
	struct dcpw_present last;
	int have_last;
	uint32_t bl_nits, bl_pending;

	int failed;
	char fail_tag[5];
	uint32_t stat_cb, stat_cmd;
};

void iomfb_sess_init(struct iomfb_sess *s, uint8_t *shmem, const struct iomfb_transport *tp,
                     uint32_t cur_timing_id);
void iomfb_sess_rx(struct iomfb_sess *s, uint8_t ep, uint64_t msg);
int iomfb_sess_call(struct iomfb_sess *s, const char tag[4], const void *in, uint32_t in_len,
                    uint32_t out_len, void *out);
void iomfb_sess_backend(struct iomfb_sess *s, struct dcpw_backend *be);
/* Convert parsed timings to DCPW modes (VRR synthesized for zero-range preferred timings, as vrr/0002). */
int iomfb_timings_to_modes(const struct iomfb_timing *t, int n, struct dcpw_mode *out, uint32_t max,
                           uint32_t cur_id, uint32_t *cur_idx);

#endif
