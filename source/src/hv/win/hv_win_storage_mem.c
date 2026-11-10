/* SPDX-License-Identifier: MIT */
#include "hv_win_storage_mem.h"
#include "hv_win.h"
#include "hv.h"
#include "cpu_regs.h"
#include "smp.h"
#include "utils.h"

extern spinlock_t bhl;
extern u64 hv_cpus_in_guest;
extern bool hv_started_cpus[MAX_CPUS];

static bool sm_range_valid(struct hv_sm_range r)
{
    return r.size && r.base <= UINT64_MAX - r.size;
}

static bool sm_contains(struct hv_sm_range r, u64 a, u64 n)
{
    return a >= r.base && n <= r.size && a - r.base <= r.size - n;
}

static bool sm_overlap(struct hv_sm_range a, struct hv_sm_range b)
{
    return a.base < b.base + b.size && b.base < a.base + a.size;
}

static bool sm_single_cpu(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS && cpu == boot_cpu_idx &&
           hv_win_vgic_enabled() && hv_win.nr_vcpus == 1 && hv_win.vcpu_cpu[0] == cpu;
}

static bool sm_dispatch_context(const struct hv_storage_mem *m)
{
    int cpu = smp_id();
    if (cpu != m->owner_cpu || !sm_single_cpu(cpu) ||
        __atomic_load_n(&bhl.lock, __ATOMIC_ACQUIRE) != cpu || bhl.count <= 0 ||
        __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE) ||
        (mrs(DAIF) & (BIT(7) | BIT(6))) != (BIT(7) | BIT(6)))
        return false;
    for (int i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i] != (i == cpu))
            return false;
    return true;
}

bool hv_storage_mem_register(struct hv_storage_mem *m, const struct hv_sm_config *c)
{
    if (!m || m->state != HV_SM_EMPTY)
        return false;
    m->state = HV_SM_DEAD; /* Any failed first registration consumes the object. */
    int cpu = smp_id();
    if (!c || !c->epoch || !sm_single_cpu(cpu) || hv_cpus_in_guest ||
        !c->ram || !c->protected || !c->nr_ram || c->nr_ram > HV_SM_MAX_RAM ||
        !c->nr_protected || c->nr_protected > HV_SM_MAX_PROTECTED ||
        !sm_range_valid(c->resident))
        return false;
    for (int i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return false;
    for (size_t i = 0; i < c->nr_protected; i++)
        if (!sm_range_valid(c->protected[i]))
            return false;
    for (size_t i = 0; i < c->nr_ram; i++) {
        struct hv_sm_range r = c->ram[i];
        if (!sm_range_valid(r) || ((r.base | r.size) & (HV_SM_PAGE - 1)) ||
            sm_overlap(r, c->resident) ||
            (i && r.base < c->ram[i-1].base + c->ram[i-1].size))
            return false;
    }
    m->epoch = c->epoch;
    m->owner_cpu = cpu;
    m->resident = c->resident;
    m->nr_ram = c->nr_ram;
    m->nr_protected = c->nr_protected;
    for (size_t i = 0; i < c->nr_ram; i++)
        m->ram[i] = c->ram[i];
    for (size_t i = 0; i < c->nr_protected; i++)
        m->protected[i] = c->protected[i];
    m->state = HV_SM_READY;
    return true;
}

bool hv_storage_mem_begin(struct hv_storage_mem *m, u64 epoch)
{
    if (!m || m->state != HV_SM_READY || !epoch || epoch != m->epoch)
        return false;
    if (!sm_dispatch_context(m)) {
        m->state = HV_SM_DEAD;
        return false;
    }
    m->state = HV_SM_LEASED;
    return true;
}

bool hv_storage_mem_end(struct hv_storage_mem *m)
{
    if (!m || m->state != HV_SM_LEASED)
        return false;
    if (!sm_dispatch_context(m)) {
        m->state = HV_SM_DEAD;
        return false;
    }
    m->state = HV_SM_READY;
    return true;
}

void hv_storage_mem_retire(struct hv_storage_mem *m)
{
    if (m)
        m->state = HV_SM_DEAD;
}

void *hv_storage_guest_span(void *opaque, u64 ipa, size_t length, bool write)
{
    struct hv_storage_mem *m = opaque;
    (void)write; /* Both directions deliberately require RW: stricter than read permission. */
    if (!m || m->state != HV_SM_LEASED)
        return NULL;
    if (!sm_dispatch_context(m)) {
        m->state = HV_SM_DEAD;
        return NULL;
    }
    if (!ipa || !length || length > HV_SM_MAX_SPAN || ipa > UINT64_MAX - length)
        return NULL;
    bool owned = false;
    for (size_t i = 0; i < m->nr_ram; i++)
        if (sm_contains(m->ram[i], ipa, length)) {
            owned = true;
            break;
        }
    if (!owned)
        return NULL;
    struct hv_sm_range span = {ipa, length};
    if (sm_overlap(span, m->resident))
        return NULL;
    for (size_t i = 0; i < m->nr_protected; i++)
        if (sm_overlap(span, m->protected[i]))
            return NULL;

    u64 end = ipa + length;
    /* At most nine 16 KiB leaves for an unaligned 128 KiB request. */
    unsigned walked = 0;
    for (u64 a = ipa; a < end;) {
        struct hv_s2_leaf leaf;
        if (++walked > 9 || !hv_pt_resolve_leaf(a, &leaf))
            return NULL;
        u64 d = leaf.descriptor;
        const u64 address = GENMASK(49, 14);
        const u64 tracked = BIT(55), flips = GENMASK(58, 56), xn = BIT(54);
        const u64 expected = BIT(10) | (3UL << 8) | (3UL << 6) | (15UL << 2) | 1;
        /* Ordinary m1n1 normal-memory leaves only, no Device/NC/traces or unknown
         * attributes. L2 type0/L3 type1 is checked separately by the resolver. */
        if ((d & ~(address | tracked | flips | xn | BIT(1))) != expected ||
            (leaf.size != HV_SM_PAGE && leaf.size != (1UL << 25)) ||
            leaf.ipa_base != leaf.pa_base || a < leaf.ipa_base ||
            a - leaf.ipa_base >= leaf.size ||
            ((d & tracked) && (!(d & xn) || leaf.size != HV_SM_PAGE)) ||
            (!(d & tracked) && (d & flips)))
            return NULL;
        u64 n = leaf.size - (a - leaf.ipa_base);
        a += n < end - a ? n : end - a;
    }
    return (void *)ipa;
}
