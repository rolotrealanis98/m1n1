/* SPDX-License-Identifier: MIT */
/* Adapted from paulsmir/m1n1_windows@9cd80ac src/hv_nvme.c. The register
 * model/CC/doorbell structure is reused; native backend, raw translation,
 * donor vGIC/LR ownership and target MMIO hooks are deliberately removed. */
#include "model.h"
#include "ram_disk.h"
#include <string.h>

#define NVME_CAP 0x00
#define NVME_VS 0x08
#define NVME_INTMS 0x0c
#define NVME_INTMC 0x10
#define NVME_CC 0x14
#define NVME_CSTS 0x1c
#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30
#define NVME_DBS 0x1000
#define CSTS_RDY 1u
#define CSTS_CFS 2u
#define CSTS_SHST_COMPLETE 8u
#define NVME_CAP_VALUE ((VNVME_MAX_QSIZE-1ULL) | (1ULL<<16) | (10ULL<<24) | (1ULL<<37))
#define NVME_VS_VALUE 0x00010300u

/* ---- r2.1 S1DG diagnostics: model-local values only, never guest memory ---- */
static u32 s1dg_now(void)
{
#ifdef VNVME_HOST_TEST
    return 0;
#else
    u64 v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return (u32)v;
#endif
}
void s1dg_init(struct s1_model *m)
{
    struct s1_diag *g=&m->diag;
    memset(g,0,sizeof(*g));
    g->magic=S1DG_MAGIC;g->version=S1DG_VERSION;g->ring_entries=S1DG_RING;
    g->entry_size=sizeof(struct s1dg_entry);g->struct_size=sizeof(*g);
}
void s1dg_event(struct s1_model *m,u8 type,u8 width,u8 qid,u8 opcode,u16 cid,u16 status,
                u64 a,u64 b,u32 c,u32 d)
{
    struct s1_diag *g=&m->diag;
    struct s1dg_entry *e=&g->ring[g->write_seq%S1DG_RING];
    e->seq=(u32)g->write_seq;e->t=s1dg_now();e->type=type;e->width=width;e->qid=qid;
    e->opcode=opcode;e->cid=cid;e->status=status;e->a=a;e->b=b;e->c=c;e->d=d;
    g->write_seq++;
}
static unsigned width_index(unsigned bytes){return bytes==1?0:bytes==2?1:bytes==4?2:3;}
void s1dg_access(struct s1_model *m,u8 kind,u64 offset,unsigned bytes,bool write,bool ok,u64 value)
{
    struct s1_diag *g=&m->diag;
    if (kind) g->cfg_access[write?1:0]++;
    else g->mmio_width[write?1:0][width_index(bytes)]++;
    if (g->mmio_first_count<S1DG_MMIO_FIRST) {
        struct s1dg_access *x=&g->mmio_first[g->mmio_first_count++];
        x->seq=(u32)g->write_seq;x->offset=(u32)offset;x->kind=kind;x->width=(u8)bytes;
        x->write=write;x->ok=ok;x->csts=m->regs.csts;x->value=value;
    }
    s1dg_event(m,(u8)(kind?(write?S1DG_CFG_W:S1DG_CFG_R):(write?S1DG_MMIO_W:S1DG_MMIO_R)),
               (u8)bytes,0,0,0,ok,offset,value,m->regs.csts,0);
}
static void s1dg_fatal(struct s1_model *m,u8 reason)
{
    struct s1_diag *g=&m->diag;
    g->fatal_count++;
    if (!g->first_fatal_reason) g->first_fatal_reason=reason;
    s1dg_event(m,S1DG_FATAL,0,0,0,0,0,0,0,reason,m->regs.csts);
}
static void s1dg_trace(void *opaque,const struct vnvme_trace_event *ev)
{
    struct s1_model *m=opaque;
    struct s1_diag *g=&m->diag;
    if (ev->type==VNVME_TRACE_FATAL) { s1dg_fatal(m,(u8)ev->status); return; }
    if (ev->type==VNVME_TRACE_SUBMISSION && ev->command) {
        const struct vnvme_command *c=ev->command;
        if (ev->qid==0) {
            if (c->opcode<32) g->admin_opcode[c->opcode]++; else {g->admin_other++;g->last_admin_other=c->opcode;}
            if (g->admin_first_count<S1DG_ADMIN_FIRST) {
                struct s1dg_admin *x=&g->admin_first[g->admin_first_count++];
                memcpy(x->sqe,c,sizeof(x->sqe));x->status=0;x->completed=0;x->dw0=0;
            }
        } else {
            if (c->opcode<32) g->io_opcode[c->opcode]++; else {g->io_other++;g->last_io_other=c->opcode;}
            if (c->opcode==VNVME_IO_WRITE) {
                g->writes++;g->last_write_lba=((u64)c->cdw11<<32)|c->cdw10;g->last_write_nlb=(c->cdw12&0xffff)+1u;
            }
        }
        s1dg_event(m,S1DG_SQE,0,(u8)ev->qid,c->opcode,c->cid,ev->status,
                   ((u64)c->cdw11<<32)|c->cdw10,c->prp1,c->nsid,c->cdw12);
        return;
    }
    if (ev->type==VNVME_TRACE_COMPLETION) {
        if (ev->status) {
            g->nonzero_status++;g->last_err_qid=ev->qid;g->last_err_cid=ev->cid;g->last_err_status=ev->status;
        }
        if (ev->cqid<2 && ev->cq_pending>g->max_cq_pending[ev->cqid]) g->max_cq_pending[ev->cqid]=ev->cq_pending;
        if (ev->qid==0)
            for (u32 i=g->admin_first_count;i-->0;) {
                struct s1dg_admin *x=&g->admin_first[i];
                u16 cid=(u16)(x->sqe[2]|(x->sqe[3]<<8));
                if (!x->completed && cid==ev->cid) {x->completed=1;x->status=ev->status;x->dw0=ev->result;break;}
            }
        s1dg_event(m,S1DG_CQE,0,(u8)ev->qid,0,ev->cid,ev->status,ev->result,ev->cqid,0,
                   (u32)ev->cq_pending|((u32)ev->sq_head<<16));
    }
}
static void *guest_span(void *opaque,u64 addr,size_t n,bool write)
{
    struct s1_model *m=opaque;
    u8 reason=0;
    void *p=NULL;
    if (!m->mapped) reason=S1DG_REF_NOT_MAPPED;
    else if (m->faulted) reason=S1DG_REF_FAULTED;
    else if (!(m->command&S1_BME)) reason=S1DG_REF_NO_BME;
    else if (!(p=m->ops->guest_span(m->opaque,addr,n,write))) reason=S1DG_REF_PROVIDER;
    if (reason) {
        m->diag.refusals[reason]++;
        s1dg_event(m,S1DG_REFUSAL,0,0,0,0,0,addr,(u64)n|((u64)write<<63),reason,0);
    }
    return p;
}
/* r3: backend indirection. Default = the immutable RAM fixture (r2.2 behaviour). */
static u64 fixture_blocks(void){return s0_fixture.block_count;}
static bool fixture_read(u64 lba,u32 blocks,void *buffer){return s0_disk_read(&s0_fixture,lba,blocks,buffer);}
static const struct s1_backend fixture_backend={
    .name="immutable RAM fixture",.blocks=fixture_blocks,.read=fixture_read,
};
static const struct s1_backend *active_backend=&fixture_backend;
static bool backend_selected;
bool s1_select_backend(const struct s1_backend *backend)
{
    if (backend_selected || !backend || !backend->name || !backend->blocks || !backend->read ||
        !backend->blocks()) return false;
    backend_selected=true;
    active_backend=backend;
    return true;
}
/* r3: a failed native selection poisons the frontend: zero blocks, every PREP refused,
 * no silent fallback to the fixture. */
static u64 dead_blocks(void){return 0;}
static bool dead_read(u64 lba,u32 blocks,void *buffer){(void)lba;(void)blocks;(void)buffer;return false;}
static const struct s1_backend dead_backend={.name="refused native window",.blocks=dead_blocks,.read=dead_read};
void s1_poison_backend(void){backend_selected=true;active_backend=&dead_backend;}
const char *s1_backend_name(void){return active_backend->name;}
u64 s1_backend_blocks(void){return active_backend->blocks();}
void s1_backend_poll(void){if (active_backend->poll) active_backend->poll();}
void s1_backend_retire(void){if (active_backend->retire) active_backend->retire();}
static bool backend_read(void *opaque,u64 lba,u32 blocks,void *buffer)
{
    struct s1_model *m=opaque;
    m->backend_reads++;
    return active_backend->read(lba,blocks,buffer);
}
static void publish(void *opaque)
{
    struct s1_model *m=opaque;
    if (m->ops->publish) m->ops->publish(m->opaque);
}
void s1_recompute_irq(struct s1_model *m)
{
    bool level=m->mapped && !m->faulted && (m->command&S1_MEM) &&
        (m->command&S1_BME) && !(m->command&S1_INTX_DISABLE) &&
        (m->regs.csts&CSTS_RDY) && !(m->regs.intms&1) && m->queue.irq_asserted;
    if (level==m->external_irq) return;
    m->external_irq=level;
    m->irq_changes++;
    m->diag.irq_edges++;
    s1dg_event(m,S1DG_IRQ_EDGE,0,0,0,0,0,0,0,level,m->regs.intms);
    m->ops->irq(m->opaque,level);
}
static void backend_irq(void *opaque,bool asserted)
{
    (void)asserted; /* actual queue state is updated before the callback */
    s1_recompute_irq(opaque);
}
static const struct vnvme_backend_ops backend_ops={
    .guest_span=guest_span,.read_blocks=backend_read,.publish=publish,.irq=backend_irq,
    .trace=s1dg_trace,
};
void s1_reset_controller(struct s1_model *m)
{
    /* Lower the external wire before clearing the logical queue state. */
    m->regs.csts=0;
    s1_recompute_irq(m);
    memset(&m->regs,0,sizeof(m->regs));
    vnvme_init(&m->queue,s1_backend_blocks(),&backend_ops,m);
}
/* r2.1 (NVMe 1.3): IOSQES/IOCQES may still be 0 at enable; they must be 6/4
 * before I/O queue creation (checked there). Other values cannot be honoured. */
static bool valid_es(u32 cc)
{
    u32 sq=(cc>>16)&15,cq=(cc>>20)&15;
    return (sq==0||sq==6) && (cq==0||cq==4);
}
static bool valid_cc(u32 cc)
{
    /* SHN 01b (normal) and 10b (abrupt) are both valid; 11b is reserved. */
    const u32 allowed=S1_CC_EN|S1_CC_SHN|(15u<<16)|(15u<<20);
    return !(cc&~allowed) && valid_es(cc) && (cc&S1_CC_SHN)!=S1_CC_SHN;
}
static void set_es(struct s1_model *m,u32 cc)
{
    m->queue.iosqes=(u8)((cc>>16)&15);m->queue.iocqes=(u8)((cc>>20)&15);
}
static void fatal(struct s1_model *m,u8 reason)
{
    /* r2.2 D1: every CFS also stops the queue engine (no SQE fetch until reset). */
    if (!m->queue.fatal) m->queue.fatal_reason=reason;
    m->queue.fatal=true;
    m->regs.csts|=CSTS_CFS;
    s1dg_fatal(m,reason);
    s1_recompute_irq(m);
}
/* Every CC write is accepted (never a guest abort). Problems become
 * NVMe-visible: CSTS.CFS at enable for invalid CC, missing BME or refused
 * admin queue memory; shutdown always completes (SHST=10b). */
static bool cc_write(struct s1_model *m,u32 value)
{
    u32 old=m->regs.cc;
    struct s1_diag *g=&m->diag;
    g->cc_writes++;
    if ((old^value)&S1_CC_EN) g->en_transitions++;
    if ((old^value)&S1_CC_SHN) g->shn_transitions++;
    if (!(value&S1_CC_EN)) {
        /* Disable is synchronous: no backend request can outlive a host call.
         * Keep programmed admin addresses available for a subsequent enable.
         * r2.1: INTMS resets too (NVMe 1.3 7.3.2; only AQA/ASQ/ACQ survive). */
        m->regs.csts=0;m->regs.cc=value;m->regs.intms=0;
        s1_recompute_irq(m);
        vnvme_init(&m->queue,s1_backend_blocks(),&backend_ops,m);
        s1dg_event(m,S1DG_RESET,0,0,0,0,0,old,value,0,0);
        return true;
    }
    if (!(old&S1_CC_EN)) {
        m->regs.cc=value;
        if (!valid_cc(value)) { fatal(m,S1DG_FATAL_ENABLE_CC); return true; }
        if (!(m->command&S1_BME)) { fatal(m,S1DG_FATAL_ENABLE_BME); return true; }
        u16 asq_size=(m->regs.aqa&0xfff)+1;
        u16 acq_size=((m->regs.aqa>>16)&0xfff)+1;
        if (!vnvme_set_admin_queue(&m->queue,m->regs.asq,m->regs.acq,asq_size,acq_size)) {
            fatal(m,S1DG_FATAL_ENABLE_ADMQ); return true;
        }
        set_es(m,value);
        m->regs.csts=CSTS_RDY;
        s1dg_event(m,S1DG_ENABLE,0,0,0,0,0,m->regs.asq,m->regs.acq,value,m->regs.aqa);
    } else {
        /* While enabled only SHN may change, plus IOSQES/IOCQES while no I/O
         * queue exists (r2.1); other field changes are ignored. */
        u32 es=(15u<<16)|(15u<<20);
        bool io=m->queue.queues[1].sq_valid||m->queue.queues[1].cq_valid;
        u32 keep=(!io && valid_es(value))?(value&es):(old&es);
        u32 shn_old=old&S1_CC_SHN;
        value=(old&~(S1_CC_SHN|es))|keep|(value&S1_CC_SHN);
        m->regs.cc=value;
        set_es(m,value);
        if ((value&S1_CC_SHN)==S1_CC_SHN) { fatal(m,S1DG_FATAL_SHN11); return true; }
        if (shn_old && !(value&S1_CC_SHN) && m->queue.shutdown) {
            /* r2.1 N5: host cleared SHN without a reset; nothing is fetched until EN=0. */
            g->shn_clear_no_reset++;
            s1dg_event(m,S1DG_SHN_CLEAR_NO_RESET,0,0,0,0,0,old,value,0,0);
        }
    }
    u32 shn=value&S1_CC_SHN;
    if (shn) {
        u64 ab=m->queue.aborted_commands,di=m->queue.discarded_commands;
        bool was=m->queue.shutdown;
        /* Immutable synchronous backend: no write cache or in-flight DMA. Normal
         * shutdown aborts unexecuted entries (and the held AER) with completions;
         * abrupt discards them and drops the queues. */
        vnvme_shutdown(&m->queue,shn==0x8000u);
        m->regs.csts|=CSTS_SHST_COMPLETE;
        if (m->queue.fatal) m->regs.csts|=CSTS_CFS;
        if (!was) {
            if (shn==0x8000u) g->shutdowns_abrupt++; else g->shutdowns_normal++;
            g->aborted+=m->queue.aborted_commands-ab;g->discarded+=m->queue.discarded_commands-di;
            s1dg_event(m,S1DG_SHUTDOWN,0,0,0,0,0,m->queue.aborted_commands-ab,
                       m->queue.discarded_commands-di,shn==0x8000u,m->regs.csts);
        }
    } else {
        m->regs.csts&=~CSTS_SHST_COMPLETE;
    }
    s1_recompute_irq(m);return true;
}
/* Side-effect-free dword view of the register file (0 for reserved). */
static u32 read32(struct s1_model *m,u32 off)
{
    switch(off) {
    case NVME_CAP:return (u32)NVME_CAP_VALUE;
    case NVME_CAP+4:return (u32)(NVME_CAP_VALUE>>32);
    case NVME_VS:return NVME_VS_VALUE;
    case NVME_INTMS:case NVME_INTMC:return m->regs.intms;
    case NVME_CC:return m->regs.cc;
    case NVME_CSTS:return m->regs.csts;
    case NVME_AQA:return m->regs.aqa;
    case NVME_ASQ:return (u32)m->regs.asq;
    case NVME_ASQ+4:return (u32)(m->regs.asq>>32);
    case NVME_ACQ:return (u32)m->regs.acq;
    case NVME_ACQ+4:return (u32)(m->regs.acq>>32);
    default:
        m->diag.reserved_access++;m->diag.last_reserved_offset=off;
        return 0; /* reserved, CMB/BP/PMR absent, doorbells read as zero */
    }
}
static void doorbell_write(struct s1_model *m,u32 index,u32 value)
{
    /* Invalid doorbell writes are ignored (counted), never a guest abort.
     * r2.1 B2: CQ head doorbells stay honoured after CFS so the guest can
     * acknowledge outstanding CQEs and lower INTx; nothing new is fetched
     * because the queue engine stops all fetching once fatal. */
    bool cq=index&1;
    u32 why=0;
    if (!(m->regs.csts&CSTS_RDY)) why|=S1DG_DB_NOT_READY;
    if (!cq && (m->regs.csts&CSTS_CFS)) why|=S1DG_DB_CFS;
    if (!(m->command&S1_BME)) why|=S1DG_DB_NO_BME;
    if (value>UINT16_MAX) why|=S1DG_DB_VALUE;
    if (index>=2*VNVME_MAX_QUEUES) why|=S1DG_DB_INDEX;
    if (!why && !cq && m->queue.shutdown) why|=S1DG_DB_SHUTDOWN;
    bool ok=!why && (m->command&S1_BME) && value<=UINT16_MAX && index<2*VNVME_MAX_QUEUES &&
        (cq?vnvme_cq_doorbell(&m->queue,(u16)(index/2),(u16)value):
            vnvme_sq_doorbell(&m->queue,(u16)(index/2),(u16)value));
    if (!ok) {
        if (!why) why=S1DG_DB_QUEUE;
        m->queue.invalid_doorbells++;
        for (unsigned i=0;i<7;i++) if (why&(1u<<i)) m->diag.invalid_db[i]++;
        s1dg_event(m,S1DG_INVALID_DB,4,(u8)(index/2),0,0,0,index,value,why,m->regs.csts);
    }
    if (m->queue.fatal) m->regs.csts|=CSTS_CFS;
    s1_recompute_irq(m);
}
static void write32(struct s1_model *m,u32 off,u32 v)
{
    if (off>=NVME_DBS) {
        if ((off-NVME_DBS)%4==0) doorbell_write(m,(off-NVME_DBS)/4,v);
        return;
    }
    switch(off) {
    case NVME_INTMS:m->diag.intms_writes++;m->regs.intms|=v&1;s1_recompute_irq(m);return;
    case NVME_INTMC:m->diag.intmc_writes++;m->regs.intms&=~(v&1);s1_recompute_irq(m);return;
    case NVME_CC:cc_write(m,v);return;
    case NVME_AQA:
        if (!(m->regs.cc&S1_CC_EN)) m->regs.aqa=v&0x0fff0fffu; /* reserved bits ignored */
        return;
    case NVME_ASQ:case NVME_ASQ+4:case NVME_ACQ:case NVME_ACQ+4: {
        if (m->regs.cc&S1_CC_EN) return; /* changes while enabled are ignored */
        u64 *r=off<NVME_ACQ?&m->regs.asq:&m->regs.acq;
        unsigned shift=(off&4)*8;
        *r=(*r&~(UINT64_C(0xffffffff)<<shift))|((u64)v<<shift);
        return;
    }
    case NVME_CAP:case NVME_CAP+4:case NVME_VS:case NVME_CSTS:return; /* read-only */
    default:
        m->diag.reserved_access++;m->diag.last_reserved_offset=off;
        return; /* NSSR unsupported; reserved */
    }
}
/* r2: all widths handled. Aligned dword and qword accesses act on the
 * registers (qword = low dword then high dword, as NVMe 64-bit registers);
 * narrower or misaligned reads return the corresponding bytes of the
 * side-effect-free register view; narrower or misaligned writes are ignored
 * (undefined in NVMe). Accesses outside the BAR never reach here. */
bool s1_nvme_access(struct s1_model *m,u32 off,unsigned bytes,bool write,u64 *value)
{
    if (!value || (bytes!=1 && bytes!=2 && bytes!=4 && bytes!=8)) return false;
    if (off%4==0 && bytes>=4) {
        if (!write) {
            u64 v=read32(m,off);
            if (bytes==8) v|=(u64)read32(m,off+4)<<32;
            *value=v;
        } else {
            write32(m,off,(u32)*value);
            if (bytes==8) write32(m,off+4,(u32)(*value>>32));
        }
        return true;
    }
    if (write) return true;
    u64 v=0;
    for (unsigned i=0;i<bytes;i++) {
        u32 a=off+i;
        v|=(u64)((read32(m,a&~3u)>>((a&3)*8))&0xff)<<(8*i);
    }
    *value=v;
    return true;
}
