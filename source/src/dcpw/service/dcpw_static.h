/* SPDX-License-Identifier: MIT */
#ifndef DCPW_STATIC_H
#define DCPW_STATIC_H
#include "dcpw_core.h"

struct dcpw_static {
	int (*present_hook)(void *ctx, const struct dcpw_present *p); /* resident: point hv_win_fb at the surface */
	void *hook_ctx;
	struct dcpw_present last;
	int have_last;
};

const struct dcpw_mode *dcpw_j716s_modes(uint32_t *n);
void dcpw_static_backend(struct dcpw_static *s, struct dcpw_backend *be);
#endif
