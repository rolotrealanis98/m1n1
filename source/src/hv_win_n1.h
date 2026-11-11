/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_N1_H
#define HV_WIN_N1_H
#include "types.h"
s64 hv_win_n1_probe(u64 flags);
u64 hv_win_n1_stat(u64 index);
s64 hv_win_n1_start(u64 fw, u64 fw_len, u64 stage, u64 flags);
s64 hv_win_n1_ctl(u64 op, u64 arg);
void *hv_win_n1_core(void); /* struct n1 * once the radio is up, else NULL */
void hv_win_n1_tick(void);
s64 hv_win_n1_park(void);
void hv_win_n1_retire(void);
#endif
