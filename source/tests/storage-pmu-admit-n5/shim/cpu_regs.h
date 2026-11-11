/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/cpu_regs.h (tests/storage-pmu-admit-n5): mrs(DAIF) reads a variable */
#ifndef CPU_REGS_H
#define CPU_REGS_H
#include "types.h"
extern u64 host_sysreg_DAIF;
#define mrs(reg) (host_sysreg_##reg)
#endif
