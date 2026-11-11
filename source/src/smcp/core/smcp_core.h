/* SPDX-License-Identifier: MIT */
/*
 * SMCP core: a non-blocking SMC client, a key poller and the SMCP v1 register file.
 * Freestanding (stdint/stdbool/stddef only, no allocation, no floating point): the same file is
 * compiled into the resident, the host tests and the QEMU twin device.
 *
 * Behaviour reference (not copied): Linux drivers/mfd/macsmc.c, drivers/power/supply/
 * macsmc-power.c, drivers/input/misc/macsmc-input.c, drivers/power/reset/macsmc-reboot.c
 * (all "GPL-2.0-only OR MIT"), pinned tree a0f2f30c. Citations are in LINUX-MAP.md.
 */
#ifndef SMCP_CORE_H
#define SMCP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../abi/smcp_abi.h"

#define SMCP_KEY(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* SMC endpoint 0x20 message types (macsmc.c:34-40) */
#define SMCP_MSG_READ_KEY     0x10
#define SMCP_MSG_WRITE_KEY    0x11
#define SMCP_MSG_GET_KEY_INFO 0x13
#define SMCP_MSG_NOTIFICATION 0x18

#define SMCP_TIMEOUT_US        500000u   /* macsmc.c:49 SMC_TIMEOUT_MS */
#define SMCP_ATOMIC_TIMEOUT_US 5000000u  /* macsmc.c:51 SMC_ATOMIC_TIMEOUT_MS */
#define SMCP_QUARANTINE_US     1000000u  /* after a timeout: let a late reply land before reuse */
#define SMCP_SLOW_PERIOD_US    2000000u
#define SMCP_FAST_PERIOD_US    50000u
#define SMCP_STALE_US          10000000u
#define SMCP_MAX_TIMEOUTS      16u       /* consecutive: then FAILED, no more commands */
#define SMCP_RECV_BUDGET       8u        /* messages drained per tick */
#define SMCP_REPULSE_US        100000u   /* re-pulse the GED edge while enabled events stay unacked */
/* nightly-2 (review finding 3): ops.send may answer BUSY (mailbox full, nothing sent); the step is retried on a
 * later tick, never polled. A mailbox full for this long counts as one failed send (asc_send's old 200 ms poll). */
#define SMCP_SEND_BUSY         1
#define SMCP_MAX_BUSY_US       200000u

/* transport, supplied by the resident glue / the mock */
typedef struct smcp_ops {
    void *ctx;
    int (*send)(void *ctx, uint64_t msg);                 /* 0 sent, SMCP_SEND_BUSY full (retry later), <0 failed */
    int (*recv)(void *ctx, uint64_t *msg);                /* 1 message, 0 none, <0 dead */
    void (*shmem_read)(void *ctx, uint8_t *dst, unsigned len);
    void (*shmem_write)(void *ctx, const uint8_t *src, unsigned len);
    uint64_t (*now_us)(void *ctx);
} smcp_ops;

/* key slots: fixed table in smcp_core.c, every key one Linux reads on this machine */
enum smcp_slot {
    K_BUIC, K_B0AV, K_B0AC, K_B0RM, K_B0FC, K_B0DC, K_B0CT, K_B0AT, K_B0TE, K_B0TF,
    K_BNCB, K_BBAD, K_BSFC, K_BCF0,
    K_CHIS, K_CHCE, K_CHCC, K_CHSC, K_CH0R, K_CHNC, K_ACi, K_CHWA, K_CHLS, K_ACSt, K_ACPW,
    K_TCMb, K_TB0T, K_TCHP, K_TW0P, K_TH0x, K_F0Ac, K_F1Ac, K_F0Mx, K_F1Mx,
    K_MSLD, K_bHLD,
    SMCP_NKEYS
};

typedef struct smcp_val {
    uint8_t b[8];
    uint8_t len;       /* size reported by key info */
    bool present;      /* key info succeeded with an acceptable size */
    bool ok;           /* last read succeeded */
    bool ever;         /* at least one read succeeded */
} smcp_val;

enum smcp_phase { SMCP_PH_INFO, SMCP_PH_RUN, SMCP_PH_FAILED };
enum smcp_kind { SMCP_K_NONE, SMCP_K_INFO, SMCP_K_READ, SMCP_K_WRITE };

typedef struct smcp {
    smcp_ops ops;
    /* client: at most one command outstanding */
    uint8_t msgid;
    bool busy;
    uint8_t cur_id;
    uint8_t cur_kind;
    uint8_t cur_len;
    int cur_slot;
    uint32_t cur_key;
    uint64_t t_sent;
    uint64_t timeout_us;
    uint64_t quarantine_until;
    int last_result;   /* of the last completed command: SMC result byte, -1 timeout, -2 send */
    uint64_t busy_since;   /* nightly-2: first BUSY send of the current step (0 = none) */
    uint32_t send_busy;    /* nightly-2: BUSY answers (statistics) */
    bool last_send_busy;   /* nightly-2: the last send_cmd() returned false because of BUSY */
    uint8_t last_reply_len;
    uint32_t last_reply_data;
    /* schedule */
    uint8_t phase;
    int info_idx;
    int slow_idx;      /* -1 = no slow round in progress */
    int fast_idx;      /* -1 = no fast round in progress */
    uint64_t next_slow, next_fast, last_slow_done;
    bool fw_ge_27;     /* BCF0 is 1 byte (macsmc-power.c:1108-1121) */
    bool has_chwa, has_chls;
    smcp_val v[SMCP_NKEYS];
    /* outputs */
    uint32_t regs[SMCP_R_REG_END / 4];
    uint32_t evt_pending, evt_enable;
    uint32_t irq_edges;    /* rising edges for the edge-triggered GED line (see smcp_irq_edges) */
    uint64_t last_edge_us;
    bool prev_valid;
    uint32_t prev_bst_state, prev_percent, prev_psr, prev_lid, prev_full, prev_bhld;
    uint32_t err_count, last_err_key, timeouts, consecutive_timeouts;
    uint32_t notifications, last_notification;
    bool flag_power_actions;
} smcp;

void smcp_init(smcp *s, const smcp_ops *ops, uint32_t record_flags);
void smcp_tick(smcp *s);                       /* non-blocking; call often (resident: 1 kHz) */
uint32_t smcp_reg_read(smcp *s, uint32_t off); /* off: byte offset, 4-aligned */
void smcp_reg_write(smcp *s, uint32_t off, uint32_t value);
bool smcp_irq(const smcp *s);
/* The GED interrupt is edge-triggered: a level line that stays high until _EVT runs (passive level)
 * storms a single vCPU (twin, runs/i1). The count goes up whenever an enabled event becomes pending
 * that was not pending before, or an already-pending event is enabled; the glue pulses the line on
 * every change of this count. While enabled events stay pending and unacknowledged the count is
 * bumped again every SMCP_REPULSE_US (an edge before Windows connected the interrupt was lost in
 * the twin, runs/e-i1). */
uint32_t smcp_irq_edges(const smcp *s);
bool smcp_failed(const smcp *s);

/* decoders (exported for tests) */
int32_t smcp_flt_to_milli(uint32_t bits);      /* IEEE754 binary32 -> value * 1000, truncated, saturating */
int smcp_batt_status(const smcp *s);           /* SMCP_CHG_*; port of macsmc_battery_get_status */
int smcp_cap_level(const smcp *s);             /* SMCP_CAP_*; port of macsmc_battery_get_capacity_level */

/* Blocking command for the power sequencer (guest stopped, nothing else uses the client). */
int smcp_cmd_blocking(smcp *s, uint8_t kind, uint32_t key, const uint8_t *wbuf, uint8_t len,
                      uint64_t timeout_us);

#endif
