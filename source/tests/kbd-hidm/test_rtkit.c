#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "rtkit.h"
#include "utils.h"
#define BASE 0x294608000ull
static unsigned checks, groups, sends, reads, cpu_writes; static u64 clock_us;static bool full, auto_boot;
static struct asc_message rx[512], tx[512];static unsigned head,tail;static u64 staged;
void *adt;
#define CHECK(x) do {checks++;assert(x);}while(0)
static void push(unsigned ep,u64 msg) {CHECK(head<512);rx[head++]=(struct asc_message){msg,ep};}
u32 read32(u64 a) {CHECK(a==BASE+0x110 || a==BASE+0x114);return a==BASE+0x110?(full?1<<16:0):(tail==head?1<<17:0);}
u64 read64(u64 a) {CHECK(tail<head);CHECK(a==BASE+0x830 || a==BASE+0x838);if(a==BASE+0x830)return rx[tail].msg0;reads++;return rx[tail++].msg1;}
void write64(u64 a,u64 v)
{
 CHECK(a==BASE+0x800 || a==BASE+0x808);
 if(a==BASE+0x800){staged=v;return;}
 CHECK(sends<512);tx[sends++]=(struct asc_message){staged,v};
 if(!auto_boot || v)return;
 unsigned type=staged>>52;
 if(type==6)push(0,(1ull<<52)|(12<<16)|11);
 if(type==2)push(0,(8ull<<52)|(1ull<<51)|0x51f);
 if(type==5 && ((staged>>32)&255)==10) {
   push(1,(1ull<<52)|(4ull<<44)|0x294c60000);
   push(2,(1ull<<52)|(4ull<<44)|0x294c64000);
   push(4,(1ull<<52)|(4ull<<44)|0x294c68000);
   push(8,(1ull<<56)|(0x3000ull<<36)|(0x294c6c000ull>>12));
   push(0,(7ull<<52)|0x20);
 }
 if(type==11)push(0,(11ull<<52)|0x20);
}
void write32(u64 a,u32 v){(void)a;(void)v;assert(0);}
void set32(u64 a,u32 v){CHECK(a==0x294600044 && v==16);cpu_writes++;}
void clear32(u64 a,u32 v){(void)a;(void)v;assert(0);}
int poll32(u64 a,u32 mask,u32 value,u32 us){(void)a;(void)mask;(void)value;(void)us;return full?-1:0;}
u64 timeout_calculate(u32 us){return clock_us+us;}
bool timeout_expired(u64 d){clock_us+=10;return clock_us>=d;}
int adt_path_offset_trace(const void*a,const char*b,int*c){(void)a;(void)b;c[0]=1;return 1;}
int adt_get_reg(const void*a,int*b,const char*c,int d,u64*e,u64*f){(void)a;(void)b;(void)c;(void)d;*e=0x294600000;if(f)*f=0x88000;return 0;}
int adt_first_child_offset(const void*a,int b){(void)a;(void)b;return 2;}
void *memalign(size_t a,size_t n){(void)a;return malloc(n);}
bool is_heap(void*p){(void)p;return false;}
int dart_map(dart_dev_t*a,u64 b,void*c,size_t d){(void)a;(void)b;(void)c;(void)d;assert(0);return -1;}
void dart_unmap(dart_dev_t*a,u64 b,size_t c){(void)a;(void)b;(void)c;assert(0);}
void *dart_translate(dart_dev_t*a,u64 b){(void)a;(void)b;assert(0);return NULL;}
u64 iova_alloc(iova_domain_t*a,size_t b){(void)a;(void)b;assert(0);return 0;}
void iova_free(iova_domain_t*a,u64 b,size_t c){(void)a;(void)b;(void)c;assert(0);}
bool sart_add_allowed_region(sart_dev_t*a,void*b,size_t c){(void)a;(void)b;(void)c;assert(0);return false;}
bool sart_remove_allowed_region(sart_dev_t*a,void*b,size_t c){(void)a;(void)b;(void)c;assert(0);return false;}
static asc_dev_t *asc;static rtkit_dev_t *rtk;
static void reset(bool mode)
{
 if(rtk)rtkit_free(rtk);if(asc)asc_free(asc);
 sends=reads=cpu_writes=head=tail=0;clock_us=0;full=auto_boot=false;groups++;
 asc=asc_init("/arm-io/mtp");rtk=rtkit_init("mtp",asc,NULL,NULL,NULL,true);CHECK(rtk);
 if(mode)rtkit_mtp_mode(rtk,0x294c00000,0xc4000);
}
int main(void)
{
 struct rtkit_message msg;
 reset(true);auto_boot=true;CHECK(rtkit_boot(rtk));CHECK(!cpu_writes && sends==10);CHECK(tx[0].msg0==((6ull<<52)|0x220));
 {static const unsigned order[6]={1,2,3,4,8,10};unsigned j=0;for(unsigned i=0;i<sends;i++)if((tx[i].msg0>>52)==5){CHECK(j<6 && ((tx[i].msg0>>32)&255)==order[j]);j++;}CHECK(j==6);} /* Linux ascending START_EP order */
 push(2,(5ull<<52)|3);push(4,8ull<<52);push(4,12ull<<52);push(8,2ull<<56);
 for(unsigned i=0;i<4;i++)CHECK(rtkit_recv(rtk,&msg)==0);CHECK(sends==14);
 CHECK(tx[10].msg1==2 && tx[13].msg1==8);
 reset(true);CHECK(!rtkit_boot(rtk));CHECK(clock_us>=1000000 && sends==1);
 reset(true);push(2,(1ull<<52)|(4ull<<44));CHECK(rtkit_recv(rtk,&msg)<0);CHECK(!sends); /* host allocation refused */
 reset(true);push(2,(1ull<<52)|(4ull<<44)|0x294cc3000);CHECK(rtkit_recv(rtk,&msg)<0); /* outside SRAM */
 reset(true);push(8,(1ull<<56)|(0x3000ull<<36)|(0x294cc3000ull>>12));CHECK(rtkit_recv(rtk,&msg)<0);
 reset(true);push(8,2ull<<56);CHECK(!rtkit_recv(rtk,&msg) && !sends); /* no buffer, no echo */
 reset(true);push(1,(1ull<<52)|(4ull<<44)|0x294c60000);push(1,1ull<<52);CHECK(rtkit_recv(rtk,&msg)<0);CHECK(!sends);
 reset(true);full=true;push(2,5ull<<52);CHECK(!rtkit_recv(rtk,&msg) && !sends);CHECK(!rtkit_recv(rtk,&msg));full=false;CHECK(!rtkit_recv(rtk,&msg) && sends==1);
 reset(true);full=true;push(2,5ull<<52);CHECK(!rtkit_recv(rtk,&msg));clock_us+=500001;CHECK(rtkit_recv(rtk,&msg)<0);
 reset(true);for(unsigned i=0;i<100;i++)push(2,5ull<<52);u64 before=clock_us;CHECK(!rtkit_recv(rtk,&msg));CHECK(reads<=8 && clock_us-before<=60 && tail<head);
 reset(true);CHECK(!rtkit_quiesce(rtk) && !rtkit_sleep(rtk) && !sends);
 reset(false);push(2,5ull<<52);CHECK(!rtkit_recv(rtk,&msg));CHECK(sends==1); /* existing default path */
 rtkit_free(rtk);asc_free(asc);
 printf("RTKit/ASC: %u groups, %u checks PASS\n",groups,checks);return 0;
}
