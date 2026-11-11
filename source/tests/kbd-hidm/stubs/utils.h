#ifndef TEST_UTILS_H
#define TEST_UTILS_H
#include <stdio.h>
#include "types.h"
#define BIT(n) (1ULL<<(n))
#define GENMASK(h,l) ((~0ULL >> (63-(h))) & (~0ULL << (l)))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctzll(m))
#define FIELD_PREP(m,v) (((u64)(v)<<__builtin_ctzll(m))&(m))
#define ALIGN_UP(v,a) (((v)+(a)-1)&~((a)-1))
#define SZ_16K 16384
#define USEC_PER_SEC 1000000
#define min(a,b) ((a)<(b)?(a):(b))
#define debug_printf(...) do {} while(0)
#define dma_rmb() do {} while(0)
#define dma_wmb() do {} while(0)
u64 timeout_calculate(u32);
bool timeout_expired(u64);
u32 read32(u64);
u64 read64(u64);
void write32(u64,u32);
void write64(u64,u64);
void set32(u64,u32);
void clear32(u64,u32);
int poll32(u64,u32,u32,u32);
#endif
extern u64 test_ticks;
#define CNTPCT_EL0 0
#define CNTFRQ_EL0 1
#define mrs(r) ((r)==CNTFRQ_EL0?1000000:++test_ticks)
void udelay(u32);
