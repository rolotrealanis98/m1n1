/* Host stub environment for compiling the actual hv_win_fb.c (orig and patched). */
#ifndef FBSTUB_H
#define FBSTUB_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned long u64; typedef long s64; typedef uint32_t u32; typedef uint8_t u8;
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define MAX_CPUS 32
#define REGION_RWX_EL0 0x8000000000ULL
#define REGION_RW_EL0  0xa000000000ULL
#define REGION_RX_EL1  0xc000000000ULL
#define MAIR_IDX_NORMAL_NC 1
#define PERM_RW 1
#define PERM_RW_EL0 2
#define CNTFRQ_EL0 0
struct exc_info { u64 far, esr, elr; };
struct boot_args { struct { u64 base, display, stride, width, height, depth; } video; };
struct hv_win_state { u64 nr_vcpus; };
extern struct hv_win_state hv_win;
extern struct boot_args cur_boot_args;
bool hv_win_vgic_enabled(void);
u64 stub_mrs(int r);
#define mrs(r) stub_mrs(r)
u64 get_ticks(void);
void stub_sysop(const char *s);
#define sysop(s) stub_sysop(s)
void dc_civac_range(void *p, size_t n);
size_t heapblock_remaining(void);
void *heapblock_alloc_aligned(size_t sz, size_t al);
void mmu_rm_mapping(u64 va, u64 sz);
int mmu_add_mapping(u64 va, u64 pa, u64 sz, int attr, int perm);
int hv_map_hw_nc(u64 from, u64 to, u64 size);
int hv_wp_prepare(u64 ipa, u64 size);
int hv_wp_set(u64 ipa, bool writable);
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa);
void hv_xt_tlbi(bool broadcast);
void stub_tlbi(const char *op, u64 v);
#define FB_TLBI(op, v) stub_tlbi(op, v)
#ifdef FB_HARNESS_COST
void stub_cost(u64 rd, u64 wr);
#define FB_COST(rd, wr) stub_cost(rd, wr)
#endif
/* disp2: EL2 console print (FBS lines) goes to the host stdout when enabled */
int stub_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) stub_printf(__VA_ARGS__)
/* both orig and patched prototypes are declared by the including TU */
#endif
