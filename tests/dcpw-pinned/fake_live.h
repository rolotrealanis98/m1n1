/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FAKE_LIVE_H
#define FAKE_LIVE_H
#include "../service/iomfb_live.h"

#define FL_QMAX 64
#define FL_LOG 128
struct fl_cb { char tag[5]; unsigned ctx; uint32_t off, end, in_len, out_len; int nested_script, inner; };

struct fake_live {
	struct live *live;
	struct dart4 *dart_dcp, *dart_piodma, *dart_disp;
	void *(*ptr)(void *, uint64_t);
	void *ctx;
	uint8_t *shm;
	uint64_t shm_dva;
	uint64_t qm[FL_QMAX];
	int qn;
	struct fl_cb cbs[16];
	int cbn, inner_cb_base;
	int step, nest_step, a451_open, a000_open, booted, a461;
	uint32_t chunk_off;
	const uint8_t *blob;
	uint32_t blob_len;
	uint64_t last_alloc;
	uint32_t ids[16];
	uint64_t dvas[16];
	int nids;
	uint32_t swap_id, last_swap, complete_pending, cur_timing, adaptive;
	uint64_t last_swap_pa;
	int last_bl, auto_complete;
	char cmdlog[FL_LOG][5];
	int ncmds;
	int errors, replies;
};

void fake_live_init(struct fake_live *f, struct live *l, struct dart4 *dcp, struct dart4 *piodma, struct dart4 *disp,
                    void *(*ptr)(void *, uint64_t), void *ctx, const uint8_t *blob, uint32_t blob_len);
void fake_live_transport(struct fake_live *f, void (**send)(void *, uint8_t, uint64_t), int (**pump)(void *));
#endif
