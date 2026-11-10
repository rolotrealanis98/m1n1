/* SPDX-License-Identifier: MIT */
#include "model.h"
#include "guest_memory.h"
#include "ram_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ECAM UINT64_C(0xf00000000)
#define WINDOW UINT64_C(0xf10000000)
#define RAM UINT64_C(0x100000000)
#define PAGE 4096u
#define RAM_SIZE (256u*1024u)
#define QSIZE 8u
#define ASQ RAM
#define ACQ (RAM+PAGE)
#define IOSQ (RAM+2*PAGE)
#define IOCQ (RAM+3*PAGE)
#define DATA (RAM+0x10000)
static struct s1_model m;
static struct {u8 before[32], bytes[RAM_SIZE],after[32];} arena;
static u8 perms[RAM_SIZE/PAGE];
static struct s0_guest_memory memory={RAM,arena.bytes,RAM_SIZE,perms,RAM_SIZE/PAGE};
static unsigned checks,cases,reads,spans,maps,unmaps,publishes,edges;
static bool line,hook_active,fail_map,fail_unmap;
static u64 hook_address;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x);exit(1);}}while(0)
static void *span(void *opaque,u64 addr,size_t n,bool write)
{CHECK(opaque==&memory);spans++;return s0_guest_span(&memory,addr,n,write);}
static bool map(void *opaque,u64 addr,size_t n)
{CHECK(opaque==&memory);CHECK(n==S1_BAR_SIZE);maps++;if(fail_map)return false;CHECK(!hook_active);hook_active=true;hook_address=addr;return true;}
static bool unmap(void *opaque,u64 addr,size_t n)
{CHECK(opaque==&memory);CHECK(n==S1_BAR_SIZE);unmaps++;CHECK(hook_active&&addr==hook_address);if(fail_unmap)return false;hook_active=false;return true;}
static void irq(void *opaque,bool level)
{CHECK(opaque==&memory);CHECK(level!=line);line=level;edges++;}
static void publish(void *opaque){CHECK(opaque==&memory);publishes++;}
static const struct s1_ops ops={span,map,unmap,irq,publish};
static void *ptr(u64 addr){CHECK(addr>=RAM && addr-RAM<RAM_SIZE);return arena.bytes+(addr-RAM);}
static bool cfgw(u32 off,unsigned bytes,u64 v){return s1_config(&m,ECAM+off,bytes,true,&v);}
static u64 cfgr(u32 off,unsigned bytes){u64 v=0;CHECK(s1_config(&m,ECAM+off,bytes,false,&v));return v;}
static bool wr(u32 off,unsigned bytes,u64 v){return s1_mmio(&m,m.mapped_base+off,bytes,true,&v);}
static u64 rd(u32 off,unsigned bytes){u64 v=0;CHECK(s1_mmio(&m,m.mapped_base+off,bytes,false,&v));return v;}
static void fresh(void)
{
    cases++;line=false;hook_active=false;fail_map=fail_unmap=false;
    memset(&m,0,sizeof(m)); /* host fixture teardown; never a live model reset */
    memset(&arena,0xa7,sizeof(arena));memset(arena.bytes,0,RAM_SIZE);memset(perms,3,sizeof(perms));
    CHECK(s1_init(&m,&ops,&memory,ECAM,WINDOW,2*S1_BAR_SIZE));
    CHECK(!m.mapped && !m.external_irq && !line);
}
static void mapped(void)
{
    fresh();CHECK(cfgw(0x10,4,(u32)WINDOW));CHECK(cfgw(0x14,4,WINDOW>>32));
    CHECK(cfgw(4,2,S1_MEM|S1_BME));CHECK(m.mapped && hook_active && hook_address==WINDOW);
}
static void ready(void)
{
    mapped();CHECK(wr(0x24,4,((QSIZE-1)<<16)|(QSIZE-1)));
    CHECK(wr(0x28,4,(u32)ASQ));CHECK(wr(0x2c,4,ASQ>>32));CHECK(wr(0x30,8,ACQ));
    CHECK(rd(0x28,8)==ASQ && rd(0x30,8)==ACQ);
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);
}
static struct vnvme_command command(u8 opcode,u32 nsid,u64 p1)
{return(struct vnvme_command){.opcode=opcode,.nsid=nsid,.prp1=p1,.cid=0x55};}
static u16 submit(unsigned qid,struct vnvme_command c,bool ack)
{
    struct vnvme_queue *q=&m.queue.queues[qid];u16 cqslot=q->cq_tail;
    memcpy(ptr(q->sq_addr+(u64)q->sq_tail*sizeof(c)),&c,sizeof(c));
    CHECK(wr(0x1000+qid*8,4,(q->sq_tail+1)%q->sq_size));
    CHECK(q->cq_pending==1);
    struct vnvme_completion done;memcpy(&done,ptr(q->cq_addr+(u64)cqslot*sizeof(done)),sizeof(done));
    CHECK(done.cid==c.cid && done.sq_id==qid);
    if(ack){CHECK(wr(0x1004+qid*8,4,q->cq_tail));CHECK(q->cq_pending==0);}
    return done.status>>1;
}
static void io_ready(void)
{
    ready();struct vnvme_command c=command(VNVME_ADMIN_CREATE_CQ,0,IOCQ);
    c.cdw10=((QSIZE-1)<<16)|1;c.cdw11=3;CHECK(submit(0,c,true)==0);
    c=command(VNVME_ADMIN_CREATE_SQ,0,IOSQ);c.cdw10=((QSIZE-1)<<16)|1;c.cdw11=(1<<16)|1;
    CHECK(submit(0,c,true)==0);
}
static void pattern(unsigned lba)
{
    const u8 *p=ptr(DATA);for(unsigned i=0;i<PAGE;i++)CHECK(p[i]==(i<32?(u8)(lba*17+i):0));
}
static void test_config(void)
{
    fresh();CHECK(cfgr(0,4)==0x00101b36);CHECK(cfgr(0,2)==0x1b36);CHECK(cfgr(2,2)==0x10);
    CHECK(cfgr(0,1)==0x36 && cfgr(1,1)==0x1b);CHECK(cfgr(8,4)==0x01080202);
    CHECK(cfgr(0x34,4)==0 && cfgr(0x3d,1)==1);
    CHECK(cfgr(0x8000,4)==UINT32_MAX && cfgr(0x1000,2)==0xffff);
    CHECK(cfgw(0x8000,4,0));CHECK(cfgr(0,4)==0x00101b36);
    CHECK(cfgw(0,4,0));CHECK(cfgr(0,4)==0x00101b36);
    CHECK(cfgw(0x3c,1,61));CHECK(cfgr(0x3c,2)==0x13d);
    CHECK(cfgw(0x3d,1,0xff));CHECK(cfgr(0x3c,2)==0x13d);
    u64 v=0;const unsigned bad[]={0,3,8,16,32};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)CHECK(!s1_config(&m,ECAM,bad[i],false,&v));
    CHECK(!s1_config(&m,ECAM+1,2,true,&v));CHECK(!s1_config(&m,ECAM+2,4,true,&v));
    CHECK(!s1_config(&m,ECAM-1,1,false,&v));CHECK(!s1_config(&m,ECAM+S1_ECAM_SIZE,1,false,&v));
    CHECK(!s1_config(&m,UINT64_MAX,4,false,&v));CHECK(!s1_config(&m,ECAM,4,false,NULL));
    CHECK(cfgw(4,4,UINT32_MAX));CHECK(cfgr(4,4)==(S1_MEM|S1_BME|S1_INTX_DISABLE));
    CHECK(cfgw(5,1,0));CHECK(cfgr(4,4)==(S1_MEM|S1_BME));CHECK(!m.mapped);
}
static void test_mapping(void)
{
    mapped();u64 old=m.mapped_base;unsigned u=unmaps;
    CHECK(cfgw(0x10,4,(u32)(WINDOW+S1_BAR_SIZE)));CHECK(m.mapped_base==WINDOW+S1_BAR_SIZE);
    CHECK(unmaps==u+1);u64 v=0;CHECK(!s1_mmio(&m,old,4,false,&v));
    CHECK(rd(8,4)==0x00010300);CHECK(cfgw(4,2,S1_BME));CHECK(!m.mapped&&!hook_active);
    CHECK(!s1_mmio(&m,WINDOW+S1_BAR_SIZE,4,false,&v));
    CHECK(cfgw(4,2,S1_MEM|S1_BME));CHECK(m.mapped);
    CHECK(!cfgw(0x10,4,(u32)(WINDOW+2*S1_BAR_SIZE)));CHECK(!m.mapped&&!hook_active);
    CHECK(cfgw(0x10,4,(u32)WINDOW));CHECK(m.mapped);
    CHECK(!cfgw(0x14,4,UINT32_MAX-1));CHECK(!m.mapped);
    CHECK(cfgw(0x14,4,WINDOW>>32));CHECK(m.mapped);
    CHECK(cfgw(0x10,4,UINT32_MAX));CHECK(!m.mapped);CHECK(cfgr(0x10,4)==0xff000004);
    CHECK(cfgw(0x14,4,UINT32_MAX));CHECK(!m.mapped);
    CHECK(cfgw(0x10,4,(u32)WINDOW));CHECK(!m.mapped);
    CHECK(cfgw(0x14,4,WINDOW>>32));CHECK(m.mapped);
    mapped();fail_map=true;CHECK(!cfgw(0x10,4,(u32)(WINDOW+S1_BAR_SIZE)));
    CHECK(m.faulted&&!m.mapped&&!hook_active&&!line);unsigned before=maps;
    CHECK(!cfgw(0x10,4,(u32)WINDOW));CHECK(maps==before);
    CHECK(!s1_mmio(&m,WINDOW,4,false,&v));
    mapped();fail_unmap=true;CHECK(!cfgw(4,2,0));CHECK(m.faulted&&!m.mapped&&hook_active);
    CHECK(!s1_mmio(&m,WINDOW,4,false,&v));before=unmaps;CHECK(!cfgw(4,2,0));CHECK(unmaps==before);
}
static void test_cc_widths(void)
{
    /* r2: register accesses never fail inside the BAR; NVMe-visible effects only. */
    ready();CHECK(rd(0,8)==((255ULL)|(1ULL<<16)|(10ULL<<24)|(1ULL<<37)));
    CHECK(rd(0,4)==(u32)rd(0,8));CHECK(rd(4,4)==(rd(0,8)>>32));
    CHECK(rd(0,2)==0xff && rd(2,1)==1 && rd(3,1)==10 && rd(0x1c,1)==1); /* sub-dword reads */
    CHECK(rd(0x29,2)==((ASQ>>8)&0xffff));                                 /* misaligned read */
    CHECK(wr(0x28,8,ASQ+PAGE));CHECK(rd(0x28,8)==ASQ);                    /* ignored while enabled */
    CHECK(wr(0x24,4,0));CHECK(rd(0x24,4)==(((QSIZE-1)<<16)|(QSIZE-1)));
    CHECK(wr(0x14,4,S1_CC_VALID|(1<<7)));CHECK(rd(0x14,4)==S1_CC_VALID&&rd(0x1c,4)==1); /* live edit ignored */
    const unsigned bad[]={0,3,16};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){u64 v=0;CHECK(!s1_mmio(&m,WINDOW+0x14,bad[i],true,&v));}
    const unsigned narrow[]={1,2};
    for(unsigned i=0;i<2;i++){u64 v=0;CHECK(s1_mmio(&m,WINDOW+0x14,narrow[i],true,&v));CHECK(rd(0x1c,4)==1&&rd(0x14,4)==S1_CC_VALID);}
    CHECK(wr(0x15,4,0));CHECK(rd(0x14,4)==S1_CC_VALID);                   /* misaligned write ignored */
    CHECK(wr(0x14,8,S1_CC_VALID));CHECK(rd(0x1c,4)==1);                   /* qword = CC then reserved 0x18 */
    CHECK(wr(0x2c,8,0));CHECK(rd(0x28,8)==ASQ&&rd(0x30,8)==ACQ);
    u64 inv=m.queue.invalid_doorbells;
    CHECK(wr(0x1000,8,0));CHECK(m.queue.invalid_doorbells==inv);          /* SQ0 tail 0 + CQ0 head 0 */
    CHECK(wr(0x1002,4,1));CHECK(m.queue.invalid_doorbells==inv);          /* misaligned: ignored */
    CHECK(wr(0x1010,4,1));CHECK(wr(0x1000,4,0x10001));CHECK(wr(0x1000,4,QSIZE));
    CHECK(m.queue.invalid_doorbells==inv+3&&m.queue.stats.commands==0);
    CHECK(rd(0x1000,4)==0&&rd(0x2000,4)==0&&rd(0x40,8)==0);               /* doorbells/reserved read 0 */
    CHECK(rd(0x1c,4)==1);CHECK(wr(0x1c,4,0));CHECK(rd(0x1c,4)==1);
    CHECK(wr(0x14,4,0));CHECK(rd(0x1c,4)==0);CHECK(!m.queue.queues[0].sq_valid);
    CHECK(wr(0x1000,4,1));CHECK(m.queue.stats.commands==0);CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);
    CHECK(wr(0x14,4,0));CHECK(wr(0x24,4,0xf007f007));CHECK(rd(0x24,4)==0x00070007); /* reserved masked */
    CHECK(wr(0x14,4,S1_CC_VALID|(1<<7)));CHECK(rd(0x1c,4)==2);            /* invalid CC -> CFS */
    CHECK(wr(0x14,4,0));CHECK(wr(0x28,8,UINT64_MAX-4095));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==2);                   /* refused ASQ -> CFS */
    ready();CHECK(cfgw(4,2,S1_MEM));unsigned before=spans;
    CHECK(wr(0x1000,4,1));CHECK(spans==before);
    CHECK(wr(0x14,4,0));CHECK(wr(0x14,4,S1_CC_VALID));CHECK(spans==before);
    CHECK(rd(0x1c,4)==2);CHECK(wr(0x14,4,0));CHECK(cfgw(4,2,S1_MEM|S1_BME));
    CHECK(wr(0x14,4,S1_CC_VALID));CHECK(rd(0x1c,4)==1);
}
static void test_reads_irq(void)
{
    io_ready();struct vnvme_command c=command(VNVME_IO_READ,1,DATA);c.cdw10=39;
    CHECK(submit(1,c,false)==0);pattern(39);reads++;
    CHECK(line&&m.queue.irq_asserted&&publishes>0);unsigned e=edges;
    CHECK(wr(0xc,4,2));CHECK(line&&edges==e); /* nonexistent vector1 doesn't mask0 */
    CHECK(wr(0xc,4,1));CHECK(!line&&m.queue.irq_asserted);
    CHECK(wr(0x10,4,1));CHECK(line);
    CHECK(cfgw(4,2,S1_MEM|S1_BME|S1_INTX_DISABLE));CHECK(!line);
    CHECK(cfgw(4,2,S1_MEM|S1_BME));CHECK(line);
    CHECK(cfgw(4,2,S1_MEM));CHECK(!line);unsigned before=spans;
    CHECK(wr(0x100c,4,m.queue.queues[1].cq_tail));CHECK(spans==before&&m.queue.queues[1].cq_pending==1); /* r2: ignored without BME */
    CHECK(cfgw(4,2,S1_MEM|S1_BME));CHECK(line);
    CHECK(wr(0x100c,4,m.queue.queues[1].cq_tail));CHECK(!line&&!m.queue.irq_asserted);
    CHECK(submit(1,c,false)==0);CHECK(line);reads++;
    CHECK(wr(0x14,4,0));CHECK(!line&&!m.queue.irq_asserted);
    CHECK(!m.queue.queues[1].sq_valid);CHECK(wr(0x14,4,S1_CC_VALID));CHECK(!line);
    io_ready();c=command(VNVME_IO_READ,1,DATA);CHECK(submit(1,c,false)==0);reads++;
    CHECK(cfgw(0x10,4,(u32)(WINDOW+S1_BAR_SIZE)));CHECK(!line&&rd(0x1c,4)==0);
    CHECK(!m.queue.queues[1].sq_valid);
}
static void test_admin_and_policy(void)
{
    io_ready();struct vnvme_command c=command(VNVME_ADMIN_IDENTIFY,0,DATA);c.cdw10=1;
    CHECK(submit(0,c,true)==0);CHECK(memcmp((u8*)ptr(DATA)+24,"S0",2)==0);
    c.cdw10=0;c.nsid=1;CHECK(submit(0,c,true)==0);u64 blocks;memcpy(&blocks,ptr(DATA),8);CHECK(blocks==40);
    c=command(VNVME_IO_READ,1,DATA);CHECK(submit(1,c,true)==0);pattern(0);reads++;
    const u8 denied[]={9,8,0x80,0xff};u64 before=m.backend_reads;
    for(unsigned i=0;i<sizeof(denied);i++){c.opcode=denied[i];CHECK(submit(1,c,true)==VNVME_SC_INVALID_OPCODE);}
    c.opcode=1;CHECK(submit(1,c,true)==VNVME_SC_NS_WRITE_PROTECTED); /* r2 */
    c.opcode=0;CHECK(submit(1,c,true)==VNVME_SC_SUCCESS);            /* r2 FLUSH no-op */
    c.opcode=VNVME_IO_READ;
    CHECK(m.backend_reads==before);CHECK(s0_fixture_guards_valid());
    c=command(VNVME_IO_READ,2,DATA);CHECK(submit(1,c,true)==VNVME_SC_INVALID_NAMESPACE);
    c.nsid=1;c.cdw10=40;CHECK(submit(1,c,true)==VNVME_SC_INVALID_FIELD);
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK(rd(0x1c,4)==9);
    CHECK(wr(0x1008,4,(m.queue.queues[1].sq_tail+1)%QSIZE));CHECK(m.backend_reads==before); /* r2: ignored */
    /* r2: after shutdown a reset is required (NVMe 1.3 7.6.2). */
    io_ready();before=m.backend_reads;
    /* Invalid guest output must reach the S0 validator; no backend read. */
    perms[16]=1;c.cdw10=0;CHECK(submit(1,c,true)==VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(m.backend_reads==before);
}
static void test_init_bounds(void)
{
    mapped();unsigned before=maps;
    CHECK(!s1_init(&m,&ops,&memory,ECAM,WINDOW,2*S1_BAR_SIZE));
    CHECK(m.mapped&&hook_active&&maps==before);
    fresh();memset(&m,0,sizeof(m));
    CHECK(!s1_init(&m,&ops,&memory,ECAM+1,WINDOW,S1_BAR_SIZE));
    CHECK(!s1_init(&m,&ops,&memory,ECAM,WINDOW+1,S1_BAR_SIZE));
    CHECK(!s1_init(&m,&ops,&memory,ECAM,WINDOW,0));
    CHECK(!s1_init(&m,&ops,&memory,ECAM,WINDOW,S1_BAR_SIZE+1));
    CHECK(!s1_init(&m,&ops,&memory,WINDOW,WINDOW,S1_BAR_SIZE));
    CHECK(!s1_init(&m,&ops,&memory,ECAM,WINDOW,UINT64_MAX));
    CHECK(!s1_init(&m,NULL,&memory,ECAM,WINDOW,S1_BAR_SIZE));
    CHECK(!s1_init(&m,&ops,&memory,WINDOW+S1_BAR_SIZE,WINDOW,2*S1_BAR_SIZE));
}
static void test_shutdown_backpressure(void)
{
    io_ready();
    struct vnvme_command c=command(VNVME_IO_READ,1,DATA);
    memcpy(ptr(IOSQ),&c,sizeof(c));c.cid++;c.cdw10=1;
    memcpy(ptr(IOSQ+sizeof(c)),&c,sizeof(c));
    CHECK(wr(0x1008,4,2));
    CHECK(m.queue.queues[1].sq_head==1 && m.queue.queues[1].sq_tail==2);
    CHECK(m.queue.queues[1].cq_pending==1 && line);
    u64 before=m.backend_reads;reads++;
    /* r2: normal shutdown with an unexecuted entry: it completes with Command
     * Abort Requested (no backend read) and SHST completes; never a guest abort. */
    CHECK(wr(0x14,4,S1_CC_VALID|0x4000));CHECK(rd(0x1c,4)==9);
    CHECK(m.backend_reads==before && m.queue.queues[1].sq_head==2 && m.queue.queues[1].cq_pending==2);
    struct vnvme_completion d;memcpy(&d,ptr(IOCQ+sizeof(d)),sizeof(d));
    CHECK(d.cid==c.cid && (d.status>>1)==VNVME_SC_ABORT_REQUESTED && m.queue.aborted_commands==1);
    CHECK(wr(0x100c,4,m.queue.queues[1].cq_tail));
    CHECK(m.backend_reads==before && !line && m.queue.queues[1].cq_pending==0);
    u64 inv=m.queue.invalid_doorbells;
    CHECK(wr(0x1008,4,3));CHECK(m.backend_reads==before&&m.queue.invalid_doorbells==inv+1);
    /* r2: abrupt shutdown discards and drops queues. */
    io_ready();memcpy(ptr(IOSQ),&c,sizeof(c));memcpy(ptr(IOSQ+sizeof(c)),&c,sizeof(c));
    CHECK(wr(0x1008,4,2));before=m.backend_reads;
    CHECK(wr(0x14,4,S1_CC_VALID|0x8000));CHECK(rd(0x1c,4)==9);
    CHECK(!m.queue.queues[0].sq_valid&&!m.queue.queues[1].cq_valid&&!line&&m.queue.discarded_commands==1);
    CHECK(m.backend_reads==before);
    /* reserved SHN 11b -> CFS */
    io_ready();CHECK(wr(0x14,4,S1_CC_VALID|0xc000));CHECK((rd(0x1c,4)&2)==2);
}
int main(void)
{
    test_config();test_mapping();test_cc_widths();test_reads_irq();test_admin_and_policy();test_init_bounds();test_shutdown_backpressure();
    for(unsigned i=0;i<32;i++)CHECK(arena.before[i]==0xa7&&arena.after[i]==0xa7);
    CHECK(s0_fixture_guards_valid());
    printf("{\"status\":\"pass\",\"checks\":%u,\"fixture_resets\":%u,\"read_scenarios\":%u,\"map_calls\":%u,\"unmap_calls\":%u,\"irq_edges\":%u,\"physical_backend\":false}\n",checks,cases,reads,maps,unmaps,edges);
    return 0;
}
