/* Host shim: m1n1 utils.h. All MMIO goes to the fake ANS model; time is virtual. */
#ifndef UTILS_H
#define UTILS_H
#include "types.h"
#include <stdio.h>
#include <string.h>

#define BIT(x)               (1UL << (x))
#define GENMASK(h, l)        (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_PREP(mask, val) (((u64)(val) << __builtin_ctzll(mask)) & (mask))
#define FIELD_GET(mask, reg)  ((((u64)(reg)) & (mask)) >> __builtin_ctzll(mask))
#define ALIGN_UP(x, a)       (((x) + ((a) - 1)) & ~((a) - 1))
#define SZ_4K                0x1000
#define SZ_16K               0x4000
#define USEC_PER_SEC         1000000UL
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* no printf-format attr: frozen m1n1 uses %lx for 64-bit u64 (unsigned long on target) */
int fake_log(const char *fmt, ...);
#define printf(...)       fake_log(__VA_ARGS__)
#define debug_printf(...) fake_log(__VA_ARGS__)

u32 fake_read32(u64 addr);
void fake_write32(u64 addr, u32 val);
u64 fake_read64(u64 addr);
void fake_write64(u64 addr, u64 val);
void fake_udelay(u32 us);
void fake_barrier(int kind);
u64 fake_now(void);
bool fake_is_heap(void *p);

static inline u32 read32(u64 addr) { return fake_read32(addr); }
static inline void write32(u64 addr, u32 v) { fake_write32(addr, v); }
static inline u64 read64(u64 addr) { return fake_read64(addr); }
static inline void write64(u64 addr, u64 v) { fake_write64(addr, v); }
static inline u32 set32(u64 addr, u32 set) { u32 v = read32(addr) | set; write32(addr, v); return v; }
static inline u32 clear32(u64 addr, u32 clear) { u32 v = read32(addr) & ~clear; write32(addr, v); return v; }
static inline u32 mask32(u64 addr, u32 clear, u32 set) { u32 v = (read32(addr) & ~clear) | set; write32(addr, v); return v; }
static inline void write64_lo_hi(u64 addr, u64 val) { write32(addr, (u32)val); write32(addr + 4, (u32)(val >> 32)); }
static inline void udelay(u32 us) { fake_udelay(us); }
static inline int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    while (--timeout > 0) {
        if ((read32(addr) & mask) == target)
            return 0;
        udelay(1);
    }
    return -1;
}
#define dma_wmb() fake_barrier(1)
#define sysop(x) fake_barrier(3)
#define dma_rmb() fake_barrier(2)
static inline u64 timeout_calculate(u32 usec) { return fake_now() + usec; }
static inline bool timeout_expired(u64 t) { return fake_now() > t; }
static inline bool is_heap(void *p) { return fake_is_heap(p); }
#endif
