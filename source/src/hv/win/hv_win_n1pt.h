/* SPDX-License-Identifier: MIT */
/* n1pt (wireless/n1-native-20261111): N1 Wi-Fi/BT PCIe passthrough, resident glue (hv_win_n1pt.c) */
#ifndef HV_WIN_N1PT_H
#define HV_WIN_N1PT_H
#include "types.h"
s64 hv_win_n1pt_prep(u64 win, u64 ecam, u64 iova, u64 spi); /* P_HV_WIN_N1PT_PREP, pre-guest */
u64 hv_win_n1pt_stat(u64 index);                            /* P_HV_WIN_N1PT_STAT */
s64 hv_win_n1pt_map(void);                                  /* P_HV_WIN_N1PT_MAP: pool in stage 2, pre-guest */
bool hv_win_n1pt_unmapped(u64 ipa);                         /* hv_handle_dabort: repair an unmapped n1pt IPA */
bool hv_win_n1pt_start(void);                               /* hv_start(); never refuses the guest */
bool hv_win_n1pt_irq(u32 line);                             /* hv_win_handle_irq(): true = N1 line */
void hv_win_n1pt_retire(void);                              /* guest exit / resident retire: PARK */
#endif
