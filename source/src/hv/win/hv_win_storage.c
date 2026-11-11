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

static struct s1_model controller;
static struct hv_storage_mem memory;
static bool attempted, ready, frames_reserved;
static u64 epoch, dispatches, rejections, lease_failures;
extern char _base[], _end[];

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
    if(opaque==&memory)hv_vgic3_set_spi_level(HV_S1_GSIV,level);
}
static void publish(void *opaque)
{
    (void)opaque;dma_wmb();
}
static const struct s1_ops ops={
    .guest_span=guest_span,.map=logical_map,.unmap=logical_unmap,.irq=irq,.publish=publish,
};
void hv_win_storage_retire(void)
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
    s1_backend_poll();
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
static bool access(struct exc_info *ctx,u64 address,u64 *value,bool write,int width)
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
s64 hv_win_storage_prepare(u64 data,u64 size)
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
       !hv_win_vgic_enabled() || hv_win.nr_vcpus!=1 || hv_win.vcpu_cpu[0]!=boot_cpu_idx ||
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
