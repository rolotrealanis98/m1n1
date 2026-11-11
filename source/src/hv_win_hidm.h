/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_HIDM_H
#define HV_WIN_HIDM_H
#include "types.h"
s64 hv_win_hidm_configure(u64 version);
void hv_win_hidm_tick(void);
void hv_win_hidm_retire(void);
bool hv_win_hidm_claims_spi(u64 gsiv);
#endif
