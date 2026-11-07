/* SPDX-License-Identifier: MIT */

#ifndef PCIE_T6050_H
#define PCIE_T6050_H

#include "types.h"

/* Train T6050 apcie0 port 0 (the N1) from cold; 0 once its link is up, -1 after a stop */
int pcie_t6050_n1_train(bool smc_release);

#endif
