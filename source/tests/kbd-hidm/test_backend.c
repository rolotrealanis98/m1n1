#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mtp_kbd.h"
#include "vectors.h"
static unsigned checks, groups;
#define CHECK(x) do { checks++; assert(x); } while (0)
struct mock { unsigned char rx[16384], tx[4096]; unsigned head, tail, tx_n, free, overrun; hm_u64 us; int control; };
static hm_u32 rd(void *ctx, hm_u64 a)
{
    struct mock *m=ctx;
    if(a==MTP_RX_COUNT) return m->overrun ? m->overrun : m->head-m->tail;
    if(a==MTP_TX_FREE) return m->free;
    CHECK(a==MTP_RX32 && m->head-m->tail>=4);
    hm_u32 v=hm_le32(m->rx+m->tail);m->tail+=4;return v;
}
static void wr(void *ctx,hm_u64 a,hm_u32 v)
{ struct mock*m=ctx;CHECK(a==MTP_TX32 && m->tx_n+4<=sizeof(m->tx));hm_put32(m->tx+m->tx_n,v);m->tx_n+=4; }
static hm_u64 now(void *ctx) { return ++((struct mock*)ctx)->us; }
static int ctl(void *ctx) { return ((struct mock*)ctx)->control; }
static struct mock m; static struct mtp_kbd k; static hm_service s;
static void reset(void)
{
    memset(&m,0,sizeof(m));memset(&s,0,sizeof(s));m.free=MTP_FIFO_SIZE;
    struct mtp_io io={&m,rd,wr,now,ctl,NULL,getenv("KBD_LOG")?printf:NULL};mtp_kbd_init(&k,&io,&s);groups++;
}
static void feed(const void*p,unsigned n)
{ CHECK(m.head+n<=sizeof(m.rx));memcpy(m.rx+m.head,p,n);m.head+=n; }
static void pump(void) { for(unsigned i=0;i<100 && k.phase!=MTP_FAILED;i++) mtp_kbd_poll(&k); }
static void vec(unsigned i) { feed(vectors[i].p,vectors[i].n);pump(); }
static unsigned frame(unsigned char *f,unsigned ch,unsigned seq,unsigned iface,unsigned flags,const void *msg,unsigned n)
{
 unsigned body=(8+n+3)&~3u;memset(f,0,body+12);f[0]=8;f[1]=ch;f[2]=body;f[3]=body>>8;
 f[4]=seq;f[5]=iface;f[8]=flags;f[10]=n;f[11]=n>>8;memcpy(f+16,msg,n);
 hm_u32 sum=0;for(unsigned i=0;i<body+8;i+=4)sum+=hm_le32(f+i);hm_put32(f+body+8,~sum);return body+12;
}
static void ack_ready(unsigned seq,unsigned iface,int ready)
{
 unsigned char f[64],a[2]={0xb4,0},r[2]={0xf1,(unsigned char)iface};
 feed(f,frame(f,0x11,seq,0,0x80,a,2));if(ready)feed(f,frame(f,0x12,0,0,0,r,2));pump();
}
static void kbd_only(void) { m.us+=MTP_STM_ANNOUNCE_US+1;pump(); } /* stm never announced */
static void enumerate(void)
{
 for(unsigned i=0;i<7;i++)vec(i); /* seven INIT packets (one split), stm = iface 3 */
 CHECK(k.phase==MTP_STM && m.tx_n==24 && m.tx[16]==0xb4 && m.tx[17]==3 && m.tx[4]==0); /* stm first, seq 0 */
 ack_ready(0,3,1);
 CHECK(m.tx_n==48 && m.tx[24+16]==0xb4 && m.tx[24+17]==2 && m.tx[24+4]==1); /* keyboard, seq 1 (as Linux) */
 ack_ready(1,2,1);
 CHECK(k.phase==MTP_LIVE && k.iface==2 && k.ack && k.ready && k.stm_ready);
 CHECK(hm_service_init(&s,&k.keyboard,1,mtp_kbd_report,&k)==HM_OK);
 s.ready=s.enabled=s.active=1;
}
static void only_ifaces_2_3(void){for(unsigned o=0;o+24<=m.tx_n;o+=24)if(m.tx[o+16]==0xb4)CHECK(m.tx[o+17]==2||m.tx[o+17]==3);}
int main(void)
{
 unsigned char f[8192], msg[8192]; unsigned n;
 reset();enumerate();CHECK(k.desc_len==76 && s.count==1);
 unsigned reports=0;
 for(unsigned i=9;i<sizeof(vectors)/sizeof(vectors[0]);i++) {
   vec(i);hm_slot slot;hm_copy(&slot,s.data+HM_RING_OFF+((s.head-1)%HM_SLOTS)*HM_SLOT_BYTES,sizeof(slot));
   CHECK(slot.iface==2 && slot.length==10 && !memcmp(slot.payload,vectors[i].p+16,10));
   hm_service_write(&s,0,HM_R_TAIL,s.head);reports++;
 }
 CHECK(k.reports==reports && reports==22); /* A/S/D/F, Shift+A, Esc, arrows */
 reset();m.us=20000001;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_TIMEOUT && !s.count);
 reset();m.control=-1;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_RTKit);
 reset();m.overrun=2049;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_OVERRUN);
 reset();memcpy(f,vectors[2].p,vectors[2].n);f[0]=7;feed(f,8);pump();CHECK(k.error==MTP_E_FRAME);
 reset();memcpy(f,vectors[2].p,vectors[2].n);f[2]=0xff;f[3]=0xff;feed(f,8);pump();CHECK(k.error==MTP_E_FRAME);
 reset();memcpy(f,vectors[2].p,vectors[2].n);f[30]^=1;feed(f,vectors[2].n);pump();CHECK(k.error==MTP_E_FRAME);
 reset();feed(vectors[2].p,12);pump();m.us+=500001;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_TIMEOUT);
 reset();m.free=0;vec(2);kbd_only();m.us+=500001;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_TIMEOUT && !m.tx_n);
 reset();vec(2);kbd_only();CHECK(k.phase==MTP_ENABLE && m.tx[17]==2 && m.tx[4]==0);m.us+=5000001;mtp_kbd_poll(&k);CHECK(k.error==MTP_E_TIMEOUT); /* no ACK/READY */
 reset();vec(2);kbd_only();n=frame(f,0x11,99,0,0x80,"\xb4",1);feed(f,n);vec(8);CHECK(k.phase==MTP_ENABLE && !k.ack);
 reset();vec(2);kbd_only();n=frame(f,0x11,0,0,0x80,"\xb4",1);f[12]=1;hm_put32(f+n-4,hm_le32(f+n-4)-1);feed(f,n);pump();CHECK(k.error==MTP_E_ACK);
 reset();enumerate();vec(2);CHECK(k.error==MTP_E_TOPOLOGY && !s.ready);
 reset(); /* Unknown block tolerated, descriptor in second INIT. */
 memset(msg,0,22);msg[0]=0xf0;msg[3]=9;memcpy(msg+4,"keyboard",8);msg[20]=1;
 msg[22]=99;msg[23]=0;msg[24]=msg[25]=0;n=frame(f,0x12,0,0,0,msg,26);feed(f,n);pump();CHECK(k.phase==MTP_ENUM);
 memcpy(msg,vectors[2].p+16,145);msg[3]=9;n=frame(f,0x12,1,0,0,msg,145);feed(f,n);pump();kbd_only();CHECK(k.iface==9 && k.complete && m.tx[17]==9);
 reset();memcpy(msg,vectors[2].p+16,145);msg[24]=0xff;msg[25]=0xff;n=frame(f,0x12,0,0,0,msg,145);feed(f,n);pump();CHECK(k.error==MTP_E_FRAME);
 reset();memset(msg,0,sizeof(msg));msg[0]=0xf0;msg[3]=2;memcpy(msg+4,"keyboard",8);msg[24]=1;msg[25]=0x10;
 n=frame(f,0x12,0,0,0,msg,26+4097); /* stream > FIFO capacity a chunk at a time */
 for(unsigned pos=0;pos<n && k.phase!=MTP_FAILED;) {unsigned chunk=n-pos>1024?1024:n-pos;feed(f+pos,chunk);pump();pos+=chunk;}
 CHECK(k.error==MTP_E_DESCRIPTOR);
 reset();enumerate();memset(msg,0,257);n=frame(f,0x12,0,2,0,msg,257);feed(f,n);pump();CHECK(k.error==MTP_E_FRAME);
 reset();n=frame(f,0x12,0,0,0,"\xa0\x02\x05\0\x03",5);feed(f,n);pump();CHECK(m.tx_n==32 && m.tx[16]==0xa1 && hm_le32(m.tx+17)==0xe000f00d);
 reset();enumerate();hm_u32 len=1;CHECK(mtp_kbd_report(&k,2,HM_OUTPUT,1,msg,&len)==HM_DENIED);CHECK(m.tx_n==48);
 reset();enumerate();for(unsigned i=0;i<24;i++)vec(9+i%2);CHECK(s.overflow==16 && s.pending==1);hm_service_write(&s,0,HM_R_TAIL,8);CHECK(s.head==9 && !s.pending);
 /* stm decision points */
 reset();for(unsigned i=0;i<7;i++)vec(i);ack_ready(0,3,0);m.us+=MTP_STM_READY_US+1;pump();
 CHECK(k.phase==MTP_ENABLE && k.stm_ack==1 && !k.stm_ready && m.tx_n==48 && m.tx[24+17]==2);ack_ready(1,2,1);CHECK(k.phase==MTP_LIVE);only_ifaces_2_3();
 reset();for(unsigned i=0;i<7;i++)vec(i);m.us+=MTP_STM_READY_US+1;pump();CHECK(k.phase==MTP_ENABLE && m.tx[24+17]==2); /* no stm ACK at all */
 reset();for(unsigned i=0;i<7;i++)vec(i);n=frame(f,0x11,0,0,0x80,"\xb4\0",2);f[12]=5;hm_put32(f+n-4,hm_le32(f+n-4)-5);feed(f,n);
 n=frame(f,0x12,0,0,0,"\xf1\x03",2);feed(f,n);pump();CHECK(k.stm_ack==-1 && k.phase==MTP_ENABLE && m.tx[24+17]==2); /* stm ACK error: keyboard still tried */
 reset();vec(2);vec(3);vec(4);CHECK(k.phase==MTP_STM && m.tx[17]==3);ack_ready(0,3,1);ack_ready(1,2,1);CHECK(k.phase==MTP_LIVE);only_ifaces_2_3();
 reset();memset(msg,0,26);msg[0]=0xf0;msg[3]=2;memcpy(msg+4,"stm",3);n=frame(f,0x12,0,0,0,msg,26);feed(f,n);pump();vec(2);CHECK(k.error==MTP_E_TOPOLOGY); /* stm and keyboard same iface */
 reset();memcpy(msg,vectors[2].p+16,145);memcpy(msg+4,"keyboardX",9);n=frame(f,0x12,0,0,0,msg,145);feed(f,n);pump();m.us+=MTP_STM_READY_US+1;pump();CHECK(!k.found && !m.tx_n); /* name must match exactly */
 printf("backend: %u groups, %u checks PASS\n",groups,checks);
 return 0;
}
