/* SPDX-License-Identifier: MIT */
#ifndef N1PT_MODEL_H
#define N1PT_MODEL_H
#include "n1pt_host.h"

struct model {
    u64 now, reads, writes;
    bool link, l2, released, fw_running, never_link, trip_storage_on_perst, verbose;
    u32 rom_stage, gp13, gp13_log[8], ngp13;
    u32 perst_asserts, link_ups, stops, tunables, smc_sessions, frees;
    u32 dart_nsid, dart_error, dart_protect, dart_enabled, dart_flushes;
    bool dart_no_four;
    u32 mmc[3];
    u32 cfg_writes[3][1024];
    bool no_820, pme_stuck;   /* 0x820 bit 2 never sets (w24c); PME_Turn_Off never completes */
    u32 dumps, drops_logged, pm_logged;
    struct { u64 a; u32 v; } trace[8192];
    u32 ntrace;
    char log[64];
    u32 nlog;
};
extern struct model M;
extern const u64 M_ECAM, M_RC, M_PHY, M_PHYCTL, M_GPIO, M_DART, M_ANS, M_NVME, M_SART, M_PS;
u64 m_port(int p);
u64 m_lane(int p);
u64 m_i2axi(int p);
u32 m_get(u64 a);
void m_set(u64 a, u32 v);
void model_reset(void);
#endif
