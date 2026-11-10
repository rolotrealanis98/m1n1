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
/* Consume a refused write without touching its destination, including checksum-off sentinel. */
bool s1n_link_discard(u64 size, bool sentinel, ssize_t (*read_bytes)(void *, size_t));
#endif
