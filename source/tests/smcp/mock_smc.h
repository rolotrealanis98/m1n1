/* SPDX-License-Identifier: MIT */
/* Mock SMC endpoint for host tests: key store, latency, drops, errors, notifications, write log. */
#ifndef MOCK_SMC_H
#define MOCK_SMC_H
#include <stdbool.h>
#include <stdint.h>
#include "../../src/smcp/core/smcp_core.h"
#include "../../src/smcp/core/smcp_power.h"

#define MOCK_MAXKEYS 96
#define MOCK_MAXQ 64
#define MOCK_MAXLOG 4096

typedef struct mock_key {
    uint32_t key;
    uint32_t type;
    uint8_t size;
    uint8_t b[32];
    bool exists;
    int drop;          /* >0: the next N reads get no reply */
    uint32_t late_us;  /* reply this late (for timeout tests) */
    uint8_t err;       /* non-zero: reads answer with this result */
    uint32_t reads;
} mock_key;

typedef struct mock_msg { uint64_t due; uint64_t msg; } mock_msg;

typedef struct mock_log { uint64_t msg; uint8_t data[8]; uint8_t len; } mock_log;

typedef struct mock_smc {
    uint64_t t;                  /* mock clock, us */
    uint32_t latency_us;         /* default reply latency */
    mock_key k[MOCK_MAXKEYS];
    int nk;
    mock_msg q[MOCK_MAXQ];
    int nq;
    uint8_t shmem[4096];
    mock_log log[MOCK_MAXLOG];   /* every message the client sent */
    int nlog;
    int sends, recvs, recv_calls;
    bool dead;                   /* recv returns -1 */
    bool send_fail;
    int busy_left;               /* nightly-2: the next N sends answer SMCP_SEND_BUSY (mailbox full) */
    int busy_answers;
    uint8_t write_result;        /* result for WRITE_KEY */
    bool writes_no_reply;
    /* PMU */
    uint8_t pmu[0x10000];
    int pmu_reads, pmu_writes;
    bool pmu_fail;
    uint64_t delayed_us;
} mock_smc;

void mock_init(mock_smc *m);
mock_key *mock_set(mock_smc *m, const char *name, const char *type, uint8_t size, const char *hex);
mock_key *mock_get(mock_smc *m, const char *name);
void mock_set_u(mock_smc *m, const char *name, uint64_t v); /* little-endian into existing key */
void mock_remove(mock_smc *m, const char *name);
void mock_notify(mock_smc *m, uint32_t ev);
void mock_load_t1b(mock_smc *m);    /* raw bytes of runs/20261110-145557-t1b-gpu pass A */
smcp_ops mock_ops(mock_smc *m);
smcp_pmu_ops mock_pmu_ops(mock_smc *m);
int mock_count_type(const mock_smc *m, uint8_t type);
#endif
