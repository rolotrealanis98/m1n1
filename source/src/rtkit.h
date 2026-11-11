/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef RTKIT_H
#define RTKIT_H

#include "asc.h"
#include "dart.h"
#include "iova.h"
#include "sart.h"
#include "types.h"

typedef struct rtkit_dev rtkit_dev_t;

struct rtkit_message {
    u8 ep;
    u64 msg;
};

struct rtkit_buffer {
    void *bfr;
    u64 dva;
    size_t sz;
};

rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, dart_dev_t *dart,
                        iova_domain_t *dart_iovad, sart_dev_t *sart, bool sram);
bool rtkit_quiesce(rtkit_dev_t *rtk);
bool rtkit_sleep(rtkit_dev_t *rtk);
void rtkit_free(rtkit_dev_t *rtk);

bool rtkit_start_ep(rtkit_dev_t *rtk, u8 ep);
bool rtkit_boot(rtkit_dev_t *rtk);
/* Opt-in SRAM-only MTP policy: bounds, no memory logging, nonblocking receive.
 * CPU is already started by caller after checking the exact platform geometry. */
void rtkit_mtp_mode(rtkit_dev_t *rtk, u64 sram_base, size_t sram_size);
/* rtkit_boot()'s HELLO and endpoint-map wait in microseconds; 0 restores the 1 s default */
void rtkit_set_boot_timeout(u64 us);
/* r3: bound for the power-state waits in microseconds; 0 (default) = unbounded, as before */
void rtkit_set_power_timeout(u64 us);
/* r3.5: bits of the bounded waits that timed out since the last call; reading clears them */
#define RTKIT_TIMEOUT_HELLO           (1u << 0) /* rtkit_boot: HELLO (boot timeout) */
#define RTKIT_TIMEOUT_EPMAP           (1u << 1) /* rtkit_boot: endpoint map (boot timeout) */
#define RTKIT_TIMEOUT_IOP_POWER_ON    (1u << 2) /* rtkit_boot: IOP power ON (power timeout) */
#define RTKIT_TIMEOUT_AP_QUIESCE      (1u << 3) /* sleep/quiesce: AP power QUIESCED (power timeout) */
#define RTKIT_TIMEOUT_IOP_POWER_STATE (1u << 4) /* sleep/quiesce: IOP target state (power timeout) */
u32 rtkit_take_timeouts(void);

bool rtkit_can_recv(rtkit_dev_t *rtk);

int rtkit_recv(rtkit_dev_t *rtk, struct rtkit_message *msg);
bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg);

bool rtkit_map(rtkit_dev_t *rtk, void *phys, size_t sz, u64 *dva);
bool rtkit_unmap(rtkit_dev_t *rtk, u64 dva, size_t sz);

bool rtkit_alloc_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr, size_t sz);
bool rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr);

#endif
