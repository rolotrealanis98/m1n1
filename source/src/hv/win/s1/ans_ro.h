/* SPDX-License-Identifier: MIT */
/* r3 native read-only ANS driver for T6050 (J716s) internal storage.
 * Register sequence is the frozen m1n1 src/nvme.c one that stage 1 runs on every boot
 * and that the 2026-11-09 proxy probe reproduced on hardware (single base = ADT reg[9],
 * which mirrors the NVMMU bank). Additions: bounded waits, freeze-on-failure, a command
 * allowlist (Identify, Create/Delete I/O CQ/SQ, Read), static private queues/bounce.
 * No write, flush, format, features, namespace or firmware command exists here. */
#ifndef S1_ANS_RO_H
#define S1_ANS_RO_H
#include "types.h"

#define ANS_RO_BLOCK 4096u

enum ans_ro_state {
    ANS_RO_OFF = 0,      /* never touched */
    ANS_RO_REFUSED,      /* admission refused before any write */
    ANS_RO_LIVE,         /* RTKit up, controller enabled, I/O queue live */
    ANS_RO_FROZEN,       /* uncertain DMA/ownership: no further access, nothing freed */
    ANS_RO_STOPPED,      /* clean stage-1-style shutdown completed */
};

enum ans_ro_reason {
    ANS_RO_OK = 0,
    ANS_RO_E_ADT, ANS_RO_E_GEOMETRY, ANS_RO_E_RUNNING, ANS_RO_E_SART_SLOTS, ANS_RO_E_ALLOC,
    ANS_RO_E_RTKIT, ANS_RO_E_BOOT_STATUS, ANS_RO_E_DISABLE, ANS_RO_E_ENABLE, ANS_RO_E_CMD_TIMEOUT,
    ANS_RO_E_CMD_TAG, ANS_RO_E_TCB, ANS_RO_E_CMD_STATUS, ANS_RO_E_IDENTIFY, ANS_RO_E_NOT_ALLOWED,
    ANS_RO_E_CRASH, ANS_RO_E_STATE, ANS_RO_E_SHUTDOWN, ANS_RO_E_ARGS,
};

struct ans_ro_stats {
    u64 commands, reads, read_errors, polls, mailbox_msgs, poll_iters_max;
    u32 state, reason, sart_protected, sart_free;
    u64 nsze;
    u32 lbads, rtkit_timeouts;
    /* r3.2: capacity is pinned, not from Identify NS. Identify Controller is advisory. */
    u64 identify_nsze;      /* NSZE the controller reported on Identify Namespace path (diagnostic) */
    u32 identity_confirmed; /* Identify Controller model+fw matched (advisory, not required) */
};

/* Bring ANS up from the stopped stage-1 handoff. One shot. Returns false on refusal or
 * freeze; ans_ro_state() distinguishes the two. */
bool ans_ro_init(void);
/* Read exactly one 4 KiB block of NSID 1 into the private bounce and copy it to out.
 * Returns false on error; uncertain DMA freezes the driver. */
bool ans_ro_read_block(u64 lba, void *out);
/* Namespace geometry from Identify Namespace (valid after a successful init). */
u64 ans_ro_nsze(void);
u32 ans_ro_lbads(void);
/* Drain the RTKit mailbox (syslog acks etc). Bounded; safe to call from the EL2 tick. */
void ans_ro_poll(void);
/* Stage-1 shutdown sequence (Delete SQ/CQ, SHN, CC.EN=0, RTKit sleep, PMGR reset).
 * No-op unless LIVE; never runs on a frozen driver. */
bool ans_ro_shutdown(void);
enum ans_ro_state ans_ro_state(void);
const struct ans_ro_stats *ans_ro_stats(void);
/* True for the physical AIC lines owned by ANS (mailbox 1347..1350, NVMe 2338). */
bool ans_ro_owns_aic_line(u32 line);
/* Private DMA memory (queues, TCBs, bounce): must lie inside the resident exclusion. */
void ans_ro_dma_region(u64 *start, u64 *size);
/* r3.1 R2: zero the private bounce (after GPT verification). */
void ans_ro_scrub(void);
#ifdef ANS_RO_HOST_TEST
int ans_ro_test_submit(bool admin, u8 opcode);
#endif
#endif
