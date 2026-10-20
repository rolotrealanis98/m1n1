/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "dapf.h"
#include "adt.h"
#include "assert.h"
#include "malloc.h"
#include "memory.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

struct dapf_t8020_config {
    u64 start;
    u64 end;
    u8 unk1;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk2;
    u32 r4;
} PACKED;

static int dapf_init_t8020(const char *path, u64 base, int node)
{
    u32 length;
    const char *prop = "filter-data-instance-0";
    const struct dapf_t8020_config *config = adt_getprop(adt, node, prop, &length);

    if (!config || !length || (length % sizeof(*config)) != 0) {
        printf("dapf: Error getting ADT node %s property %s.\n", path, prop);
        return -1;
    }

    int count = length / sizeof(*config);

    for (int i = 0; i < count; i++) {
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        base += 0x40;
    }
    return 0;
}

struct dapf_t8110_config {
    u64 start;
    u64 end;
    u32 r20;
    u32 unk1;
    u32 r4;
    u32 unk2[5];
    u8 unk3;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk4;
} PACKED;

struct dapf_t8110b_config {
    u64 start;
    u64 end;
    u32 r20;
    u32 unk1;
    u32 r4;
    u32 unk2[5];
    u8 unk3;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk4;
    u32 pad;
} PACKED;

static int dapf_init_t8110a(u64 base, u64 size, struct dapf_t8110_config *config, u32 length)
{
    int count = length / sizeof(*config);

    /* Each entry consumes 0x40 bytes of register space. The ADT property
     * length and the register window are independent, and on an unsupported
     * SoC a mis-sized property walks straight off the end of the window into
     * unmapped MMIO -> external abort -> asynchronous SError. */
    if (size && (u64)count * 0x40 > size) {
        printf("dapf: clamping %d entries to %ld (window 0x%lx at 0x%lx)\n", count, size / 0x40,
               size, base);
        count = size / 0x40;
    }

    for (int i = 0; i < count; i++) {
        /* Per-slice barrier: the abort these writes can raise is an
         * asynchronous SError, so without this the reported PC is wherever
         * the CPU happened to be, not the slice that caused it. */
        printf("dapf:   slice %d @ 0x%lx\n", i, base);
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        write32(base + 0x20, config[i].r20);
        sysop("dsb sy");
        sysop("isb");
        base += 0x40;
    }
    return 0;
}

static int dapf_init_t8110b(u64 base, u64 size, struct dapf_t8110b_config *config, u32 length)
{
    int count = length / sizeof(*config);

    if (size && (u64)count * 0x40 > size) {
        printf("dapf: clamping %d entries to %ld (window 0x%lx at 0x%lx)\n", count, size / 0x40,
               size, base);
        count = size / 0x40;
    }

    for (int i = 0; i < count; i++) {
        printf("dapf:   slice %d @ 0x%lx\n", i, base);
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        write32(base + 0x20, config[i].r20);
        sysop("dsb sy");
        sysop("isb");
        base += 0x40;
    }
    return 0;
}

static int dapf_init_t8110(const char *path, u64 base, u64 size, int node)
{
    u32 length;
    const char *prop = "dapf-instance-0";
    const void *config = adt_getprop(adt, node, prop, &length);

    if (!config || !length) {
        printf("dapf: Error getting ADT node %s property %s.\n", path, prop);
        return -1;
    }

    printf("dapf: %s base 0x%lx window 0x%lx, %s length %u\n", path, base, size, prop, length);

    // The least common multiple of 52 and 56 is 728 which is in the range of
    // the observed lengthe for "dapf-instance-0". The 52 byte variant is more
    // common and prefering that works so far.
    if (length % sizeof(struct dapf_t8110_config) == 0) {
        return dapf_init_t8110a(base, size, (struct dapf_t8110_config *)config, length);
    } else if (length % sizeof(struct dapf_t8110b_config) == 0) {
        return dapf_init_t8110b(base, size, (struct dapf_t8110b_config *)config, length);
    } else {
        printf("dapf: Invalid length for %s property %s\n", path, prop);
        return -1;
    }
}

int dapf_init(const char *path, int index)
{
    int ret;
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    if (node < 0) {
        printf("dapf: Error getting DAPF %s node.\n", path);
        return -1;
    }

    u32 pwr;
    if (!adt_getprop(adt, node, "clock-gates", &pwr))
        pwr = 0;
    if (pwr && (pmgr_adt_power_enable(path) < 0))
        return -1;

    u64 base, size = 0;
    if (adt_get_reg(adt, dart_path, "reg", index, &base, &size) < 0) {
        printf("dapf: Error getting DAPF %s base address (index %d).\n", path, index);
        return -1;
    }

    if (adt_is_compatible(adt, node, "dart,t8020")) {
        ret = dapf_init_t8020(path, base, node);
    } else if (adt_is_compatible(adt, node, "dart,t6000")) {
        ret = dapf_init_t8020(path, base, node);
    } else if (adt_is_compatible(adt, node, "dart,t8110")) {
        ret = dapf_init_t8110(path, base, size, node);
    } else {
        printf("dapf: DAPF %s at 0x%lx is of an unknown type\n", path, base);
        return -1;
    }

    if (pwr)
        pmgr_adt_power_disable(path);

    if (!ret)
        printf("dapf: Initialized %s\n", path);

    return ret;
}

struct entry {
    const char *path;
    int index;
};

struct entry dapf_entries[] = {
    {"/arm-io/dart-aop", 1}, {"/arm-io/dart-mtp", 1},  {"/arm-io/dart-pmp", 1},
    {"/arm-io/dart-isp", 5}, {"/arm-io/dart-isp0", 5}, {NULL, -1},
};

int dapf_init_all(void)
{
    int ret = 0;
    int count = 0;
    struct entry *entry = dapf_entries;

    while (entry->path != NULL) {
        if (adt_path_offset(adt, entry->path) < 0) {
            entry++;
            continue;
        }
        /* T6050 (M5 Pro) bring-up: programming /arm-io/dart-aop's DAPF
         * raises an asynchronous SError on slice 6 (L2C_ERR_ADR =
         * base + 0x180) even though the runtime ADT lists 18 slices and
         * they fit inside the 0x4000 window. The on-disk ADT carries
         * "allow-dram-apf-slices-0 = 0" for this DART, which m1n1 ignores,
         * so the hardware very likely exposes fewer slices than the
         * property describes. AOP DMA is not needed to boot Linux, so skip
         * it here until the real slice count is known. */
        if (chip_id == T6050 && !strcmp(entry->path, "/arm-io/dart-aop")) {
            printf("dapf: skipping %s on T6050 (known SError, bring-up workaround)\n", entry->path);
            entry++;
            continue;
        }
        if (dapf_init(entry->path, entry->index) < 0) {
            ret = -1;
        }
        entry++;
        count += 1;
    }
    return ret ? ret : count;
}
