/* SPDX-License-Identifier: MIT */
#ifndef HIDM_SERVICE_H
#define HIDM_SERVICE_H
#include "../abi/hidm_abi.h"
typedef struct hm_backend_interface { hm_info info; const hm_u8 *descriptor; } hm_backend_interface;
/* Nonblocking callback. GET writes at most *length bytes (v1.1: for GET_FEATURE
 * *length is offset+chunk <= HM_MAX_FEATURE; write min(capacity, report size) bytes
 * from the start of the report and return that count; never more than *length); SET/OUTPUT consumes
 * a local copy. Return HM_DENIED for unsupported reports. Never retain data. */
typedef hm_u32 (*hm_backend_report)(void *,hm_u32,hm_u32,hm_u32,hm_u8 *,hm_u32 *);
/* Ring-full resynchronization (integration fix 2026-11-10): HID input reports
 * are state snapshots, so when the ring is full the service keeps only the
 * LATEST report per (interface, report ID) and publishes it as soon as the
 * guest frees a slot. A dropped key release can therefore no longer leave a key
 * stuck; intermediate states are lost (and counted in OVERFLOW). */
#define HM_COALESCE 16u
typedef struct hm_service {
    hm_u8 data[HM_PAGE];
    hm_backend_interface interfaces[HM_MAX_IFACES];
    hm_u32 count, head, tail, overflow, enabled, ready, active, last_seq;
    hm_u32 ids; /* bit i: interface i declares report IDs */
    hm_u32 pending;
    hm_slot pend[HM_COALESCE];
    hm_u8 big[HM_MAX_FEATURE]; /* v1.1 GET_FEATURE scratch, private, never mapped */
    hm_backend_report report;
    void *backend;
} hm_service;
/* Caller serializes every entry (including backend pushes). No allocator,
 * OS calls, atomics, or external library dependencies. */
hm_u32 hm_service_init(hm_service *,const hm_backend_interface *,hm_u32,hm_backend_report,void *);
hm_u32 hm_service_read(hm_service *,hm_u32 page,hm_u32 offset);
void hm_service_write(hm_service *,hm_u32 page,hm_u32 offset,hm_u32 value);
hm_u32 hm_service_push(hm_service *,hm_u32,const hm_u8 *,hm_u32,hm_u64);
int hm_service_irq(const hm_service *);
#endif
