/* SPDX-License-Identifier: MIT */
#include "hv_win_storage_mem.h"
#include "hv_win_storage.h"
#include "hv_win.h"
#include "hv.h"
#include "cpu_regs.h"
#include "smp.h"
#include "utils.h"

extern spinlock_t bhl;

struct hv_sm_refusal hv_sm_last_refusal;
static void *sm_refuse(u64 why, u64 ipa, u64 length, u64 detail)
{
    hv_sm_last_refusal.why = why;
    hv_sm_last_refusal.ipa = ipa;
    hv_sm_last_refusal.length = length;
    hv_sm_last_refusal.detail = detail;
    if (why < HV_SM_WHY_NR)
        hv_sm_last_refusal.count[why]++;
    return NULL;
}
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
    /* smp-storage: 1..cap vCPUs, cap 1 (default) = the r4.14 nr_vcpus == 1 condition */
    return cpu >= 0 && cpu < MAX_CPUS && cpu == boot_cpu_idx && hv_win_vgic_enabled() &&
           (u32)hv_win.nr_vcpus >= 1 &&
           (u32)hv_win.nr_vcpus <= hv_win_storage_max_cpus() && hv_win.vcpu_cpu[0] == cpu;
}

/* smp-storage (cap > 1): the dispatching CPU hosts one of the admitted vCPUs and holds bhl and the storage
 * lock with IRQ/FIQ masked. Other vCPUs may run: every guest structure is copied once into resident memory
 * before it is validated or used, and stage 2 changes only under bhl (SMP-STORAGE-DESIGN.md). */
static bool sm_smp_dispatch_context(int cpu)
{
    int v = (cpu >= 0 && cpu < MAX_CPUS) ? hv_win.cpu_vcpu[cpu] : -1;
    return v >= 0 && v < MAX_CPUS && hv_win_vgic_enabled() && (u32)hv_win.nr_vcpus >= 1 &&
           (u32)hv_win.nr_vcpus <= hv_win_storage_max_cpus() && (u32)v < (u32)hv_win.nr_vcpus &&
           hv_win.vcpu_cpu[v] == cpu && __atomic_load_n(&bhl.lock, __ATOMIC_ACQUIRE) == cpu &&
           bhl.count > 0 && hv_storage_lock_held() &&
           (mrs(DAIF) & (BIT(7) | BIT(6))) == (BIT(7) | BIT(6));
}

static bool sm_dispatch_context(const struct hv_storage_mem *m)
{
    int cpu = smp_id();
    if (hv_win_storage_max_cpus() > 1)
        return sm_smp_dispatch_context(cpu);
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
    /* Both directions deliberately require RW: stricter than read permission. An execute-tracked
     * leaf counts as RW in every PMU-rewrite state but SCANNING: only RW pages are ever tracked,
     * and CODE's read-only S2AP is the rewrite's write trap, not an ownership statement. */
    if (!m || m->state != HV_SM_LEASED)
        return sm_refuse(HV_SM_WHY_STATE, ipa, length, m ? m->state : 0);
    if (!sm_dispatch_context(m)) {
        m->state = HV_SM_DEAD;
        return sm_refuse(HV_SM_WHY_CONTEXT, ipa, length, 0);
    }
    if (!ipa || !length || length > HV_SM_MAX_SPAN || ipa > UINT64_MAX - length)
        return sm_refuse(HV_SM_WHY_ARGS, ipa, length, 0);
    bool owned = false;
    for (size_t i = 0; i < m->nr_ram; i++)
        if (sm_contains(m->ram[i], ipa, length)) {
            owned = true;
            break;
        }
    if (!owned)
        return sm_refuse(HV_SM_WHY_NOT_RAM, ipa, length, 0);
    struct hv_sm_range span = {ipa, length};
    if (sm_overlap(span, m->resident))
        return sm_refuse(HV_SM_WHY_RESIDENT, ipa, length, m->resident.base);
    for (size_t i = 0; i < m->nr_protected; i++)
        if (sm_overlap(span, m->protected[i]))
            return sm_refuse(HV_SM_WHY_PROTECTED, ipa, length, m->protected[i].base);

    u64 end = ipa + length;
    /* At most nine 16 KiB leaves for an unaligned 128 KiB request. */
    unsigned walked = 0;
    for (u64 a = ipa; a < end;) {
        struct hv_s2_leaf leaf;
        if (++walked > 9 || !hv_pt_resolve_leaf(a, &leaf))
            return sm_refuse(HV_SM_WHY_LEAF_WALK, ipa, length, a);
        u64 d = leaf.descriptor;
        const u64 address = GENMASK(49, 14);
        const u64 tracked = BIT(55), flips = GENMASK(58, 56), xn = BIT(54);
        const u64 s2ap_w = BIT(7); /* S2AP 0b11 RW, 0b01 RO */
        const u64 expected = BIT(10) | (3UL << 8) | (3UL << 6) | (15UL << 2) | 1;
        /* pmu-reads: tracked DIRTY (RW+XN), CODE (RO+X) and PINNED (RW+X) are admitted;
         * SCANNING (RO+XN) is refused. A write re-arms CODE/PINNED below. */
        u64 g = (d & tracked) ? d | s2ap_w : d;
        /* Ordinary m1n1 normal-memory leaves only, no Device/NC/traces or unknown
         * attributes. L2 type0/L3 type1 is checked separately by the resolver. */
        if ((g & ~(address | tracked | flips | xn | BIT(1))) != expected ||
            (leaf.size != HV_SM_PAGE && leaf.size != (1UL << 25)) ||
            leaf.ipa_base != leaf.pa_base || a < leaf.ipa_base ||
            a - leaf.ipa_base >= leaf.size ||
            ((d & tracked) && (((d & xn) && !(d & s2ap_w)) || leaf.size != HV_SM_PAGE)) ||
            (!(d & tracked) && (d & flips)))
            return sm_refuse(((d & tracked) && (d & xn) && !(d & s2ap_w)) ? HV_SM_WHY_SCANNING
                                                                          : HV_SM_WHY_LEAF_ATTR,
                             ipa, length, d);
        u64 n = leaf.size - (a - leaf.ipa_base);
        a += n < end - a ? n : end - a;
    }
    /* pmu-reads: EL2 is about to store device data here through its own mapping, which stage 2
     * does not check. Re-arm the PMU rewrite on these pages first (no-op when it is off), so
     * code paged in from disk is rescanned at its first execution. Every write-direction
     * admission precedes the copy (hv_nvme_queue.c plan_prps/post_completion, ANSM). */
    if (write)
        hv_win_pmu_dma_write(ipa, length);
    return (void *)ipa;
}
