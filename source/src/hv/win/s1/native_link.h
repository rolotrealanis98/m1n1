/* SPDX-License-Identifier: MIT */
#ifndef S1_NATIVE_LINK_H
#define S1_NATIVE_LINK_H
#include "types.h"
bool s1n_storage_context(void);
bool s1n_canary_context(void);
void s1n_link_lock(void);
bool s1n_link_locked(void);
u64 s1n_link_refusals(void);
bool s1n_link_proxy(u64 opcode, bool guest_exception);
bool s1n_link_raw(void);
/* r4.8: once locked, host memory access is refused EXCEPT, inside a guest-exception proxy session,
 * the exception context the resident passed and guest RAM. Everything else (SSD controller MMIO, the
 * resident's storage state/heap, the image) stays refused. [addr, addr+size) must lie ENTIRELY in one
 * permitted region. size 0 is a no-op (allowed). Counts a refusal like s1n_link_raw. */
bool s1n_link_mem_ok(u64 addr, u64 size);
/* Bracket an exception proxy session: pass the exc_info pointer the resident handed to the proxy
 * (uartproxy_run) so host context reads/writes are admitted; clear with (0, 0) on exit. */
void s1n_link_exc_ctx(u64 addr, u64 size);
/* Consume a refused write without touching its destination, including checksum-off sentinel. */
bool s1n_link_discard(u64 size, bool sentinel, ssize_t (*read_bytes)(void *, size_t));
#endif
