/* SPDX-License-Identifier: MIT */
#include "hv_nvme_queue.h"
#include "ram_disk.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE UINT64_C(0x100000)
#define MEM (256u * 1024u)
#define PAGE 4096u
#define READ 1u
#define WRITE 2u
#define DATA (BASE + 0x10000)
#define LIST (BASE + 0x6000)
#define QSIZE 8u
static unsigned checks, cases, rejected_spans, reads, write_calls, flush_calls, publishes, irqs;
static bool external_irq;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static struct vnvme_ctrl ctrl;
static struct {
    unsigned char before[32], bytes[MEM], after[32];
} arena;
static unsigned char permissions[MEM/PAGE];
static const struct s0_disk *disk;
static unsigned char sink[32*PAGE];

static const struct s0_guest_memory guest = {
    BASE, arena.bytes, MEM, permissions, MEM/PAGE
};
static void *span(void *opaque, u64 addr, size_t n, bool write)
{
    (void)opaque;
    void *p = s0_guest_span(&guest,addr,n,write);
    if (!p) rejected_spans++;
    return p;
}
static bool read_blocks(void *opaque, u64 lba, u32 blocks, void *out)
{
    (void)opaque; reads++;
    return s0_disk_read(disk, lba, blocks, out);
}
/* Tripwires: frontend must never call even an accidentally supplied writable backend. */
static bool write_blocks(void *opaque, u64 lba, u32 blocks, const void *in)
{ (void)opaque;(void)lba;(void)blocks;(void)in;write_calls++;return true; }
static bool flush(void *opaque) { (void)opaque;flush_calls++;return true; }
static void publish(void *opaque) { (void)opaque;publishes++; }
static void irq(void *opaque, bool asserted) { (void)opaque;external_irq=asserted;if(asserted){CHECK(publishes>0);irqs++;} }
static const struct vnvme_backend_ops ops = {
    .guest_span=span,.read_blocks=read_blocks,.write_blocks=write_blocks,
    .flush=flush,.publish=publish,.irq=irq,
};
static unsigned char *mem(u64 addr) { CHECK(addr>=BASE && addr-BASE<MEM);return arena.bytes+(addr-BASE); }
static u64 le64(const unsigned char *p) { u64 v=0;for(unsigned i=0;i<8;i++)v|=(u64)p[i]<<(i*8);return v; }
static struct vnvme_command cmd(u8 op,u32 ns,u64 lba,u32 blocks,u64 p1,u64 p2)
{
    return (struct vnvme_command){.opcode=op,.nsid=ns,.cid=37,.prp1=p1,.prp2=p2,
        .cdw10=(u32)lba,.cdw11=(u32)(lba>>32),.cdw12=blocks?blocks-1:0};
}
static u16 submit(unsigned qid,struct vnvme_command c)
{
    struct vnvme_queue *q=&ctrl.queues[qid];
    u16 slot=q->sq_tail, cqslot=q->cq_tail;
    memcpy(mem(q->sq_addr+(u64)slot*sizeof(c)),&c,sizeof(c));
    CHECK(vnvme_sq_doorbell(&ctrl,qid,(slot+1)%q->sq_size));
    CHECK(q->cq_pending==1);
    struct vnvme_completion done;
    memcpy(&done,mem(q->cq_addr+(u64)cqslot*sizeof(done)),sizeof(done));
    CHECK(done.cid==c.cid && done.sq_id==qid);
    u16 status=done.status>>1;
    CHECK(vnvme_cq_doorbell(&ctrl,qid,q->cq_tail));
    CHECK(q->cq_pending==0);
    return status;
}
static void reset(void)
{
    cases++;
    memset(&arena,0xa7,sizeof(arena));
    memset(arena.bytes,0,MEM);
    memset(permissions,READ|WRITE,sizeof(permissions));
    /* A read-only PRP-list page; valid metadata, invalid output destination. */
    permissions[(LIST-BASE)/PAGE]=READ;
    /* Unmapped hole modeling protected/MMIO/non-RAM space. */
    permissions[MEM/PAGE-1]=0;
    disk=&s0_fixture;
    external_irq=false; /* fixture teardown, not a controller reset operation */
    vnvme_init(&ctrl,disk->block_count,&ops,NULL);
    ctrl.iosqes=6;ctrl.iocqes=4; /* r2.1: the S1 model sets these from CC; S0 drives the engine directly */
    CHECK(vnvme_set_admin_queue(&ctrl,BASE,BASE+PAGE,QSIZE,QSIZE));
    struct vnvme_command c=cmd(VNVME_ADMIN_CREATE_CQ,0,0,0,BASE+3*PAGE,0);
    c.cdw10=((QSIZE-1)<<16)|1;c.cdw11=3;
    CHECK(submit(0,c)==0);
    c=cmd(VNVME_ADMIN_CREATE_SQ,0,0,0,BASE+2*PAGE,0);
    c.cdw10=((QSIZE-1)<<16)|1;c.cdw11=(1<<16)|1;
    CHECK(submit(0,c)==0);
}
static void pattern(u64 addr,unsigned lba,unsigned blocks)
{
    unsigned char *p=mem(addr);
    for(unsigned b=0;b<blocks;b++)for(unsigned i=0;i<PAGE;i++)
        CHECK(p[b*PAGE+i]==(i<32?(unsigned char)((lba+b)*17+i):0));
}
static void unchanged(u64 addr,size_t n,unsigned char value)
{ unsigned char *p=mem(addr);for(size_t i=0;i<n;i++)CHECK(p[i]==value); }
static void read_reject(struct vnvme_command c,u16 status)
{
    memset(mem(DATA),0x5c,32*PAGE);
    unsigned before=reads;
    CHECK(submit(1,c)==status);
    CHECK(reads==before);
    unchanged(DATA,32*PAGE,0x5c);
}
static void tests_identify(void)
{
    reset();
    struct vnvme_command c=cmd(VNVME_ADMIN_IDENTIFY,1,0,0,DATA,0);
    CHECK(submit(0,c)==0);CHECK(le64(mem(DATA))==40);CHECK(mem(DATA)[130]==12);
    c.nsid=0;c.cdw10=1;CHECK(submit(0,c)==0);
    CHECK(mem(DATA)[525]==0 && mem(DATA)[520]==0);
    CHECK(memcmp(mem(DATA)+24,"S0 immutable RAM disk",21)==0);
    c.cdw10=2;CHECK(submit(0,c)==0);CHECK(le64(mem(DATA))==1);
    c.cdw10=0;c.nsid=2;CHECK(submit(0,c)==VNVME_SC_INVALID_NAMESPACE);
    c.nsid=1;c.prp1=LIST;CHECK(submit(0,c)==VNVME_SC_DATA_TRANSFER_ERROR);
    c.prp1=UINT64_MAX-100;CHECK(submit(0,c)==VNVME_SC_DATA_TRANSFER_ERROR);
}
static void tests_reads(void)
{
    reset();CHECK(submit(1,cmd(VNVME_IO_READ,1,0,1,DATA,0))==0);pattern(DATA,0,1);
    CHECK(submit(1,cmd(VNVME_IO_READ,1,39,1,DATA,0))==0);pattern(DATA,39,1);
    CHECK(submit(1,cmd(VNVME_IO_READ,1,4,2,DATA,DATA+PAGE))==0);pattern(DATA,4,2);
    /* Unaligned first PRP with a short second page. */
    CHECK(submit(1,cmd(VNVME_IO_READ,1,7,1,DATA+19,DATA+PAGE))==0);pattern(DATA+19,7,1);
    /* Noncontiguous guest pages: map logical block order through a list. */
    u64 shuffled[2]={DATA+5*PAGE,DATA+3*PAGE};
    memcpy(mem(LIST),shuffled,sizeof(shuffled));
    CHECK(submit(1,cmd(VNVME_IO_READ,1,10,3,DATA,LIST))==0);
    pattern(DATA,10,1);pattern(DATA+5*PAGE,11,1);pattern(DATA+3*PAGE,12,1);
    /* Actual PRP-list path, maximum request and all 33 segments. */
    u64 entries[32];for(unsigned i=0;i<32;i++)entries[i]=DATA+(i+1)*PAGE;
    memcpy(mem(LIST),entries,sizeof(entries));
    CHECK(submit(1,cmd(VNVME_IO_READ,1,8,32,DATA+7,LIST))==0);pattern(DATA+7,8,32);
    /* Validate every actual fixture sector against the independently specified pattern. */
    for (unsigned lba=0;lba<40;lba++) {
        CHECK(submit(1,cmd(VNVME_IO_READ,1,lba,1,DATA,0))==0);pattern(DATA,lba,1);
    }
    read_reject(cmd(VNVME_IO_READ,2,0,1,DATA,0),VNVME_SC_INVALID_NAMESPACE);
    read_reject(cmd(VNVME_IO_READ,1,40,1,DATA,0),VNVME_SC_LBA_OUT_OF_RANGE); /* r4.13: was 0x02 */
    read_reject(cmd(VNVME_IO_READ,1,39,2,DATA,DATA+PAGE),VNVME_SC_LBA_OUT_OF_RANGE);
    read_reject(cmd(VNVME_IO_READ,1,UINT64_MAX,2,DATA,DATA+PAGE),VNVME_SC_LBA_OUT_OF_RANGE);
    read_reject(cmd(VNVME_IO_READ,1,0,33,DATA,LIST),VNVME_SC_INVALID_FIELD);
}
static void tests_prps(void)
{
    reset();
    read_reject(cmd(VNVME_IO_READ,1,0,1,0,0),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,1,UINT64_MAX-63,DATA),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,1,BASE+MEM-32,DATA),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,1,LIST,0),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,2,DATA,DATA+1),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,2,DATA,0),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST+8),VNVME_SC_DATA_TRANSFER_ERROR);
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,BASE+MEM-PAGE),VNVME_SC_DATA_TRANSFER_ERROR);
    u64 entries[2]={DATA+PAGE,LIST};memcpy(mem(LIST),entries,sizeof(entries));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    entries[1]=0;memcpy(mem(LIST),entries,sizeof(entries));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    entries[1]=UINT64_MAX-(PAGE-1);memcpy(mem(LIST),entries,sizeof(entries));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    entries[1]=DATA+PAGE+1;memcpy(mem(LIST),entries,sizeof(entries));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    permissions[(LIST-BASE)/PAGE]=WRITE; /* metadata must be readable */
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    permissions[(LIST-BASE)/PAGE]=READ;
    permissions[(DATA-BASE)/PAGE+1]=READ;
    read_reject(cmd(VNVME_IO_READ,1,0,2,DATA,DATA+PAGE),VNVME_SC_DATA_TRANSFER_ERROR);
    /* General unsupported metadata/SGL/fused command flags fail before backend. */
    struct vnvme_command c=cmd(VNVME_IO_READ,1,0,1,DATA,0);c.flags=0x40;
    read_reject(c,VNVME_SC_INVALID_FIELD);c.flags=0;c.metadata=DATA;
    read_reject(c,VNVME_SC_INVALID_FIELD);c.metadata=0;c.cdw12=1u<<26; /* r2: PRINFO still rejected */
    read_reject(c,VNVME_SC_INVALID_FIELD);c.cdw12=1u<<20; /* r2: DTYPE rejected */
    read_reject(c,VNVME_SC_INVALID_FIELD);c.cdw12=0;c.cdw13=1u<<16; /* r2: DSPEC rejected */
    read_reject(c,VNVME_SC_INVALID_FIELD);
    /* r2: FUA, LR and DSM hints are benign on READ. */
    c.cdw13=0x7;c.cdw12=(1u<<30)|(1u<<31);CHECK(submit(1,c)==0);pattern(DATA,0,1);
}
static void tests_reject_commands(void)
{
    reset();
    /* r2: WRITE -> Namespace Is Write Protected; FLUSH -> no-op success; the
     * unadvertised optional commands stay Invalid Opcode. Never backend writes. */
    read_reject(cmd(VNVME_IO_WRITE,1,0,1,DATA,0),VNVME_SC_NS_WRITE_PROTECTED);
    read_reject(cmd(VNVME_IO_FLUSH,1,0,1,DATA,0),VNVME_SC_SUCCESS);
    read_reject(cmd(VNVME_IO_FLUSH,0xffffffffu,0,1,DATA,0),VNVME_SC_SUCCESS);
    read_reject(cmd(VNVME_IO_FLUSH,2,0,1,DATA,0),VNVME_SC_INVALID_NAMESPACE);
    const u8 io[]={VNVME_IO_DATASET_MANAGEMENT,4,8,0x80,0xff};
    for(size_t i=0;i<sizeof(io);i++)read_reject(cmd(io[i],1,0,1,DATA,0),VNVME_SC_INVALID_OPCODE);
    const u8 admin[]={0x0d,0x10,0x11,0x15,0x80,0x81,0x84,0xc0,0xff};
    for(size_t i=0;i<sizeof(admin);i++)CHECK(submit(0,cmd(admin[i],1,0,1,DATA,0))==VNVME_SC_INVALID_OPCODE);
    CHECK(write_calls==0 && flush_calls==0);
}
static void tests_queues(void)
{
    reset();
    CHECK(!vnvme_set_admin_queue(&ctrl,UINT64_MAX-(PAGE-1),BASE+PAGE,QSIZE,QSIZE));
    CHECK(!vnvme_set_admin_queue(&ctrl,BASE,LIST,QSIZE,QSIZE));
    CHECK(!vnvme_set_admin_queue(&ctrl,BASE+MEM-2*PAGE,BASE+PAGE,256,QSIZE));
    CHECK(!vnvme_set_admin_queue(&ctrl,BASE+1,BASE+PAGE,QSIZE,QSIZE));
    CHECK(!vnvme_sq_doorbell(&ctrl,2,1));CHECK(!vnvme_sq_doorbell(&ctrl,1,QSIZE));
    CHECK(!vnvme_cq_doorbell(&ctrl,1,QSIZE));
    struct vnvme_command c=cmd(VNVME_IO_READ,1,0,1,DATA,0);
    memcpy(mem(BASE+2*PAGE),&c,sizeof(c));
    permissions[2]=0;unsigned before=reads;
    CHECK(vnvme_sq_doorbell(&ctrl,1,1)&&ctrl.fatal);CHECK(reads==before); /* r2: refused queue memory -> fatal (CFS), not a refused doorbell */
    reset();memcpy(mem(BASE+2*PAGE),&c,sizeof(c));permissions[3]=READ;before=reads;
    CHECK(vnvme_sq_doorbell(&ctrl,1,1)&&ctrl.fatal);CHECK(reads==before); /* r2: refused queue memory -> fatal (CFS), not a refused doorbell */
    reset();ctrl.queues[1].cq_addr=UINT64_MAX-15;before=reads;
    CHECK(vnvme_sq_doorbell(&ctrl,1,1)&&ctrl.fatal);CHECK(reads==before); /* r2: refused queue memory -> fatal (CFS), not a refused doorbell */
    reset();ctrl.queues[1].sq_addr=UINT64_MAX-63;before=reads;
    CHECK(vnvme_sq_doorbell(&ctrl,1,1)&&ctrl.fatal);CHECK(reads==before); /* r2: refused queue memory -> fatal (CFS), not a refused doorbell */
    reset();
    CHECK(submit(0,cmd(VNVME_ADMIN_DELETE_SQ,0,1,0,0,0))==0);
    CHECK(submit(0,cmd(VNVME_ADMIN_DELETE_CQ,0,1,0,0,0))==0);
    c=cmd(VNVME_ADMIN_CREATE_CQ,0,((QSIZE-1)<<16)|1,0,LIST,0);c.cdw11=3;
    CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD);
    c.prp1=BASE+3*PAGE;CHECK(submit(0,c)==0);
    c.opcode=VNVME_ADMIN_CREATE_SQ;c.prp1=BASE+MEM-PAGE;c.cdw11=(1<<16)|1;
    CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD);
}
static void tests_backend(void)
{
    cases++;
    struct s0_guest_memory bad=guest;bad.base=UINT64_MAX-1023;
    CHECK(s0_guest_span(&bad,bad.base,16,true)==NULL);
    bad=guest;bad.permission_count--;
    CHECK(s0_guest_span(&bad,BASE,16,false)==NULL);
    CHECK(s0_guest_span(&guest,BASE-1,16,false)==NULL);
    CHECK(s0_guest_span(&guest,BASE,SIZE_MAX,true)==NULL);
    CHECK(s0_guest_span(&guest,BASE,0,true)==NULL);
    CHECK(s0_disk_valid(&s0_fixture));
    CHECK(s0_disk_read(&s0_fixture,39,1,sink));
    CHECK(!s0_disk_read(&s0_fixture,39,2,sink));
    CHECK(!s0_disk_read(&s0_fixture,UINT64_MAX,1,sink));
    CHECK(!s0_disk_read(&s0_fixture,0,0,sink));
    CHECK(!s0_disk_read(&s0_fixture,0,1,NULL));
    struct s0_disk invalid=s0_fixture;invalid.block_count=UINT64_MAX;
    CHECK(!s0_disk_valid(&invalid));invalid=s0_fixture;invalid.read_only=false;
    CHECK(!s0_disk_read(&invalid,0,1,sink));invalid=s0_fixture;invalid.byte_length--;
    CHECK(!s0_disk_valid(&invalid));
    reset();invalid=s0_fixture;invalid.block_count=1;invalid.byte_length=PAGE;disk=&invalid;
    /* Frontend advertises40, independently narrower backend refuses39; no output copy. */
    memset(mem(DATA),0x5c,PAGE);
    CHECK(submit(1,cmd(VNVME_IO_READ,1,39,1,DATA,0))==VNVME_SC_INTERNAL);
    unchanged(DATA,PAGE,0x5c);
    CHECK(s0_fixture_guards_valid());
    for(unsigned i=0;i<32;i++)CHECK(arena.before[i]==0xa7 && arena.after[i]==0xa7);
}
static void tests_queue_lifecycle(void)
{
    reset();
    CHECK(submit(1,cmd(VNVME_IO_READ,1,0,1,DATA,0))==0);
    CHECK(ctrl.queues[1].sq_head==1 && ctrl.queues[1].sq_tail==1);
    CHECK(submit(0,cmd(VNVME_ADMIN_DELETE_SQ,0,1,0,0,0))==0);
    struct vnvme_command c=cmd(VNVME_ADMIN_CREATE_SQ,0,((QSIZE-1)<<16)|1,
                                0,BASE+2*PAGE,0);
    c.cdw11=(1<<16)|1;
    CHECK(submit(0,c)==0);
    CHECK(ctrl.queues[1].sq_head==0 && ctrl.queues[1].sq_tail==0);
    unsigned before=reads;
    CHECK(vnvme_cq_doorbell(&ctrl,1,ctrl.queues[1].cq_head));
    CHECK(reads==before && ctrl.queues[1].cq_pending==0);
    CHECK(submit(1,cmd(VNVME_IO_READ,1,9,1,DATA,0))==0);pattern(DATA,9,1);

    reset();
    c=cmd(VNVME_IO_READ,1,0,1,DATA,0);
    memcpy(mem(BASE+2*PAGE),&c,sizeof(c));
    CHECK(vnvme_sq_doorbell(&ctrl,1,1));
    CHECK(ctrl.irq_asserted && external_irq);
    /* Invalid reset is nonmutating; successful reset must lower the real callback line. */
    CHECK(!vnvme_set_admin_queue(&ctrl,0,BASE+PAGE,QSIZE,QSIZE));
    CHECK(ctrl.irq_asserted && external_irq && ctrl.queues[1].cq_pending==1);
    CHECK(vnvme_set_admin_queue(&ctrl,BASE,BASE+PAGE,QSIZE,QSIZE));
    CHECK(!ctrl.irq_asserted && !external_irq);
    CHECK(!ctrl.queues[1].sq_valid && !ctrl.queues[1].cq_valid);
    CHECK(submit(0,cmd(VNVME_ADMIN_IDENTIFY,0,VNVME_IDENTIFY_CONTROLLER,0,DATA,0))==0);
    CHECK(!ctrl.irq_asserted && !external_irq);
}
static void tests_alias_policy(void)
{
    reset();
    /* All four active queues are protected from READ/Identify/GetLog payloads. */
    for (unsigned i=0;i<4;i++) {
        u64 dest=BASE+i*PAGE;
        read_reject(cmd(VNVME_IO_READ,1,0,1,dest,0),VNVME_SC_DATA_TRANSFER_ERROR);
        CHECK(submit(0,cmd(VNVME_ADMIN_IDENTIFY,0,VNVME_IDENTIFY_CONTROLLER,0,dest,0))==
              VNVME_SC_DATA_TRANSFER_ERROR);
        struct vnvme_command c=cmd(VNVME_ADMIN_GET_LOG_PAGE,0,2|(127u<<16),0,dest,0);
        CHECK(submit(0,c)==VNVME_SC_DATA_TRANSFER_ERROR);
    }
    /* Late invalid spans may not copy earlier valid destinations or call backend. */
    u64 entries[3]={DATA+PAGE,BASE+3*PAGE,0};
    memcpy(mem(LIST),entries,2*sizeof(u64));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    entries[1]=DATA+PAGE;memcpy(mem(LIST),entries,2*sizeof(u64));
    /* r4.14: an exact whole-page repeat of a READ destination is accepted (Windows points the
     * resident pages of a clustered in-page read at one dummy page); r2.1-r4.13 completed it 0x04.
     * The last copy wins, the third page is untouched; the partial overlap below is still refused. */
    {memset(mem(DATA),0x5c,32*PAGE);unsigned b4=reads;
     CHECK(submit(1,cmd(VNVME_IO_READ,1,0,3,DATA,LIST))==0);CHECK(reads>b4);
     pattern(DATA,0,1);pattern(DATA+PAGE,2,1);unchanged(DATA+2*PAGE,30*PAGE,0x5c);}
    entries[0]=DATA;entries[1]=DATA+PAGE;memcpy(mem(LIST),entries,2*sizeof(u64));
    /* nightly-4: a partial overlap (unaligned PRP1 DATA+19, then DATA as a whole page) is accepted and copied
     * in command order: DATA ends with stream bytes 4077..8172, DATA+PAGE starts with 8173..8191, nothing else
     * changes. r2.1-r4.14 completed it 0x04. */
    {memset(mem(DATA),0x5c,32*PAGE);unsigned b4=reads;
     CHECK(submit(1,cmd(VNVME_IO_READ,1,0,2,DATA+19,LIST))==0);CHECK(reads>b4);
     CHECK(s0_disk_read(disk,0,2,sink));
     CHECK(memcmp(mem(DATA),sink+4077,PAGE)==0);CHECK(memcmp(mem(DATA+PAGE),sink+4077+PAGE,19)==0);
     unchanged(DATA+PAGE+19,31*PAGE-19,0x5c);}
    /* Writable metadata: alias rejection must not depend on a read-only list. */
    permissions[(LIST-BASE)/PAGE]=READ|WRITE;
    entries[0]=DATA;entries[1]=DATA+PAGE;memcpy(mem(LIST),entries,2*sizeof(u64));
    unsigned before=reads;
    CHECK(submit(1,cmd(VNVME_IO_READ,1,0,3,LIST,LIST))==VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(reads==before && memcmp(mem(LIST),entries,2*sizeof(u64))==0);
    entries[0]=DATA+PAGE;entries[1]=LIST;memcpy(mem(LIST),entries,2*sizeof(u64));
    read_reject(cmd(VNVME_IO_READ,1,0,3,DATA,LIST),VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(memcmp(mem(LIST),entries,2*sizeof(u64))==0);
    /* Exact consumed bytes matter: list24bytes adjacent to first output is legal. */
    entries[0]=DATA;entries[1]=DATA+PAGE;entries[2]=DATA+2*PAGE;
    memcpy(mem(LIST),entries,sizeof(entries));
    CHECK(submit(1,cmd(VNVME_IO_READ,1,3,3,LIST+24,LIST))==0);
    CHECK(s0_disk_read(disk,3,3,sink));
    CHECK(memcmp(mem(LIST),entries,sizeof(entries))==0);
    CHECK(memcmp(mem(LIST+24),sink,PAGE-24)==0);
    CHECK(memcmp(mem(DATA),sink+PAGE-24,2*PAGE+24)==0);
    /* Output touching either endpoint of a queue is legal, without overlap. */
    CHECK(submit(1,cmd(VNVME_IO_READ,1,5,1,BASE+3*PAGE+QSIZE*16,DATA))==0);
    CHECK(s0_disk_read(disk,5,1,sink));
    CHECK(memcmp(mem(BASE+3*PAGE+QSIZE*16),sink,PAGE-QSIZE*16)==0);
    CHECK(memcmp(mem(DATA),sink+PAGE-QSIZE*16,QSIZE*16)==0);
    CHECK(submit(1,cmd(VNVME_IO_READ,1,6,1,BASE+PAGE-16,DATA))==0);
    CHECK(s0_disk_read(disk,6,1,sink));
    CHECK(memcmp(mem(BASE+PAGE-16),sink,16)==0);
    CHECK(memcmp(mem(DATA),sink+16,PAGE-16)==0);

    /* Queue admission must reject overlap before any queue state mutation. */
    reset();
    struct vnvme_ctrl saved=ctrl;
    CHECK(!vnvme_set_admin_queue(&ctrl,BASE,BASE,QSIZE,QSIZE));
    CHECK(!vnvme_set_admin_queue(&ctrl,BASE,BASE+PAGE,128,QSIZE));
    CHECK(memcmp(&saved,&ctrl,sizeof(ctrl))==0);
    CHECK(vnvme_set_admin_queue(&ctrl,BASE,BASE+PAGE,64,QSIZE)); /* adjacent */
    struct vnvme_command c=cmd(VNVME_ADMIN_CREATE_CQ,0,((QSIZE-1)<<16)|1,0,BASE,0);
    c.cdw11=3;
    CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD); /* adminSQ */
    c.prp1=BASE+PAGE;CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD); /* adminCQ */
    CHECK(!ctrl.queues[1].cq_valid);
    c.prp1=BASE+3*PAGE;CHECK(submit(0,c)==0);
    c=cmd(VNVME_ADMIN_CREATE_SQ,0,((QSIZE-1)<<16)|1,0,BASE,0);
    c.cdw11=(1<<16)|1;
    CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD);
    c.prp1=BASE+PAGE;CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD);
    c.prp1=BASE+3*PAGE;CHECK(submit(0,c)==VNVME_SC_INVALID_FIELD);
    CHECK(!ctrl.queues[1].sq_valid);
    c.prp1=BASE+2*PAGE;c.cdw10=(63u<<16)|1;
    CHECK(submit(0,c)==0); /* IO SQ ends exactly at IO CQ start */
    CHECK(submit(1,cmd(VNVME_IO_READ,1,0,1,DATA,0))==0);pattern(DATA,0,1);
}
int main(void)
{
    tests_identify();tests_reads();tests_prps();tests_reject_commands();tests_queues();tests_backend();tests_queue_lifecycle();tests_alias_policy();
    CHECK(write_calls==0 && flush_calls==0);CHECK(s0_fixture_guards_valid());
    printf("{\"status\":\"pass\",\"checks\":%u,\"reset_cases\":%u,\"backend_read_calls\":%u,"
           "\"backend_write_calls\":%u,\"backend_flush_calls\":%u,\"rejected_guest_spans\":%u,"
           "\"irq_assertions\":%u,\"physical_backend\":false}\n",checks,cases,reads,write_calls,flush_calls,rejected_spans,irqs);
    return 0;
}
