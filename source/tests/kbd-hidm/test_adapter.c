#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "mtp_kbd_resident.c" /* copied by run.sh so stubs/ shadow the resident headers */
#include "vectors.h"
static unsigned checks,groups,nwrites,rx_head,rx_tail,ntx,dapfs,mbox_pending,enables;static bool bad_geometry,bad_dapf,stale,stuck,boot_fail,no_stm_ready;static unsigned char en_iface[4];
static unsigned char rxbuf[4096],txbuf[32],dapf_prop[468];
u64 test_ticks;void *adt;
#define CHECK(x) do {checks++;assert(x);}while(0)
static void enqueue(unsigned i) {CHECK(rx_head+vectors[i].n<=sizeof(rxbuf));memcpy(rxbuf+rx_head,vectors[i].p,vectors[i].n);rx_head+=vectors[i].n;}
static unsigned frame(unsigned char *f,unsigned ch,unsigned seq,unsigned iface,unsigned flags,const void *msg,unsigned n)
{
 unsigned body=(8+n+3)&~3u;memset(f,0,body+12);f[0]=8;f[1]=ch;f[2]=body;f[3]=body>>8;
 f[4]=seq;f[5]=iface;f[8]=flags;f[10]=n;f[11]=n>>8;memcpy(f+16,msg,n);
 u32 sum=0;for(unsigned i=0;i<body+8;i+=4)sum+=hm_le32(f+i);hm_put32(f+body+8,~sum);return body+12;
}
static void enqueue_raw(const unsigned char *p,unsigned n){CHECK(rx_head+n<=sizeof(rxbuf));memcpy(rxbuf+rx_head,p,n);rx_head+=n;}
u32 read32(u64 a)
{
 if(a==0x294600044)return 0;
 if(a==0x294608114)return mbox_pending?0:1u<<17;
 if(a==MTP_RX_COUNT)return stuck?4:rx_head-rx_tail;
 if(a==MTP_TX_FREE)return 2048;
 CHECK(a==MTP_RX32 && (stuck || rx_head-rx_tail>=4));if(stuck)return 0;u32 v=hm_le32(rxbuf+rx_tail);rx_tail+=4;return v;
}
u64 read64(u64 a){CHECK((a==0x294608830 || a==0x294608838) && mbox_pending);if(a==0x294608838)mbox_pending--;return 0;}
void write32(u64 a,u32 v)
{
 nwrites++;
 CHECK(a!=0x294b30000 && a!=0x294b30004); /* thresholds never programmed */
 if(a==0x294b14000){CHECK(v==0);return;}
 if(a==0x294b14004){CHECK(v==0xffffffffu);return;}
 if(a==0x294600044){CHECK(v==16);return;}
 CHECK(a==MTP_TX32 && ntx+4<=sizeof(txbuf));hm_put32(txbuf+ntx,v);ntx+=4;
 if(ntx==24){
  CHECK(txbuf[16]==0xb4 && (txbuf[17]==2 || txbuf[17]==3) && txbuf[4]==enables);en_iface[enables++]=txbuf[17];ntx=0;
  unsigned char f[64],m[2]={0xb4,0};unsigned n=frame(f,0x11,txbuf[4],0,0x80,m,2);enqueue_raw(f,n);
  if(txbuf[17]==3 && no_stm_ready)return;
  m[0]=0xf1;m[1]=txbuf[17];n=frame(f,0x12,0,0,0,m,2);enqueue_raw(f,n);
 }
}
int adt_path_offset_trace(const void*a,const char*b,int*c)
{(void)a;int n=!strcmp(b,"/arm-io/mtp")?1:!strcmp(b,"/arm-io/dart-mtp")?2:3;c[0]=n;return n;}
int adt_get_reg(const void*a,int*b,const char*c,int d,u64*e,u64*f)
{
 (void)a;(void)c;u64 v=b[0]==1?0x294600000:b[0]==2?0x294810000:d==1?0x294b14000:d==2?0x294b30000:MTP_DATA;
 *e=v+bad_geometry;*f=b[0]==1?0x88000:0x4000;return 0;
}
const void *adt_getprop(const void*a,int b,const char*c,u32*d)
{(void)a;(void)b;if(!strcmp(c,"clock-gates"))return NULL;*d=sizeof(dapf_prop);return dapf_prop;}
bool adt_is_compatible(const void*a,int b,const char*c){(void)a;(void)b;return !strcmp(c,"dart,t8110");}
int dapf_init(const char*p,int i){CHECK(!strcmp(p,"/arm-io/dart-mtp") && i==1 && !bad_dapf);dapfs++;return 0;}
asc_dev_t *asc_init(const char*p){CHECK(!strcmp(p,"/arm-io/mtp"));return (asc_dev_t*)1;}
void asc_free(asc_dev_t*a){(void)a;}
rtkit_dev_t *rtkit_init(const char*n,asc_dev_t*a,dart_dev_t*d,iova_domain_t*i,sart_dev_t*s,bool b)
{CHECK(!strcmp(n,"mtp") && a && !d && !i && !s && b);return (rtkit_dev_t*)1;}
void rtkit_mtp_mode(rtkit_dev_t*r,u64 b,size_t n){CHECK(r && b==0x294c00000 && n==0xc4000);}
bool rtkit_boot(rtkit_dev_t*r){CHECK(r);if(boot_fail)return false;for(unsigned i=0;i<7;i++)enqueue(i);return true;}
int rtkit_recv(rtkit_dev_t*r,struct rtkit_message*m){CHECK(r && m);return 0;}
void udelay(u32 us){test_ticks+=us;}
void wdt_kick(void){}
static struct mtp_kbd k;static hm_service svc;
static void reset(void)
{
 groups++;attempted=false;mtp_rtkit=NULL;nwrites=rx_head=rx_tail=ntx=dapfs=mbox_pending=enables=0;test_ticks=0;
 bad_geometry=bad_dapf=stale=stuck=boot_fail=no_stm_ready=false;memset(&svc,0,sizeof(svc));memset(dapf_prop,0,sizeof(dapf_prop));
 const u64 starts[]={0x28830c000,0x288320900,0x2881e4050,0x288360100,0x288f00014,0x288380020,0x288200324,0x2881e4064,0x2881e4068};
 const u64 ends[]={0x28830c07f,0x288320b1b,0x2881e4053,0x28836010b,0x288f00017,0x288380023,0x288200327,0x2881e4067,0x2881e406b};
 for(unsigned i=0;i<9;i++){unsigned char*p=dapf_prop+i*52;hm_put32(p,starts[i]);hm_put32(p+4,starts[i]>>32);hm_put32(p+8,ends[i]);hm_put32(p+12,ends[i]>>32);hm_put32(p+16,1);p[49]=3;p[50]=1;}
}
int main(void)
{
 reset();CHECK(!mtp_kbd_resident_start(&k,&svc));CHECK(k.phase==MTP_LIVE && dapfs==1 && nwrites==15); /* 3 setup + 2x6 FIFO words */
 CHECK(enables==2 && en_iface[0]==3 && en_iface[1]==2 && k.stm_ready && k.stm_ack==1); /* Linux order: stm, then keyboard */
 reset();no_stm_ready=true;CHECK(!mtp_kbd_resident_start(&k,&svc));CHECK(k.phase==MTP_LIVE && enables==2 && !k.stm_ready);
 reset();bad_geometry=true;CHECK(mtp_kbd_resident_start(&k,&svc)<0 && !nwrites && !dapfs);
 reset();bad_dapf=true;dapf_prop[49]^=1;CHECK(mtp_kbd_resident_start(&k,&svc)<0 && !nwrites && !dapfs);
 reset();stuck=true;CHECK(mtp_kbd_resident_start(&k,&svc)<0 && !nwrites && !dapfs); /* FIFO never drains */
 reset();mbox_pending=3;{unsigned char j[8]={1,2,3,4,5,6,7,8};enqueue_raw(j,8);}
 CHECK(!mtp_kbd_resident_start(&k,&svc) && !mbox_pending && k.phase==MTP_LIVE); /* stale state drained, as Linux */
 reset();boot_fail=true;CHECK(mtp_kbd_resident_start(&k,&svc)<0 && nwrites==3 && dapfs==1 && k.phase==MTP_FAILED);
 CHECK(mtp_kbd_resident_start(&k,&svc)<0 && nwrites==3); /* no second wake */
 printf("adapter: %u groups, %u checks PASS\n",groups,checks);return 0;
}
