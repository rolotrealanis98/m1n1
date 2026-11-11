/* SPDX-License-Identifier: GPL-2.0-only */
/* fake_dcp.h -- a scripted IOMFB endpoint for host tests (our code). */
#ifndef FAKE_DCP_H
#define FAKE_DCP_H
#include "../service/iomfb_session.h"

#define FAKE_MAX_CALLS 64
enum fake_fault { FF_NONE = 0, FF_DROP_ACK, FF_SWAP_RET, FF_DIGITAL_RET, FF_SWAP_START_RET, FF_BAD_CMD_TAG };

struct fake_call { char tag[5]; uint32_t in_len, out_len; uint8_t in[IOMFB_SWAP_SUBMIT_LEN]; };

struct fake_dcp {
	uint8_t *shmem;
	struct iomfb_sess *sess;
	uint64_t q[64];
	int qh, qt;
	struct fake_call calls[FAKE_MAX_CALLS];
	int ncalls;
	uint32_t next_swap_id, last_submitted;
	int cb_acked, unexpected;
	enum fake_fault fault;
	uint32_t cur_timing, adaptive_q16;
	uint64_t now;
	uint64_t gpa_base, gpa_size, iova_base;
};

void fake_init(struct fake_dcp *f, uint8_t *shmem, struct iomfb_sess *s);
struct iomfb_transport fake_transport(struct fake_dcp *f);
/* DCP-initiated callback; returns 1 if the AP acked it. */
int fake_callback(struct fake_dcp *f, const char tag[4], const void *in, uint32_t in_len, uint32_t out_len, uint8_t *out_copy);
int fake_deliver_prop(struct fake_dcp *f, const char *key, const uint8_t *blob, uint32_t len);
int fake_complete_swap(struct fake_dcp *f, uint32_t id);
void fake_reset_calls(struct fake_dcp *f);

/* Serializer for DCP dictionaries (test side only). */
struct ser { uint8_t *b; uint32_t cap, pos; int err; };
void ser_header(struct ser *s);
void ser_tag(struct ser *s, uint32_t size, uint32_t type, int last);
void ser_int(struct ser *s, int64_t v, int last);
void ser_str(struct ser *s, const char *str, int last);
void ser_bool(struct ser *s, int v, int last);
uint32_t j716s_timing_blob(uint8_t *buf, uint32_t cap);
#endif
