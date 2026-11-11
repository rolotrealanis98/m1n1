/* smp-storage stress: platform stubs for the REAL hv_win_storage.c + hv_win_storage_mem.c (appended after this
 * file with their #include lines stripped) and the real S1 model (hv_nvme.c, hv_pci.c, hv_nvme_queue.c,
 * ram_disk.c, compiled separately). Every thread is a "CPU" (smp_id() = thread-local id). bhl is NOT a real lock
 * here: each thread sees "bhl held by me", so the storage lock is the only serialisation under test.
 * Host only: no hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "model.h"
#include "ram_disk.h"
#include "hv_win_storage.h"
#include "hv_win_storage_mem.h"

#define MAX_CPUS 24
#ifndef GENMASK
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#endif
extern __thread int t_cpu;
static int smp_id(void) { return t_cpu; }
static int boot_cpu_idx = 0;
static char resident_marker[0x1000];
#define _base resident_marker
#define _end (resident_marker + sizeof(resident_marker))
#define sysop(x) __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define dma_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define dma_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define mrs(x) ((u64)0xc0) /* DAIF: IRQ and FIQ masked, as in every EL2 handler */
struct exc_info { int unused; };
struct { u32 nr_vcpus; int vcpu_cpu[MAX_CPUS]; int cpu_vcpu[MAX_CPUS]; } hv_win;
u64 hv_cpus_in_guest;
bool hv_started_cpus[MAX_CPUS];
typedef struct { s64 lock; s64 count; } spinlock_t;
spinlock_t *stress_bhl(void);
#define bhl (*stress_bhl())
static bool hv_win_vgic_enabled(void) { return true; }
bool hv_vgic3_spi_unassigned(u32 n) { return n == 66; }
u32 hv_win_pl011_gsiv(void) { return 64; }
static u64 hv_pt_walk(u64 a) { (void)a; return 0; }
typedef bool (*hook_t)(struct exc_info *, u64, u64 *, bool, int);
static hook_t hooks[2];
static int hv_map_hook(u64 a, hook_t h, u64 size)
{
    (void)size;
    hooks[a == HV_S1_ECAM ? 0 : 1] = h;
    return 0;
}
static int irq_level, irq_edges; /* written by the storage path (under its lock) */
static void hv_vgic3_set_spi_level(u32 n, bool level)
{
    if (n != 66) abort();
    irq_level = level;
    irq_edges++;
}
struct hv_s2_leaf { u64 ipa_base, pa_base, size, descriptor; };
static u64 guest_base, guest_size;
/* identity 16 KiB normal-memory RW leaves over guest RAM (the real resolver is tested by check-memory.py) */
static bool hv_pt_resolve_leaf(u64 a, struct hv_s2_leaf *leaf)
{
    if (a < guest_base || a - guest_base >= guest_size) return false;
    u64 base = a & ~0x3fffUL;
    leaf->ipa_base = leaf->pa_base = base;
    leaf->size = 0x4000;
    leaf->descriptor = (base & GENMASK(49, 14)) | BIT(10) | (3UL << 8) | (3UL << 6) | (15UL << 2) | 3;
    return true;
}
static u64 pmu_dma_calls;
static u32 hv_win_pmu_dma_write(u64 ipa, u64 size) { (void)ipa; (void)size; pmu_dma_calls++; return 0; }
