/* SPDX-License-Identifier: MIT */
#ifndef S0_GUEST_MEMORY_H
#define S0_GUEST_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define S0_GUEST_READ 1u
#define S0_GUEST_WRITE 2u
/* An explicit RAM arena only, never an IPA->PA or MMIO fallback. In S0 the
 * caller owns these immutable/stable mappings throughout each synchronous call. */
struct s0_guest_memory {
    uint64_t base;
    uint8_t *ram;
    size_t length;
    const uint8_t *page_permissions;
    size_t permission_count;
};
void *s0_guest_span(const struct s0_guest_memory *map, uint64_t address,
                    size_t length, bool write);
#endif
