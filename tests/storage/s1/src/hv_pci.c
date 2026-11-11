/* SPDX-License-Identifier: MIT */
/* Adapted from paulsmir/m1n1_windows@9cd80ac src/hv_pci.c. Reuses its
 * one-function config identity/layout and byte-write merging, with explicit
 * reserved BAR bounds, mapping failure, width, BME and INTx admission. */
#include "model.h"
#include "ram_disk.h"
#include <string.h>
#define PCI_VENDOR_ID 0x1B36
#define PCI_DEVICE_ID 0x0010
#define PCI_CLASS_CODE 0x010802
#define PCI_REVISION 0x02
#define BAR0_TYPE_BITS 0x4
#define BAR0_ADDR_MASK 0xFF000000u

static bool extent(u64 start,u64 size) {return size && start<=UINT64_MAX-(size-1);}
static bool contains(u64 start,u64 size,u64 addr,u64 length)
{return extent(start,size) && length && addr>=start && length<=size && addr-start<=size-length;}
static u64 bar0_addr(struct s1_model *m)
{return ((u64)m->bar_hi<<32)|(m->bar_lo&BAR0_ADDR_MASK);}
static bool bar_reeval(struct s1_model *m)
{
    u64 addr=bar0_addr(m);
    bool sizing=m->bar_hi==UINT32_MAX || (m->bar_lo&BAR0_ADDR_MASK)==BAR0_ADDR_MASK;
    bool requested=(m->command&S1_MEM) && addr && !sizing;
    bool want=requested && !(addr&(S1_BAR_SIZE-1)) &&
        contains(m->window,m->window_size,addr,S1_BAR_SIZE);
    if(m->mapped && (!want || addr!=m->mapped_base)) {
        m->mapped=false;s1_reset_controller(m);m->unmap_calls++;
        if(!m->ops->unmap(m->opaque,m->mapped_base,S1_BAR_SIZE)) {
            m->faulted=true;return false;
        }
        m->mapped_base=0;
    }
    if(want && !m->mapped) {
        m->map_calls++;
        if(!m->ops->map(m->opaque,addr,S1_BAR_SIZE)) {
            m->faulted=true;s1_recompute_irq(m);return false;
        }
        m->mapped_base=addr;m->mapped=true;s1_reset_controller(m);
    }
    s1_recompute_irq(m);
    return !requested || want;
}
static u32 cfg_read_dword(struct s1_model *m,u32 reg)
{
    switch(reg) {
    case 0:return (PCI_DEVICE_ID<<16)|PCI_VENDOR_ID;
    case 4:return m->command;
    case 8:return (PCI_CLASS_CODE<<8)|PCI_REVISION;
    case 0x0c:return 0;
    case 0x10:return (m->bar_lo&BAR0_ADDR_MASK)|BAR0_TYPE_BITS;
    case 0x14:return m->bar_hi;
    case 0x2c:return (PCI_DEVICE_ID<<16)|PCI_VENDOR_ID;
    case 0x34:return 0; /* no MSI/MSI-X/ITS capability */
    case 0x3c:return 0x100|m->intr_line;
    default:return 0;
    }
}
static bool cfg_write(struct s1_model *m,u32 reg,u32 boff,unsigned bytes,u64 data)
{
    u32 mask=bytes==4?UINT32_MAX:((1u<<(bytes*8))-1)<<(boff*8);
    u32 merged=(cfg_read_dword(m,reg)&~mask)|((u32)(data<<(boff*8))&mask);
    switch(reg) {
    case 4:m->command=merged&(S1_MEM|S1_BME|S1_INTX_DISABLE);return bar_reeval(m);
    case 0x10:m->bar_lo=merged&0xfffffff0;return bar_reeval(m);
    case 0x14:m->bar_hi=merged;return bar_reeval(m);
    case 0x3c:m->intr_line=(u8)merged;return true;
    default:return true;
    }
}
bool s1_init(struct s1_model *m,const struct s1_ops *ops,void *opaque,
             u64 ecam,u64 window,u64 window_size)
{
    /* Caller supplies a zero-initialized object. Refuse live re-init; an
     * ambiguous failed mapping must not be retried through the constructor. */
    if(!m || m->initialized || !ops || !ops->guest_span || !ops->map || !ops->unmap || !ops->irq ||
        !s0_disk_valid(&s0_fixture) || !extent(ecam,S1_ECAM_SIZE) ||
        !extent(window,window_size) || ecam%S1_ECAM_SIZE || window%S1_BAR_SIZE ||
        window_size%S1_BAR_SIZE || window_size<S1_BAR_SIZE) return false;
    /* Overflow-safe overlap using subtraction, including end == 2^64. */
    if(ecam<=window ? window-ecam<S1_ECAM_SIZE : ecam-window<window_size) return false;
    memset(m,0,sizeof(*m));s1dg_init(m);m->ops=ops;m->opaque=opaque;
    m->ecam=ecam;m->window=window;m->window_size=window_size;
    m->intr_line=0xff;m->initialized=true;s1_reset_controller(m);return true;
}
static bool config_access(struct s1_model *m,u64 address,unsigned bytes,bool write,u64 *value);
bool s1_config(struct s1_model *m,u64 address,unsigned bytes,bool write,u64 *value)
{
    bool ok=config_access(m,address,bytes,write,value);
    /* r2.1 S1DG: only for an initialized model whose ECAM contains the access. */
    if(m && m->initialized && value && contains(m->ecam,S1_ECAM_SIZE,address,bytes?bytes:1))
        s1dg_access(m,1,address-m->ecam,bytes,write,ok,(ok||write)?*value:0); /* r2.2: failed read logs 0 */
    return ok;
}
static bool config_access(struct s1_model *m,u64 address,unsigned bytes,bool write,u64 *value)
{
    if(!m || !m->initialized || m->faulted || !value ||
        (bytes!=1 && bytes!=2 && bytes!=4) || address%bytes ||
        !contains(m->ecam,S1_ECAM_SIZE,address,bytes))return false;
    u64 off=address-m->ecam;
    u32 dev=(off>>15)&0x1f,fn=(off>>12)&7,reg=off&0xffc,boff=off&3;
    if(boff+bytes>4)return false;
    if(dev || fn){if(!write)*value=bytes==4?UINT32_MAX:(1u<<(8*bytes))-1;return true;}
    if(write)return cfg_write(m,reg,boff,bytes,*value);
    u32 mask=bytes==4?UINT32_MAX:(1u<<(8*bytes))-1;
    *value=(cfg_read_dword(m,reg)>>(boff*8))&mask;return true;
}
bool s1_mmio(struct s1_model *m,u64 address,unsigned bytes,bool write,u64 *value)
{
    if(!m || !m->initialized || m->faulted || !m->mapped || !(m->command&S1_MEM) ||
        !value || !contains(m->mapped_base,S1_BAR_SIZE,address,bytes)) {
        if(m && m->initialized) m->diag.mmio_rejected++;
        return false;
    }
    bool ok=s1_nvme_access(m,(u32)(address-m->mapped_base),bytes,write,value);
    s1dg_access(m,0,address-m->mapped_base,bytes,write,ok,*value);
    return ok;
}
