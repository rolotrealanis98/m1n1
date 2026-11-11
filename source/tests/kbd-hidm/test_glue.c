#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HIDM_HOST_TEST 1
#include "hv_win_hidm.c" /* copied by run.sh */
static unsigned checks, groups, maps, irq_changes; static bool line, collision, mapped, backend_fail, map_fail;
int boot_cpu_idx=0; bool hv_started_cpus[MAX_CPUS];
#define CHECK(x) do { checks++; assert(x); } while(0)
int smp_id(void) {return 0;}
bool hv_win_vgic_enabled(void) {return true;}
u64 hv_win_pl011_gsiv(void) {return collision?68:64;}
bool hv_win_storage_claims_spi(u32 n) {(void)n;return false;}
bool hv_vgic3_spi_unassigned(u32 n) {CHECK(n==68);return true;}
void hv_vgic3_set_spi_level(u32 n,bool level) {CHECK(n==68);line=level;irq_changes++;}
u64 hv_pt_walk(u64 n) {CHECK(n==HM_REG_BASE || n==HM_DATA_BASE);return mapped;}
int hv_map_hook(u64 base,bool (*fn)(struct exc_info*,u64,u64*,bool,int),u64 size)
{CHECK(base==HM_REG_BASE || base==HM_DATA_BASE);CHECK(fn==access_page && size==0x4000);maps++;return map_fail && maps==2?-1:0;}
static hm_u64 clock_now(void *ctx) {(void)ctx;return 1;}
static hm_u32 read_none(void *ctx,hm_u64 addr) {(void)ctx;(void)addr;return 0;}
static void write_none(void *ctx,hm_u64 addr,hm_u32 val) {(void)ctx;(void)addr;(void)val;assert(0);}
static int control_none(void *ctx) {(void)ctx;return 0;}
int mtp_kbd_resident_start(struct mtp_kbd *k,hm_service *s)
{
 struct mtp_io io={NULL,read_none,write_none,clock_now,control_none,NULL,NULL};mtp_kbd_init(k,&io,s);
 if(backend_fail) {mtp_kbd_fail(k,MTP_E_TIMEOUT);return -1;}
 static const hm_u8 desc[]={5,1,9,6,0xa1,1,0xc0};
 k->phase=MTP_LIVE;k->iface=2;k->keyboard=(hm_backend_interface){{.id=2,.flags=1,.descriptor_length=sizeof(desc)},desc};return 0;
}
/* trackpad adapter stubs (2026-11-11): finish returns a PTP interface when tp_on */
static bool tp_on; static u64 armed_fw=~0ull;
void (*mtp_kbd_dma_hook)(struct rtkit_dev *); void mtp_dma_rtkit_hook(struct rtkit_dev *r){(void)r;}
static const hm_u8 tp_desc[]={5,0x0d,9,5,0xa1,1,0x85,1,0x09,0x42,0x15,0,0x25,1,0x75,8,0x95,1,0x81,2,0xc0};
void mtp_tp_resident_arm(u64 addr,u64 len){(void)len;armed_fw=addr;}
const hm_backend_interface *mtp_tp_resident_finish(struct mtp_kbd *k)
{static hm_backend_interface t;(void)k;if(!tp_on)return NULL;t=(hm_backend_interface){{.id=1,.flags=1,.descriptor_length=sizeof(tp_desc)},tp_desc};return &t;}
static void reset(void)
{
 attempted=exposed=retired=irq=line=collision=mapped=backend_fail=map_fail=tp_on=false;armed_fw=~0ull;maps=irq_changes=0;
 memset(&service,0,sizeof(service));memset(hv_started_cpus,0,sizeof(hv_started_cpus));groups++;
}
static void wrpage(u64 addr,u32 val) {u64 v=val;CHECK(access_page(NULL,addr,&v,true,2));}
static u64 rdpage(u64 addr,int width) {u64 v[2]={~0ull,~0ull};CHECK(access_page(NULL,addr,v,false,width));return v[0];}
static void cmd(unsigned seq,unsigned op,unsigned iface,unsigned len)
{
 hm_request q={.seq=seq,.abi=1,.op=op,.iface=iface,.length=len};
 for(unsigned i=0;i<sizeof(q);i+=4)wrpage(HM_DATA_BASE+i,hm_le32((const hm_u8*)&q+i));
 wrpage(HM_REG_BASE+HM_R_DOORBELL,seq);
 CHECK(rdpage(HM_DATA_BASE+HM_RESP_OFF,2)==seq);
}
int main(void)
{
 reset();CHECK(!hv_win_hidm_configure(1,0,0));CHECK(maps==2 && service.count==1);CHECK(hv_win_hidm_claims_spi(68));CHECK(hv_win_hidm_configure(1,0,0)==-1);
 CHECK(rdpage(HM_REG_BASE,2)==HM_MAGIC);CHECK(rdpage(HM_REG_BASE+4,2)==1);
 for(unsigned off=0x1000;off<0x4000;off+=4)CHECK(rdpage(HM_REG_BASE+off,2)==0 && rdpage(HM_DATA_BASE+off,2)==0);
 for(int width=0;width<=4;width++)if(width!=2)CHECK(!rdpage(HM_REG_BASE,width));
 CHECK(!rdpage(HM_REG_BASE+1,2));CHECK(!rdpage(HM_DATA_BASE+4095,2));
 u64 v=0;CHECK(!access_page(NULL,HM_REG_BASE-4,&v,false,2));CHECK(!access_page(NULL,HM_DATA_BASE+0x4000,&v,false,2));
 cmd(1,HM_HELLO,0,0);CHECK(service.ready && !line);cmd(2,HM_ACTIVATE,2,0);wrpage(HM_REG_BASE+HM_R_ENABLE,1);
 hm_u8 report[8]={2,0,4};CHECK(hm_service_push(&service,2,report,8,1000)==HM_OK);hv_win_hidm_tick();CHECK(line && irq_changes==1);
 wrpage(HM_REG_BASE+HM_R_TAIL,99);CHECK(line);wrpage(HM_REG_BASE+HM_R_ENABLE,0);CHECK(!line);wrpage(HM_REG_BASE+HM_R_ENABLE,1);CHECK(line);
 wrpage(HM_REG_BASE+HM_R_TAIL,1);CHECK(!line);cmd(3,HM_OUTPUT,2,1);CHECK(rdpage(HM_DATA_BASE+HM_RESP_OFF+4,2)==HM_DENIED);
 cmd(4,HM_DESCRIPTOR,2,7);CHECK(rdpage(HM_DATA_BASE+HM_RESP_OFF+4,2)==HM_OK);CHECK(rdpage(HM_DATA_BASE+HM_RESP_OFF+8,2)==7);
 hm_service_push(&service,2,report,8,2000);hv_win_hidm_tick();CHECK(line);cmd(5,HM_BYE,0,0);CHECK(!line);
 reset();collision=true;CHECK(hv_win_hidm_configure(1,0,0)==-1 && !maps);
 reset();mapped=true;CHECK(hv_win_hidm_configure(1,0,0)==-1 && !maps);
 reset();hv_started_cpus[3]=true;CHECK(hv_win_hidm_configure(1,0,0)==-1 && !maps);
 reset();CHECK(hv_win_hidm_configure(99,0,0)==-1 && !maps);
 reset();map_fail=true;CHECK(hv_win_hidm_configure(1,0,0)==-1 && maps==2);CHECK(!rdpage(HM_REG_BASE,2));
 reset();backend_fail=true;CHECK(hv_win_hidm_configure(1,0,0)==0 && !service.count);cmd(1,HM_HELLO,0,0);cmd(2,HM_ENUM,0,0);CHECK(rdpage(HM_DATA_BASE+HM_RESP_OFF+4,2)==HM_NOT_FOUND);
 reset();CHECK(!hv_win_hidm_configure(1,0,0));service.ready=service.active=service.enabled=1;hm_service_push(&service,2,report,8,0);hv_win_hidm_tick();CHECK(line);hv_win_hidm_retire();CHECK(!line && !rdpage(HM_REG_BASE,2));
 /* trackpad: armed with the launcher's firmware pointer; published as a second interface */
 reset();mtp_kbd_dma_hook=NULL;CHECK(!hv_win_hidm_configure(1,0,0) && armed_fw==0 && service.count==1 && mtp_kbd_dma_hook==mtp_dma_rtkit_hook); /* host RTKit buffers always armed */
 reset();tp_on=true;CHECK(!hv_win_hidm_configure(1,0x12340000,81420) && armed_fw==0x12340000 && service.count==2);
 CHECK(service.interfaces[0].info.id==2 && service.interfaces[1].info.id==1);
 reset();tp_on=true;backend_fail=true;CHECK(!hv_win_hidm_configure(1,0x12340000,81420) && !service.count); /* no keyboard: nothing */
 printf("glue: %u groups, %u checks PASS\n",groups,checks);return 0;
}
