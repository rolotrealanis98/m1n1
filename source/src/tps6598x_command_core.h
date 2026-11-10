/* SPDX-License-Identifier: MIT */
#ifndef TPS6598X_COMMAND_CORE_H
#define TPS6598X_COMMAND_CORE_H

#ifdef TPS6598X_COMMAND_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

#define TPS6598X_COMMAND_DEADLINE_MS 1500
#define TPS6598X_COMMAND_TIMEOUT     -2

struct tps6598x_command_ops {
    int (*write)(void *ctx, u8 reg, const u8 *data, size_t len);
    int (*read)(void *ctx, u8 reg, u8 *data, size_t len);
    u64 (*now_ms)(void *ctx);
    void (*delay_us)(void *ctx, unsigned usec);
};

int tps6598x_command_execute(const struct tps6598x_command_ops *ops, void *ctx, const char *cmd,
                             const u8 *data_in, size_t len_in, u8 *data_out, size_t len_out);

#endif
