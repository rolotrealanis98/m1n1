/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/smp.h (tests/storage-pmu-admit-n5) */
#ifndef SMP_H
#define SMP_H
#include "smp_asm.h"
extern int host_cpu;
extern int boot_cpu_idx;
static inline int smp_id(void)
{
    return host_cpu;
}
#endif
