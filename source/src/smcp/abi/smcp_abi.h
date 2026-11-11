/* SPDX-License-Identifier: MIT */
/*
 * SMCP v1: the mediated SMC platform-power register block.
 *
 * The hypervisor owns the SMC (RTKit endpoint 0x20) and the PMU. It polls a fixed list of SMC
 * keys that Linux reads on this machine, decodes them the way Linux's macsmc-power /
 * macsmc-input / macsmc-hwmon do, and publishes the results here. The guest never talks to the
 * SMC. ACPI methods (acpi/smcp_ssdt.asl) read these registers; Windows' inbox drivers (CmBatt,
 * the ACPI lid / power button / AC adapter / thermal zone support) consume the methods.
 *
 * One 16 KiB stage-2 granule, unmapped and trapped. Only 32-bit aligned accesses inside the
 * first 0x200 bytes mean anything; everything else reads as zero and ignores writes.
 * All registers are little-endian u32. RO unless marked.
 */
#ifndef SMCP_ABI_H
#define SMCP_ABI_H

#define SMCP_REG_BASE    0xf38110000ull   /* guest IPA, inside the hand-off window */
#define SMCP_REG_GRANULE 0x4000u          /* trapped size */
#define SMCP_REG_BYTES   0x1000u          /* size declared in _CRS */
#define SMCP_GSIV        72u              /* GIC SPI 40, level, active high (GED interrupt) */
#define SMCP_MAGIC       0x50434d53u      /* "SMCP" */
#define SMCP_VERSION     1u

/* /chosen/m5w,smcp record (launcher -> resident -> Mu), little-endian */
#define SMCP_PROP        "m5w,smcp"
#define SMCP_RECORD_SIZE 24u /* u64 base, u64 bytes, u32 gsiv, u32 flags */
#define SMCP_REC_F_POWER_ACTIONS 0x1u /* PSCI SYSTEM_OFF/RESET end in a real SMC power-off/restart */

/* register offsets */
#define SMCP_R_MAGIC        0x000
#define SMCP_R_VERSION      0x004
#define SMCP_R_STATUS       0x008
#define SMCP_R_SEQ          0x00c /* full slow refreshes completed */
#define SMCP_R_EVT_PENDING  0x010
#define SMCP_R_EVT_ACK      0x014 /* WO, write 1 to clear */
#define SMCP_R_EVT_ENABLE   0x018 /* RW */
#define SMCP_R_PWRB_COUNT   0x01c /* power button presses seen */
#define SMCP_R_BST_STATE    0x020 /* ACPI _BST state bits */
#define SMCP_R_BST_RATE     0x024 /* mA, |B0AC|; 0xffffffff unknown */
#define SMCP_R_BST_REMAIN   0x028 /* mAh, B0RM */
#define SMCP_R_BST_VOLTAGE  0x02c /* mV, B0AV */
#define SMCP_R_BIX_DESIGN   0x030 /* mAh, B0DC */
#define SMCP_R_BIX_FULL     0x034 /* mAh, B0FC */
#define SMCP_R_BIX_VOLTAGE  0x038 /* mV, BNCB * 3800 (Linux MACSMC_NOMINAL_CELL_VOLTAGE_MV) */
#define SMCP_R_BIX_CYCLES   0x03c /* B0CT */
#define SMCP_R_BATT_PERCENT 0x040 /* BUIC */
#define SMCP_R_BATT_TEMP    0x044 /* 0.1 K, B0AT */
#define SMCP_R_BATT_TTE     0x048 /* minutes, B0TE; 0xffffffff n/a */
#define SMCP_R_BATT_TTF     0x04c /* minutes, B0TF; 0xffffffff n/a */
#define SMCP_R_PSR          0x050 /* 1 = AC online (CHIS != 0) */
#define SMCP_R_LID          0x054 /* 1 = open (MSLD == 0) */
#define SMCP_R_CHG_STATUS   0x058 /* SMCP_CHG_* (Linux power_supply status) */
#define SMCP_R_CAP_LEVEL    0x05c /* SMCP_CAP_* (Linux capacity level) */
#define SMCP_R_BATT_HEALTH  0x060 /* 0 good, 1 dead (BBAD) */
#define SMCP_R_AC_POWER     0x064 /* mW, ACPW */
#define SMCP_R_TEMP0        0x080 /* 0.1 K; 0 = absent. TCMb TB0T TCHP TW0P TH0x */
#define SMCP_NTEMPS         5
#define SMCP_R_FAN0_RPM     0x0a0
#define SMCP_R_FAN1_RPM     0x0a4
#define SMCP_R_FAN0_MAX     0x0a8
#define SMCP_R_FAN1_MAX     0x0ac
#define SMCP_R_ERR_COUNT    0x100 /* SMC command failures (incl. absent optional keys at first read) */
#define SMCP_R_LAST_ERR     0x104 /* key of the last failure */
#define SMCP_R_TIMEOUTS     0x108
#define SMCP_R_REG_END      0x200

/* STATUS bits */
#define SMCP_ST_BACKEND  0x001 /* SMC endpoint initialised, polling */
#define SMCP_ST_VALID    0x002 /* at least one complete slow refresh */
#define SMCP_ST_BATT     0x004 /* battery present (B0AV readable and a status was derived) */
#define SMCP_ST_AC       0x008 /* CHIS key present */
#define SMCP_ST_LIDKEY   0x010 /* MSLD key present */
#define SMCP_ST_PWRBKEY  0x020 /* bHLD key present */
#define SMCP_ST_STALE    0x040 /* no successful slow refresh in SMCP_STALE_MS */
#define SMCP_ST_FAILED   0x080 /* backend stopped (SMC dead or too many timeouts) */
#define SMCP_ST_POWERACT 0x100 /* SYSTEM_OFF/RESET will power off/restart the machine */

/* EVT bits (GED _EVT reads PENDING & ENABLE, notifies, writes ACK) */
#define SMCP_EV_BATT  0x01 /* _BST/_BIX content changed */
#define SMCP_EV_AC    0x02 /* _PSR changed */
#define SMCP_EV_LID   0x04 /* _LID changed */
#define SMCP_EV_PWRB  0x08 /* power button pressed */
#define SMCP_EV_THERM 0x10 /* reserved (thermal zones are polled through _TZP in v1) */
#define SMCP_EV_ALL   0x1f

/* ACPI _BST state */
#define SMCP_BST_DISCHARGING 0x1
#define SMCP_BST_CHARGING    0x2
#define SMCP_BST_CRITICAL    0x4

/* Linux enum power_supply_status values (include/linux/power_supply.h) */
#define SMCP_CHG_UNKNOWN      0
#define SMCP_CHG_CHARGING     1
#define SMCP_CHG_DISCHARGING  2
#define SMCP_CHG_NOT_CHARGING 3
#define SMCP_CHG_FULL         4

/* Linux enum power_supply_capacity_level values */
#define SMCP_CAP_UNKNOWN  0
#define SMCP_CAP_CRITICAL 1
#define SMCP_CAP_LOW      2
#define SMCP_CAP_NORMAL   3
#define SMCP_CAP_HIGH     4
#define SMCP_CAP_FULL     5

#endif
