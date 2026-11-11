/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: resident stand-ins for the glue (hv_win_n1pt.c): stage-2 map, vGIC SPI, AIC, order queries. */
#include <sys/mman.h>
#include "glue.h"
#include "hv_win.h"

struct glue G;
bool hv_started_cpus[MAX_CPUS];

void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }
u64 m_now(void);
u64 get_ticks(void) { return m_now(); }
u64 ticks_to_usecs(u64 t) { return t; }
u64 top_of_memory_alloc(size_t size)
{
    void *p = mmap(NULL, size + 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "glue: pool mmap failed\n"); exit(2); }
    G.pool = (void *)(((uintptr_t)p + 0x3fff) & ~(uintptr_t)0x3fff);
    return (u64)(uintptr_t)G.pool;
}
bool hv_win_aic_claim_t6050(void) { return !G.aic_bad; }
bool hv_win_disk_path_used(void) { return G.disk_used; }
bool hv_win_smcp_attempted(void) { return G.smcp_attempted; }
bool hv_win_n1_touched(void) { return G.n1_touched; }
bool hv_win_storage_claims_spi(u32 g) { (void)g; return G.storage_claims; }
bool hv_vgic3_spi_unassigned(u32 g) { (void)g; return !G.spi_taken; }
void hv_vgic3_set_spi_level(u32 intid, bool level)
{
    if (level && !G.spi_level)
        G.spi_rises++;
    G.spi_level = level;
    G.spi_intid = intid;
}
void aic_mask(u32 irq) { if (irq < 4096) { G.mask[irq]++; G.masked[irq] = true; } }
void aic_unmask(u32 irq) { if (irq < 4096) { G.unmask[irq]++; G.masked[irq] = false; } }

static int add_map(u64 from, u64 size, u64 to, hv_hook_t *hook, bool dev)
{
    if (G.nmap >= G_MAPS) { fprintf(stderr, "glue: map table full\n"); exit(2); }
    G.map[G.nmap].from = from; G.map[G.nmap].size = size; G.map[G.nmap].to = to;
    G.map[G.nmap].hook = hook; G.map[G.nmap].dev = dev;
    G.nmap++;
    return 0;
}
int hv_map_hw_dev(u64 from, u64 to, u64 size) { return add_map(from, size, to, NULL, true); }
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    if (G.hook_fail_at && ++G.nhook_calls == G.hook_fail_at)
        return -1;
    return add_map(from, size, 0, hook, false);
}

static int find(u64 ipa)
{
    for (int i = (int)G.nmap - 1; i >= 0; i--) /* the latest map of a range wins, as stage 2 */
        if (ipa >= G.map[i].from && ipa < G.map[i].from + G.map[i].size)
            return i;
    return -1;
}
int guest_read(u64 ipa, int width, u64 *val)
{
    int i = find(ipa);
    if (i < 0) return -1;
    if (G.map[i].dev) {
        u64 pa = G.map[i].to + (ipa - G.map[i].from);
        u32 v = read32(pa & ~3UL);
        *val = width >= 2 ? v : (v >> ((pa & 3) * 8)) & (width == 1 ? 0xffff : 0xff);
        return 1;
    }
    struct exc_info e = {0};
    G.map[i].hook(&e, ipa, val, false, width);
    return 0;
}
int guest_write(u64 ipa, int width, u64 val)
{
    int i = find(ipa);
    if (i < 0) return -1;
    if (G.map[i].dev) { write32(G.map[i].to + (ipa - G.map[i].from), (u32)val); return 1; }
    struct exc_info e = {0};
    G.map[i].hook(&e, ipa, &val, true, width);
    return 0;
}
void glue_reset(void)
{
    memset(&G, 0, sizeof(G));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
}
