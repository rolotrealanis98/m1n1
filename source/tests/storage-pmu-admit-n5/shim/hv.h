/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/hv/hv.h: what hv_win_storage_mem.c takes (tests/storage-pmu-admit-n5) */
#ifndef HV_H
#define HV_H
#include "types.h"
struct hv_s2_leaf {
    u64 ipa_base, pa_base, size, descriptor;
};
bool hv_pt_resolve_leaf(u64 ipa, struct hv_s2_leaf *out);
#endif
