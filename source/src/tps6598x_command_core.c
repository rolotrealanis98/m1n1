/* SPDX-License-Identifier: MIT */

#include "tps6598x_command_core.h"

#define TPS_REG_CMD1    0x08
#define TPS_REG_DATA1   0x09
#define TPS_CMD_INVALID 0x444d4321 /* !CMD, little endian */

static bool deadline_expired(const struct tps6598x_command_ops *ops, void *ctx, u64 start)
{
    return ops->now_ms(ctx) - start >= TPS6598X_COMMAND_DEADLINE_MS;
}

int tps6598x_command_execute(const struct tps6598x_command_ops *ops, void *ctx, const char *cmd,
                             const u8 *data_in, size_t len_in, u8 *data_out, size_t len_out)
{
    if (!ops || !ops->write || !ops->read || !ops->now_ms || !ops->delay_us || !cmd)
        return -1;

    u64 start = ops->now_ms(ctx);
    if (len_in && ops->write(ctx, TPS_REG_DATA1, data_in, len_in) != (int)len_in)
        goto transport_error;
    if (deadline_expired(ops, ctx, start))
        return TPS6598X_COMMAND_TIMEOUT;
    if (ops->write(ctx, TPS_REG_CMD1, (const u8 *)cmd, 4) != 4)
        goto transport_error;

    for (;;) {
        if (deadline_expired(ops, ctx, start))
            return TPS6598X_COMMAND_TIMEOUT;
        u8 bytes[4];
        if (ops->read(ctx, TPS_REG_CMD1, bytes, sizeof(bytes)) != sizeof(bytes))
            goto transport_error;
        u32 status =
            (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
        if (status == TPS_CMD_INVALID)
            return -1;
        if (!status)
            break;
        ops->delay_us(ctx, 100);
    }

    if (len_out && ops->read(ctx, TPS_REG_DATA1, data_out, len_out) != (int)len_out)
        goto transport_error;
    return deadline_expired(ops, ctx, start) ? TPS6598X_COMMAND_TIMEOUT : 0;

transport_error:
    return deadline_expired(ops, ctx, start) ? TPS6598X_COMMAND_TIMEOUT : -1;
}
