/* SPDX-License-Identifier: MIT */
/*
 * SMCP resident glue: brings up the SMC endpoint (same calls as m1n1's smc_init), runs the SMCP
 * core from the hypervisor tick, serves the trapped SMCP register page to the guest, drives the
 * GED interrupt, and on a guest SYSTEM_OFF/RESET (only with the power-actions flag) runs the
 * Linux macsmc-reboot sequence. Design and evidence: platform/power-audio-20261111/.
 * Trap and tick already hold bhl (hv_exc_entry); configure is pre-guest only.
 */
#include "hv_win_smcp.h"
#include "smcp/core/smcp_core.h"
#include "smcp/core/smcp_power.h"

#include "adt.h"
#include "asc.h"
#include "hv/hv.h"
#include "iodev.h"
#include "hv/win/hv_win.h"
#include "hv/win/hv_win_storage.h"
#include "rtkit.h"
#include "smp.h"
#include "spmi.h"
#include "utils.h"

#define SMC_ENDPOINT 0x20
#define SMC_MSG_INITIALIZE 0x17
/* t6050-die0.dtsi:47-52: iop-smc-nub region-base 0x28de00000 +0x140000, the SRAM the SMC's
 * shared buffer lives in (t1b readout: shmem 0x28de90000). */
#define SMC_SRAM_BASE 0x28de00000ull
#define SMC_SRAM_SIZE 0x140000ull

static smcp core;
static asc_dev_t *smc_asc;
static rtkit_dev_t *smc_rtk;
static volatile u8 *shmem;
static bool attempted, exposed, retired, power_done;
static u32 rec_flags;
static u32 guest_reads, guest_writes, irq_raises; /* evidence that Windows evaluated the ACPI methods */

/* GED line is edge-triggered (SSDT): one rising edge per new enabled event (smcp_irq_edges). The
 * vGIC latches pending on the rising edge of an SPI the guest configured as edge (hv_vgic3.c
 * vgic_set_level), so a pulse is high-then-low under the same lock. */
static u32 edges_seen;
static void update_irq(void)
{
    u32 e = smcp_irq_edges(&core);
    if (!exposed || retired || e == edges_seen)
        return;
    edges_seen = e;
    irq_raises++;
    hv_vgic3_set_spi_level(SMCP_GSIV, true);
    hv_vgic3_set_spi_level(SMCP_GSIV, false);
}

bool hv_win_smcp_claims_spi(u64 gsiv) { return attempted && gsiv == SMCP_GSIV; }

/* ---- transport ---- */
static int t_send(void *ctx, uint64_t msg)
{
    (void)ctx;
    struct rtkit_message m = {.ep = SMC_ENDPOINT, .msg = msg};
    return rtkit_send(smc_rtk, &m) ? 0 : -1;
}

static int t_recv(void *ctx, uint64_t *msg)
{
    (void)ctx;
    struct rtkit_message m;
    int r = rtkit_recv(smc_rtk, &m);
    if (r <= 0)
        return r;
    if (m.ep != SMC_ENDPOINT)
        return 0; /* foreign app endpoint: dropped (m1n1 smc.c does the same) */
    *msg = m.msg;
    return 1;
}

static void t_shr(void *ctx, uint8_t *dst, unsigned len)
{
    (void)ctx;
    for (unsigned i = 0; i < len; i++)
        dst[i] = shmem[i];
}

static void t_shw(void *ctx, const uint8_t *src, unsigned len)
{
    (void)ctx;
    for (unsigned i = 0; i < len; i++)
        shmem[i] = src[i];
}

static uint64_t t_now(void *ctx)
{
    (void)ctx;
    return ticks_to_usecs(get_ticks());
}

/* ---- PMU (primary PMU, nub-spmi0 USID 0xe) for the power sequencer ---- */
static spmi_dev_t *pmu_spmi;
static int p_read8(void *ctx, uint16_t reg, uint8_t *val)
{
    (void)ctx;
    return pmu_spmi && spmi_ext_read_long(pmu_spmi, SMCP_PMU_USID, reg, val, 1) >= 0 ? 0 : -1;
}
static int p_write8(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx;
    return pmu_spmi && spmi_ext_write_long(pmu_spmi, SMCP_PMU_USID, reg, &val, 1) >= 0 ? 0 : -1;
}
static void p_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    udelay(us);
}

/* ---- the trapped register page ---- */
static bool access_page(struct exc_info *ctx, u64 address, u64 *value, bool write, int width)
{
    (void)ctx;
    if (address < SMCP_REG_BASE || address - SMCP_REG_BASE >= SMCP_REG_GRANULE || !value)
        return false;
    u32 off = (u32)(address - SMCP_REG_BASE);
    if (!write) {
        *value = 0;
        if (width == 4)
            value[1] = 0;
    }
    if (write)
        guest_writes++;
    else
        guest_reads++;
    if (exposed && width == 2 && !(off & 3)) {
        if (write)
            smcp_reg_write(&core, off, (u32)*value);
        else
            *value = smcp_reg_read(&core, off);
    }
    update_irq();
    return true; /* everything else RAZ/WI */
}

static bool smc_bringup(void)
{
    smc_asc = asc_init("/arm-io/smc");
    if (!smc_asc)
        return false;
    smc_rtk = rtkit_init("smc", smc_asc, NULL, NULL, NULL, true);
    if (!smc_rtk)
        return false;
    if (!rtkit_boot(smc_rtk) || !rtkit_start_ep(smc_rtk, SMC_ENDPOINT))
        return false;
    struct rtkit_message m = {.ep = SMC_ENDPOINT, .msg = SMC_MSG_INITIALIZE}; /* id 0, as smc_init */
    if (!rtkit_send(smc_rtk, &m))
        return false;
    u64 deadline = timeout_calculate(500000); /* macsmc.c:522-525 */
    while (!timeout_expired(deadline)) {
        int r = rtkit_recv(smc_rtk, &m);
        if (r < 0)
            return false;
        if (r == 0 || m.ep != SMC_ENDPOINT)
            continue;
        if (m.msg < SMC_SRAM_BASE || m.msg + 0x1000 > SMC_SRAM_BASE + SMC_SRAM_SIZE) {
            printf("HV/win SMCP: SMC shared buffer 0x%lx outside the SMC SRAM; refused\n", m.msg);
            return false;
        }
        shmem = (volatile u8 *)m.msg;
        return true;
    }
    printf("HV/win SMCP: no INITIALIZE reply\n");
    return false;
}

extern bool hv_started_cpus[];

s64 hv_win_smcp_configure(u64 version, u64 flags)
{
    if (attempted || version != SMCP_VERSION || (flags & ~(u64)SMCP_REC_F_POWER_ACTIONS))
        return -1;
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return -1;
    if (!hv_win_vgic_enabled() || hv_win_pl011_gsiv() == SMCP_GSIV || hv_win_storage_claims_spi(SMCP_GSIV) ||
        !hv_vgic3_spi_unassigned(SMCP_GSIV) || hv_pt_walk(SMCP_REG_BASE))
        return -1;
    attempted = true;
    rec_flags = (u32)flags;
    if (hv_map_hook(SMCP_REG_BASE, access_page, SMCP_REG_GRANULE) < 0)
        return -1;
    if (!smc_bringup()) {
        printf("HV/win SMCP: SMC bring-up failed; the block reads as zero (no SMCP magic)\n");
        return -2; /* hook stays: the guest sees a zero page and hides every device */
    }
    smcp_ops ops = {NULL, t_send, t_recv, t_shr, t_shw, t_now};
    smcp_init(&core, &ops, rec_flags);
    /* run until the first complete refresh so ACPI enumeration sees real data (bounded 3 s) */
    u64 deadline = timeout_calculate(3000000);
    while (!timeout_expired(deadline) && !smcp_failed(&core) &&
           !(smcp_reg_read(&core, SMCP_R_STATUS) & SMCP_ST_VALID)) {
        smcp_tick(&core);
        udelay(200);
    }
    exposed = true;
    update_irq();
    printf("HV/win SMCP: status 0x%x seq %u errors %u timeouts %u batt %u%% %u mV psr %u lid %u flags 0x%x\n",
           smcp_reg_read(&core, SMCP_R_STATUS), smcp_reg_read(&core, SMCP_R_SEQ),
           smcp_reg_read(&core, SMCP_R_ERR_COUNT), smcp_reg_read(&core, SMCP_R_TIMEOUTS),
           smcp_reg_read(&core, SMCP_R_BATT_PERCENT), smcp_reg_read(&core, SMCP_R_BST_VOLTAGE),
           smcp_reg_read(&core, SMCP_R_PSR), smcp_reg_read(&core, SMCP_R_LID), rec_flags);
    return smcp_failed(&core) ? -3 : 0;
}

#define SMCP_STAT_PERIOD_US 30000000u /* one console line every 30 s while the guest runs */

void hv_win_smcp_tick(void)
{
    static u64 next_stat;
    if (!exposed || retired || smp_id() != boot_cpu_idx)
        return;
    smcp_tick(&core);
    update_irq();
    u64 t = ticks_to_usecs(get_ticks());
    if (t >= next_stat) {
        if (next_stat)
            hv_win_smcp_stat();
        next_stat = t + SMCP_STAT_PERIOD_US;
    }
}

void hv_win_smcp_retire(void)
{
    if (!attempted)
        return;
    retired = true;
    update_irq();
}

s64 hv_win_smcp_stat(void)
{
    if (!exposed)
        return -1;
    printf("HV/win SMCP: status 0x%x seq %u pending 0x%x enable 0x%x pwrb %u errors %u (last %08x) timeouts %u "
           "notif %u bst %x/%u/%u/%u psr %u lid %u tcmb %u guest r/w %u/%u irq %u\n",
           smcp_reg_read(&core, SMCP_R_STATUS), smcp_reg_read(&core, SMCP_R_SEQ),
           smcp_reg_read(&core, SMCP_R_EVT_PENDING), smcp_reg_read(&core, SMCP_R_EVT_ENABLE),
           smcp_reg_read(&core, SMCP_R_PWRB_COUNT), smcp_reg_read(&core, SMCP_R_ERR_COUNT),
           smcp_reg_read(&core, SMCP_R_LAST_ERR), smcp_reg_read(&core, SMCP_R_TIMEOUTS), core.notifications,
           smcp_reg_read(&core, SMCP_R_BST_STATE), smcp_reg_read(&core, SMCP_R_BST_RATE),
           smcp_reg_read(&core, SMCP_R_BST_REMAIN), smcp_reg_read(&core, SMCP_R_BST_VOLTAGE),
           smcp_reg_read(&core, SMCP_R_PSR), smcp_reg_read(&core, SMCP_R_LID), smcp_reg_read(&core, SMCP_R_TEMP0),
           guest_reads, guest_writes, irq_raises);
    return smcp_reg_read(&core, SMCP_R_STATUS);
}

static const char *step_name(u8 k)
{
    return k == SMCP_STEP_SMC_WRITE ? "smc-write" : k == SMCP_STEP_PMU_READ ? "pmu-read"
           : k == SMCP_STEP_PMU_WRITE ? "pmu-write" : "delay";
}

/* action: SMCP_POWER_OFF / SMCP_POWER_RESTART. Only after every vCPU left the guest. */
s64 hv_win_smcp_power(u64 action)
{
    if (!exposed || smcp_failed(&core) || power_done)
        return -1;
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return -1;
    power_done = true;
    retired = true;
    update_irq();
    pmu_spmi = spmi_init("/arm-io/nub-spmi0");
    smcp_pmu_ops pmu = {NULL, p_read8, p_write8, p_delay};
    smcp_power_trace t;
    printf("HV/win SMCP: %s via SMC (macsmc-reboot sequence)\n", action == SMCP_POWER_OFF ? "power off" : "restart");
    iodev_console_flush();
    int r = smcp_power_run(&core, &pmu, (int)action, &t);
    for (unsigned i = 0; i < t.n; i++)
        printf("HV/win SMCP: step %u %s %08x = %08x -> %d\n", i, step_name(t.step[i].kind), t.step[i].key_or_reg,
               t.step[i].value, t.step[i].result);
    printf("HV/win SMCP: still running after the power command (%d)\n", r);
    return r;
}

void hv_win_smcp_after_exit(void)
{
    if (exposed)
        hv_win_smcp_stat(); /* one line at every guest exit: the run's evidence */
    if (!(rec_flags & SMCP_REC_F_POWER_ACTIONS))
        return;
    if (hv_win.exit_reason == HV_WIN_EXIT_SYSTEM_OFF)
        hv_win_smcp_power(SMCP_POWER_OFF);
    else if (hv_win.exit_reason == HV_WIN_EXIT_SYSTEM_RESET)
        hv_win_smcp_power(SMCP_POWER_RESTART);
}
