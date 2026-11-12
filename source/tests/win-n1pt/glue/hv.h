/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: stand-in for src/hv/hv.h (stage-2 hooks recorded by glue.c) */
#ifndef N1PT_GLUE_HV_H
#define N1PT_GLUE_HV_H
#include "n1pt_host.h"
struct exc_info { int x; };
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hw_dev(u64 from, u64 to, u64 size);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
int hv_map_hw(u64 from, u64 to, u64 size);
u64 hv_pt_walk(u64 addr);
struct hv_s2_leaf {
    u64 ipa_base, pa_base, size, descriptor;
};
bool hv_pt_resolve_leaf(u64 ipa, struct hv_s2_leaf *out);
#endif
