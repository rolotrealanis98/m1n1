/* SPDX-License-Identifier: MIT */
/*
 * dcpw_core.h -- resident side of the DCPW display service: the trapped register
 * page, request validation, the virtual vblank and the backend interface.
 * Freestanding C, no allocator, one caller at a time (the resident's device lock).
 */
#ifndef DCPW_CORE_H
#define DCPW_CORE_H

#include <stdint.h>
#include "../abi/dcpw_abi.h"

struct dcpw_present {
	uint64_t gpa;
	uint32_t stride, width, height, format;
	uint32_t dx, dy, dw, dh; /* dirty rect, inside width x height */
};

/* A backend owns the hardware: "static" (no DCP traffic) or "iomfb" (M3). */
struct dcpw_backend {
	void *ctx;
	uint32_t caps; /* DCPW_CAP_* the backend can honour */
	/* Fill up to max modes; set *cur to the active one. Return count or <0. */
	int (*get_modes)(void *ctx, struct dcpw_mode *out, uint32_t max, uint32_t *cur);
	int (*set_mode)(void *ctx, const struct dcpw_mode *m, int vrr_on);
	int (*set_brightness)(void *ctx, uint32_t mnits);
	int (*present)(void *ctx, const struct dcpw_present *p);
};

/* What the resident provides. */
struct dcpw_env {
	void *ctx;
	int (*gpa_ok)(void *ctx, uint64_t gpa, uint64_t len); /* inside guest RAM, not a carve-out */
	void (*set_irq)(void *ctx, int level);
};

struct dcpw_dev {
	struct dcpw_backend be;
	struct dcpw_env env;
	int ready;
	uint32_t status, caps;
	struct dcpw_mode modes[DCPW_MAX_MODES];
	uint32_t nmodes, cur;
	uint32_t vrr, bl_mnits, bl_max_mnits;
	uint32_t irq_status, irq_enable;
	uint64_t vbl_count, vbl_ns, next_vbl_ns, vbl_latch_hi, vbl_ns_latch_hi;
	uint32_t presents;
	uint32_t req_raw[sizeof(struct dcpw_req) / 4];
	struct dcpw_rsp rsp;
	uint32_t stat_bad_access, stat_requests, stat_rejects;
};

int dcpw_configure(struct dcpw_dev *d, const struct dcpw_backend *be, const struct dcpw_env *env,
                   uint32_t bl_max_mnits, uint32_t bl_initial_mnits, uint64_t now_ns);
uint32_t dcpw_read32(struct dcpw_dev *d, uint32_t off);
void dcpw_write32(struct dcpw_dev *d, uint32_t off, uint32_t val, uint64_t now_ns);
/* Timer vblank (when the backend has no hardware vblank). Call from the resident tick. */
void dcpw_tick(struct dcpw_dev *d, uint64_t now_ns);
/* Hardware vblank / present completion from the backend (swap_complete). */
void dcpw_backend_vblank(struct dcpw_dev *d, uint64_t now_ns);
void dcpw_backend_present_done(struct dcpw_dev *d);
void dcpw_backend_failed(struct dcpw_dev *d);

uint64_t dcpw_period_ns(uint32_t refresh_mhz);

#endif
