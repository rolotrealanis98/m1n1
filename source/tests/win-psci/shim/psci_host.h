/* SPDX-License-Identifier: MIT */
/*
 * Host build of src/hv/win/hv_win_psci.c (tests/win-psci): the m1n1 pieces the file uses -- the
 * spin table (smp_is_alive/smp_get_mpidr), hv_start_secondary/hv_exit_cpu, the stage-2 RAM check,
 * cache maintenance and printf -- are mocks in test_psci.c. Types come from tests/win-vgic3/shim.
 */
#ifndef PSCI_HOST_H
#define PSCI_HOST_H

#include <stdio.h>
#include <string.h>

#include "exception.h"
#include "smp_asm.h"
#include "types.h"

int host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf host_printf

extern int host_cpu;
extern int boot_cpu_idx;
static inline int smp_id(void)
{
    return host_cpu;
}
bool smp_is_alive(int cpu);
u64 smp_get_mpidr(int cpu);
void smp_send_ipi(int cpu);

bool hv_pt_is_ram(u64 addr);
void dc_civac_range(void *addr, size_t length);
int hv_start_secondary_bounded(int cpu, void *entry, u64 regs[4]);
void iodev_console_flush(void);
void hv_exit_cpu(int cpu);

#endif
