/* SPDX-License-Identifier: MIT */
/*
 * SMCP power sequencer: turns a guest PSCI SYSTEM_OFF / SYSTEM_RESET into the exact SMC + PMU
 * sequence Linux's macsmc-reboot runs on this machine (drivers/power/reset/macsmc-reboot.c,
 * "GPL-2.0-only OR MIT"; behaviour ported, no code copied). Blocking; run only after every vCPU
 * has left the guest. Every write it can make is listed in WRITES.md.
 */
#ifndef SMCP_POWER_H
#define SMCP_POWER_H

#include "smcp_core.h"

/* PMU scratchpad cells, primary PMU (nub-spmi0 USID 0xe), t6050-die0.dtsi:224-262 */
#define SMCP_PMU_USID          0x0e
#define SMCP_PMU_BOOT_STAGE    0xf801 /* byte */
#define SMCP_PMU_SHUTDOWN_REG  0xf80f /* shutdown_flag = bit 3 */
#define SMCP_PMU_SHUTDOWN_BIT  3
#define SMCP_BOOT_STAGE_SHUTDOWN 0x00 /* macsmc-reboot.c:31 */

typedef struct smcp_pmu_ops {
    void *ctx;
    int (*read8)(void *ctx, uint16_t reg, uint8_t *val);  /* SPMI extended register read long */
    int (*write8)(void *ctx, uint16_t reg, uint8_t val);  /* SPMI extended register write long */
    void (*delay_us)(void *ctx, uint32_t us);
} smcp_pmu_ops;

enum smcp_power_action { SMCP_POWER_OFF = 1, SMCP_POWER_RESTART = 2 };

/* one record per step, for the console and for the host tests */
enum smcp_step_kind { SMCP_STEP_SMC_WRITE = 1, SMCP_STEP_PMU_READ, SMCP_STEP_PMU_WRITE, SMCP_STEP_DELAY };
typedef struct smcp_step {
    uint8_t kind;
    uint32_t key_or_reg;
    uint32_t value;
    int result;
} smcp_step;

#define SMCP_MAX_STEPS 8
typedef struct smcp_power_trace {
    smcp_step step[SMCP_MAX_STEPS];
    unsigned n;
} smcp_power_trace;

/*
 * Returns only if the machine did not go down: 0 = the final command was accepted but power
 * stayed on for 100 ms (Linux then WARNs, macsmc-reboot.c:104-105), <0 = the final command
 * failed. The caller then falls back to what it did before (report to host / watchdog).
 */
int smcp_power_run(smcp *s, const smcp_pmu_ops *pmu, int action, smcp_power_trace *trace);

#endif
