/* SPDX-License-Identifier: MIT */
/* r3 native read-only ANS driver for T6050 (J716s) internal storage.
 * Register sequence is the frozen m1n1 src/nvme.c one that stage 1 runs on every boot
 * and that the 2026-11-09 proxy probe reproduced on hardware (single base = ADT reg[9],
 * which mirrors the NVMMU bank). Additions: bounded waits, freeze-on-failure, a command
 * allowlist (Create/Delete I/O CQ/SQ, Read; rw: Write/Flush; r4.3: Identify removed), static private
 * queues/bounce, and (r4.3) a pre-doorbell DMA guard: every DMA buffer is its own 16 KiB-aligned
 * region (queue arrays sized like m1n1 nvme.c's), and a command whose PRP/TCB does not describe
 * exactly the expected buffer pages is never submitted (the driver freezes instead).
 * r4.4: = r3.5 (10 s RTKit waits, which-wait marker, TCB status freeze on reg[3]+0x28120 only) +
 * I/O tag 2, FLUSH instead of FUA, +0x1200/+0x1208 zeroed at teardown.
 * Only READ (always) and WRITE/FLUSH (rw contract, verified window) data commands exist; no
 * format, features, namespace, sanitize or firmware command exists here. */
#ifndef S1_ANS_RO_H
#define S1_ANS_RO_H
#include "types.h"
#include "s1_stage.h" /* r4.5: S1N_STAGE, pinned window, stage caps */

#define ANS_RO_BLOCK 4096u
/* r4: private transfer buffer = device MDTS (1 MiB) */
#define ANS_XFER_BYTES (1u << 20)

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
    ANS_RO_E_DMA_GUARD, /* r4.3 (20): FROZEN - a command failed the pre-doorbell alignment/extent guard */
    ANS_RO_E_LAYOUT,    /* r4.4 (21): REFUSED - a private DMA buffer is not 16 KiB aligned; ANS untouched
                         * (r4.3 reported this as 20 too; r3.4 review L2) */
    /* r4.5 abort rules (review C9): every one of these FREEZES the session - no further I/O. */
    ANS_RO_E_WINDOW,    /* 22: the pre-doorbell check refused a command (out of window/zone, shape) */
    ANS_RO_E_ABORT,     /* 23: an upper layer saw an out-of-window/invalid request (s1n, transfer, xfer) */
    ANS_RO_E_STATUS,    /* 24: an admin or I/O command completed with a non-zero NVMe status */
    ANS_RO_E_CAP,       /* 25: a per-session stage cap (WRITE commands/blocks, FLUSH count) would be exceeded */
    ANS_RO_E_CQE,       /* 26: duplicate or unexpected completion (CQE valid before submit / after consume) */
    ANS_RO_E_COUNTER,   /* 27: session counters inconsistent (submitted/completed/by type/canary totals) */
    ANS_RO_E_CANARY,    /* 28: W1/W2 canary procedure: read-back mismatch or GPT changed before first write */
    ANS_RO_E_REENTRY,   /* 29: concurrent or nested command submission */
    ANS_RO_E_CONTEXT,   /* 30: not boot CPU, or exception/bark proxy */
};

struct ans_ro_stats {
    u64 commands, reads, read_errors, polls, mailbox_msgs, poll_iters_max;
    u32 state, reason, sart_protected, sart_free;
    u64 nsze;
    u32 lbads;
    u32 rtkit_timeouts;     /* r4.4: RTKIT_TIMEOUT_* bits (rtkit.h): bit 0 HELLO, 1 endpoint map,
                             * 2 IOP power on, 3 AP quiesce, 4 IOP state */
    /* r4 */
    u64 writes, write_errors, flushes, flush_errors, ans_writes, window_refusals;
    u64 w_abs_min_first, w_abs_max_last;
    /* r4.1 (= r3.2): capacity pinned. r4.2+: no Identify is issued, so these stay "not attempted". */
    u64 identify_nsze;      /* always UINT64_MAX */
    u32 identity_confirmed; /* always 0 */
    /* r4.4: reg[9]+0x29120 diagnostics (never freezes) and the teardown register clear */
    u32 tcb_alias_nonzero, tcb_alias_last;
    u32 ioq_regs_cleared;   /* 1 once +0x1200/+0x1208 were zeroed after Delete SQ/CQ */
    /* r4.5 per-session counters for the verifier (taken from the validated, submitted command) */
    u64 cmd_reads, cmd_writes, cmd_flushes, cmd_admin;  /* commands submitted, by type */
    u64 io_submitted, io_completed;                     /* I/O doorbells / I/O completions consumed */
    u64 blocks_written, lowest_written, highest_written; /* successful WRITE completions only */
    u64 cqe_anomalies;      /* duplicate/unexpected CQEs seen (each one froze) */
    u64 aborts;             /* upper-layer aborts (ans_ro_abort) */
    u32 io_tags_used;       /* bit n set: I/O tag n was used (r4.5: 2 and 3 alternate) */
};

/* Bring ANS up from the stopped stage-1 handoff. One shot. Returns false on refusal or
 * freeze; ans_ro_state() distinguishes the two. */
bool ans_ro_access_ok(void);
bool ans_ro_init(void);
/* Read exactly one 4 KiB block of NSID 1 into the private bounce and copy it to out.
 * Returns false on error; uncertain DMA freezes the driver. */
bool ans_ro_read_block(u64 lba, void *out);
/* r4: one ANS READ/WRITE of <= max_blocks at absolute lba, data in the private transfer
 * buffer at `offset`. WRITE requires ans_ro_set_window(..., rw=true). */
bool ans_ro_xfer(bool write, u64 lba, u32 blocks, u64 offset);
bool ans_ro_flush(void);
bool ans_ro_read_gpt(u64 lba, void *out);
bool ans_ro_run_canary(void);
/* r4: confine every later data command to the window; rw enables WRITE/FLUSH. One shot, LIVE only,
 * after GPT verification. r4.5: the window is the compile-time pin (s1_stage.h), not an argument;
 * WRITE exists only in stages 1-3 and is further confined to the stage sub-window. */
bool ans_ro_set_window(bool rw, u32 max_blocks);
/* r4.5 abort rule: an upper layer saw an out-of-window or invalid request: freeze (E_ABORT). */
void ans_ro_abort(enum ans_ro_reason r);
/* r4.5: SART register snapshot for the stats call (which: 0 CONFIG, 1 PADDR, 2 SIZE; i < 16). Reads
 * the SART MMIO only (never ANS/NVMe/mailbox), replacing the launcher's raw read32 after select. */
u32 ans_ro_sart_reg(unsigned which, unsigned i);
void *ans_ro_xfer_buffer(size_t *capacity);
void ans_ro_canary(u64 *min_first, u64 *max_last, u64 *ans_writes, u64 *refusals);
/* Namespace geometry: the pinned J716s values (no Identify is issued; valid after init). */
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
/* r4.3: the DMA guard exists (host tests use this to select the r4.3 scenarios). */
#define ANS_RO_HAVE_DMA_GUARD 1
#define ANS_RO_DMA_BUFFERS 9
/* r4.4: r3.5 diagnostics/reason 21 + I/O tag 2 + teardown register clear (host tests key on these). */
#define ANS_RO_HAVE_R35 1
#define ANS_RO_HAVE_R44 1
#define ANS_RO_IO_TAG 2
/* r4.5: I/O tags alternate 2, 3, 2, ... so a late duplicate CQE carries the wrong tag (review M4). */
#define ANS_RO_HAVE_R45 1
#define ANS_RO_IO_TAG_ALT 3
#ifdef ANS_RO_HOST_TEST
int ans_ro_test_submit(bool admin, u8 opcode);
bool ans_ro_test_stage_write(u64 lba, u32 blocks);
/* r4.3: submit an allowed opcode with caller-chosen PRP1/PRP2/CDW10/11/12 (guard tests). */
int ans_ro_test_submit_raw(bool admin, u8 opcode, u64 prp1, u64 prp2, u32 cdw10, u32 cdw11, u32 cdw12);
/* r4.3: each private DMA buffer (index 0..8): address and size; returns false past the end. */
bool ans_ro_test_dma_buffer(unsigned i, u64 *addr, u64 *size, const char **name);
#endif
#endif
