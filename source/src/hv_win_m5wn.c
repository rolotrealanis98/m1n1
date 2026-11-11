/* SPDX-License-Identifier: MIT */
/* M5WN glue (wireless/n1-20261111): two trapped pages + vGIC SPI 37 for the mediated network
 * device. Backend 1 = synthetic Wi-Fi + local fake gateway (192.168.77.1, DHCP .2); no Apple
 * hardware is touched. Guest RAM is reached only through a private hv_storage_mem instance
 * (the storage line's lease contract, unchanged) registered from the launcher's final plan with
 * the same wire layout as the S1 registration; storage files are not modified. */
#include "hv_win_m5wn.h"
#include "hv_win_feature_policy.h"
#include "m5wn/service/m5wn_fake.h"
#include "m5wn/n1/n1_core.h"
#include "hv_win_n1.h"
#include "hv/hv.h"
#include "hv/win/hv_win.h"
#include "hv/win/hv_win_storage.h"
#include "hv/win/hv_win_storage_mem.h"
#include "string.h"
#include "smp.h"
#include "utils.h"

/* Trap and tick already hold bhl (hv_exc_entry); configure is preguest only. */
static wn_service service;
static wn_fake fake;
static wn_platform plat;
static bool attempted, exposed, retired, irq;
static u64 traps, refused_copies, epoch;
static struct hv_storage_mem memory;
extern char _base[], _end[];
static const wn_u8 fake_mac[6] = {0x02, 0x4d, 0x35, 0x57, 0x4e, 0x02};

static void update_irq(void)
{
    bool level = exposed && !retired && wn_service_irq(&service);
    if (level != irq) {
        irq = level;
        hv_vgic3_set_spi_level(WN_GSIV, level);
    }
}
/* One bounded copy per lease; no pointer outlives the call (hv_win_storage_mem.h contract). */
static bool copy(u64 ipa, void *buf, size_t len, bool to_guest)
{
    void *p;
    bool closed;
    if (!len || !hv_storage_mem_begin(&memory, epoch)) return false;
    p = hv_storage_guest_span(&memory, ipa, len, to_guest);
    if (p) {
        if (to_guest) { memcpy(p, buf, len); dma_wmb(); }
        else { dma_rmb(); memcpy(buf, p, len); }
    }
    closed = hv_storage_mem_end(&memory);
    return closed && p;
}
static int g_read(void *ctx, wn_u64 ipa, void *dst, wn_u32 len)
{
    (void)ctx;
    if (copy(ipa, dst, len, false)) return 0;
    refused_copies++;
    return -1;
}
static int g_write(void *ctx, wn_u64 ipa, const void *src, wn_u32 len)
{
    (void)ctx;
    if (copy(ipa, (void *)src, len, true)) return 0;
    refused_copies++;
    return -1;
}
static int g_region(void *ctx, wn_u64 ipa, wn_u64 len)
{
    (void)ctx;
    for (size_t i = 0; i < memory.nr_ram; i++) {
        struct hv_sm_range r = memory.ram[i];
        if (len && ipa >= r.base && len <= r.size && ipa - r.base <= r.size - len) return 0;
    }
    return -1;
}

static bool backend_n1;
static int n1_sink_rx(void *s, const n1_u8 *f, n1_u32 l) { return wn_service_rx(s, f, l); }
static void n1_sink_link(void *s, int up, n1_u32 mbps) { wn_service_set_link(s, up, mbps); }
static void n1_sink_event(void *s) { wn_service_event(s); }
static const wn_backend_ops n1_ops = {n1_wn_tx, 0, 0, n1_wn_scan, n1_wn_results, n1_wn_join, n1_wn_leave, n1_wn_status};

bool hv_win_m5wn_attempted(void) { return attempted; }
bool hv_win_m5wn_claims_spi(u64 gsiv) { return attempted && gsiv == WN_GSIV; }

static bool access_page(struct exc_info *ctx, u64 address, u64 *value, bool write, int width)
{
    (void)ctx;
    unsigned page;
    u64 off;
    u32 before;
    if (address >= WN_REG_BASE && address - WN_REG_BASE < 0x4000) {
        page = 0; off = address - WN_REG_BASE;
    } else if (address >= WN_CTRL_BASE && address - WN_CTRL_BASE < 0x4000) {
        page = 1; off = address - WN_CTRL_BASE;
    } else return false;
    if (!value) return false;
    if (!write) {
        *value = 0;
        if (width == 4) value[1] = 0;
    }
    traps++;
    if (exposed && !retired && width == 2 && !(off & 3) && off <= WN_PAGE - 4) {
        before = service.status;
        if (write) wn_service_write(&service, page, off, (wn_u32)*value);
        else *value = wn_service_read(&service, page, off);
        if (before != service.status)
            printf("HV/win M5WN: status %x -> %x tx=%u rx=%u bad=%u refused=%lu\n", before, service.status,
                   service.tx_cons, service.rx_prod, service.tx_bad, refused_copies);
    }
    update_irq();
    return true; /* padding/invalid-width accesses are RAZ/WI */
}

extern bool hv_started_cpus[];
static bool contains(u64 base, u64 size, u64 addr, u64 length)
{
    return size && base <= UINT64_MAX - size && length && length <= size && addr >= base &&
           addr - base <= size - length;
}

/* Registration wire (same layout as struct hv_s1_registration): 64-byte descriptor
 * {u32 'M5WN', u32 1, u64 REG, u64 CTRL, u32 GSIV, u32 backend, 32 zero bytes}, then
 * u64 epoch, resident_base, resident_size; u32 nr_ram, nr_protected; then {base,size} pairs. */
struct wn_registration {
    u32 magic, version;
    u64 reg, ctrl;
    u32 gsiv, backend;
    u64 zero[4];
    u64 epoch, resident_base, resident_size;
    u32 nr_ram, nr_protected;
};
_Static_assert(sizeof(struct wn_registration) == 96, "M5WN registration ABI");

s64 hv_win_m5wn_configure(u64 data, u64 size)
{
    struct wn_registration c;
    if (attempted) return -1;
    if (!fp_m5wn_configure_ok(hv_win_disk_path_used())) { /* nightly-2: before any state, lease or hook */
        printf("HV/win M5WN: configure REFUSED: a disk path was used this session\n");
        return -1;
    }
    attempted = true; /* one attempt per launch, success or not */
    if (!data || (data & 7) || data > UINT64_MAX - size || size < sizeof(c) ||
        size > sizeof(c) + (HV_SM_MAX_RAM + HV_SM_MAX_PROTECTED) * 16)
        return -1;
    memcpy(&c, (const void *)data, sizeof(c));
    if (c.magic != WN_MAGIC || c.version != WN_VERSION || c.reg != WN_REG_BASE || c.ctrl != WN_CTRL_BASE ||
        c.gsiv != WN_GSIV || (c.backend != 1 && c.backend != 2) || c.zero[0] || c.zero[1] || c.zero[2] || c.zero[3] || !c.epoch ||
        !c.nr_ram || c.nr_ram > HV_SM_MAX_RAM || !c.nr_protected || c.nr_protected > HV_SM_MAX_PROTECTED ||
        size != sizeof(c) + (u64)(c.nr_ram + c.nr_protected) * 16)
        return -1;
    if (!contains(c.resident_base, c.resident_size, (u64)_base, (u64)_end - (u64)_base) ||
        !contains(c.resident_base, c.resident_size, (u64)&service, sizeof(service)))
        return -1;
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i]) return -1;
    if (!hv_win_vgic_enabled() || hv_win_pl011_gsiv() == WN_GSIV || hv_win_storage_claims_spi(WN_GSIV) ||
        !hv_vgic3_spi_unassigned(WN_GSIV) || hv_pt_walk(WN_REG_BASE) || hv_pt_walk(WN_CTRL_BASE))
        return -1;
    {
        const struct hv_sm_range *ram = (const void *)(data + sizeof(c));
        const struct hv_sm_range *prot = ram + c.nr_ram;
        for (u32 i = 0; i < c.nr_ram; i++) {
            if (!ram[i].size || ram[i].base > UINT64_MAX - ram[i].size) return -1;
            if ((ram[i].base < WN_REG_BASE + 0x4000 && WN_REG_BASE < ram[i].base + ram[i].size) ||
                (ram[i].base < WN_CTRL_BASE + 0x4000 && WN_CTRL_BASE < ram[i].base + ram[i].size))
                return -1;
        }
        const struct hv_sm_config cfg = {.epoch = c.epoch, .resident = {c.resident_base, c.resident_size},
                                         .ram = ram, .protected = prot, .nr_ram = c.nr_ram,
                                         .nr_protected = c.nr_protected};
        if (!hv_storage_mem_register(&memory, &cfg)) return -1;
        epoch = c.epoch;
    }
    plat.ctx = 0;
    plat.guest_read = g_read;
    plat.guest_write = g_write;
    plat.region_ok = g_region;
    if (c.backend == 2) {
        /* real N1 (hv_win_n1.c brought the radio up before this call) */
        struct n1 *core = hv_win_n1_core();
        if (!core) return -1;
        wn_service_init(&service, &plat, &n1_ops, core, core->mac, 0);
        core->sink = &service; core->sink_rx = n1_sink_rx; core->sink_link = n1_sink_link; core->sink_event = n1_sink_event;
        backend_n1 = true;
    } else {
        wn_service_init(&service, &plat, &wn_fake_ops, &fake, fake_mac, WN_F_FAKE);
        wn_fake_init(&fake, &service, 1, 1);
    }
    if (hv_map_hook(WN_REG_BASE, access_page, 0x4000) < 0 || hv_map_hook(WN_CTRL_BASE, access_page, 0x4000) < 0)
        return -1;
    exposed = true;
    update_irq();
    if (backend_n1)
        printf("HV/win M5WN: N1 backend, REG %lx CTRL %lx GSIV %u\n", (u64)WN_REG_BASE, (u64)WN_CTRL_BASE, WN_GSIV);
    else
        printf("HV/win M5WN: fake backend (gateway 192.168.77.1), REG %lx CTRL %lx GSIV %u (no Apple hardware)\n",
               (u64)WN_REG_BASE, (u64)WN_CTRL_BASE, WN_GSIV);
    return 0;
}

u64 hv_win_m5wn_stat(u64 index)
{
    switch (index) {
    case 0: return service.status | (u64)attempted << 8 | (u64)exposed << 9 | (u64)retired << 10;
    case 1: return service.stats.tx_packets;
    case 2: return service.stats.rx_packets;
    case 3: return traps;
    case 4: return refused_copies;
    case 7: return memory.state;
    case 5: return fake.dhcp_acks | (u64)fake.icmp_replies << 16 | (u64)fake.arp_replies << 32;
    case 6: return service.stats.faults;
    default: return 0;
    }
}

void hv_win_m5wn_tick(void)
{
    if (!exposed || retired || smp_id() != boot_cpu_idx) return;
    /* N1 RX frames arrive from hv_win_n1_tick() (each guest copy takes its own lease) */
    update_irq();
}

void hv_win_m5wn_retire(void)
{
    if (!attempted) return;
    retired = true;
    wn_service_reset(&service);
    hv_storage_mem_retire(&memory);
    update_irq();
}
