/* SPDX-License-Identifier: MIT */
#ifndef PCIE_T6050_N1_H
#define PCIE_T6050_N1_H
#include "types.h"
struct n1_addrs;
int pcie_t6050_n1_addrs(struct n1_addrs *a);
int pcie_t6050_n1_route(u32 rid, u32 sid);
int pcie_t6050_n1_cycle(void);
int pcie_t6050_n1_park(void);
/* n1pt: storage register guard around runtime PORT_CYCLE / PARK (0 reference, 1 compare; -1 mismatch) */
int pcie_t6050_storage_guard(int compare);
#endif
