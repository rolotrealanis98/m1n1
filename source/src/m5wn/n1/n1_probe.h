/* SPDX-License-Identifier: MIT */
#ifndef N1_PROBE_H
#define N1_PROBE_H
#include "n1_hal.h"

#define N1_PROBE_SMC 1u   /* also read the SMC reset key (one RTKit session: INIT, READ_KEY) */

/* Result slots, readable one by one through the proxy (P_HV_WIN_N1_STAT). ~0 = not read. */
enum n1_probe_slot {
    N1P_VERSION, N1P_FLAGS, N1P_RC_CTL, N1P_RC_STAT, N1P_P0_STATUS, N1P_P0_LINKSTS, N1P_P0_LTSSM,
    N1P_P1_LINKSTS, N1P_P2_LINKSTS, N1P_P3_LINKSTS,
    N1P_PS_FIRST, /* N1_PS_COUNT power-state words */
    N1P_P0_APPCLK = N1P_PS_FIRST + N1_PS_COUNT, N1P_P0_INT_RESET, N1P_P0_CLKRST, N1P_P0_MSICFG,
    N1P_PHY_CTL_STATUS, N1P_PHY_REFCLK, N1P_PERST_PAD,
    N1P_RP_ID, N1P_RP_BUSES, N1P_EP_ID, N1P_EP_COMMAND, N1P_RID2SID_VALID /* count of valid entries */, N1P_DART_PROTECT,
    N1P_DART_208, N1P_DART_STREAMS,
    N1P_ST_FIRST, /* 7 storage words */
    N1P_SMC_KEY = N1P_ST_FIRST + 7, N1P_SMC_RC, N1P_SMC_VALUE, N1P_VERDICT,
    N1P_COUNT
};

/* Verdict bits */
#define N1V_GP_POWER   0x01 /* APCIE_SYS_GP and APCIE_PHY_SW actual on */
#define N1V_RC_READY   0x02
#define N1V_LINK_UP    0x04
#define N1V_EP_ROM     0x08 /* endpoint 106b:1900 */
#define N1V_EP_RUNNING 0x10 /* endpoint 106b:1901 (a warm N1) */
#define N1V_PERST_LOW  0x20 /* pad output-low (N1 held in reset) */
#define N1V_DART_CLEAN 0x40
#define N1V_SMC_RELEASED 0x80 /* gP13 reads template|1 */
#define N1V_STORAGE_OK 0x100   /* ANS running and BOOT_STATUS 0xde71ce55 */

/* Read-only probe. Order and gating follow reads m1n1 has already issued on this machine:
 * port/RC status (kboot_n1 n1_port_up), PMGR PS words, the PERST# pad (stage A entry), the
 * shared-PHY status only with GP power or a running RC (stage S), ECAM/RID2SID/DART only with a
 * live link (stage F/G; a DART whose port is down SErrors), storage words (storage_check). */
int n1_probe(const struct n1_hal *h, const struct n1_addrs *a, unsigned flags, n1_u64 out[N1P_COUNT]);
#endif
