/* SPDX-License-Identifier: MIT */
/* r2 behaviour suite on the actual resident S1 model + S0 queue engine, with
 * the S0 permission-checked guest memory mock (unadmitted pages are refused by
 * s0_guest_span exactly as the live provider refuses them). Host only. */
#include "model.h"
#include "guest_memory.h"
#include "ram_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ECAM UINT64_C(0xf28000000)
#define BARB UINT64_C(0xf30000000)
#define RAM UINT64_C(0x100000000)
#define PAGE 4096u
#define RAM_SIZE (512u*1024u)
#define ASQ RAM
#define ACQ (RAM+PAGE)
#define IOSQ (RAM+2*PAGE)
#define IOCQ (RAM+3*PAGE)
#define DATA (RAM+0x10000)
static struct s1_model m;
static struct {u8 before[32], bytes[RAM_SIZE], after[32];} arena;
static u8 perms[RAM_SIZE/PAGE];
static struct s0_guest_memory memory={RAM,arena.bytes,RAM_SIZE,perms,RAM_SIZE/PAGE};
static unsigned checks, cases, edges, trace_steps;
static bool line;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x);exit(1);}}while(0)
static void *span(void *o,u64 a,size_t n,bool w){CHECK(o==&memory);return s0_guest_span(&memory,a,n,w);}
static bool map(void *o,u64 a,size_t n){(void)o;return a==BARB&&n==S1_BAR_SIZE;}
static bool unmap(void *o,u64 a,size_t n){(void)o;return a==BARB&&n==S1_BAR_SIZE;}
static void irq(void *o,bool l){CHECK(o==&memory);CHECK(l!=line);line=l;edges++;}
static const struct s1_ops ops={span,map,unmap,irq,NULL};
static void *ptr(u64 a){CHECK(a>=RAM&&a-RAM<RAM_SIZE);return arena.bytes+(a-RAM);}
static bool cfgw(u32 off,unsigned b,u64 v){return s1_config(&m,ECAM+off,b,true,&v);}
static u64 cfgr(u32 off,unsigned b){u64 v=0;CHECK(s1_config(&m,ECAM+off,b,false,&v));return v;}
static bool wr(u32 off,unsigned b,u64 v){return s1_mmio(&m,BARB+off,b,true,&v);}
static u64 rd(u32 off,unsigned b){u64 v=0;CHECK(s1_mmio(&m,BARB+off,b,false,&v));return v;}
static u16 le16(const u8 *p){return (u16)(p[0]|(p[1]<<8));}
static u32 le32(const u8 *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((u32)p[3]<<24);}
static u64 le64(const u8 *p){return le32(p)|((u64)le32(p+4)<<32);}

struct q { u16 n, tail, head; u8 phase; u64 sq, cq; unsigned id; };
static struct q aq, ioq;
static u16 next_cid=1;
static void fresh(void)
{
    cases++;line=false;
    memset(&m,0,sizeof(m));
    memset(&arena,0xa7,sizeof(arena));memset(arena.bytes,0,RAM_SIZE);memset(perms,3,sizeof(perms));
    CHECK(s1_init(&m,&ops,&memory,ECAM,BARB,S1_BAR_SIZE));
    CHECK(cfgw(0x10,4,(u32)BARB));CHECK(cfgw(0x14,4,BARB>>32));CHECK(cfgw(4,2,S1_MEM|S1_BME));CHECK(m.mapped);
}
static void enable(u16 n)
{
    aq=(struct q){n,0,0,1,ASQ,ACQ,0};
    CHECK(wr(0x24,4,((u32)(n-1)<<16)|(n-1)));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);
}
/* status, or -1 when no CQE is posted for this submission */
static int submit(struct q *q,struct vnvme_command c,bool ack,u16 *cid_out)
{
    c.cid=next_cid++;if(cid_out)*cid_out=c.cid;
    memcpy(ptr(q->sq+(u64)q->tail*64),&c,64);q->tail=(u16)((q->tail+1)%q->n);
    CHECK(wr(0x1000+q->id*8,4,q->tail));
    struct vnvme_completion d;memcpy(&d,ptr(q->cq+(u64)q->head*16),16);
    if((d.status&1)!=q->phase||d.cid!=c.cid)return -1;
    if(ack){q->head=(u16)((q->head+1)%q->n);if(!q->head)q->phase^=1;CHECK(wr(0x1004+q->id*8,4,q->head));}
    return d.status>>1;
}
static struct vnvme_completion cqe(struct q *q,u16 slot){struct vnvme_completion d;memcpy(&d,ptr(q->cq+(u64)slot*16),16);return d;}
static struct vnvme_command cmd(u8 op,u32 nsid,u64 data,u32 c10,u32 c11)
{struct vnvme_command c;memset(&c,0,sizeof c);c.opcode=op;c.nsid=nsid;c.prp1=data;c.cdw10=c10;c.cdw11=c11;return c;}
static u32 result_of(struct q *q){return cqe(q,(u16)((q->head+q->n-1)%q->n)).result;}
static void io_pair(u16 n)
{
    ioq=(struct q){n,0,0,1,IOSQ,IOCQ,1};
    CHECK(submit(&aq,cmd(5,0,IOCQ,((u32)(n-1)<<16)|1,3),true,NULL)==0);
    CHECK(submit(&aq,cmd(1,0,IOSQ,((u32)(n-1)<<16)|1,(1u<<16)|1),true,NULL)==0);
}
/* nightly-4: bytes [off, off+n) of the READ stream starting at lba (fixture block pattern) */
static void stream(u64 a,unsigned lba,size_t off,size_t n){const u8 *p=ptr(a);for(size_t k=0;k<n;k++){size_t s=off+k;unsigned i=(unsigned)(s%PAGE);CHECK(p[k]==(i<32?(u8)((lba+s/PAGE)*17+i):0));}}
static void pattern(u64 a,unsigned lba){const u8 *p=ptr(a);for(unsigned i=0;i<PAGE;i++)CHECK(p[i]==(i<32?(u8)(lba*17+i):0));}

static void test_identify_and_ids(void)
{
    fresh();enable(8);u8 *d=ptr(DATA);
    CHECK(submit(&aq,cmd(6,0,DATA,1,0),true,NULL)==0);
    CHECK(d[260]==0x03 && le16(d+266)==VNVME_WCTEMP && le16(d+268)==VNVME_CCTEMP && d[259]==0);
    u8 fr[8];memcpy(fr,d+64,8);
    CHECK(submit(&aq,cmd(6,1,DATA,0,0),true,NULL)==0);
    CHECK(le64(d)==40 && d[99]==1 && d[130]==12);
    u8 nguid[16],eui[8];memcpy(nguid,d+104,16);memcpy(eui,d+120,8);
    CHECK(eui[0]==0x02 && memcmp(eui+1,"M5WS1",5)==0);
    bool nz=false;for(unsigned i=0;i<16;i++)nz|=nguid[i]!=0;CHECK(nz);
    CHECK(submit(&aq,cmd(6,1,DATA,3,0),true,NULL)==0);           /* CNS3 */
    CHECK(d[0]==1&&d[1]==8&&memcmp(d+4,eui,8)==0&&d[12]==2&&d[13]==16&&memcmp(d+16,nguid,16)==0&&d[32]==0);
    CHECK(submit(&aq,cmd(6,2,DATA,3,0),true,NULL)==VNVME_SC_INVALID_NAMESPACE);
    /* r2.1 ruling B1: OACS.NMS=0 -> NSID FFFFFFFFh is Invalid Namespace (0Bh); no data written */
    memset(d,0x5c,PAGE);
    CHECK(submit(&aq,cmd(6,0xffffffffu,DATA,0,0),true,NULL)==VNVME_SC_INVALID_NAMESPACE);
    for(unsigned i=0;i<PAGE;i++)CHECK(d[i]==0x5c);
    CHECK(submit(&aq,cmd(6,0,DATA,0,0),true,NULL)==VNVME_SC_INVALID_NAMESPACE);
    CHECK(submit(&aq,cmd(6,2,DATA,0,0),true,NULL)==VNVME_SC_INVALID_NAMESPACE);
    /* stable across controller reset and fresh model */
    fresh();enable(8);CHECK(submit(&aq,cmd(6,1,DATA,0,0),true,NULL)==0);
    CHECK(memcmp(d+104,nguid,16)==0&&memcmp(d+120,eui,8)==0);
    /* FW slot log FRS1 equals Identify FR */
    CHECK(submit(&aq,cmd(2,0,DATA,(127u<<16)|3,0),true,NULL)==0);
    CHECK(d[0]==1&&memcmp(d+8,fr,8)==0);for(unsigned i=16;i<512;i++)CHECK(d[i]==0);
}
static void test_logs(void)
{
    fresh();enable(8);u8 *d=ptr(DATA);
    memset(d,0x5c,PAGE);
    CHECK(submit(&aq,cmd(2,0xffffffffu,DATA,(127u<<16)|2,0),true,NULL)==0);
    CHECK(le16(d+1)==VNVME_COMPOSITE_TEMP&&d[3]==100&&d[4]==10&&d[0]==0);
    struct vnvme_command c=cmd(2,0,DATA,(1u<<16)|3,0);c.cdw12=8;  /* offset 8, 8 bytes */
    memset(d,0x5c,PAGE);CHECK(submit(&aq,c,true,NULL)==0);CHECK(memcmp(d,"0.1     ",8)==0&&d[8]==0x5c);
    c.cdw12=6;CHECK(submit(&aq,c,true,NULL)==VNVME_SC_INVALID_FIELD);  /* unaligned */
    c.cdw12=512;CHECK(submit(&aq,c,true,NULL)==VNVME_SC_INVALID_FIELD);/* past end */
    c.cdw12=0;c.cdw13=1;CHECK(submit(&aq,c,true,NULL)==VNVME_SC_INVALID_FIELD);
    c=cmd(2,0,DATA,(1023u<<16)|3,0);memset(d,0x5c,PAGE);CHECK(submit(&aq,c,true,NULL)==0);
    CHECK(d[0]==1);for(unsigned i=512;i<4096;i++)CHECK(d[i]==0);         /* zero beyond log */
    CHECK(submit(&aq,cmd(2,0,DATA,(15u<<16)|1,0),true,NULL)==0);
    const u8 bad[]={0,4,5,6,0x0c,0x80,0xc0};
    for(unsigned i=0;i<sizeof bad;i++)CHECK(submit(&aq,cmd(2,0,DATA,(15u<<16)|bad[i],0),true,NULL)==VNVME_SC_INVALID_LOG_PAGE);
}
static void test_features(void)
{
    fresh();enable(8);
    CHECK(submit(&aq,cmd(10,0,0,4,0),true,NULL)==0&&result_of(&aq)==VNVME_WCTEMP);
    CHECK(submit(&aq,cmd(10,0,0,4,1u<<20),true,NULL)==0&&result_of(&aq)==0);
    CHECK(submit(&aq,cmd(9,0,0,4,350),true,NULL)==0);
    CHECK(submit(&aq,cmd(9,0,0,4,(1u<<20)|273),true,NULL)==0);
    CHECK(submit(&aq,cmd(10,0,0,4,0),true,NULL)==0&&result_of(&aq)==350);
    CHECK(submit(&aq,cmd(10,0,0,4,1u<<20),true,NULL)==0&&result_of(&aq)==273);
    CHECK(submit(&aq,cmd(9,0,0,4,(1u<<16)|300),true,NULL)==VNVME_SC_INVALID_FIELD); /* TMPSEL 1 */
    CHECK(submit(&aq,cmd(9,0,0,4,(2u<<20)|300),true,NULL)==VNVME_SC_INVALID_FIELD); /* THSEL 2 */
    CHECK(submit(&aq,cmd(9,0,0,4|(1u<<31),300),true,NULL)==VNVME_SC_FEATURE_NOT_SAVEABLE);
    CHECK(submit(&aq,cmd(10,0,0,4,0),true,NULL)==0&&result_of(&aq)==350);
    CHECK(submit(&aq,cmd(9,0,0,6,1),true,NULL)==VNVME_SC_INVALID_FIELD);  /* VWC absent */
    CHECK(submit(&aq,cmd(10,0,0,6,0),true,NULL)==VNVME_SC_INVALID_FIELD);
    CHECK(submit(&aq,cmd(9,0,0,2,1),true,NULL)==VNVME_SC_INVALID_FIELD);  /* PS1 absent */
    CHECK(submit(&aq,cmd(9,0,0,2,0),true,NULL)==0);
    CHECK(submit(&aq,cmd(9,0,0,7,0x003f003f),true,NULL)==0&&result_of(&aq)==0);
    /* features revert on controller reset */
    CHECK(wr(0x14,4,0));enable(8);
    CHECK(submit(&aq,cmd(10,0,0,4,0),true,NULL)==0&&result_of(&aq)==VNVME_WCTEMP);
}
static void test_io_commands(void)
{
    fresh();enable(8);io_pair(8);
    u64 r=m.backend_reads;
    struct vnvme_command c=cmd(2,1,DATA,0,0);c.cdw12=(1u<<30)|(1u<<31);c.cdw13=0x23;c.cdw14=0x1234;c.cdw15=0x5678;
    CHECK(submit(&ioq,c,true,NULL)==0);pattern(DATA,0);CHECK(m.backend_reads==r+1);
    c.cdw12=1u<<26;CHECK(submit(&ioq,c,true,NULL)==VNVME_SC_INVALID_FIELD);
    c.cdw12=1u<<24;CHECK(submit(&ioq,c,true,NULL)==VNVME_SC_INVALID_FIELD);
    c.cdw12=0;c.cdw13=1u<<8;CHECK(submit(&ioq,c,true,NULL)==VNVME_SC_INVALID_FIELD);
    r=m.backend_reads;
    CHECK(submit(&ioq,cmd(0,1,0,0,0),true,NULL)==0);
    CHECK(submit(&ioq,cmd(0,0xffffffffu,0,0,0),true,NULL)==0);
    CHECK(submit(&ioq,cmd(0,3,0,0,0),true,NULL)==VNVME_SC_INVALID_NAMESPACE);
    CHECK(submit(&ioq,cmd(1,1,DATA,0,0),true,NULL)==VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(submit(&ioq,cmd(8,1,0,0,0),true,NULL)==VNVME_SC_INVALID_OPCODE);
    CHECK(submit(&ioq,cmd(9,1,DATA,0,4),true,NULL)==VNVME_SC_INVALID_OPCODE);
    CHECK(m.backend_reads==r);
}
static void test_aer_abort_shutdown(void)
{
    fresh();enable(8);io_pair(8);
    u16 aer,aer2;
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer2)==VNVME_SC_AER_LIMIT);
    /* Abort the held AER: AER completes with Abort Requested, Abort DW0=0. */
    u16 slot=aq.head;
    CHECK(submit(&aq,cmd(8,0,0,((u32)aer<<16)|0,0),false,NULL)==-1); /* first CQE is the AER */
    struct vnvme_completion d=cqe(&aq,slot);CHECK(d.cid==aer&&(d.status>>1)==VNVME_SC_ABORT_REQUESTED);
    d=cqe(&aq,(u16)((slot+1)%aq.n));CHECK((d.status>>1)==0&&d.result==0);
    aq.head=(u16)((slot+2)%aq.n);if(aq.head<slot)aq.phase^=1;CHECK(wr(0x1004,4,aq.head));
    CHECK(submit(&aq,cmd(8,0,0,((u32)aer<<16)|0,0),true,NULL)==0&&result_of(&aq)==1); /* nothing left */
    /* Normal shutdown with a held AER and an unexecuted I/O entry. */
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);
    struct vnvme_command a=cmd(2,1,DATA,0,0),b=cmd(2,1,DATA+PAGE,39,0);a.cid=900;b.cid=901;
    memcpy(ptr(IOSQ+(u64)ioq.tail*64),&a,64);memcpy(ptr(IOSQ+(u64)(ioq.tail+1)*64),&b,64);
    ioq.tail=(u16)(ioq.tail+2);CHECK(wr(0x1008,4,ioq.tail));
    u64 r=m.backend_reads;u16 aslot=aq.head,islot=ioq.head;
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK(rd(0x1c,4)==9);
    CHECK(m.backend_reads==r&&m.queue.aborted_commands>=2&&!m.queue.aer_outstanding);
    d=cqe(&ioq,(u16)((islot+1)%ioq.n));CHECK(d.cid==901&&(d.status>>1)==VNVME_SC_ABORT_REQUESTED);
    d=cqe(&aq,aslot);CHECK(d.cid==aer&&(d.status>>1)==VNVME_SC_ABORT_REQUESTED);
    CHECK(line); /* completions asserted INTx */
    CHECK(wr(0x1004,4,(u16)((aslot+1)%aq.n)));CHECK(wr(0x100c,4,(u16)((islot+2)%ioq.n)));CHECK(!line&&m.backend_reads==r);
    /* nothing more is fetched until reset */
    u64 inv=m.queue.invalid_doorbells;CHECK(wr(0x1000,4,(u16)((aq.tail+1)%aq.n)));CHECK(m.queue.invalid_doorbells==inv+1);
    /* CQ full during normal shutdown: deterministic discard */
    fresh();enable(8);ioq=(struct q){2,0,0,1,IOSQ,IOCQ,1};
    CHECK(submit(&aq,cmd(5,0,IOCQ,(1u<<16)|1,3),true,NULL)==0);
    CHECK(submit(&aq,cmd(1,0,IOSQ,(3u<<16)|1,(1u<<16)|1),true,NULL)==0);
    for(unsigned i=0;i<3;i++){a.cid=(u16)(700+i);memcpy(ptr(IOSQ+i*64u),&a,64);}
    CHECK(wr(0x1008,4,3));r=m.backend_reads;
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK(rd(0x1c,4)==9);
    CHECK(m.queue.aborted_commands==0&&m.queue.discarded_commands==2&&m.backend_reads==r); /* r2.1 N3: a 2-entry CQ holds 1 */
    /* Abrupt with held AER: discarded, queues dropped, line low */
    fresh();enable(8);io_pair(8);CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,NULL)==-1);
    CHECK(wr(0x14,4,S1_CC_VALID|0x8000));CHECK(rd(0x1c,4)==9);
    CHECK(!m.queue.queues[0].cq_valid&&!m.queue.queues[1].sq_valid&&!m.queue.aer_outstanding&&m.queue.discarded_commands==1&&!line);
    CHECK(wr(0x14,4,0));CHECK(rd(0x1c,4)==0);enable(8);  /* recovers after reset */
}
static void test_register_widths(void)
{
    fresh();enable(8);
    static const u32 regs[]={0,4,8,0xc,0x14,0x1c,0x24,0x28,0x2c,0x30,0x34};
    for(unsigned i=0;i<sizeof regs/4;i++){
        u32 v=(u32)rd(regs[i],4);
        for(unsigned b=0;b<4;b++)CHECK(rd(regs[i]+b,1)==((v>>(8*b))&0xff));
        CHECK(rd(regs[i],2)==(v&0xffff)&&rd(regs[i]+2,2)==(v>>16));
        CHECK(rd(regs[i]+1,4)==((((u64)rd(regs[i]+4,4)<<24)|(v>>8))&0xffffffffu));
    }
    CHECK(rd(0x28,8)==ASQ&&rd(0x30,8)==ACQ&&rd(0x14,8)==rd(0x14,4)&&rd(0x18,8)==((u64)rd(0x1c,4)<<32)); /* 0x18 reserved */
    for(u32 off=0x38;off<0x1000;off+=4){CHECK(wr(off,4,0xffffffff));CHECK(rd(off,4)==0);}
    for(u32 off=0x38;off<0x1000;off+=8){CHECK(wr(off,8,~0ULL));CHECK(rd(off,8)==0);}
    CHECK(rd(0x14,4)==S1_CC_VALID&&rd(0x1c,4)==1&&rd(0xc,4)==0);
    CHECK(wr(0x14,1,0)&&wr(0x14,2,0)&&wr(0x16,2,0)&&wr(0x13,4,0)&&wr(0x12,8,0));      /* ignored */
    CHECK(rd(0x14,4)==S1_CC_VALID&&rd(0x1c,4)==1);
    CHECK(wr(0x0c,8,0x100000001ULL));CHECK(rd(0x0c,4)==0);   /* INTMS=1 then INTMC=1 */
    CHECK(wr(0xc,4,1)&&rd(0x0c,4)==1&&rd(0x10,4)==1);CHECK(wr(0x10,4,1)&&rd(0x0c,4)==0);
    /* qword doorbell: SQ0 tail then CQ0 head */
    struct vnvme_command c=cmd(6,0,DATA,1,0);c.cid=77;memcpy(ptr(ASQ+(u64)aq.tail*64),&c,64);
    u16 slot=aq.head;aq.tail=(u16)((aq.tail+1)%aq.n);
    CHECK(wr(0x1000,8,((u64)aq.head<<32)|aq.tail));CHECK(cqe(&aq,slot).cid==77);
    aq.head=(u16)((aq.head+1)%aq.n);CHECK(wr(0x1000,8,((u64)aq.head<<32)|aq.tail));CHECK(!line);
    u64 inv=m.queue.invalid_doorbells;
    CHECK(wr(0x1003,1,1)&&wr(0x1010,4,1)&&wr(0x1ffc,4,1)&&wr(0xfff000,4,1));
    CHECK(m.queue.invalid_doorbells==inv+3&&rd(0x1000,8)==0&&rd(0xfffff8,8)==0);
    CHECK(rd(0x1c,4)==1);
}
static void test_fatal_admission(void)
{
    /* enable with refused admin SQ memory -> CFS, no guest abort */
    fresh();perms[0]=0;
    CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==2);CHECK(!m.queue.queues[0].sq_valid);
    CHECK(wr(0x14,4,0));CHECK(rd(0x1c,4)==0);perms[0]=3;CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);
    /* enable without BME -> CFS */
    fresh();CHECK(cfgw(4,2,S1_MEM));CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==2);
    /* SQ memory revoked after enable: a valid READ placed in the refused page
     * must never execute (no backend read) and CFS is reported. */
    fresh();enable(8);io_pair(8);
    struct vnvme_command c=cmd(2,1,DATA,0,0);memcpy(ptr(IOSQ),&c,64);
    perms[2]=0;u64 r=m.backend_reads;
    CHECK(wr(0x1008,4,1));CHECK(rd(0x1c,4)==3&&m.queue.fatal&&m.backend_reads==r);
    CHECK(wr(0x1008,4,2));CHECK(m.backend_reads==r);          /* fatal: nothing fetched */
    perms[2]=3;CHECK(wr(0x1008,4,3));CHECK(m.backend_reads==r);/* still fatal until reset */
    CHECK(wr(0x14,4,0));CHECK(rd(0x1c,4)==0&&!m.queue.fatal);
    /* CQ write permission revoked: CFS, completion never written */
    fresh();enable(8);io_pair(8);memcpy(ptr(IOSQ),&c,64);perms[3]=1;
    memset(ptr(IOCQ),0x5a,PAGE);
    CHECK(wr(0x1008,4,1));CHECK((rd(0x1c,4)&2)==2);
    const u8 *p=ptr(IOCQ);for(unsigned i=0;i<PAGE;i++)CHECK(p[i]==0x5a);
    /* bad PRP on a command stays a per-command NVMe error */
    fresh();enable(8);io_pair(8);perms[DATA/PAGE-RAM/PAGE]=1;r=m.backend_reads;
    CHECK(submit(&ioq,cmd(2,1,DATA,0,0),true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(submit(&aq,cmd(6,0,DATA,1,0),true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(submit(&ioq,cmd(2,1,DATA+3,0,0),true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(m.backend_reads==r&&rd(0x1c,4)==1);
}
/* StorNVMe-like start/IO/stop trace. The ORDER and content are reconstructed
 * from public NVMe-driver behaviour and memory of stornvme logs, NOT from a
 * captured trace: treat as an approximation (see BUILD-NOTES). */
static void step(int got,int want){trace_steps++;CHECK(got==want);}
static void test_stornvme_like_trace(void)
{
    fresh();
    /* pci.sys: config identity, BAR sizing with decode off, restore, enable */
    CHECK(cfgr(0,4)==0x00101b36&&(cfgr(8,4)>>8)==0x010802);
    CHECK(cfgw(4,2,0));CHECK(cfgw(0x10,4,0xffffffff)&&cfgw(0x14,4,0xffffffff));
    CHECK((cfgr(0x10,4)&0xfffffff0)==0xff000000);
    CHECK(cfgw(0x10,4,(u32)BARB)&&cfgw(0x14,4,BARB>>32)&&cfgw(4,2,S1_MEM|S1_BME));
    /* simulate firmware having left the controller enabled */
    enable(2);
    /* stornvme: CAP (64-bit), VS, CC.EN=0, wait RDY=0 */
    u64 cap=rd(0,8);CHECK((cap&0xffff)==255&&((cap>>37)&1)==1&&((cap>>48)&0xf)==0);
    CHECK(rd(8,4)==0x00010300);
    CHECK(wr(0x14,4,rd(0x14,4)&~1u));CHECK((rd(0x1c,4)&1)==0);
    /* admin queues: AQA, ASQ/ACQ as 64-bit writes, mask, enable */
    CHECK(wr(0x0c,4,1));
    aq=(struct q){32,0,0,1,ASQ,ACQ,0};
    CHECK(wr(0x24,4,(31u<<16)|31));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);CHECK(wr(0x10,4,1));
    u8 *d=ptr(DATA);
    step(submit(&aq,cmd(6,0,DATA,1,0),true,NULL),0);               /* Identify Controller */
    u32 nn=le32(d+516);CHECK(nn==1);
    step(submit(&aq,cmd(6,0,DATA,2,0),true,NULL),0);               /* Active NS list */
    CHECK(le32(d)==1&&le32(d+4)==0);
    step(submit(&aq,cmd(6,0xffffffffu,DATA,0,0),true,NULL),VNVME_SC_INVALID_NAMESPACE); /* r2.1 B1 */
    step(submit(&aq,cmd(6,1,DATA,0,0),true,NULL),0);               /* Identify NS 1 */
    step(submit(&aq,cmd(6,1,DATA,3,0),true,NULL),0);               /* NS descriptors */
    step(submit(&aq,cmd(9,0,0,7,0x003f003f),true,NULL),0);         /* Number of Queues */
    CHECK(result_of(&aq)==0);
    step(submit(&aq,cmd(9,0,0,8,0),true,NULL),0);                  /* IRQ coalescing off */
    step(submit(&aq,cmd(9,0,0,0x0b,0x1ff),true,NULL),0);           /* AE config */
    step(submit(&aq,cmd(10,0,0,4,0),true,NULL),0);                 /* temp threshold */
    step(submit(&aq,cmd(10,0,0,2,0),true,NULL),0);                 /* power state */
    step(submit(&aq,cmd(2,0xffffffffu,DATA,(127u<<16)|2,0),true,NULL),0); /* SMART */
    step(submit(&aq,cmd(2,0,DATA,(15u<<16)|1,0),true,NULL),0);     /* error log */
    step(submit(&aq,cmd(2,0,DATA,(127u<<16)|3,0),true,NULL),0);    /* FW slot log */
    step(submit(&aq,cmd(0x0c,0,0,0,0),true,NULL),-1);              /* AER held */
    ioq=(struct q){64,0,0,1,IOSQ,IOCQ,1};
    step(submit(&aq,cmd(5,0,IOCQ,(63u<<16)|1,3),true,NULL),0);
    step(submit(&aq,cmd(1,0,IOSQ,(63u<<16)|1,(1u<<16)|1),true,NULL),0);
    /* disk.sys/partmgr: LBA0, LBA1, last LBA (GPT backup), with FUA on one */
    unsigned lbas[]={0,1,39};
    for(unsigned i=0;i<3;i++){
        struct vnvme_command c=cmd(2,1,DATA,lbas[i],0);if(i==1)c.cdw12=1u<<30;
        unsigned before=edges;step(submit(&ioq,c,false,NULL),0);CHECK(line);
        CHECK(wr(0x0c,4,1)&&!line);                                /* ISR masks */
        ioq.head=(u16)((ioq.head+1)%ioq.n);CHECK(wr(0x100c,4,ioq.head));
        CHECK(wr(0x10,4,1)&&!line&&edges>before);pattern(DATA,lbas[i]);
    }
    step(submit(&ioq,cmd(1,1,DATA,0,0),true,NULL),VNVME_SC_NS_WRITE_PROTECTED);
    step(submit(&ioq,cmd(0,1,0,0,0),true,NULL),0);                 /* FLUSH */
    /* shutdown: delete I/O queues, normal SHN, poll SHST */
    step(submit(&aq,cmd(0,0,0,1,0),true,NULL),0);
    step(submit(&aq,cmd(4,0,0,1,0),true,NULL),0);
    u16 aslot=aq.head;
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK((rd(0x1c,4)&0xc)==8);
    CHECK((cqe(&aq,aslot).status>>1)==VNVME_SC_ABORT_REQUESTED);    /* held AER aborted */
    CHECK(m.queue.invalid_doorbells==0&&m.backend_reads==3);
}

/* r2.1: gaps exposed by the reviewer's surviving mutants. */
static int ring_index_of(u8 type,u8 qid,u64 from)
{
    for (u64 q=from;q<m.diag.write_seq;q++) {
        const struct s1dg_entry *e=&m.diag.ring[q%S1DG_RING];
        if (e->type==type&&e->qid==qid) return (int)q;
    }
    return -1;
}
static void test_gaps(void)
{
    u16 aer,x;
    /* Abort with the wrong CID leaves the held AER outstanding. */
    fresh();enable(8);
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);
    CHECK(submit(&aq,cmd(8,0,0,((u32)(u16)(aer+1)<<16)|0,0),true,NULL)==0&&result_of(&aq)==1);
    CHECK(submit(&aq,cmd(8,0,0,((u32)aer<<16)|1,0),true,NULL)==0&&result_of(&aq)==1); /* wrong SQID */
    CHECK(m.queue.aer_outstanding&&m.queue.aborted_commands==0);
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&x)==VNVME_SC_AER_LIMIT);
    /* Abort with a nearly full admin CQ: a 2-entry CQ holds 1 CQE, so the AER
     * cannot be completed alongside the Abort; Abort reports "not aborted". */
    fresh();enable(2);
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);
    CHECK(submit(&aq,cmd(8,0,0,((u32)aer<<16),0),true,NULL)==0&&result_of(&aq)==1);
    CHECK(m.queue.aer_outstanding&&m.queue.queues[0].cq_pending==0);
    /* 3-entry CQ has room for both */
    fresh();enable(3);
    CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);
    u16 slot=aq.head;
    CHECK(submit(&aq,cmd(8,0,0,((u32)aer<<16),0),false,NULL)==-1);
    CHECK(cqe(&aq,slot).cid==aer&&(cqe(&aq,(u16)((slot+1)%aq.n)).result)==0&&!m.queue.aer_outstanding);
    /* Shutdown aborts I/O entries before admin entries (order in the S1DG ring). */
    fresh();enable(8);io_pair(8);
    struct vnvme_command a=cmd(2,1,DATA,0,0);a.cid=600;
    memcpy(ptr(IOSQ+(u64)ioq.tail*64),&a,64);a.cid=601;memcpy(ptr(IOSQ+(u64)(ioq.tail+1)*64),&a,64);
    ioq.tail=(u16)(ioq.tail+2);CHECK(wr(0x1008,4,ioq.tail));
    struct vnvme_command g=cmd(10,0,0,7,0);g.cid=700;memcpy(ptr(ASQ+(u64)aq.tail*64),&g,64);
    g.cid=701;memcpy(ptr(ASQ+(u64)((aq.tail+1)%aq.n)*64),&g,64);aq.tail=(u16)((aq.tail+2)%aq.n);
    CHECK(wr(0x1000,4,aq.tail));
    u64 from=m.diag.write_seq;
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK(rd(0x1c,4)==9&&m.queue.aborted_commands==2);
    int io_at=ring_index_of(S1DG_CQE,1,from),adm_at=ring_index_of(S1DG_CQE,0,from);
    CHECK(io_at>=0&&adm_at>=0&&io_at<adm_at);
    CHECK(m.diag.shutdowns_normal==1&&m.diag.aborted==2);
}
/* r2.1 B4: S1DG content after the StorNVMe-like trace (no guest memory involved). */
static void test_diag_after_trace(void)
{
    const struct s1_diag *g=&m.diag;
    CHECK(g->magic==S1DG_MAGIC&&g->version==1&&g->ring_entries==512&&g->entry_size==40&&g->struct_size==S1DG_SIZE);
    CHECK((u8*)g-(u8*)&m==S1DG_MODEL_OFFSET);
    CHECK(g->admin_opcode[6]==5&&g->admin_opcode[9]==3&&g->admin_opcode[10]==2&&g->admin_opcode[2]==3);
    CHECK(g->admin_opcode[0x0c]==1&&g->admin_opcode[5]==1&&g->admin_opcode[1]==1&&g->admin_opcode[0]==1&&g->admin_opcode[4]==1);
    CHECK(g->io_opcode[2]==3&&g->io_opcode[1]==1&&g->io_opcode[0]==1);
    CHECK(g->writes==1&&g->last_write_lba==0&&g->last_write_nlb==1);
    CHECK(g->nonzero_status==3&&g->last_err_status==VNVME_SC_ABORT_REQUESTED&&g->last_err_qid==0);
    CHECK(g->admin_first_count==18);
    const struct s1dg_admin *f=&g->admin_first[0];
    CHECK(f->sqe[0]==6&&f->completed&&f->status==0);
    u32 c10;memcpy(&c10,f->sqe+40,4);CHECK(c10==1);
    CHECK(g->admin_first[2].sqe[0]==6&&g->admin_first[2].status==VNVME_SC_INVALID_NAMESPACE);
    CHECK(g->admin_first[13].sqe[0]==0x0c&&g->admin_first[13].completed&&g->admin_first[13].status==VNVME_SC_ABORT_REQUESTED);
    CHECK(g->mmio_first[0].kind==1&&g->mmio_first[0].offset==0x10&&g->mmio_first[0].write&&g->mmio_first[0].ok&&g->mmio_first[0].value==(u32)BARB);CHECK(g->mmio_first[3].kind==1&&g->mmio_first[3].offset==0&&!g->mmio_first[3].write);
    CHECK(g->mmio_first_count==82&&g->write_seq==177); /* deterministic trace */
    CHECK(g->shutdowns_normal==1&&g->irq_edges==m.irq_changes&&g->fatal_count==0&&g->first_fatal_reason==0);
    for(unsigned i=0;i<8;i++)CHECK(g->refusals[i]==0&&g->invalid_db[i]==0);
    CHECK(g->en_transitions>=3&&g->intms_writes>=4&&g->intmc_writes>=4&&g->max_cq_pending[1]==1);
    u64 n=g->write_seq<S1DG_RING?g->write_seq:S1DG_RING;
    for(u64 q=g->write_seq-n;q<g->write_seq;q++)CHECK(g->ring[q%S1DG_RING].seq==(u32)q);
    const char *dump=getenv("S1DG_DUMP");
    if(dump){FILE *fp=fopen(dump,"wb");CHECK(fp);CHECK(fwrite(g,1,sizeof(*g),fp)==sizeof(*g));fclose(fp);}
    /* first-N logs saturate without wrapping; the ring wraps by seq */
    struct s1dg_admin keep0=g->admin_first[0];
    for(unsigned i=0;i<700;i++)CHECK(rd(8,4)==0x00010300);
    CHECK(g->mmio_first_count==S1DG_MMIO_FIRST&&g->mmio_first[S1DG_MMIO_FIRST-1].offset==8);
    CHECK(memcmp(&keep0,&g->admin_first[0],sizeof keep0)==0&&g->write_seq==177+700);
    for(u64 q=g->write_seq-S1DG_RING;q<g->write_seq;q++)CHECK(g->ring[q%S1DG_RING].seq==(u32)q&&g->ring[q%S1DG_RING].type==S1DG_MMIO_R);
}
/* r2.1: refusal/fatal/invalid-doorbell reasons are recorded. */
static void test_diag_reasons(void)
{
    fresh();perms[0]=0;CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==2);
    CHECK(m.diag.first_fatal_reason==S1DG_FATAL_ENABLE_ADMQ&&m.diag.refusals[S1DG_REF_PROVIDER]>=1);
    fresh();enable(8);io_pair(8);perms[2]=0;u64 from=m.diag.write_seq;CHECK(wr(0x1008,4,1));
    CHECK(m.diag.first_fatal_reason==VNVME_FATAL_QSPAN_SQ&&ring_index_of(S1DG_FATAL,0,from)>=0);
    CHECK(wr(0x1008,4,2));CHECK(m.diag.invalid_db[1]==1); /* CFS bit */
    fresh();enable(8);CHECK(wr(0x1000,4,0x10001)&&wr(0x1018,4,0));CHECK(m.diag.invalid_db[3]==1&&m.diag.invalid_db[4]==1);
    CHECK(wr(0x14,4,S1_CC_VALID|0xc000));CHECK(m.diag.first_fatal_reason==S1DG_FATAL_SHN11);
    fresh();CHECK(cfgw(4,2,S1_MEM));
    CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));CHECK(wr(0x14,4,S1_CC_VALID));
    CHECK(m.diag.first_fatal_reason==S1DG_FATAL_ENABLE_BME);
    fresh();CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));CHECK(wr(0x14,4,S1_CC_VALID|(1u<<7)));
    CHECK(m.diag.first_fatal_reason==S1DG_FATAL_ENABLE_CC);
    CHECK(rd(0x40,4)==0&&m.diag.reserved_access>=1&&m.diag.last_reserved_offset==0x40);
    CHECK(cfgw(4,2,0));u64 v=0;CHECK(!s1_mmio(&m,BARB,4,false,&v));CHECK(m.diag.mmio_rejected==1);
}
static void test_r21_fixes(void)
{
    /* B2: after CFS, CQ head acks are honoured (INTx drops), SQ doorbells fetch nothing */
    fresh();enable(8);io_pair(8);
    CHECK(submit(&aq,cmd(10,0,0,7,0),false,NULL)==0);CHECK(line);
    perms[2]=0;CHECK(wr(0x1008,4,1));CHECK((rd(0x1c,4)&2)&&m.queue.fatal);
    u64 inv=m.queue.invalid_doorbells;
    aq.head=(u16)((aq.head+1)%aq.n);CHECK(wr(0x1004,4,aq.head));
    CHECK(!line&&m.queue.invalid_doorbells==inv&&m.queue.queues[0].cq_pending==0);
    u64 c=m.queue.stats.commands;CHECK(wr(0x1000,4,(u16)((aq.tail+1)%aq.n)));
    CHECK(m.queue.stats.commands==c&&m.queue.invalid_doorbells==inv+1);
    /* B3: PRP list at a qword offset, and chained through the page's last entry */
    fresh();enable(8);io_pair(8);
    u64 L=RAM+0x40000;u64 *l=ptr(L+256);
    for(unsigned i=0;i<31;i++)l[i]=DATA+(u64)(i+1)*PAGE;
    struct vnvme_command r=cmd(2,1,DATA,0,0);r.prp2=L+256;r.cdw12=31;
    CHECK(submit(&ioq,r,true,NULL)==0);for(unsigned b=0;b<32;b++)pattern(DATA+b*PAGE,b);
    u64 *t=ptr(L+PAGE-24);u64 *nx=ptr(L+2*PAGE);
    t[0]=DATA+PAGE;t[1]=DATA+2*PAGE;t[2]=L+2*PAGE;for(unsigned i=0;i<29;i++)nx[i]=DATA+(u64)(i+3)*PAGE;
    memset(ptr(DATA),0,32*PAGE);r.prp2=L+PAGE-24;CHECK(submit(&ioq,r,true,NULL)==0);
    for(unsigned b=0;b<32;b++)pattern(DATA+b*PAGE,b);
    r.prp2=L+PAGE-20;CHECK(submit(&ioq,r,true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR);   /* not qword aligned */
    {u64 *o=ptr(L+3*PAGE+64);for(unsigned i=0;i<29;i++)o[i]=DATA+(u64)(i+3)*PAGE;}
    t[2]=L+3*PAGE+64;r.prp2=L+PAGE-24;CHECK(submit(&ioq,r,true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR); /* chained pointer with offset */
    t[2]=L+2*PAGE;perms[(L-RAM)/PAGE+2]=0;u64 rb=m.backend_reads;
    CHECK(submit(&ioq,r,true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR&&m.backend_reads==rb);  /* chained page refused */
    perms[(L-RAM)/PAGE+2]=3;nx[4]=L+2*PAGE;CHECK(submit(&ioq,r,true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR); /* overlap */
    /* r4.14: each refusal above is recorded with its rule (the last one: data page over a list fragment) */
    {const struct vnvme_prp_refusal *lr=&m.queue.prp_log[(m.queue.prp_refusal_total-1)%VNVME_PRP_LOG];
     CHECK(m.queue.prp_refusal_total>=4&&lr->rule==VNVME_PRP_DATA_LIST&&lr->addr==L+2*PAGE&&lr->len==PAGE&&
           lr->index==7&&lr->opcode==2&&lr->to_guest&&lr->ordinal==m.queue.prp_refusal_total);
     const struct vnvme_prp_refusal *l3=&m.queue.prp_log[(m.queue.prp_refusal_total-2)%VNVME_PRP_LOG];
     CHECK(l3->rule==VNVME_PRP_LIST_ADMIT&&l3->addr==L+2*PAGE&&l3->index==1);
     const struct vnvme_prp_refusal *l2=&m.queue.prp_log[(m.queue.prp_refusal_total-3)%VNVME_PRP_LOG];
     CHECK(l2->rule==VNVME_PRP_LIST_PTR&&l2->addr==L+3*PAGE+64&&l2->index==1);
     const struct vnvme_prp_refusal *l1=&m.queue.prp_log[(m.queue.prp_refusal_total-4)%VNVME_PRP_LOG];
     CHECK(l1->rule==VNVME_PRP_LIST_PTR&&l1->addr==L+PAGE-20&&l1->index==0);}
    /* r4.14: a READ may name one destination page more than once (Windows' clustered in-page reads
     * point already-resident pages at one dummy page). It succeeds; the repeated page holds the last
     * copy, every other page its block, the skipped page is untouched. A list fragment or a queue as
     * a destination is still refused (above / review-tests). */
    {u64 rf=m.queue.prp_refusal_total,dd=m.queue.prp_dup_dest_reads,rb2=m.backend_reads;
     nx[4]=DATA+5*PAGE;memset(ptr(DATA),0,32*PAGE);CHECK(submit(&ioq,r,true,NULL)==0);
     for(unsigned b=0;b<32;b++){if(b==7)continue;pattern(DATA+b*PAGE,b==5?7:b);}
     {const u8 *z=ptr(DATA+7*PAGE);for(unsigned i=0;i<PAGE;i++)CHECK(z[i]==0);}
     CHECK(m.queue.prp_refusal_total==rf&&m.queue.prp_dup_dest_reads==dd+1&&m.queue.prp_dup_dest_last==DATA&&
           m.backend_reads>rb2);
     /* the first segment (PRP1) repeated later in the list, as in RUN B #33/#35 */
     nx[4]=DATA;memset(ptr(DATA),0,32*PAGE);CHECK(submit(&ioq,r,true,NULL)==0);
     for(unsigned b=1;b<32;b++){if(b==7)continue;pattern(DATA+b*PAGE,b);}
     pattern(DATA,7);CHECK(m.queue.prp_refusal_total==rf&&m.queue.prp_dup_dest_reads==dd+2);
     /* a repeated page that is not admitted is still refused, by data admission */
     nx[4]=RAM+RAM_SIZE+PAGE;CHECK(submit(&ioq,r,true,NULL)==VNVME_SC_DATA_TRANSFER_ERROR);
     CHECK(m.queue.prp_refusal_total==rf+1&&m.queue.prp_log[rf%VNVME_PRP_LOG].rule==VNVME_PRP_DATA_ADMIT);
     /* nightly-4: a partial overlap (unaligned PRP1 repeated in the list as a whole page) is accepted (r4.14:
      * refused, rule data-overlap) and copied in command order: DATA = stream 2048..6143, DATA+PAGE =
      * 6144..10239, DATA+2*PAGE starts with 10240..12287 and keeps its old second half. */
     {u64 L5=L+5*PAGE;u64 *e5=ptr(L5);e5[0]=DATA;e5[1]=DATA+PAGE;e5[2]=DATA+2*PAGE;
      struct vnvme_command p3=cmd(2,1,DATA+2048,0,0);p3.prp2=L5;p3.cdw12=2;u64 rb3=m.backend_reads;
      u64 pd=m.queue.prp_part_dest_reads,rf3=m.queue.prp_refusal_total;
      memset(ptr(DATA),0xa5,4*PAGE);
      CHECK(submit(&ioq,p3,true,NULL)==0&&m.backend_reads>rb3);
      stream(DATA,0,2048,PAGE);stream(DATA+PAGE,0,6144,PAGE);stream(DATA+2*PAGE,0,10240,2048);
      {const u8 *z=ptr(DATA+2*PAGE+2048);for(unsigned i=0;i<PAGE+2048;i++)CHECK(z[i]==0xa5);}
      CHECK(m.queue.prp_refusal_total==rf3&&m.queue.prp_part_dest_reads==pd+1&&m.queue.prp_part_dest_last==DATA+2048);}
     nx[4]=DATA+7*PAGE;}
    /* N2: shutdown after fatal posts nothing more (held AER discarded, admin CQ untouched) */
    fresh();enable(8);io_pair(8);
    {u16 aer;CHECK(submit(&aq,cmd(0x0c,0,0,0,0),true,&aer)==-1);}
    struct vnvme_command rr=cmd(2,1,DATA,0,0);rr.cid=800;memcpy(ptr(IOSQ),&rr,64);rr.cid=801;memcpy(ptr(IOSQ+64),&rr,64);
    ioq.tail=2;CHECK(wr(0x1008,4,2));perms[3]=1; /* IO CQ now read-only: abort CQE refused */
    {u64 comp=m.queue.stats.completions;u8 snap[PAGE];memcpy(snap,ptr(ACQ),PAGE);
     CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK((rd(0x1c,4)&0xa)==0xa);
     CHECK(m.queue.stats.completions==comp&&m.queue.discarded_commands==2&&!m.queue.aer_outstanding);
     CHECK(memcmp(snap,ptr(ACQ),PAGE)==0);}
    /* N1: INTMS resets on CC.EN 1->0 */
    fresh();enable(8);CHECK(wr(0x0c,4,1));CHECK(wr(0x14,4,0));enable(8);CHECK(rd(0x0c,4)==0);
    /* N4: IOSQES/IOCQES may be 0 at enable; required at I/O queue creation */
    fresh();CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,1));CHECK(rd(0x1c,4)==1);aq=(struct q){8,0,0,1,ASQ,ACQ,0};
    CHECK(submit(&aq,cmd(5,0,IOCQ,(7u<<16)|1,3),true,NULL)==VNVME_SC_INVALID_FIELD);
    CHECK(wr(0x14,4,1|(4u<<20)));CHECK(submit(&aq,cmd(5,0,IOCQ,(7u<<16)|1,3),true,NULL)==0);
    CHECK(submit(&aq,cmd(1,0,IOSQ,(7u<<16)|1,(1u<<16)|1),true,NULL)==VNVME_SC_INVALID_FIELD);
    CHECK(wr(0x14,4,1|(6u<<16)|(4u<<20)));CHECK(rd(0x14,4)==(1u|(4u<<20))); /* frozen: CQ exists */
    CHECK(wr(0x14,4,1|(3u<<16)));CHECK(rd(0x1c,4)==1);
    fresh();CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,1|(5u<<16)));CHECK(rd(0x1c,4)==2);                       /* unsupported size: CFS */
    /* N3: admin queue of size 1 cannot be enabled (CFS) */
    fresh();CHECK(wr(0x24,4,0));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==2);
}

/* r2.2 D1: CFS from reserved CC.SHN=11b also stops fetching; a later CQ ack must not run queued READs.
 * Nit: a failed config read is logged with value 0, not the caller's buffer. */
static void test_r22(void)
{
    fresh();enable(8);io_pair(8);
    struct vnvme_command r=cmd(2,1,DATA,0,0);r.cid=900;memcpy(ptr(IOSQ),&r,64);r.cid=901;memcpy(ptr(IOSQ+64),&r,64);
    ioq.tail=2;CHECK(wr(0x1008,4,2));
    u64 reads=m.backend_reads,comp=m.queue.stats.completions,cmds=m.queue.stats.commands;
    CHECK(wr(0x14,4,S1_CC_VALID|0xc000));CHECK((rd(0x1c,4)&2)&&m.queue.fatal);
    CHECK(wr(0x100c,4,1));CHECK(!line);
    CHECK(m.backend_reads==reads&&m.queue.stats.completions==comp&&m.queue.stats.commands==cmds);
    CHECK(m.diag.first_fatal_reason==S1DG_FATAL_SHN11&&m.queue.fatal_reason==S1DG_FATAL_SHN11);
    CHECK(wr(0x14,4,0));CHECK(!m.queue.fatal);enable(8);              /* reset recovers */
    /* enable-time CFS also stops the engine */
    fresh();CHECK(wr(0x24,4,0x00070007));CHECK(wr(0x28,8,ASQ));CHECK(wr(0x30,8,ACQ));
    CHECK(wr(0x14,4,S1_CC_VALID|(1u<<7)));CHECK(rd(0x1c,4)==2&&m.queue.fatal);
    /* failed config reads log 0 */
    fresh();u32 n=m.diag.mmio_first_count;u64 v=0xdeadbeefcafef00dULL;
    CHECK(!s1_config(&m,ECAM+2,4,false,&v));CHECK(v==0xdeadbeefcafef00dULL);
    CHECK(m.diag.mmio_first_count==n+1&&!m.diag.mmio_first[n].ok&&m.diag.mmio_first[n].value==0);
    const struct s1dg_entry *e=&m.diag.ring[(m.diag.write_seq-1)%S1DG_RING];
    CHECK(e->type==S1DG_CFG_R&&e->status==0&&e->b==0);
    v=0x1122334455667788ULL;CHECK(!s1_config(&m,ECAM,8,false,&v));
    CHECK(m.diag.mmio_first[n+1].value==0&&!m.diag.mmio_first[n+1].ok);
    v=0x55;CHECK(!s1_config(&m,ECAM+1,2,true,&v));CHECK(m.diag.mmio_first[n+2].value==0x55&&m.diag.mmio_first[n+2].write);
}
int main(void)
{
    test_identify_and_ids();test_logs();test_features();test_io_commands();
    test_aer_abort_shutdown();test_register_widths();test_fatal_admission();test_gaps();test_r21_fixes();test_r22();test_diag_reasons();test_stornvme_like_trace();test_diag_after_trace();
    for(unsigned i=0;i<32;i++)CHECK(arena.before[i]==0xa7&&arena.after[i]==0xa7);
    CHECK(s0_fixture_guards_valid());
    printf("{\"status\":\"pass\",\"checks\":%u,\"cases\":%u,\"trace_steps\":%u,\"irq_edges\":%u}\n",checks,cases,trace_steps,edges);
    return 0;
}
