"""Actual resident controller+S1 queue/model, mocked platform and lease boundary.
The separate check-memory.py qualifies the real resolver/provider. No hardware.
"""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parent
SRC=ROOT/'source/src/hv/win'
out=Path(tempfile.mkdtemp(prefix='m5w-s1-controller-'))
(out/'types.h').write_text('#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\ntypedef uint64_t u64;typedef uint32_t u32;typedef uint16_t u16;typedef uint8_t u8;typedef int64_t s64;\n')
code=(SRC/'hv_win_storage.c').read_text()
code='\n'.join(line for line in code.splitlines() if not line.startswith('#include') and line!='extern char _base[], _end[];')
pre=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "ram_disk.h"
#include "hv_win_storage.h"
#include "hv_win_storage_mem.h"
#define _base ((char*)0x1000)
#define _end ((char*)0x2000)
#define sysop(x) ((void)0)
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
struct exc_info {int unused;};
static struct {unsigned nr_vcpus;int vcpu_cpu[1];} hv_win={1,{0}};
static int boot_cpu_idx;
static bool gic=true,spi_free=true;
static u32 uart_irq=64;
static unsigned checks,hook_calls,begin_calls,end_calls,span_calls;
static bool reg_fail,begin_fail,end_fail,busy,irq_level;
static unsigned fail_hook;
typedef bool (*hook_t)(struct exc_info*,u64,u64*,bool,int);
static hook_t hooks[2];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x);exit(1);}}while(0)
static bool hv_win_vgic_enabled(void){return gic;}
bool hv_vgic3_spi_unassigned(u32 n){CHECK(n==66);return spi_free;}
u32 hv_win_pl011_gsiv(void){return uart_irq;}
static u64 hv_pt_walk(u64 a){CHECK((a>=HV_S1_ECAM&&a<HV_S1_ECAM+S1_ECAM_SIZE)||(a>=HV_S1_BAR&&a<HV_S1_BAR+S1_BAR_SIZE));return busy?1:0;}
static int hv_map_hook(u64 a,hook_t h,u64 size){unsigned i=a==HV_S1_ECAM?0:1;CHECK(a==HV_S1_ECAM||a==HV_S1_BAR);CHECK(size==(i?S1_BAR_SIZE:S1_ECAM_SIZE));hook_calls++;if(fail_hook==hook_calls)return -1;hooks[i]=h;return 0;}
static void hv_vgic3_set_spi_level(u32 n,bool level){CHECK(n==66);irq_level=level;}
static unsigned char guest[32768];
#define GUEST UINT64_C(0x100000000)
bool hv_storage_mem_register(struct hv_storage_mem *m,const struct hv_sm_config *c){CHECK(m->state==HV_SM_EMPTY);CHECK(c->nr_ram==1&&c->nr_protected==1);m->epoch=c->epoch;m->state=reg_fail?HV_SM_DEAD:HV_SM_READY;return !reg_fail;}
bool hv_storage_mem_begin(struct hv_storage_mem *m,u64 e){begin_calls++;CHECK(m->epoch==e);if(begin_fail){m->state=HV_SM_DEAD;return false;}CHECK(m->state==HV_SM_READY);m->state=HV_SM_LEASED;return true;}
bool hv_storage_mem_end(struct hv_storage_mem *m){end_calls++;CHECK(m->state==HV_SM_LEASED);m->state=end_fail?HV_SM_DEAD:HV_SM_READY;return !end_fail;}
void hv_storage_mem_retire(struct hv_storage_mem *m){m->state=HV_SM_DEAD;}
void *hv_storage_guest_span(void *opaque,u64 a,size_t n,bool w){(void)w;span_calls++;struct hv_storage_mem *m=opaque;CHECK(m->state==HV_SM_LEASED);if(a<GUEST||n>sizeof(guest)||a-GUEST>sizeof(guest)-n)return NULL;return guest+(a-GUEST);}
'''
post=r'''
static struct {struct hv_s1_registration head;struct hv_sm_range ranges[2];} packet;
static void reset(void){
 memset(&controller,0,sizeof(controller));memset(&memory,0,sizeof(memory));
 attempted=ready=frames_reserved=false;epoch=dispatches=rejections=lease_failures=0;
 reg_fail=begin_fail=end_fail=busy=irq_level=false;fail_hook=0;hook_calls=begin_calls=end_calls=span_calls=0;
 memset(hooks,0,sizeof(hooks));memset(guest,0,sizeof(guest));gic=spi_free=true;uart_irq=64;
 packet=(typeof(packet)){.head={.device={HV_S1_MAGIC,1,HV_S1_ECAM,HV_S1_BAR,S1_BAR_SIZE,66,7,4096,0,40,0},.epoch=17,.resident_base=0x1000,.resident_size=UINT64_MAX-0x1001,.nr_ram=1,.nr_protected=1},.ranges={{GUEST,sizeof(guest)},{GUEST+sizeof(guest),4096}}};
}
static s64 prep(void){return hv_win_storage_prepare((u64)&packet,sizeof(packet));}
static bool access_at(u64 a,u64 v){CHECK(hooks[1]!=NULL);return hooks[1](NULL,a,&v,true,2);}
static u16 submit(unsigned qid,struct vnvme_command c){
 struct vnvme_queue *q=&controller.queue.queues[qid];u16 slot=q->cq_tail;
 memcpy(guest+(q->sq_addr-GUEST)+(u64)q->sq_tail*sizeof(c),&c,sizeof(c));
 CHECK(access_at(HV_S1_BAR+0x1000+qid*8,(q->sq_tail+1)%q->sq_size));CHECK(q->cq_pending==1);
 struct vnvme_completion done;memcpy(&done,guest+(q->cq_addr-GUEST)+(u64)slot*sizeof(done),sizeof(done));
 CHECK(done.cid==c.cid&&done.sq_id==qid);CHECK(access_at(HV_S1_BAR+0x1004+qid*8,q->cq_tail));CHECK(q->cq_pending==0);
 return done.status>>1;
}
int main(void){
 CHECK(sizeof(packet)==128);CHECK(offsetof(struct hv_s1_registration,epoch)==64);CHECK(offsetof(struct hv_s1_registration,nr_ram)==88);
 CHECK(offsetof(struct hv_s1_descriptor,blocks)==48);
 reset();CHECK(prep()==0);CHECK(ready&&frames_reserved&&controller.mapped&&memory.state==HV_SM_READY);CHECK(hook_calls==2);CHECK(prep()==-1);
 u64 v=0;CHECK(hooks[0](NULL,HV_S1_ECAM,&v,false,2));CHECK(v==0x00101b36);
 CHECK(access_at(HV_S1_BAR+0x24,0x70007));
 v=GUEST;CHECK(hooks[1](NULL,HV_S1_BAR+0x28,&v,true,3));v=GUEST+4096;CHECK(hooks[1](NULL,HV_S1_BAR+0x30,&v,true,3));
 CHECK(access_at(HV_S1_BAR+0x14,S1_CC_VALID));CHECK(controller.regs.csts==1);
 struct vnvme_command c={.opcode=VNVME_ADMIN_IDENTIFY,.cid=42,.prp1=GUEST+8192,.cdw10=1};
 memcpy(guest,&c,sizeof(c));CHECK(access_at(HV_S1_BAR+0x1000,1));CHECK(controller.queue.queues[0].cq_pending==1);
 struct vnvme_completion done;memcpy(&done,guest+4096,sizeof(done));CHECK(done.cid==42&&(done.status>>1)==0);CHECK(memcmp(guest+8192+24,"S0",2)==0);CHECK(irq_level);
 CHECK(access_at(HV_S1_BAR+0x1004,1));CHECK(!irq_level);CHECK(begin_calls==end_calls&&memory.state==HV_SM_READY&&span_calls>0);
 c=(struct vnvme_command){.opcode=VNVME_ADMIN_CREATE_CQ,.cid=43,.prp1=GUEST+16384,.cdw10=(7<<16)|1,.cdw11=3};CHECK(submit(0,c)==0);
 c=(struct vnvme_command){.opcode=VNVME_ADMIN_CREATE_SQ,.cid=44,.prp1=GUEST+12288,.cdw10=(7<<16)|1,.cdw11=(1<<16)|1};CHECK(submit(0,c)==0);
 c=(struct vnvme_command){.opcode=VNVME_IO_READ,.cid=45,.nsid=1,.prp1=GUEST+20480,.cdw10=39};CHECK(submit(1,c)==0);
 for(unsigned i=0;i<4096;i++)CHECK(guest[20480+i]==(i<32?(u8)(39*17+i):0));
 CHECK(hv_win_storage_stat(5)==1&&controller.backend_reads==1);CHECK(begin_calls==end_calls&&!irq_level);
 unsigned end_before=end_calls;CHECK(access_at(HV_S1_BAR+0x1000,0x10001));CHECK(controller.queue.invalid_doorbells==1&&controller.backend_reads==1&&controller.regs.csts==1);/*r2: invalid doorbell ignored, not a guest abort*/CHECK(end_calls==end_before+1&&memory.state==HV_SM_READY);
 hv_win_storage_retire();CHECK(!ready&&memory.state==HV_SM_DEAD);CHECK(!access_at(HV_S1_BAR+8,0));
 reset();packet.head.device.bar+=S1_BAR_SIZE;CHECK(prep()==-1&&hook_calls==0&&!ready);CHECK(prep()==-1);
 reset();packet.head.nr_ram=65;CHECK(prep()==-1&&hook_calls==0);
 reset();packet.head.device.reserved1=1;CHECK(prep()==-1&&hook_calls==0);
 reset();packet.head.device.flags=3;CHECK(prep()==-1&&hook_calls==0);
 reset();packet.head.resident_size=1;CHECK(prep()==-1&&hook_calls==0);
 reset();packet.ranges[0].base=HV_S1_BAR;CHECK(prep()==-1&&hook_calls==0);
 reset();uart_irq=66;CHECK(prep()==-1&&hook_calls==0);
 reset();spi_free=false;CHECK(prep()==-1&&hook_calls==0);
 reset();busy=true;CHECK(prep()==-1&&hook_calls==0);
 reset();reg_fail=true;CHECK(prep()==-1&&hook_calls==0);
 reset();fail_hook=1;CHECK(prep()==-1&&hook_calls==1&&!ready&&memory.state==HV_SM_DEAD);
 reset();fail_hook=2;CHECK(prep()==-1&&hook_calls==2&&!ready&&memory.state==HV_SM_DEAD);CHECK(hooks[0]);v=0;CHECK(!hooks[0](NULL,HV_S1_ECAM,&v,false,2));
 reset();CHECK(prep()==0);begin_fail=true;CHECK(!access_at(HV_S1_BAR+8,0));CHECK(!ready&&memory.state==HV_SM_DEAD&&end_calls==0);
 reset();CHECK(prep()==0);end_fail=true;CHECK(!access_at(HV_S1_BAR+8,0));CHECK(!ready&&memory.state==HV_SM_DEAD&&end_calls==1);
 reset();CHECK(hv_win_storage_prepare((u64)&packet,sizeof(packet)-1)==-1&&hook_calls==0);
 reset();CHECK(hv_win_storage_prepare((u64)&packet+1,sizeof(packet))==-1&&hook_calls==0);
 printf("%u actual-controller/model checks PASS (platform+lease boundary mocked; realprovider tested separately)\n",checks);
 return 0;
}
'''
(out/'test.c').write_text(pre+code+post)
cmd=['clang','-std=gnu11','-DVNVME_HOST_TEST','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(SRC),'-I'+str(SRC/'s1'),str(out/'test.c')]+[str(SRC/'s1'/n) for n in ['hv_nvme.c','hv_pci.c','hv_nvme_queue.c','ram_disk.c']]+['-o',str(out/'test')]
build=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
if build.returncode:raise RuntimeError(build.stderr)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1')
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,env=env,timeout=15)
receipt={'status':'pass','output_directory':str(out),'output':r.stdout,'commands':[cmd],'source_sha256':hashlib.sha256((SRC/'hv_win_storage.c').read_bytes()).hexdigest(),'hardware_access':False,'platform_and_lease_boundary_mocked':True}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
