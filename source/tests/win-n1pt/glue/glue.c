/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: resident stand-ins for the glue (hv_win_n1pt.c): stage-2 map, vGIC SPI, AIC, order queries. */
#include <sys/mman.h>
#include "glue.h"
#include "hv_win.h"
#include "aic.h"

static struct aic aic_t6050 = {0x280400000UL, 4096, {0x10000}};
struct aic *aic = &aic_t6050;

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

static int add_map2(u64 from, u64 size, u64 to, hv_hook_t *hook, bool dev, bool ram);
static int add_map(u64 from, u64 size, u64 to, hv_hook_t *hook, bool dev)
{
    return add_map2(from, size, to, hook, dev, false);
}
static int add_map2(u64 from, u64 size, u64 to, hv_hook_t *hook, bool dev, bool ram)
{
    if (G.nmap >= G_MAPS) { fprintf(stderr, "glue: map table full\n"); exit(2); }
    G.map[G.nmap].from = from; G.map[G.nmap].size = size; G.map[G.nmap].to = to;
    G.map[G.nmap].hook = hook; G.map[G.nmap].dev = dev; G.map[G.nmap].ram = ram;
    G.nmap++;
    return 0;
}
int hv_map_hw_dev(u64 from, u64 to, u64 size) { return add_map(from, size, to, NULL, true); }
int hv_map_hw(u64 from, u64 to, u64 size) { return G.ram_fail ? -1 : add_map2(from, size, to, NULL, false, true); }
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
u64 hv_pt_walk(u64 a)
{
    if (!G.tables_built) { fprintf(stderr, "glue: hv_pt_walk before the stage-2 build\n"); exit(4); }
    if (G.foreign_ipa && a >= G.foreign_ipa && a < G.foreign_ipa + 0x4000)
        return 0x1234; /* someone else's software entry */
    int i = find(a);
    if (i < 0) return 0;
    return G.map[i].hook ? 0x2 : (G.map[i].to + (a - G.map[i].from)) | 0x7fd; /* HW: RW, MemAttr 1111, AF, valid */
}
bool hv_pt_resolve_leaf(u64 a, struct hv_s2_leaf *o)
{
    memset(o, 0, sizeof(*o));
    if (!G.tables_built) return false;
    int i = find(a);
    if (i < 0 || G.map[i].hook || (G.foreign_ipa && a >= G.foreign_ipa && a < G.foreign_ipa + 0x4000)) return false;
    u64 page = a & ~0x3fffUL;
    o->ipa_base = page;
    o->pa_base = G.map[i].to + (page - G.map[i].from);
    o->size = 0x4000;
    o->descriptor = o->pa_base | (G.map[i].ram ? 0x7fd : 0x7c1); /* device map: MemAttr 0000 */
    return true;
}
void glue_unmap(u64 from, u64 size)
{
    u32 k = 0;
    for (u32 i = 0; i < G.nmap; i++)
        if (G.map[i].from + G.map[i].size <= from || G.map[i].from >= from + size)
            G.map[k++] = G.map[i];
    G.nmap = k;
}
bool hv_win_n1pt_unmapped(u64 ipa);
int guest_access(u64 ipa, bool write, u64 *val)
{
    for (int tries = 0; tries < 3; tries++) {
        int i = find(ipa);
        if (i >= 0) {
            if (G.map[i].ram) {
                u64 *p = (u64 *)(uintptr_t)(G.map[i].to + (ipa - G.map[i].from));
                if (write) *p = *val; else *val = *p;
                return 1;
            }
            return write ? guest_write(ipa, 2, *val) : guest_read(ipa, 2, val);
        }
        if (!hv_win_n1pt_unmapped(ipa)) /* hv_handle_dabort: anything not repaired here goes to the host */
            return -1;
    }
    return -1;
}
int guest_read(u64 ipa, int width, u64 *val)
{
    int i = find(ipa);
    if (i < 0) return -1;
    if (G.map[i].ram) { *val = *(u32 *)(uintptr_t)(G.map[i].to + (ipa - G.map[i].from)); return 2; }
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
    if (G.map[i].ram) { *(u32 *)(uintptr_t)(G.map[i].to + (ipa - G.map[i].from)) = (u32)val; return 2; }
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
