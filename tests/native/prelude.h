/* Prelude for the r3 end-to-end native test: mocks of the EL2 platform and the memory lease,
 * exactly the shape check-controller.py uses, plus the vGIC deny hook. The actual
 * hv_win_storage.c and hv_win_storage_native.c text is appended after this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "ram_disk.h"
#include "hv_win_storage.h"
#include "hv_win_storage_mem.h"
#include "native_window.h"
#include "ans_ro.h"
#include "native_link.h"
#include "proxy.h"
static void *heapblock_peek(void) { return (void *)0x12340000; }
static size_t heapblock_remaining(void) { return 0x400000; }
#include "fake_ans.h"
#define _base ((char *)0x1000)
#define _end ((char *)0x2000)
#define sysop(x) ((void)0)
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
struct exc_info { int unused; };
static struct { unsigned nr_vcpus; int vcpu_cpu[1]; } hv_win = {1, {0}};
static int boot_cpu_idx;
static bool gic = true, spi_free = true;
static u32 uart_irq = 64;
static unsigned checks, hook_calls, begin_calls, end_calls, span_calls;
static bool irq_level;
typedef bool (*hook_t)(struct exc_info *, u64, u64 *, bool, int);
static hook_t hooks[2];
static u32 denied[8];
static unsigned ndenied;
static u32 deny_fail_line;
bool hv_started_cpus[24];
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL:%s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static bool hv_win_vgic_enabled(void) { return gic; }
bool hv_vgic3_spi_unassigned(u32 n) { CHECK(n == 66); return spi_free; }
u32 hv_win_pl011_gsiv(void) { return uart_irq; }
int hv_vgic3_deny_aic_line(u32 line)
{
    if (line == deny_fail_line) return -1;
    if (ndenied < 8) denied[ndenied++] = line;
    return 0;
}
static u64 hv_pt_walk(u64 a) { (void)a; return 0; }
static int hv_map_hook(u64 a, hook_t h, u64 size)
{
    unsigned i = a == HV_S1_ECAM ? 0 : 1;
    CHECK(a == HV_S1_ECAM || a == HV_S1_BAR);
    CHECK(size == (i ? S1_BAR_SIZE : S1_ECAM_SIZE));
    hook_calls++;
    hooks[i] = h;
    return 0;
}
static void hv_vgic3_set_spi_level(u32 n, bool level) { CHECK(n == 66); irq_level = level; }
static unsigned char guest[4 << 20]; /* r4: room for 1 MiB transfers */
#define GUEST UINT64_C(0x100000000)
bool hv_storage_mem_register(struct hv_storage_mem *m, const struct hv_sm_config *c)
{
    CHECK(m->state == HV_SM_EMPTY);
    CHECK(c->nr_ram == 1 && c->nr_protected == 1);
    m->epoch = c->epoch;
    m->state = HV_SM_READY;
    return true;
}
bool hv_storage_mem_begin(struct hv_storage_mem *m, u64 e)
{
    begin_calls++;
    CHECK(m->epoch == e);
    CHECK(m->state == HV_SM_READY);
    m->state = HV_SM_LEASED;
    return true;
}
bool hv_storage_mem_end(struct hv_storage_mem *m)
{
    end_calls++;
    CHECK(m->state == HV_SM_LEASED);
    m->state = HV_SM_READY;
    return true;
}
void hv_storage_mem_retire(struct hv_storage_mem *m) { m->state = HV_SM_DEAD; }
void *hv_storage_guest_span(void *opaque, u64 a, size_t n, bool w)
{
    (void)w;
    span_calls++;
    struct hv_storage_mem *m = opaque;
    CHECK(m->state == HV_SM_LEASED);
    if (a < GUEST || n > sizeof(guest) || a - GUEST > sizeof(guest) - n) return NULL;
    return guest + (a - GUEST);
}
