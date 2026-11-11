/* Test body appended to exact frozen actual-controller harness platform mock. */
static unsigned scenarios, successful_reads, irq_assertions, irq_deassertions;
static void *gp(u64 address){CHECK(address>=GUEST&&address-GUEST<sizeof(guest));return guest+(address-GUEST);}
static u64 read_at(u64 address,int width){u64 v=0;CHECK(hooks[address>=HV_S1_BAR?1:0](NULL,address,&v,false,width));return v;}
static bool cfgwrite(unsigned off,u64 value,int width){return hooks[0](NULL,HV_S1_ECAM+off,&value,true,width);}
static void wide(unsigned off,u64 value){CHECK(hooks[1](NULL,HV_S1_BAR+off,&value,true,3));}
static void ack(unsigned qid){struct vnvme_queue *q=&controller.queue.queues[qid];CHECK(access_at(HV_S1_BAR+0x1004+qid*8,q->cq_tail));}
static u16 command(unsigned qid,struct vnvme_command c,bool acknowledge){
 struct vnvme_queue *q=&controller.queue.queues[qid];u16 slot=q->cq_tail;
 memcpy(gp(q->sq_addr+(u64)q->sq_tail*sizeof(c)),&c,sizeof(c));
 CHECK(access_at(HV_S1_BAR+0x1000+qid*8,(q->sq_tail+1)%q->sq_size));CHECK(q->cq_pending==1);
 struct vnvme_completion done;memcpy(&done,gp(q->cq_addr+(u64)slot*sizeof(done)),sizeof(done));
 CHECK(done.cid==c.cid&&done.sq_id==qid);
 if(acknowledge){ack(qid);CHECK(q->cq_pending==0);}
 return done.status>>1;
}
static struct vnvme_command cmd(u8 opcode,u32 nsid,u64 data){return(struct vnvme_command){.opcode=opcode,.cid=0x51,.nsid=nsid,.prp1=data};}
static void admin_init(u64 sq,u64 cq,unsigned n){
 CHECK(access_at(HV_S1_BAR+0x24,((n-1)<<16)|(n-1)));wide(0x28,sq);wide(0x30,cq);
 CHECK(access_at(HV_S1_BAR+0x14,S1_CC_VALID));CHECK(read_at(HV_S1_BAR+0x1c,2)==1);
 CHECK(controller.queue.queues[0].sq_head==0&&controller.queue.queues[0].sq_tail==0);
}
static void io_init(u64 sq,u64 cq,unsigned n){
 struct vnvme_command c=cmd(VNVME_ADMIN_CREATE_CQ,0,cq);c.cdw10=((n-1)<<16)|1;c.cdw11=3;CHECK(command(0,c,true)==0);
 c=cmd(VNVME_ADMIN_CREATE_SQ,0,sq);c.cdw10=((n-1)<<16)|1;c.cdw11=(1<<16)|1;CHECK(command(0,c,true)==0);
}
/* nightly-4: bytes [off, off+n) of the READ stream starting at lba (fixture block pattern) */
static void stream(u64 data,unsigned lba,size_t off,size_t n){u8 *p=gp(data);for(size_t k=0;k<n;k++){size_t s=off+k;unsigned i=(unsigned)(s%4096);CHECK(p[k]==(i<32?(u8)((lba+s/4096)*17+i):0));}}
static void pattern(u64 data,unsigned lba){u8 *p=gp(data);for(unsigned i=0;i<4096;i++)CHECK(p[i]==(i<32?(u8)(lba*17+i):0));}
static void read_block(u64 data,unsigned lba,bool acknowledge){
 struct vnvme_command c=cmd(VNVME_IO_READ,1,data);c.cdw10=lba;
 u64 before=controller.backend_reads;CHECK(command(1,c,acknowledge)==0);CHECK(controller.backend_reads==before+1);pattern(data,lba);successful_reads++;
}
static void disable(void){
 CHECK(access_at(HV_S1_BAR+0x14,0));CHECK(read_at(HV_S1_BAR+0x1c,2)==0&&!irq_level);
 CHECK(!controller.queue.queues[0].sq_valid&&!controller.queue.queues[0].cq_valid);
 CHECK(!controller.queue.queues[1].sq_valid&&!controller.queue.queues[1].cq_valid);
 u64 before=controller.backend_reads,inv=controller.queue.invalid_doorbells;CHECK(access_at(HV_S1_BAR+0x1008,1));
 CHECK(controller.backend_reads==before&&controller.queue.invalid_doorbells==inv+1); /* r2: stale doorbell ignored, not a guest abort */
}
static void fresh(void){reset();scenarios++;CHECK(prep()==0);}
static void test_handoff(void){
 fresh();admin_init(GUEST,GUEST+4096,8);
 struct vnvme_command c=cmd(VNVME_ADMIN_IDENTIFY,0,GUEST+16384);c.cdw10=1;CHECK(command(0,c,true)==0);
 c=cmd(VNVME_ADMIN_IDENTIFY,1,GUEST+16384);CHECK(command(0,c,true)==0);u64 nsze;memcpy(&nsze,gp(GUEST+16384),8);CHECK(nsze==40);
 io_init(GUEST+8192,GUEST+12288,8);read_block(GUEST+16384,0,true);read_block(GUEST+16384,39,true);
 CHECK(!irq_level&&controller.queue.queues[1].cq_pending==0);disable();
 /* New owner reallocates different queues and a former queue page as data. */
 memset(guest,0x5a,20480);admin_init(GUEST+20480,GUEST+24576,32);io_init(GUEST+28672,GUEST+32768,32);
 read_block(GUEST,39,false);CHECK(irq_level);disable();
 /* Reuse exact former Windows addresses after explicit disable, no old queue state. */
 memset(guest+20480,0x6b,16384);admin_init(GUEST+20480,GUEST+24576,16);io_init(GUEST+28672,GUEST+32768,16);
 u64 before=controller.backend_reads;ack(1);CHECK(controller.backend_reads==before&&!irq_level);
 read_block(GUEST+36864,0,true);disable();
}
static void test_shutdown_recovery(void){
 /* r2: normal shutdown with an unexecuted entry completes it with Command Abort
  * Requested (no backend read), then SHST complete; later doorbells are ignored. */
 fresh();admin_init(GUEST,GUEST+4096,8);io_init(GUEST+8192,GUEST+12288,8);
 struct vnvme_command a=cmd(VNVME_IO_READ,1,GUEST+16384),b=a;b.cid++;b.cdw10=39;b.prp1=GUEST+20480;
 memcpy(gp(GUEST+8192),&a,sizeof(a));memcpy(gp(GUEST+8192+sizeof(a)),&b,sizeof(b));
 CHECK(access_at(HV_S1_BAR+0x1008,2));CHECK(controller.queue.queues[1].sq_head==1&&controller.queue.queues[1].sq_tail==2&&irq_level);
 pattern(GUEST+16384,0);successful_reads++;
 u64 before=controller.backend_reads;CHECK(access_at(HV_S1_BAR+0x14,S1_CC_VALID|0x4000));
 CHECK(read_at(HV_S1_BAR+0x1c,2)==9&&controller.backend_reads==before);
 struct vnvme_queue *q=&controller.queue.queues[1];CHECK(q->sq_head==2&&q->cq_pending==2);
 struct vnvme_completion d;memcpy(&d,gp(q->cq_addr+sizeof(d)),sizeof(d));CHECK(d.cid==b.cid&&(d.status>>1)==VNVME_SC_ABORT_REQUESTED);
 CHECK(access_at(HV_S1_BAR+0x100c,q->cq_tail));CHECK(q->cq_pending==0&&!irq_level&&controller.backend_reads==before);
 memcpy(gp(GUEST+8192+2*sizeof(a)),&a,sizeof(a));CHECK(access_at(HV_S1_BAR+0x1008,3));CHECK(controller.backend_reads==before);
 disable();admin_init(GUEST,GUEST+4096,8);io_init(GUEST+8192,GUEST+12288,8);read_block(GUEST+16384,0,true);
 /* r2: abrupt shutdown with pending work discards it and drops queues; reset recovers. */
 a.cdw10=1;b.cdw10=2;q=&controller.queue.queues[1];u16 tail=q->sq_tail;
 memcpy(gp(q->sq_addr+tail*sizeof(a)),&a,sizeof(a));memcpy(gp(q->sq_addr+(tail+1)*sizeof(b)),&b,sizeof(b));
 CHECK(access_at(HV_S1_BAR+0x1008,tail+2));before=controller.backend_reads;
 CHECK(access_at(HV_S1_BAR+0x14,S1_CC_VALID|0x8000));CHECK(read_at(HV_S1_BAR+0x1c,2)==9&&!irq_level);
 CHECK(!controller.queue.queues[0].sq_valid&&!controller.queue.queues[1].sq_valid&&!controller.queue.queues[1].cq_valid);
 CHECK(controller.backend_reads==before);successful_reads++;
 CHECK(access_at(HV_S1_BAR+0x14,0));CHECK(read_at(HV_S1_BAR+0x1c,2)==0);
 admin_init(GUEST,GUEST+4096,8);io_init(GUEST+8192,GUEST+12288,8);before=controller.backend_reads;ack(1);CHECK(controller.backend_reads==before);read_block(GUEST+16384,39,true);
}
static void test_intx_and_config(void){
 fresh();CHECK(read_at(HV_S1_ECAM+0x34,0)==0);CHECK((read_at(HV_S1_ECAM+6,1)&0x10)==0);
 CHECK(read_at(HV_S1_ECAM+0x3d,0)==1);CHECK(read_at(HV_S1_ECAM+8,2)==0x01080202);
 CHECK(cfgwrite(0x3d,0xff,0));CHECK(read_at(HV_S1_ECAM+0x3d,0)==1);
 admin_init(GUEST,GUEST+4096,8);io_init(GUEST+8192,GUEST+12288,8);read_block(GUEST+16384,0,false);CHECK(irq_level);
 CHECK(cfgwrite(4,S1_MEM|S1_BME|S1_INTX_DISABLE,1));CHECK(!irq_level&&controller.queue.irq_asserted);
 CHECK(cfgwrite(4,S1_MEM|S1_BME,1));CHECK(irq_level);
 CHECK(access_at(HV_S1_BAR+0x0c,1));CHECK(!irq_level);CHECK(access_at(HV_S1_BAR+0x10,1));CHECK(irq_level);
 CHECK(cfgwrite(4,S1_MEM,1));CHECK(!irq_level);{u64 r=controller.backend_reads;CHECK(access_at(HV_S1_BAR+0x1008,2));CHECK(controller.backend_reads==r);} /* r2: ignored without BME */CHECK(cfgwrite(4,S1_MEM|S1_BME,1));CHECK(irq_level);
 ack(1);CHECK(!irq_level);read_block(GUEST+16384,39,false);CHECK(irq_level);
 CHECK(cfgwrite(4,0,1));CHECK(!irq_level&&!controller.mapped&&!controller.queue.queues[1].sq_valid);
 CHECK(cfgwrite(0x10,UINT32_MAX,2));CHECK(cfgwrite(0x14,UINT32_MAX,2));CHECK(read_at(HV_S1_ECAM+0x10,2)==0xff000004&&read_at(HV_S1_ECAM+0x14,2)==UINT32_MAX);
 CHECK(cfgwrite(0x10,(u32)HV_S1_BAR,2));CHECK(cfgwrite(0x14,HV_S1_BAR>>32,2));CHECK(cfgwrite(4,S1_MEM|S1_BME,1));CHECK(controller.mapped&&read_at(HV_S1_BAR+0x1c,2)==0);
 admin_init(GUEST,GUEST+4096,8);io_init(GUEST+8192,GUEST+12288,8);read_block(GUEST+16384,0,true);
}
static void test_aliases(void){
 fresh();admin_init(GUEST,GUEST+4096,8);
 struct vnvme_command c=cmd(VNVME_ADMIN_CREATE_CQ,0,GUEST);c.cdw10=(7<<16)|1;c.cdw11=3;CHECK(command(0,c,true)==VNVME_SC_INVALID_FIELD);
 io_init(GUEST+8192,GUEST+12288,8);
 for(unsigned i=0;i<4;i++){
  u64 before=controller.backend_reads;c=cmd(VNVME_IO_READ,1,GUEST+4096*i);
  CHECK(command(1,c,true)==VNVME_SC_DATA_TRANSFER_ERROR);CHECK(controller.backend_reads==before);
 }
 /* Three-block READ consumes a PRP list: output cannot overwrite its metadata. */
 u64 *list=gp(GUEST+24576);list[0]=GUEST+24576;list[1]=GUEST+28672;
 c=cmd(VNVME_IO_READ,1,GUEST+16384);c.prp2=GUEST+24576;c.cdw12=2;
 u64 before=controller.backend_reads;CHECK(command(1,c,true)==VNVME_SC_DATA_TRANSFER_ERROR);CHECK(controller.backend_reads==before);
 /* r4.14: an exact whole-page repeat of PRP1 in the list (Windows' in-page dummy page) is accepted;
  * r2.1-r4.13 completed it 0x04. Segments are copied in order: PRP1's page ends with block 1 (the
  * last copy wins) and the third page holds block 2. */
 list[0]=GUEST+16384;list[1]=GUEST+28672;CHECK(command(1,c,true)==0);CHECK(controller.backend_reads>before);
 pattern(GUEST+16384,1);pattern(GUEST+28672,2);
 /* nightly-4: the same repeat behind an unaligned PRP1 is a partial overlap. r4.14 refused it (0x04); it is now
  * accepted and copied in command order: seg0 (stream 0..3583) at +512, then seg1 (stream 3584..7679) over the
  * whole page, seg2 (7680..11775) on the third page, seg3 (11776..12287) on the first 512 bytes of the fourth. */
 before=controller.backend_reads;c.prp1=GUEST+16384+512;list[0]=GUEST+16384;list[1]=GUEST+28672;list[2]=GUEST+32768;
 {static u8 tail[4096-512];memcpy(tail,gp(GUEST+32768+512),sizeof tail);
  CHECK(command(1,c,true)==0);CHECK(controller.backend_reads>before);
  stream(GUEST+16384,0,3584,4096);stream(GUEST+28672,0,7680,4096);stream(GUEST+32768,0,11776,512);
  CHECK(!memcmp(tail,gp(GUEST+32768+512),sizeof tail));
  CHECK(controller.queue.prp_part_dest_reads==1&&controller.queue.prp_part_dest_last==GUEST+16384+512);}
 c.prp1=GUEST+16384;
 /* Adjacent disjoint queue/data pages are legal, as is old queue storage after reset. */
 read_block(GUEST+16384,39,true);disable();admin_init(GUEST+20480,GUEST+24576,8);io_init(GUEST+28672,GUEST+32768,8);read_block(GUEST+8192,0,true);
}
int main(void){
 test_handoff();test_shutdown_recovery();test_intx_and_config();test_aliases();
 CHECK(begin_calls==end_calls&&memory.state==HV_SM_READY);CHECK(s0_fixture_guards_valid());
 CHECK(irq_assertions>0&&irq_deassertions>0);
 printf("{\"status\":\"pass\",\"checks\":%u,\"scenarios\":%u,\"successful_reads\":%u,\"irq_assertions\":%u,\"irq_deassertions\":%u,\"windows_driver_qualified\":false,\"hardware\":false}\n",checks,scenarios,successful_reads,irq_assertions,irq_deassertions);
 return 0;
}
