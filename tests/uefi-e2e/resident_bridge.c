/* Host bridge to the ACTUAL frozen resident S1 model (s1-resident-20261109,
 * src/hv/win/s1/{hv_pci,hv_nvme,hv_nvme_queue,ram_disk}.c, unmodified).
 * Setup mirrors hv_win_storage_prepare(): s1_init(ECAM, window=BAR, 16 MiB),
 * BAR low/high, then MEM|BME through the checked config API.
 * Host identity: guest IPA == host VA (the platform lease/S2 policy is
 * mocked; it is NOT exercised here). */
#include "model.h"
#include <stdint.h>
#include <string.h>

#define S1_ECAM_BASE UINT64_C(0xF28000000)
#define S1_BAR_BASE  UINT64_C(0xF30000000)

static struct s1_model m;
static int deny_large_writes;
static unsigned long spans, denied, rejects, irq_changes;
static int irq_level; unsigned long bridge_irq_asserts, bridge_irq_deasserts;

static void *guest_span(void *o, u64 a, size_t n, bool w)
{
    (void)o;
    spans++;
    if (!a || !n) return NULL;
    if (deny_large_writes && w && n > 64) { denied++; return NULL; }
    return (void *)(uintptr_t)a;
}
static bool map(void *o, u64 a, size_t n) { (void)o; return a == S1_BAR_BASE && n == S1_BAR_SIZE; }
static bool unmap(void *o, u64 a, size_t n) { (void)o; return a == S1_BAR_BASE && n == S1_BAR_SIZE; }
static void irq(void *o, bool l) { (void)o; irq_changes++; if (l && !irq_level) bridge_irq_asserts++; if (!l && irq_level) bridge_irq_deasserts++; irq_level = l; }
static const struct s1_ops ops = { guest_span, map, unmap, irq, NULL };

int bridge_reset(int deny)
{
    u64 v;
    memset(&m, 0, sizeof(m));
    deny_large_writes = deny;
    spans = denied = rejects = irq_changes = 0; irq_level = 0; bridge_irq_asserts = bridge_irq_deasserts = 0;
    if (!s1_init(&m, &ops, NULL, S1_ECAM_BASE, S1_BAR_BASE, S1_BAR_SIZE)) return 0;
    v = (u32)S1_BAR_BASE;
    if (!s1_config(&m, S1_ECAM_BASE + 0x10, 4, true, &v)) return 0;
    v = S1_BAR_BASE >> 32;
    if (!s1_config(&m, S1_ECAM_BASE + 0x14, 4, true, &v)) return 0;
    v = S1_MEM | S1_BME;
    if (!s1_config(&m, S1_ECAM_BASE + 4, 2, true, &v) || !m.mapped) return 0;
    return 1;
}
int bridge_mmio(uint64_t addr, unsigned bytes, int write, uint64_t *value)
{
    bool ok = s1_mmio(&m, addr, bytes, write != 0, value);
    if (!ok) rejects++;
    return ok;
}
int bridge_config_read(uint64_t off, unsigned bytes, uint64_t *value)
{
    return s1_config(&m, S1_ECAM_BASE + off, bytes, false, value);
}
void bridge_stats(uint64_t out[8])
{
    out[0] = m.backend_reads;
    out[1] = m.queue.stats.commands;
    out[2] = m.queue.stats.completions;
    out[3] = ((uint64_t)m.regs.cc << 32) | m.regs.csts;
    out[4] = rejects;
    out[5] = denied;
    out[6] = m.faulted;
    out[7] = spans;
}
int bridge_config_write(uint64_t off, unsigned bytes, uint64_t value)
{
    return s1_config(&m, S1_ECAM_BASE + off, bytes, true, &value);
}
int bridge_irq_level(void) { return irq_level; }
int bridge_queue_valid(unsigned q) { return m.queue.queues[q].sq_valid || m.queue.queues[q].cq_valid; }
uint64_t bridge_queue_sq_addr(unsigned q) { return m.queue.queues[q].sq_addr; }
