/* SPDX-License-Identifier: MIT */
#ifndef KBL_RESIDENT_H
#define KBL_RESIDENT_H
#include "hidm/tp/kbl.h"
/* Preguest: check ADT /arm-io/pwm0, power FPWM0, show the default level. 0 = ready. */
int kbl_resident_start(struct kbl *b, hm_u64 now_us);
#endif
