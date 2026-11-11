/* SPDX-License-Identifier: MIT */
/*
 * nightly-2 (review finding 2): resident-side refusals for the features the launcher already refuses in a
 * disk-writing session. Pure decisions (host-tested in tests/nightly2); the glue calls them BEFORE any
 * hardware write or any state change:
 *   - SMC power actions (MBSE/NTAP/PMU writes): never configured once a disk path was used, never run if a disk
 *     path was used this session (refused before power_done/retire and before the first SMC write);
 *   - the M5WN device (fake gateway or N1 backend): never configured once a disk path was used.
 * The reverse direction (no disk path once these are armed) is hv_win_storage_smp_gate() in hv_win.c.
 * "Disk path used" = storage admitted by the gate, native storage tried/selected, or S1 PREP attempted.
 */
#ifndef HV_WIN_FEATURE_POLICY_H
#define HV_WIN_FEATURE_POLICY_H

#define FP_SMCP_F_POWER_ACTIONS 1u /* = SMCP_REC_F_POWER_ACTIONS */

static inline bool fp_smcp_configure_ok(unsigned long long flags, bool disk_used)
{
    return !((flags & FP_SMCP_F_POWER_ACTIONS) && disk_used);
}

static inline bool fp_smcp_power_ok(bool disk_used)
{
    return !disk_used;
}

static inline bool fp_m5wn_configure_ok(bool disk_used)
{
    return !disk_used;
}

#endif
