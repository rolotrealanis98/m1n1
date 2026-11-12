/* SPDX-License-Identifier: MIT */

#ifndef PCIE_T6050_H
#define PCIE_T6050_H

#include "types.h"

#define N1_TRAIN_SMC  BIT(0) /* pulse the N1's reset through the SMC first, 1 s link-up wait */
#define N1_TRAIN_J813 BIT(1) /* J813's port order: refclk and PERST# before the reset handshake */
#define N1_TRAIN_LINUX BIT(2) /* n1pt: port part as Linux's cold_train (capture cap2): 250 ms PERST# hold, P0033 port_enable/configure/train */

/* Train T6050 apcie0 port 0 (the N1) from cold; 0 once its link is up, -1 after a stop */
int pcie_t6050_n1_train(unsigned int flags);

/* The same with ANS started for the storage checks and stopped again after (chosen.m5pro,n1=3|4) */
int pcie_t6050_n1_train_ans(unsigned int flags);

#endif
