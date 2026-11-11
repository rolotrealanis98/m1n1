#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "rtkit.h"
#include "utils.h"
/* 2026-11-11: MTP host buffers (IOVA 0) through a host allocator, as Linux rtkit-helper. */
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
   push(1,(1ull<<52)|(5ull<<44));            /* tp1-a1: "ep 0x1 iova 0x0 size 0x5000" */
   push(2,(1ull<<52)|(4ull<<44));
   push(4,(1ull<<52)|(4ull<<44));
   push(8,(1ull<<56)|(0x3000ull<<36));
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

static unsigned allocs; static bool alloc_fail; static u64 next_iova;
static bool fake_alloc(size_t sz,void **cpu,u64 *iova)
{ if(alloc_fail)return false; allocs++; *cpu=calloc(1,sz); *iova=next_iova; next_iova+=(sz+0x3fff)&~0x3fffull; return *cpu!=NULL; }
static asc_dev_t *asc;static rtkit_dev_t *rtk;
static void reset(void)
{
 if(rtk)rtkit_free(rtk);if(asc)asc_free(asc);
 sends=reads=cpu_writes=head=tail=allocs=0;clock_us=0;full=auto_boot=alloc_fail=false;next_iova=0x10002000000ull;groups++;
 asc=asc_init("/arm-io/mtp");rtk=rtkit_init("mtp",asc,NULL,NULL,NULL,true);CHECK(rtk);
 rtkit_mtp_mode(rtk,0x294c00000,0xc4000);rtkit_mtp_host_buffers(rtk,fake_alloc);
}
static int find(unsigned ep,unsigned type,int oslog){for(unsigned i=0;i<sends;i++){unsigned t=oslog?(unsigned)(tx[i].msg0>>56):(unsigned)((tx[i].msg0>>52)&255);if(tx[i].msg1==ep&&t==type)return (int)i;}return -1;}
int main(void)
{
 struct rtkit_message msg;int i;
 /* boot: four IOVA-0 requests answered with host memory, then power */
 reset();auto_boot=true;CHECK(rtkit_boot(rtk));CHECK(allocs==4);
 i=find(1,1,0);CHECK(i>=0 && ((tx[i].msg0>>44)&255)==5 && (tx[i].msg0&0xfffffffffffull)==0x10002000000ull);
 i=find(2,1,0);CHECK(i>=0 && ((tx[i].msg0>>44)&255)==4 && (tx[i].msg0&0xfffffffffffull)==0x10002008000ull);
 i=find(4,1,0);CHECK(i>=0 && (tx[i].msg0&0xfffffffffffull)==0x1000200c000ull);
 i=find(8,1,1);CHECK(i>=0 && ((tx[i].msg0>>36)&0xfffff)==0x3000 && (tx[i].msg0&0xfffffffffull)==(0x10002010000ull>>12));
 {unsigned last=0;for(unsigned k=0;k<sends;k++)if(((tx[k].msg0>>52)&255)==11 && !tx[k].msg1)last=k;CHECK(last>(unsigned)find(8,1,1));} /* replies before AP power */
 /* oslog LOG now acknowledged (buffer admitted) */
 unsigned s0=sends;push(8,2ull<<56);CHECK(!rtkit_recv(rtk,&msg) && sends==s0+1);
 /* identical repeat: reply re-sent, no new allocation; changed size: refused */
 reset();push(2,(1ull<<52)|(4ull<<44));CHECK(rtkit_recv(rtk,&msg)==0 && allocs==1 && sends==1);
 push(2,(1ull<<52)|(4ull<<44));CHECK(rtkit_recv(rtk,&msg)==0 && allocs==1 && sends==2 && tx[1].msg0==tx[0].msg0);
 push(2,(1ull<<52)|(8ull<<44));CHECK(rtkit_recv(rtk,&msg)<0 && sends==2);
 /* allocation failure: request fails, nothing sent */
 reset();alloc_fail=true;push(1,(1ull<<52)|(5ull<<44));CHECK(rtkit_recv(rtk,&msg)<0 && !sends);
 /* a coprocessor-placed SRAM buffer still takes the old path (no reply, no allocation) */
 reset();push(2,(1ull<<52)|(4ull<<44)|0x294c64000);CHECK(!rtkit_recv(rtk,&msg) && !sends && !allocs);
 /* second crashlog message after a host crashlog buffer = crash */
 reset();push(1,(1ull<<52)|(5ull<<44));CHECK(!rtkit_recv(rtk,&msg) && allocs==1);push(1,1ull<<52);CHECK(rtkit_recv(rtk,&msg)<0);
 /* IOVA above 44 bits from the allocator is refused */
 reset();next_iova=1ull<<44;push(4,(1ull<<52)|(4ull<<44));CHECK(rtkit_recv(rtk,&msg)<0 && !sends);
 /* zero size refused */
 reset();push(4,1ull<<52);CHECK(rtkit_recv(rtk,&msg)<0 && !sends && !allocs);
 rtkit_free(rtk);asc_free(asc);
 printf("RTKit host buffers: %u groups, %u checks PASS\n",groups,checks);return 0;
}
