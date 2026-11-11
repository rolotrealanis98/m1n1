/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: host stand-ins for the m1n1 headers pcie_t6050.c, n1pt_plat.c and n1pt_dart.c include.
 * The sources are compiled as byte-identical copies placed next to these headers (run.sh), so their
 * quoted includes resolve here; everything they touch goes to the register model in model.c. */
#ifndef N1PT_HOST_H
#define N1PT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64; /* LP64 like the firmware: "%lx" matches */
typedef int32_t s32;
typedef long s64;

#define BIT(x)                 (1UL << (x))
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))
#define ARRAY_SIZE(a)          (sizeof(a) / sizeof((a)[0]))
#define max(a, b)              (((a) > (b)) ? (a) : (b))
#define SZ_4K                  (1 << 12)
#define SZ_16K                 (1 << 14)
#define USEC_PER_SEC           1000000L
#define UNUSED(x)              (void)(x)
#define dma_wmb()              __atomic_thread_fence(__ATOMIC_RELEASE)

/* MMIO and time (model.c) */
u32 read32(u64 a);
void write32(u64 a, u32 v);
u16 read16(u64 a);
void write16(u64 a, u16 v);
void udelay(u32 us);
#define mdelay(m) udelay((m) * 1000)
u64 timeout_calculate(u32 usec);
bool timeout_expired(u64 t);

/* heap below 2^42 (DART tables hold 42-bit addresses) */
void *memalign(size_t align, size_t size);
void free_hook(void *p);
#define free(p) free_hook(p)

int model_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) model_printf(__VA_ARGS__)

/* ADT */
extern const void *adt;
int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
int adt_getprop_copy(const void *adt, int nodeoffset, const char *name, void *out, size_t len);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
#define ADT_GETPROP(adt, nodeoffset, name, val) adt_getprop_copy(adt, nodeoffset, name, (val), sizeof(*(val)))
#define ADT_GETPROP_ARRAY(adt, nodeoffset, name, arr) adt_getprop_copy(adt, nodeoffset, name, (arr), sizeof(arr))

/* PMGR, tunables, SMC, RTKit, NVMe, WDT */
uintptr_t pmgr_psreg_by_name(int die, const char *name);
int pmgr_power_is_on(int die, const char *name);
int pmgr_set_mode(uintptr_t addr, u8 target_mode);
struct tunable_excuse {
    u32 offset;
    u64 bits;
};
int tunables_validate_local(const char *path, const char *prop, u64 span);
int tunables_apply_local_addr(const char *path, const char *prop, uintptr_t base);
int tunables_check_local_addr(const char *path, const char *prop, uintptr_t base, const struct tunable_excuse *e);
typedef struct smc_dev smc_dev_t;
smc_dev_t *smc_init(void);
void smc_shutdown(smc_dev_t *smc);
int smc_read_u32(smc_dev_t *smc, u32 key, u32 *value);
int smc_write_u32(smc_dev_t *smc, u32 key, u32 value);
void rtkit_set_boot_timeout(u64 t);
bool nvme_init(void);
void nvme_shutdown(void);
bool nvme_read(u32 nsid, u64 lba, void *buffer);
void wdt_reboot(void);

#endif
