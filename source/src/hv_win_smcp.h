/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_SMCP_H
#define HV_WIN_SMCP_H
#include "types.h"
s64 hv_win_smcp_configure(u64 version, u64 flags); /* pre-guest; P_HV_WIN_SMCP_CONFIGURE */
void hv_win_smcp_tick(void);                       /* boot CPU hv tick */
void hv_win_smcp_retire(void);
bool hv_win_smcp_claims_spi(u64 gsiv);
s64 hv_win_smcp_stat(void);                        /* P_HV_WIN_SMCP_STAT */
s64 hv_win_smcp_power(u64 action);                 /* P_HV_WIN_SMCP_POWER: 1 off, 2 restart; guest stopped */
void hv_win_smcp_after_exit(void);                 /* hv_start exit: honours the power-actions flag */
bool hv_win_smcp_actions_armed(void); /* nightly-2: configured with the power-actions flag */
#endif
