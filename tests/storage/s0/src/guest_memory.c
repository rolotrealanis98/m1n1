/* SPDX-License-Identifier: MIT */
#include "guest_memory.h"
#define PAGE 4096u
void *s0_guest_span(const struct s0_guest_memory *map, uint64_t address,
                    size_t length, bool write)
{
    if (!map || !map->ram || !map->page_permissions || !map->length ||
        (map->base & (PAGE-1)) || map->length % PAGE ||
        map->permission_count != map->length / PAGE || map->length > UINT64_MAX-map->base ||
        !length || address < map->base || address-map->base >= map->length ||
        length > map->length-(address-map->base)) return NULL;
    size_t offset = (size_t)(address-map->base);
    unsigned permission = write ? S0_GUEST_WRITE : S0_GUEST_READ;
    for (size_t page=offset/PAGE; page<=(offset+length-1)/PAGE; page++)
        if (!(map->page_permissions[page] & permission)) return NULL;
    return map->ram+offset;
}
