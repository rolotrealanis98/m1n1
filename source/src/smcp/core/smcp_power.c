/* SPDX-License-Identifier: MIT */
#include "smcp_power.h"

static void put_le32(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xff;
    b[1] = (v >> 8) & 0xff;
    b[2] = (v >> 16) & 0xff;
    b[3] = v >> 24;
}

static void rec(smcp_power_trace *t, uint8_t kind, uint32_t kr, uint32_t value, int result)
{
    if (t && t->n < SMCP_MAX_STEPS) {
        t->step[t->n].kind = kind;
        t->step[t->n].key_or_reg = kr;
        t->step[t->n].value = value;
        t->step[t->n].result = result;
        t->n++;
    }
}

/* apple_smc_write_u32(key, value): the u32 goes into the shared buffer in CPU (little-endian)
 * byte order, size 4 (include/linux/mfd/macsmc.h write helpers; macsmc.c:161-162). */
static int smc_write_u32(smcp *s, smcp_power_trace *t, uint32_t key, uint32_t value, uint64_t timeout_us)
{
    uint8_t b[4];
    put_le32(b, value);
    int r = smcp_cmd_blocking(s, SMCP_K_WRITE, key, b, 4, timeout_us);
    rec(t, SMCP_STEP_SMC_WRITE, key, value, r);
    return r;
}

static int smc_write_u8(smcp *s, smcp_power_trace *t, uint32_t key, uint8_t value, uint64_t timeout_us)
{
    int r = smcp_cmd_blocking(s, SMCP_K_WRITE, key, &value, 1, timeout_us);
    rec(t, SMCP_STEP_SMC_WRITE, key, value, r);
    return r;
}

/* nvmem cell write of a sub-byte field: read the byte, replace the field, write it back (the
 * nvmem core does this for cells with "bits"; the regmap-spmi bus uses extended-long commands
 * for registers above 0xff, regmap-spmi.c:130-141,170-181). */
static int pmu_set_bit(const smcp_pmu_ops *pmu, smcp_power_trace *t, uint16_t reg, unsigned bit, bool on)
{
    uint8_t v = 0;
    int r = pmu->read8(pmu->ctx, reg, &v);
    rec(t, SMCP_STEP_PMU_READ, reg, v, r);
    if (r < 0)
        return r;
    uint8_t nv = on ? (uint8_t)(v | (1u << bit)) : (uint8_t)(v & ~(1u << bit));
    r = pmu->write8(pmu->ctx, reg, nv);
    rec(t, SMCP_STEP_PMU_WRITE, reg, nv, r);
    return r;
}

int smcp_power_run(smcp *s, const smcp_pmu_ops *pmu, int action, smcp_power_trace *t)
{
    bool off = action == SMCP_POWER_OFF;
    if (t)
        t->n = 0;
    if (action != SMCP_POWER_OFF && action != SMCP_POWER_RESTART)
        return -10;

    /* 1. reboot notifier (macsmc-reboot.c:127-169): MBSE = offw / rest, failure only logged */
    smc_write_u32(s, t, SMCP_KEY('M', 'B', 'S', 'E'), off ? SMCP_KEY('o', 'f', 'f', 'w') : SMCP_KEY('r', 'e', 's', 't'),
                  SMCP_TIMEOUT_US);
    /* 2. boot_stage = 0 (clean shutdown/restart), :155-158 */
    int r = pmu->write8(pmu->ctx, SMCP_PMU_BOOT_STAGE, SMCP_BOOT_STAGE_SHUTDOWN);
    rec(t, SMCP_STEP_PMU_WRITE, SMCP_PMU_BOOT_STAGE, SMCP_BOOT_STAGE_SHUTDOWN, r);
    /* 3. shutdown_flag = 1 for power off, 0 for restart, :160-168 */
    pmu_set_bit(pmu, t, SMCP_PMU_SHUTDOWN_REG, SMCP_PMU_SHUTDOWN_BIT, off);
    /* 4. prepare: enter atomic mode = NTAP 0, result ignored (macsmc.c:266-283) */
    smc_write_u8(s, t, SMCP_KEY('N', 'T', 'A', 'P'), 0, SMCP_TIMEOUT_US);
    /* 5. MBSE = off1 / phra, atomic write with the 5 s timeout (macsmc-reboot.c:95-123, macsmc.c:51) */
    r = smc_write_u32(s, t, SMCP_KEY('M', 'B', 'S', 'E'), off ? SMCP_KEY('o', 'f', 'f', '1') : SMCP_KEY('p', 'h', 'r', 'a'),
                      SMCP_ATOMIC_TIMEOUT_US);
    if (r != 0)
        return r < 0 ? r : -(0x100 | r);
    /* 6. mdelay(100): still here means the SMC accepted it but nothing happened (:104-105) */
    pmu->delay_us(pmu->ctx, 100000);
    rec(t, SMCP_STEP_DELAY, 0, 100000, 0);
    return 0;
}
