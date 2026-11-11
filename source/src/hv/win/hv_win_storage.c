/* SPDX-License-Identifier: MIT */
/* S1 storage frontend glue. r3: the backend is the immutable RAM fixture unless the native
 * read-only window was selected (hv_win_storage_native.c) immediately before PREP. */
#include "hv_win_storage.h"
#include "hv_win_storage_mem.h"
#include "hv_win.h"
#include "s1/model.h"
#include "s1/ram_disk.h"
#include "hv.h"
#include "smp.h"
#include "utils.h"
#include "string.h"
#include "cpu_regs.h" /* nightly-7: CNTPCT_EL0/CNTFRQ_EL0 for the SMP hold-time evidence */

static struct s1_model controller;
static struct hv_storage_mem memory;
static bool attempted, ready, frames_reserved;
static u64 epoch, dispatches, rejections, lease_failures;
extern char _base[], _end[];
#ifdef VNVME_HOST_TEST
#define SMPS_BHL_MINE(me) ((void)(me), true) /* host harnesses: the caller is modelled as holding bhl */
#else
extern spinlock_t bhl; /* nightly-7: breadcrumbs (hv_win_storage_mem.c declares it the same way) */
#define SMPS_BHL_MINE(me) (__atomic_load_n(&bhl.lock, __ATOMIC_ACQUIRE) == (me) && bhl.count > 0)
#endif
/* smp-storage (SMP-STORAGE-DESIGN.md): the big storage lock. Every entry into the emulated controller, the
 * lease and the backend (MMIO hooks, tick, PREP, retire, native select/quiesce, canary) holds it for the whole
 * synchronous call. Lock order: bhl (hv_exc_entry, before any hook or tick) -> storage -> vgic_lock (irq
 * callback). Recursive for its owner CPU (retire runs inside a hook). Never released inside a call. */
static struct { s64 owner; s64 depth; } storage_lock = {-1, 0};
static u32 storage_max_cpus = 1; /* launcher cap (P_HV_WIN_STORAGE_MAX_CPUS); 1 = the r4.14 disk path */
/* nightly-7: bound on one data command's wait while more than one vCPU is admitted (default 1 s; launcher-set
 * with the cap, 0.1..30 s). Cap 1 keeps the r4.14 30 s WRITE/FLUSH bounds (ans_ro.c). smp-storage review s5. */
static u32 storage_smp_timeout_us = 1000000;
/* nightly-7 run-time evidence for the SMP disk path (statistics only: nothing reads them back into a decision).
 * Written by the CPU that holds the storage lock, except contention (lock-free atomics); printed in the SM
 * status line (hv_win_status.c) when the cap is above 1. Ticks are CNTPCT_EL0. */
#ifdef VNVME_HOST_TEST
#define SMPS_NOW() 0UL /* host harnesses: no counter; hold times read 0 */
#define SMPS_FREQ() 0UL
#else
#define SMPS_NOW() mrs(CNTPCT_EL0)
#define SMPS_FREQ() mrs(CNTFRQ_EL0)
#endif
#define SMPS_CPUS 24
static struct hv_storage_smp_stats smps;
void hv_storage_lock_take(void)
{
    s64 me = smp_id();
    if (__atomic_load_n(&storage_lock.owner, __ATOMIC_RELAXED) == me) {
        storage_lock.depth++;
        return;
    }
    u64 spins = 0, t0 = 0;
    for (;;) {
        s64 none = -1;
        if (__atomic_compare_exchange_n(&storage_lock.owner, &none, me, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        if (!spins++)
            t0 = SMPS_NOW();
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    storage_lock.depth = 1;
    if (spins) { /* nightly-7: contention evidence (the review expects none: every entry is nested in bhl) */
        u64 w = SMPS_NOW() - t0;
        smps.contended++;
        if (w > smps.spin_max_ticks)
            smps.spin_max_ticks = w;
    }
}
void hv_storage_lock_release(void)
{
    if (__atomic_load_n(&storage_lock.owner, __ATOMIC_RELAXED) != smp_id() || storage_lock.depth <= 0)
        return; /* not the owner: unreachable (every take/release pair is lexical) */
    if (!--storage_lock.depth)
        __atomic_store_n(&storage_lock.owner, -1, __ATOMIC_RELEASE);
}
bool hv_storage_lock_held(void)
{
    return __atomic_load_n(&storage_lock.owner, __ATOMIC_RELAXED) == smp_id() && storage_lock.depth > 0;
}
u32 hv_win_storage_max_cpus(void)
{
    return storage_max_cpus;
}
/* Once, before PREP; hv_win.c also refuses it after any disk path was touched or a vCPU started. */
bool hv_win_storage_set_max_cpus(u32 n)
{
    if (attempted || storage_max_cpus != 1 || n < 1)
        return false;
    storage_max_cpus = n;
    return true;
}
/* nightly-7: with the cap (hv_win.c), before PREP; 0.1..30 s. */
bool hv_win_storage_set_smp_timeout(u32 us)
{
    if (attempted || us < 100000 || us > 30000000)
        return false;
    storage_smp_timeout_us = us;
    return true;
}
/* ans_ro.c (through native_link.c): the data-command wait bound in us, 0 = keep the r4.14 bounds (cap 1). */
u32 hv_win_storage_smp_cmd_timeout_us(void)
{
    return storage_max_cpus > 1 ? storage_smp_timeout_us : 0;
}
const struct hv_storage_smp_stats *hv_win_storage_smp_stats(void)
{
    return &smps;
}
/* nightly-7: breadcrumbs at every storage entry, taken with the storage lock held. An MMIO access or tick must
 * also hold bhl on this CPU (hv_exc_entry / hv_tick); PREP and the hv_init retire run on the boot proxy before
 * guest start without bhl, by design, and are counted under "other". */
static void smps_enter(u32 site)
{
    s64 me = smp_id();
    if (me >= 0 && me < SMPS_CPUS)
        smps.entries[site][me]++;
    if (!SMPS_BHL_MINE(me))
        smps.no_bhl[site]++;
}
static void smps_hold(u32 site, u64 t0, u64 address)
{
    u64 d = SMPS_NOW() - t0;
    if (d > smps.hold_max_ticks[site]) {
        smps.hold_max_ticks[site] = d;
        if (site == HV_SMPS_ACCESS)
            smps.hold_max_addr = address;
    }
    if (site == HV_SMPS_ACCESS) {
        u64 f = smps.freq ? smps.freq : (smps.freq = SMPS_FREQ());
        if (f) { /* holds >= 1 ms, 10 ms, 100 ms, 1 s */
            u64 ms = d * 1000 / f;
            for (int i = 0; i < 4; i++)
                if (ms >= (i == 0 ? 1u : i == 1 ? 10u : i == 2 ? 100u : 1000u))
                    smps.hold_hist[i]++;
        }
    }
}
/* nightly-7: the model's callbacks (guest memory lease, IRQ line, publish) only ever run inside a locked entry */
static void smps_op_check(void)
{
    if (!hv_storage_lock_held())
        smps.ops_unlocked++;
}
/* The backend may be driven from this CPU on behalf of a vCPU (native_link.c s1n_storage_context): more
 * than one vCPU admitted and this CPU holds the storage lock. The lease adds the vCPU/bhl/DAIF checks. */
bool hv_win_storage_smp_context(void)
{
    return storage_max_cpus > 1 && hv_storage_lock_held();
}

static bool contains(u64 base,u64 size,u64 addr,u64 length)
{
    return size && base<=UINT64_MAX-size && length && length<=size &&
           addr>=base && addr-base<=size-length;
}
static bool frames_empty(u64 base,u64 size)
{
    for(u64 off=0;off<size;off+=0x4000)
        if(hv_pt_walk(base+off))return false;
    return true;
}
static void *guest_span(void *opaque,u64 address,size_t length,bool write)
{
    if(opaque!=&memory)return NULL;
    smps_op_check(); /* nightly-7 */
    return hv_storage_guest_span(&memory,address,length,write);
}
static bool logical_map(void *opaque,u64 address,size_t size)
{
    /* Hooks are installed once before guest start. Model BAR changes never
     * mutate S2 while a guest-memory lease exists; only this aperture decodes. */
    return opaque==&memory && frames_reserved && address==HV_S1_BAR && size==S1_BAR_SIZE;
}
static bool logical_unmap(void *opaque,u64 address,size_t size)
{
    return opaque==&memory && frames_reserved && address==HV_S1_BAR && size==S1_BAR_SIZE;
}
static void irq(void *opaque,bool level)
{
    smps_op_check(); /* nightly-7 */
    if(opaque==&memory)hv_vgic3_set_spi_level(HV_S1_GSIV,level);
}
static void publish(void *opaque)
{
    (void)opaque;dma_wmb();
}
static const struct s1_ops ops={
    .guest_span=guest_span,.map=logical_map,.unmap=logical_unmap,.irq=irq,.publish=publish,
};
static void storage_retire_locked(void);
void hv_win_storage_retire(void)
{
    hv_storage_lock_take(); /* smp-storage */
    smps_enter(HV_SMPS_OTHER); /* nightly-7 */
    storage_retire_locked();
    hv_storage_lock_release();
}
static void storage_retire_locked(void)
{
    /* r3.1 R1: a native backend selected before PREP must still be quiesced. */
    if(!attempted){s1_backend_retire();return;}
    /* Outstanding borrowed pointers expire before any future mapping/launch
     * ownership change. This object cannot be re-registered or auto-retried. */
    hv_storage_mem_retire(&memory);
    ready=false;
    if(controller.initialized){controller.faulted=true;s1_recompute_irq(&controller);}
    /* r3: native backend quiesces ANS here (stage-1 sequence); fixture backend: no-op. */
    s1_backend_retire();
}
/* r3: EL2 tick (single vCPU, under bhl): keep the native backend's mailbox drained.
 * r3.1 R1: from native selection on, PREP or not (fixture backend: no poll hook). */
void hv_win_storage_tick(void)
{
    hv_storage_lock_take(); /* smp-storage */
    smps_enter(HV_SMPS_TICK); /* nightly-7 */
    u64 t0 = SMPS_NOW();
    s1_backend_poll();
    smps_hold(HV_SMPS_TICK, t0, 0);
    hv_storage_lock_release();
}
bool hv_win_storage_claims_spi(u32 gsiv)
{
    return attempted && gsiv==HV_S1_GSIV;
}
/* r4.8: is [addr, addr+size) entirely inside ONE registered guest-RAM range? Used by the link lock to
 * admit host exception-handler access to guest memory while the storage session is armed. Reads only the
 * ranges the launcher registered at PREP (identity IPA==PA); never the controller MMIO, resident or
 * protected ranges (those are not in ram[]). False until a registration exists. */
bool hv_win_storage_guest_ram_contains(u64 addr, u64 size)
{
    if(!attempted || !size || addr>UINT64_MAX-size)
        return false;
    for(size_t i=0;i<memory.nr_ram;i++) {
        struct hv_sm_range r=memory.ram[i];
        if(r.size && addr>=r.base && size<=r.size && addr-r.base<=r.size-size)
            return true;
    }
    return false;
}
static bool access_locked(struct exc_info *ctx,u64 address,u64 *value,bool write,int width);
/* smp-storage: an MMIO hook may run on any vCPU's CPU (under bhl); the whole access holds the storage lock. */
static bool access(struct exc_info *ctx,u64 address,u64 *value,bool write,int width)
{
    hv_storage_lock_take();
    smps_enter(HV_SMPS_ACCESS); /* nightly-7 */
    u64 t0=SMPS_NOW();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);
    hv_storage_lock_release();
    return ok;
}
static bool access_locked(struct exc_info *ctx,u64 address,u64 *value,bool write,int width)
{
    (void)ctx;
    dispatches++;
    if(!ready || width<0 || width>3 || !value){rejections++;return false;}
    if(!hv_storage_mem_begin(&memory,epoch)) {
        lease_failures++;hv_win_storage_retire();return false;
    }
    dma_rmb();
    bool ok;
    if(address>=HV_S1_ECAM && address-HV_S1_ECAM<S1_ECAM_SIZE)
        ok=s1_config(&controller,address,1u<<width,write,value);
    else
        ok=s1_mmio(&controller,address,1u<<width,write,value);
    /* No model/backend callback retains a returned pointer, remaps S2, invokes
     * proxy or releases bhl. Close lease after the complete synchronous call. */
    if(!hv_storage_mem_end(&memory)) {
        lease_failures++;hv_win_storage_retire();return false;
    }
    if(controller.faulted)hv_win_storage_retire();
    if(!ok)rejections++;
    return ok;
}
static s64 storage_prepare_locked(u64 data,u64 size);
s64 hv_win_storage_prepare(u64 data,u64 size)
{
    hv_storage_lock_take(); /* smp-storage */
    smps_enter(HV_SMPS_OTHER); /* nightly-7 */
    s64 r=storage_prepare_locked(data,size);
    hv_storage_lock_release();
    return r;
}
static s64 storage_prepare_locked(u64 data,u64 size)
{
    if(attempted)return -1;
    attempted=true;
    if(!data || (data&7) || data>UINT64_MAX-size || size<sizeof(struct hv_s1_registration) ||
        size>sizeof(struct hv_s1_registration)+(HV_SM_MAX_RAM+HV_SM_MAX_PROTECTED)*16)
        goto fail;
    struct hv_s1_registration c;
    memcpy(&c,(const void *)data,sizeof(c));
    const struct hv_s1_descriptor *d=&c.device;
    if(d->magic!=HV_S1_MAGIC || d->version!=1 || d->ecam!=HV_S1_ECAM || d->bar!=HV_S1_BAR ||
       d->bar_size!=S1_BAR_SIZE || d->gsiv!=HV_S1_GSIV || d->flags!=7 ||
       d->block_size!=4096 || d->reserved0 || d->blocks!=s1_backend_blocks() || !d->blocks || d->reserved1 || !c.epoch ||
       !c.nr_ram || c.nr_ram>HV_SM_MAX_RAM || !c.nr_protected || c.nr_protected>HV_SM_MAX_PROTECTED ||
       size!=sizeof(c)+(u64)(c.nr_ram+c.nr_protected)*16 ||
       !hv_win_vgic_enabled() || hv_win.nr_vcpus<1 || hv_win.nr_vcpus>storage_max_cpus || hv_win.vcpu_cpu[0]!=boot_cpu_idx ||
       hv_win_pl011_gsiv()==HV_S1_GSIV || !hv_vgic3_spi_unassigned(HV_S1_GSIV))goto fail;
    if(!contains(c.resident_base,c.resident_size,(u64)_base,(u64)_end-(u64)_base) ||
       !contains(c.resident_base,c.resident_size,(u64)&controller,sizeof(controller)) ||
       !contains(c.resident_base,c.resident_size,(u64)s0_fixture.bytes,s0_fixture.byte_length))goto fail;
    const struct hv_sm_range *ram=(const void *)(data+sizeof(c));
    const struct hv_sm_range *protected=ram+c.nr_ram;
    /* Exact synthetic frames must not overlap declared guest RAM. */
    for(u32 i=0;i<c.nr_ram;i++) {
        if(!ram[i].size || ram[i].base>UINT64_MAX-ram[i].size)goto fail;
        if((ram[i].base<HV_S1_ECAM+S1_ECAM_SIZE && HV_S1_ECAM<ram[i].base+ram[i].size) ||
           (ram[i].base<HV_S1_BAR+S1_BAR_SIZE && HV_S1_BAR<ram[i].base+ram[i].size))goto fail;
    }
    const struct hv_sm_config cfg={.epoch=c.epoch,
        .resident={c.resident_base,c.resident_size},.ram=ram,.protected=protected,
        .nr_ram=c.nr_ram,.nr_protected=c.nr_protected};
    if(!hv_storage_mem_register(&memory,&cfg))goto fail;
    if(!frames_empty(HV_S1_ECAM,S1_ECAM_SIZE) || !frames_empty(HV_S1_BAR,S1_BAR_SIZE))goto fail;
    /* Fresh registration only. A partial hook failure leaves inert fail-closed
     * hooks; no unmap of an uncertain/foreign frame and no native fallback. */
    if(hv_map_hook(HV_S1_ECAM,access,S1_ECAM_SIZE)<0 || hv_map_hook(HV_S1_BAR,access,S1_BAR_SIZE)<0)
        goto fail;
    frames_reserved=true;epoch=c.epoch;
    if(!s1_init(&controller,&ops,&memory,HV_S1_ECAM,HV_S1_BAR,S1_BAR_SIZE))goto fail;
    u64 value=(u32)HV_S1_BAR;
    if(!s1_config(&controller,HV_S1_ECAM+0x10,4,true,&value))goto fail;
    value=HV_S1_BAR>>32;
    if(!s1_config(&controller,HV_S1_ECAM+0x14,4,true,&value))goto fail;
    value=S1_MEM|S1_BME;
    if(!s1_config(&controller,HV_S1_ECAM+4,2,true,&value) || !controller.mapped)goto fail;
    sysop("dsb ishst");sysop("isb");
    ready=true;
    printf("HV/win S1: %s, ECAM 0x%lx BAR 0x%lx SPI %u, %lu x 4096, epoch 0x%lx\n",
        s1_backend_name(),HV_S1_ECAM,HV_S1_BAR,HV_S1_GSIV,(unsigned long)d->blocks,(unsigned long)epoch);
    return 0;
fail:
    hv_win_storage_retire();
    printf("HV/win S1: preparation rejected; fresh resident required\n");
    return -1;
}
u64 hv_win_storage_stat(u64 index)
{
    switch(index) {
    case 0:return ((u64)HV_S1_MAGIC<<32)|((u64)attempted<<2)|((u64)frames_reserved<<1)|ready;
    case 1:return memory.state;
    case 2:return dispatches;
    case 3:return rejections;
    case 4:return lease_failures;
    case 5:return controller.backend_reads;
    case 6:return controller.queue.stats.commands;
    case 7:return controller.queue.stats.completions;
    case 8:return controller.irq_changes;
    case 9:return ((u64)controller.regs.cc<<32)|controller.regs.csts;
    case 10:return controller.external_irq;
    case 11:return epoch;
    case 12:return controller.queue.refused_reads; /* r4.6: nonterminal guest READ range errors */
    default:return UINT64_MAX;
    }
}

/* r4.11 diagnostics: read-only view of the model for hv_win_status.c (firmware-only reporting). */
const struct s1_model *hv_win_storage_model(bool *is_attempted, bool *is_ready)
{
    *is_attempted = attempted;
    *is_ready = ready;
    return &controller;
}
