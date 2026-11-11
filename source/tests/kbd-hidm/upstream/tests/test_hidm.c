/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../service/hidm_script.h"
#include "../driver/hidm_core.h"
static hm_service s;static hm_script b;static hm_client c;static unsigned tests;
static hm_u32 rd(void *x,hm_u32 p,hm_u32 o){return hm_service_read(x,p,o);}
static void wr(void *x,hm_u32 p,hm_u32 o,hm_u32 v){hm_service_write(x,p,o,v);}
static hm_io io={&s,rd,wr};
#define TEST(name) do {name();tests++;printf("PASS %s\n",#name);}while(0)
static hm_u32 cmd(hm_u32 op,hm_u32 id,hm_u32 off,hm_u32 len,hm_response *r){
 hm_request q={0};q.op=op;q.iface=id;q.offset=off;q.length=len;return hm_client_command(&c,&q,r);
}
static void setup(int mouse){hm_response r;assert(hm_script_init(&s,&b,mouse)==HM_OK);assert(hm_client_open(&c,io)==HM_OK);assert(cmd(HM_ACTIVATE,2,0,0,&r)==HM_OK);}
static void enumeration(void){setup(1);assert(c.count==2);assert(c.devices[0].info.id==2);assert(c.devices[1].info.id==1);assert(c.devices[0].layout.length[0][1]==9);assert(c.devices[1].layout.length[0][1]==4);}
static void handshake(void){hm_request q={0};hm_response r;setup(0);q.seq=500;q.abi=99;q.op=HM_HELLO;hm_copy(s.data,&q,sizeof(q));wr(&s,0,HM_R_DOORBELL,500);hm_copy(&r,s.data+HM_RESP_OFF,sizeof(r));assert(r.status==HM_BAD_VERSION);assert(cmd(HM_BYE,0,0,0,&r)==HM_OK);assert(cmd(HM_ENUM,0,0,0,&r)==HM_NOT_READY);}
static void wrap(void){hm_slot v;unsigned i;setup(0);for(i=0;i<10000;i++){b.step=0;assert(hm_script_tick(&s,&b,i)==HM_OK);assert(hm_client_pop(&c,&v)==HM_OK);assert(v.timestamp_ns==i&&v.payload[3]==4);}assert(hm_client_pop(&c,&v)==HM_NOT_READY);}
static void counter_rollover(void){hm_slot v;unsigned i;setup(0);s.head=s.tail=c.tail=0xfffffffcu;for(i=0;i<16;i++){b.step=0;assert(hm_script_tick(&s,&b,i)==HM_OK);assert(hm_client_pop(&c,&v)==HM_OK);assert(v.timestamp_ns==i);}assert(s.head==12&&s.tail==12);}
static void overflow(void){hm_slot v;unsigned i;setup(0);for(i=0;i<HM_SLOTS;i++){b.step=0;assert(hm_script_tick(&s,&b,i)==HM_OK);}b.step=0;assert(hm_script_tick(&s,&b,99)==HM_FULL);assert(s.overflow==1);for(i=0;i<HM_SLOTS;i++){assert(hm_client_pop(&c,&v)==HM_OK);assert(v.timestamp_ns==i);}assert(hm_client_pop(&c,&v)==HM_OK&&v.timestamp_ns==99);assert(hm_client_pop(&c,&v)==HM_NOT_READY);}
/* Integration fix: a burst cut by a full ring must still end in the final state (no stuck key). */
static void overflow_resync(void){static const hm_u8 st[3][9]={{1,0,0,5},{1,0,0,5,0x11},{1}};hm_slot v;hm_response r;unsigned k,n=0;setup(0);
 for(k=0;k<24;k++)(void)hm_service_push(&s,2,st[k%3],9,k);assert(s.overflow==16&&s.pending==1);
 while(hm_client_pop(&c,&v)==HM_OK)n++;assert(n==9);assert(v.timestamp_ns==23&&v.payload[3]==0&&v.payload[4]==0);
 for(k=0;k<9;k++)(void)hm_service_push(&s,2,st[k%3],9,k);assert(s.pending==1);assert(cmd(HM_DEACTIVATE,2,0,0,&r)==HM_OK&&s.pending==0);
 assert(cmd(HM_ACTIVATE,2,0,0,&r)==HM_OK);for(k=0;k<HM_COALESCE+HM_SLOTS+4;k++)(void)hm_service_push(&s,2,st[k%3],9,k);assert(s.pending==1);}

static void bad_tail(void){setup(0);assert(hm_script_tick(&s,&b,0)==HM_OK);wr(&s,0,HM_R_TAIL,2);assert(s.tail==0);wr(&s,0,HM_R_TAIL,0xffffffffu);assert(s.tail==0);wr(&s,0,HM_R_TAIL,1);assert(s.tail==1);}
static void irq(void){hm_slot v;setup(0);assert(!hm_service_irq(&s));assert(hm_script_tick(&s,&b,0)==HM_OK);assert(!hm_service_irq(&s));wr(&s,0,HM_R_ENABLE,1);assert(hm_service_irq(&s));wr(&s,0,HM_R_ENABLE,0);assert(!hm_service_irq(&s));wr(&s,0,HM_R_ENABLE,1);assert(hm_client_pop(&c,&v)==HM_OK);assert(!hm_service_irq(&s));}
static void descriptor_bounds(void){hm_response r;hm_u32 n;setup(0);n=c.devices[0].info.descriptor_length;assert(cmd(HM_DESCRIPTOR,2,0,n,&r)==HM_OK);assert(!memcmp(r.payload,c.devices[0].descriptor,n));assert(cmd(HM_DESCRIPTOR,2,n,1,&r)==HM_RANGE);assert(cmd(HM_DESCRIPTOR,2,0xffffffffu,8,&r)==HM_RANGE);assert(cmd(HM_DESCRIPTOR,2,0,0,&r)==HM_RANGE);assert(cmd(HM_ENUM,1,0,0,&r)==HM_NOT_FOUND);}
static void report_bounds(void){hm_u8 p[257]={1};hm_slot v;setup(0);assert(hm_service_push(&s,2,p,257,0)==HM_RANGE);assert(hm_service_push(&s,2,p,0,0)==HM_RANGE);assert(hm_service_push(&s,99,p,9,0)==HM_NOT_FOUND);assert(hm_service_push(&s,2,p,1,0)==HM_OK);assert(hm_client_pop(&c,&v)==HM_RANGE);}
static void controls(void){hm_u8 p[2]={1,7};hm_u32 n=2;setup(0);assert(hm_client_report(&c,2,HM_OUTPUT,1,p,&n)==HM_OK&&b.leds==7);p[0]=2;p[1]=42;n=2;assert(hm_client_report(&c,2,HM_SET_FEATURE,2,p,&n)==HM_OK);p[1]=0;assert(hm_client_report(&c,2,HM_GET_FEATURE,2,p,&n)==HM_OK&&p[1]==42);n=1;assert(hm_client_report(&c,2,HM_SET_FEATURE,2,p,&n)==HM_RANGE);n=2;assert(hm_client_report(&c,2,HM_OUTPUT,3,p,&n)==HM_RANGE);s.report=NULL;p[0]=1;assert(hm_client_report(&c,2,HM_OUTPUT,1,p,&n)==HM_DENIED);}
static void malformed_request(void){hm_request q={0};hm_response r;unsigned k;setup(0);for(k=0;k<8;k++){hm_zero(&q,sizeof(q));q.seq=100+k;q.abi=HM_VERSION;q.op=HM_ENUM;if(k==0)q.seq=0;if(k==1)q.reserved=1;if(k==2)q.length=257;if(k==3)q.op=999;if(k==4)q.report_id=256;if(k==5)q.offset=1;if(k==6)q.iface=999;if(k==7)q.abi=0;hm_copy(s.data,&q,sizeof(q));wr(&s,0,HM_R_DOORBELL,100+k);hm_copy(&r,s.data+HM_RESP_OFF,sizeof(r));assert(r.status!=HM_OK&&r.length==0);}}
static void replay(void){hm_response r;hm_request q={0};setup(0);q.op=HM_ENUM;assert(hm_client_command(&c,&q,&r)==HM_OK);wr(&s,0,HM_R_DOORBELL,q.seq);hm_copy(&r,s.data+HM_RESP_OFF,sizeof(r));assert(r.status==HM_BAD_REQUEST);}
static void protected_pages(void){hm_u32 old;setup(0);old=rd(&s,1,HM_RESP_OFF);wr(&s,1,HM_RESP_OFF,0xdeadbeef);assert(rd(&s,1,HM_RESP_OFF)==old);wr(&s,0,HM_R_HEAD,123);assert(s.head==0);wr(&s,1,4095,123);wr(&s,1,0xffffffffu,123);assert(rd(&s,1,4095)==0);}
static void malformed_slot(void){hm_slot v;setup(0);assert(hm_script_tick(&s,&b,0)==HM_OK);hm_put32(s.data+HM_RING_OFF+4,0xffffffffu);assert(hm_client_pop(&c,&v)==HM_RANGE);assert(c.tail==1);s.head=c.tail+HM_SLOTS+1;assert(hm_client_pop(&c,&v)==HM_RANGE);assert(c.tail==1);}
static void ioctl_sizing(void){hm_u8 out[16],src[9]={0};hm_u32 n;memset(out,0xa5,sizeof(out));assert(hm_copy_output(out,8,src,9,&n)==HM_RANGE&&n==0&&out[0]==0xa5);assert(hm_copy_output(out,9,src,9,&n)==HM_OK&&n==9&&out[9]==0xa5);assert(hm_copy_output(NULL,9,src,9,&n)==HM_RANGE);}
static void invalid_topology(void){hm_backend_interface v[2];setup(1);memcpy(v,s.interfaces,sizeof(v));v[1].info.id=v[0].info.id;assert(hm_service_init(&s,v,2,NULL,NULL)==HM_BAD_REQUEST);assert(s.count==0);v[1].info.id=1;v[1].info.descriptor_length=4097;assert(hm_service_init(&s,v,2,NULL,NULL)==HM_RANGE);assert(hm_service_init(&s,v,9,NULL,NULL)==HM_RANGE);}
static void descriptor_fuzz(void){hm_layout l;hm_u8 buf[128];unsigned i,j;unsigned seed=12345;setup(0);for(i=0;i<20000;i++){for(j=0;j<sizeof(buf);j++){seed=seed*1664525u+1013904223u;buf[j]=(hm_u8)(seed>>24);}assert(hm_parse_descriptor(buf,i%129,&l)==HM_RANGE || i%129>0);}/* sanitizer checks all paths */}
static void scripted_sequence(void){hm_slot v;unsigned n=0;setup(0);while(hm_script_tick(&s,&b,n)==HM_OK){assert(hm_client_pop(&c,&v)==HM_OK);n++;}assert(n==12);assert(v.payload[1]==0&&v.payload[3]==0);}
static void activation(void){hm_response r;setup(0);assert(cmd(HM_DEACTIVATE,2,0,0,&r)==HM_OK);assert(hm_script_tick(&s,&b,0)==HM_NOT_READY);assert(cmd(HM_ACTIVATE,2,0,0,&r)==HM_OK);assert(hm_script_tick(&s,&b,0)==HM_OK);}
static hm_u32 evilread(void *x,hm_u32 p,hm_u32 o){hm_u32 v=rd(x,p,o);if(p==1&&o==HM_RESP_OFF+8)return 0xffffffffu;return v;}
static void malformed_response(void){hm_io evil={&s,evilread,wr};assert(hm_script_init(&s,&b,0)==HM_OK);assert(hm_client_open(&c,evil)==HM_IO_ERROR);}

static void long_descriptor(void){
 static hm_u8 desc[HM_MAX_DESC];hm_backend_interface v;hm_response r;hm_u32 n;
 setup(0);v=s.interfaces[0];n=v.info.descriptor_length;memcpy(desc,v.descriptor,n);
 while(n+2<=HM_MAX_DESC){desc[n++]=0x05;desc[n++]=0x01;}
 if(n<HM_MAX_DESC)desc[n++]=0x04;
 v.descriptor=desc;v.info.descriptor_length=n;
 assert(hm_service_init(&s,&v,1,NULL,NULL)==HM_OK);assert(hm_client_open(&c,io)==HM_OK);
 assert(c.devices[0].info.descriptor_length==HM_MAX_DESC&&!memcmp(desc,c.devices[0].descriptor,n));
 assert(cmd(HM_DESCRIPTOR,2,HM_MAX_DESC-256,256,&r)==HM_OK);
 assert(cmd(HM_DESCRIPTOR,2,HM_MAX_DESC-255,256,&r)==HM_RANGE);
}
static void unnumbered_descriptor(void){
 const hm_u8 desc[]={5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,1,0x81,0,0xc0};
 hm_backend_interface v={0};hm_response r;hm_slot slot;hm_u8 report=4;
 v.info.id=127;v.info.flags=HM_PRESENT;v.info.descriptor_length=sizeof(desc);v.descriptor=desc;
 assert(hm_service_init(&s,&v,1,NULL,NULL)==HM_OK);assert(hm_client_open(&c,io)==HM_OK);
 assert(c.devices[0].layout.ids==0&&c.devices[0].layout.length[0][0]==1);
 assert(cmd(HM_ACTIVATE,127,0,0,&r)==HM_OK);assert(hm_service_push(&s,127,&report,1,0)==HM_OK);
 assert(hm_client_pop(&c,&slot)==HM_OK&&slot.payload[0]==4);
}
static void descriptor_errors(void){
 hm_layout l;const hm_u8 truncated[]={0x77,1};const hm_u8 pop[]={0xb4};const hm_u8 end[]={0xc0};
 const hm_u8 zeroid[]={0x85,0};const hm_u8 huge[]={0xa1,1,0x75,32,0x97,0xff,0xff,0xff,0xff,0x81,2,0xc0};
 const hm_u8 mixed[]={0xa1,1,0x75,8,0x95,1,0x81,2,0x85,1,0x81,2,0xc0};
 assert(hm_parse_descriptor(truncated,sizeof(truncated),&l)==HM_RANGE);
 assert(hm_parse_descriptor(pop,sizeof(pop),&l)==HM_RANGE);assert(hm_parse_descriptor(end,sizeof(end),&l)==HM_RANGE);
 assert(hm_parse_descriptor(zeroid,sizeof(zeroid),&l)==HM_RANGE);assert(hm_parse_descriptor(huge,sizeof(huge),&l)==HM_RANGE);
 assert(hm_parse_descriptor(mixed,sizeof(mixed),&l)==HM_RANGE);
}
static hm_u32 corrupt_offset,corrupt_value;
static hm_u32 corruptread(void *x,hm_u32 p,hm_u32 o){return p==1&&o==corrupt_offset?corrupt_value:rd(x,p,o);}
static void peer_validation(void){
 hm_io bad={&s,corruptread,wr};hm_request q={0};hm_response r;unsigned k;
 setup(0);c.io=bad;
 for(k=0;k<4;k++){
  q.op=HM_ENUM;corrupt_offset=HM_RESP_OFF+(k==0?0:k==1?4:k==2?12:272);
  corrupt_value=k==0?0:k==1?HM_IO_ERROR+1:1;
  assert(hm_client_command(&c,&q,&r)==HM_IO_ERROR);
 }
}
static void mixed_interfaces(void){
 hm_response r;hm_slot slot;setup(1);assert(cmd(HM_ACTIVATE,1,0,0,&r)==HM_OK);
 assert(hm_script_tick(&s,&b,12)==HM_OK);assert(hm_client_pop(&c,&slot)==HM_OK&&slot.iface==2&&slot.length==9);
 assert(hm_client_pop(&c,&slot)==HM_OK&&slot.iface==1&&slot.length==4);
}
static void request_fuzz(void){
 unsigned k,j;hm_u32 seed=42;hm_response r;setup(0);
 for(k=0;k<20000;k++){
  for(j=0;j<sizeof(hm_request);j++){seed=seed*1664525u+1013904223u;s.data[j]=(hm_u8)(seed>>24);}
  wr(&s,0,HM_R_DOORBELL,rd(&s,1,0));memcpy(&r,s.data+HM_RESP_OFF,sizeof(r));
  assert(r.status<=HM_IO_ERROR&&r.length<=HM_PAYLOAD);
 }
}
static void seq_rollover(void){hm_response r;setup(0);c.seq=0xffffffffu;assert(cmd(HM_ENUM,0,0,0,&r)==HM_OK);assert(c.seq==1&&r.seq==1);}
int main(void){TEST(enumeration);TEST(handshake);TEST(wrap);TEST(counter_rollover);TEST(overflow);TEST(overflow_resync);TEST(bad_tail);TEST(irq);TEST(descriptor_bounds);TEST(report_bounds);TEST(controls);TEST(malformed_request);TEST(replay);TEST(protected_pages);TEST(malformed_slot);TEST(ioctl_sizing);TEST(invalid_topology);TEST(descriptor_fuzz);TEST(scripted_sequence);TEST(activation);TEST(malformed_response);TEST(long_descriptor);TEST(unnumbered_descriptor);TEST(descriptor_errors);TEST(peer_validation);TEST(mixed_interfaces);TEST(request_fuzz);TEST(seq_rollover);printf("PASS %u test groups (20000 descriptor fuzz cases; 20000 request fuzz cases; 10000 ring cycles)\n",tests);return 0;}
