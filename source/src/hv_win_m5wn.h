/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_M5WN_H
#define HV_WIN_M5WN_H
#include "types.h"
s64 hv_win_m5wn_configure(u64 data, u64 size);
u64 hv_win_m5wn_stat(u64 index);
void hv_win_m5wn_tick(void);
void hv_win_m5wn_retire(void);
bool hv_win_m5wn_claims_spi(u64 gsiv);
#endif
