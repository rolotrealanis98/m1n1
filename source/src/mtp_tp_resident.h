/* SPDX-License-Identifier: MIT */
#ifndef MTP_TP_RESIDENT_H
#define MTP_TP_RESIDENT_H
#include "mtp_kbd.h"
/* Before mtp_kbd_resident_start: remember the HIDF firmware (resident memory,
 * copied by the launcher); addr 0 = no trackpad. */
void mtp_tp_resident_arm(hm_u64 addr, hm_u64 len);
/* After keyboard LIVE: finish the trackpad; the PTP interface or NULL. */
const hm_backend_interface *mtp_tp_resident_finish(struct mtp_kbd *k);
#endif
