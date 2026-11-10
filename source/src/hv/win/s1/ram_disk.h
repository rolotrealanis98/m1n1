/* SPDX-License-Identifier: MIT */
#ifndef S0_RAM_DISK_H
#define S0_RAM_DISK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct s0_disk {
    const uint8_t *bytes;
    size_t byte_length;
    uint64_t block_count;
    uint32_t block_size;
    bool read_only;
};
extern const struct s0_disk s0_fixture;
bool s0_disk_valid(const struct s0_disk *disk);
bool s0_disk_read(const struct s0_disk *disk, uint64_t lba, uint32_t blocks, void *out);
bool s0_fixture_guards_valid(void);
#endif
