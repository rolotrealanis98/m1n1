/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmu_host.h"
#include "hv_win.h"
#include "hv_win_pmu_4k.h"
#include "hv_win_pmu_rewrite.h"
#undef printf

#define BLOCKS 8
#define FRAMES (BLOCKS * 4)
#define WORDS (FRAMES * 1024)
#define BASE 0x100000UL
#define VA 0x400000UL
#define NOP 0xd503201fU
#define IESR ((0x20UL << 26) | 15)
#define DESR ((0x24UL << 26) | 15 | BIT(6))
static u32 mem[WORDS] ALIGNED(16384), ref[WORDS];
static enum hv_xt_state states[BLOCKS];
static bool tracked[BLOCKS], attempted[FRAMES], executable[FRAMES], scanned[FRAMES];
static struct pmu_s1 s1;
static unsigned checks;
static int ref_open;
static u64 scan_count, modified_count, word_count;
static u64 store_size;
static u32 store_value;
static bool store_supported, translate_ok;
static unsigned at_calls, hpfar_calls;
int host_cpu;
u64 hv_cpus_in_guest;
struct hv_win_state hv_win;
struct boot_args cur_boot_args;
void hv_win_pmu_host_reset(void);
u8 hv_win_pmu_host_frame(u64 ipa);
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)

int host_printf(const char *fmt, ...) { return 0; }
void host_ic_ialluis(void) {}
size_t heapblock_remaining(void) { return 1UL << 30; }
bool pmu_host_s1(struct pmu_s1 *s) { *s = s1; return true; }
static int block_of(u64 ipa)
{
    return ipa >= BASE && ipa - BASE < sizeof(mem) ? (ipa - BASE) / 16384 : -1;
}
int hv_xt_track(u64 ipa, u64 size, bool dry, u64 *tables)
{
    *tables = 1;
    if (ipa != BASE || size != sizeof(mem)) return -1;
    if (!dry) for (unsigned i=0; i<BLOCKS; i++) { tracked[i]=true; states[i]=HV_XT_DIRTY; }
    return BLOCKS;
}
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips)
{
    int b=block_of(ipa);
    if (b<0 || !tracked[b]) return HV_XT_UNTRACKED;
    if (pa) *pa=(u64)&mem[b*4096];
    if (flips) *flips=0;
    return states[b];
}
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips)
{
    int b=block_of(ipa);
    CHECK(b>=0 && tracked[b]);
    CHECK(state != HV_XT_PINNED); /* no RWX escape */
    states[b]=state;
    return 0;
}
void hv_xt_tlbi(bool broadcast) {}
bool hv_xt_fault_ipa(struct exc_info *ctx, bool walker, bool write, u64 *ipa)
{
    if (walker) { hpfar_calls++; *ipa=BASE+4096; return true; }
    at_calls++;
    if (!translate_ok || ctx->far<VA || ctx->far-VA>=sizeof(mem)) return false;
    *ipa=BASE+ctx->far-VA;
    return true;
}
static void cache_check(void *addr, size_t size)
{
    size_t off=(u8 *)addr-(u8 *)mem;
    CHECK(off < sizeof(mem) && size==4096 && !(off & 4095));
    CHECK(states[off/16384]==HV_XT_SCANNING);
    CHECK(executable[off/4096] && attempted[off/4096]);
}
void dc_civac_range(void *addr, size_t size) { cache_check(addr,size); }
void dc_cvac_range(void *addr, size_t size) { cache_check(addr,size); }
bool hv_emulate_pt_store(struct exc_info *ctx, u64 far, u64 ipa, u64 *bytes)
{
    ctx->regs[0]++; /* decoder changes must not leak on failure */
    if (!store_supported) return false;
    CHECK(ipa>=BASE && ipa+store_size<=BASE+sizeof(mem));
    CHECK(states[(ipa-BASE)/16384]==HV_XT_SCANNING);
    if ((ipa & 16383)+store_size>16384) return false;
    for (u64 i=0;i<store_size;i++) ((u8 *)mem)[ipa-BASE+i]=((u8 *)&store_value)[i%4];
    *bytes=store_size;
    return true;
}
static u64 *table(unsigned frame) { return (u64 *)&mem[frame*1024]; }
static void reset(void)
{
    hv_win_pmu_host_reset();
    memset(&hv_win,0,sizeof(hv_win));
    hv_win.irq_model=HV_WIN_IRQ_VGIC3;
    hv_win.nr_vcpus=1;
    memset(states,0,sizeof(states)); memset(tracked,0,sizeof(tracked));
    memset(attempted,0,sizeof(attempted)); memset(executable,0,sizeof(executable));
    memset(scanned,0,sizeof(scanned));
    memset(mem,0,sizeof(mem));
    for (unsigned f=0;f<24;f++) {
        executable[f]=(f%2)==0;
        for (unsigned i=0;i<1024;i++) mem[f*1024+i]=NOP;
        mem[f*1024+32]=0xd53b9d05;
        mem[f*1024+64]=0xd51b9c01;
        mem[f*1024+96]=0xd53be8c3;
        mem[f*1024+97]=0x00200100;
        if (!executable[f]) {
            mem[f*1024+100]=0xd53bedc3;
            mem[f*1024+101]=0xf0200100; /* descriptor flags defeating the old guard */
        }
        mem[f*1024+1023]=0xd53b9d00;
    }
    table(28)[0]=(BASE+29*4096)|3;
    table(29)[0]=(BASE+30*4096)|3;
    table(30)[2]=(BASE+31*4096)|3;
    for (unsigned f=0;f<FRAMES;f++)
        table(31)[f]=(BASE+f*4096)|3|BIT(10)|BIT(7)|BIT(6)|
                        (executable[f]?0:BIT(53)|BIT(54));
    s1=(struct pmu_s1){.sctlr=1,.tcr=16|(16UL<<16)|(2UL<<30),.ttbr={BASE+28*4096,BASE+28*4096}};
    hv_cpus_in_guest=0; host_cpu=0; translate_ok=true; store_supported=true;
    store_size=4; store_value=0xd53b9d07;
    ref_open=-1; scan_count=modified_count=word_count=0;
    at_calls=hpfar_calls=0;
    CHECK(hv_win_pmu_arm(BASE,sizeof(mem))==BLOCKS);
    memcpy(ref,mem,sizeof(mem));
}
/* Independent reference decode: no production classifier, replacement or scan calls. */
static u32 replacement(u32 w)
{
    unsigned r=(w>>5)&0xffff, op1=(r>>11)&7, crn=(r>>7)&15, crm=(r>>3)&15;
    if ((w>>22)!=0x354 || ((r>>14)&3)!=3) return w;
    if (!((crn==9 && ((op1==3 && crm>=12 && crm<=14)||(op1==0 && crm==14))) ||
          (crn==14 && op1==3 && crm>=8))) return w;
    if (!(w & (1U<<21))) return NOP;
    return (r==0xdce8 ? 0xd53be040U : 0xd2800000U) | (w & 31);
}
static void ref_scan(unsigned f)
{
    if (scanned[f]) return;
    unsigned n=0;
    for (unsigned i=0;i<1023;i++) {
        u32 *w=&ref[f*1024+i], new=replacement(*w);
        if (new!=*w && (w[1]>>27)!=0) { *w=new; n++; }
    }
    scanned[f]=true; scan_count++; modified_count+=!!n; word_count+=n;
}
static bool fault(unsigned f, unsigned offset, bool user)
{
    struct exc_info ctx={.far=VA+f*4096+offset,.elr=VA+f*4096+offset,
                         .esr=IESR,.spsr=user?0:5};
    attempted[f]=true;
    return hv_win_handle_iabort(&ctx);
}
static void equivalent(void)
{
    CHECK(memcmp(mem,ref,sizeof(mem))==0);
    for (unsigned f=0;f<FRAMES;f++)
        CHECK(!!(hv_win_pmu_host_frame(BASE+f*4096)&PMU_FRAME_SCANNED)==scanned[f]);
    for (unsigned b=0;b<BLOCKS;b++)
        CHECK((states[b]==HV_XT_CODE)==((int)b==ref_open));
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED)==scan_count);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_MODIFIED)==modified_count);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_MODIFIED)==word_count);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_MODIFIED_NEVER_EXECUTED)==0);
}
static unsigned execute(unsigned f)
{
    CHECK(executable[f]);
    if (states[f/4]==HV_XT_CODE) return 0; /* REAL granule: sibling does not fault */
    CHECK(fault(f,128,false));
    ref_scan(f); ref_open=f/4;
    equivalent();
    return 1;
}
static void invalidate_ref(unsigned off,unsigned size)
{
    for (unsigned f=off/4096;f<=(off+size-1)/4096;f++) {
        scanned[f]=false;
        if (ref_open==(int)(f/4)) ref_open=-1;
    }
}
static void write_guest(unsigned off, unsigned size)
{
    store_size=size;
    struct exc_info ctx={.far=VA+off,.elr=VA,.esr=DESR,.spsr=5};
    int b=off/16384;
    if (states[b]==HV_XT_DIRTY) {
        for (unsigned i=0;i<size;i++) ((u8 *)mem)[off+i]=((u8 *)&store_value)[i%4];
    } else {
        CHECK(hv_win_handle_dabort(&ctx,ctx.esr));
        CHECK(ctx.elr==VA+4 && ctx.regs[0]==1);
        ref_open=-1;
    }
    for (unsigned i=0;i<size;i++) ((u8 *)ref)[off+i]=((u8 *)&store_value)[i%4];
    invalidate_ref(off,size);
    equivalent();
}
static void fill_device(unsigned off, unsigned size)
{
    unsigned n=0;
    for (unsigned f=off/4096;f<=(off+size-1)/4096;f++) n+=scanned[f];
    CHECK(hv_win_pmu_dma_write(BASE+off,size)==n);
    for (unsigned i=0;i<size;i++) ((u8 *)mem)[off+i]=((u8 *)&store_value)[i%4];
    for (unsigned i=0;i<size;i++) ((u8 *)ref)[off+i]=((u8 *)&store_value)[i%4];
    invalidate_ref(off,size);
    equivalent();
}
static void tick(void) { hv_win_pmu_tick(); ref_open=-1; equivalent(); }

static void boundary(void)
{
    u32 *words=malloc(4096); CHECK(words!=NULL);
    for (unsigned i=0;i<1024;i++) words[i]=NOP;
    words[1023]=0xd53b9d00;
    u64 counts[PMU_RW_NR_KINDS]={0};
    CHECK(pmu_rw_rewrite_frame(words,NULL,NULL,NULL,counts)==0);
    CHECK(words[1023]==0xd53b9d00 && counts[PMU_RW_SKIP_BOUNDARY]==1);
    free(words);
}
static void mixed(void)
{
    reset();
    execute(0);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_DATA_SKIPS)==1);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_SKIP_BOUNDARY)==1);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_SKIP_OTHER)==1020);
    CHECK(execute(2)==0 && !scanned[2]); /* residual window is tested, not hidden */
    tick(); execute(2);
    write_guest(1*4096+32*4,4);
    CHECK(scanned[0] && scanned[2]);
    execute(2); CHECK(scan_count==2);
    fill_device(0,4096); CHECK(!scanned[0] && scanned[2]);
    execute(0);
    fill_device(4095,2); CHECK(!scanned[0] && scanned[2]);
    execute(0); tick(); execute(2);
    write_guest(8190,8); CHECK(scanned[0] && !scanned[2]);
    for (unsigned i=0;i<12;i++) { execute(2); write_guest(8192+128,4); }
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PINS)==0);
    CHECK(hv_win_pmu_dma_write(~0UL-4,100)==0);
    CHECK(hv_win_pmu_dma_write(BASE,0)==0);
    equivalent();
}
static void denied(void)
{
    reset();
    CHECK(fault(1,128,false)); /* bounded retry; no content read */
    CHECK(memcmp(mem,ref,sizeof(mem))==0);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED)==1);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_SKIP_EXEC)==1024);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED)==0);
    translate_ok=false;
    for (unsigned i=0;i<256;i++) fault(0,128,false);
    CHECK(hpfar_calls==0 && hv_win_pmu_stat(HV_WIN_PMU_STUCK)==1);
    CHECK(memcmp(mem,ref,sizeof(mem))==0);
    translate_ok=true;
    hv_cpus_in_guest=2; CHECK(fault(0,128,false)); hv_cpus_in_guest=0;
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED)==0);
    execute(0);
    store_supported=false;
    struct exc_info ctx={.elr=VA,.far=VA+4096,.esr=DESR,.spsr=5};
    CHECK(!hv_win_handle_dabort(&ctx,ctx.esr));
    CHECK(ctx.regs[0]==0 && ctx.elr==VA);
    CHECK(states[0]==HV_XT_SCANNING && memcmp(mem,ref,sizeof(mem))==0);
    ctx.esr|=BIT(7);
    CHECK(!hv_win_handle_dabort(&ctx,ctx.esr));
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STORE_DENIED)==2);
    /* A translated data VA and a different PC cannot authorize a rewrite. */
    reset(); ctx=(struct exc_info){.elr=VA,.far=VA+128,.esr=IESR,.spsr=5};
    CHECK(hv_win_handle_iabort(&ctx)); CHECK(memcmp(mem,ref,sizeof(mem))==0);
    ctx.elr=ctx.far; ctx.esr|=BIT(10);
    CHECK(hv_win_handle_iabort(&ctx)); CHECK(memcmp(mem,ref,sizeof(mem))==0);
    reset(); ctx=(struct exc_info){.elr=VA,.far=VA,.esr=DESR,.spsr=5};
    CHECK(hv_win_handle_iabort(&ctx)); CHECK(memcmp(mem,ref,sizeof(mem))==0);
    /* AT and the software walk must agree on the output frame. */
    reset(); table(31)[0]+=4096;
    CHECK(fault(0,128,false)); CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED)==0);
}
static bool read_table(void *unused,u64 ipa,u64 *v)
{
    if (ipa<BASE || ipa+8>BASE+sizeof(mem) || ipa%8) return false;
    *v=*(u64 *)((u8 *)mem+ipa-BASE); return true;
}
static bool walk(unsigned f,unsigned mode)
{
    u64 ipa=0;
    bool ok=pmu_s1_exec(&s1,VA+f*4096,mode,read_table,NULL,&ipa);
    if (ok) CHECK(ipa==BASE+f*4096);
    return ok;
}
static void permissions(void)
{
    reset(); CHECK(walk(0,0)); CHECK(walk(0,4)); CHECK(walk(0,5));
    CHECK(!walk(0,1)); CHECK(!walk(0,0x10)); CHECK(!walk(1,0)); CHECK(!walk(1,5));
    const u64 bits[]={BIT(59),BIT(60),BIT(61)};
    for (unsigned level=28;level<31;level++) {
        unsigned idx=level==30?2:0;
        for (unsigned j=0;j<3;j++) {
            table(level)[idx]|=bits[j];
            CHECK(!walk(0,j==0?5:0));
            table(level)[idx]&=~bits[j];
        }
    }
    table(31)[0]&=~BIT(6); CHECK(!walk(0,0)); CHECK(walk(0,5)); table(31)[0]|=BIT(6);
    table(31)[0]&=~BIT(10); CHECK(!walk(0,5)); table(31)[0]|=BIT(10);
    s1.sctlr|=BIT(19); table(31)[0]&=~BIT(7); CHECK(!walk(0,5));
    CHECK(!walk(0,0)); /* WXN independent of privileged/user-writable restriction */
    table(28)[0]|=BIT(62); CHECK(walk(0,5)); table(28)[0]&=~BIT(62);
    s1.sctlr=1|BIT(20); CHECK(!walk(0,5)); CHECK(walk(0,0));
    reset(); s1.sctlr|=BIT(25); CHECK(!walk(0,5));
    reset(); s1.sctlr|=BIT(24); CHECK(!walk(0,0));
    reset(); s1.tcr|=BIT(41); CHECK(!walk(0,5));
    reset(); s1.tcr|=BIT(40); table(31)[0]|=BIT(51); CHECK(!walk(0,5));
    reset(); s1.tcr|=6UL<<32; CHECK(!walk(0,5));
    reset(); s1.tcr|=BIT(14); CHECK(!walk(0,5));
    reset(); s1.tcr|=BIT(7); CHECK(!walk(0,5));
    reset(); s1.tcr|=BIT(59); CHECK(!walk(0,5));
    reset(); table(28)[0]=0; CHECK(!walk(0,5));
    reset(); table(28)[0]=(BASE+sizeof(mem))|3; CHECK(!walk(0,5));
    reset(); table(30)[2]=BASE|BIT(10)|1; CHECK(!walk(0,5)); /* misaligned 2 MiB block */
    table(30)[2]=BIT(10)|1; u64 ipa;
    CHECK(pmu_s1_exec(&s1,VA+4096,5,read_table,NULL,&ipa) && ipa==4096);
    reset(); s1.ttbr[1]=s1.ttbr[0];
    table(28)[511]=table(28)[0]; table(29)[511]=table(29)[0]; table(30)[511]=table(30)[2];
    CHECK(pmu_s1_exec(&s1,0xfffffffffffff000UL,5,read_table,NULL,&ipa)==false);
    table(31)[511]=BASE|BIT(10)|3;
    CHECK(pmu_s1_exec(&s1,0xfffffffffffff000UL,5,read_table,NULL,&ipa) && ipa==BASE);
    s1.sctlr=0;
    CHECK(pmu_s1_exec(&s1,BASE,5,read_table,NULL,&ipa) && ipa==BASE);
    CHECK(!pmu_s1_exec(&s1,BASE+1,5,read_table,NULL,&ipa));
}
static u64 rng=0x341a20261111UL;
static u32 random32(void) { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng>>16; }
static void differential(void)
{
    for (unsigned seed=0;seed<16;seed++) {
        reset(); rng=0x341a20261111UL+seed;
        for (unsigned n=0;n<3000;n++) {
            unsigned f=random32()%24, off=f*4096+(random32()%1020)*4;
            const u32 vals[]={NOP,0xd53b9d01,0xd51befc3,0x00200100,0xd53bec03,0x87654321};
            store_value=vals[random32()%6];
            switch (random32()%4) {
            case 0: execute(f&~1U); break;
            case 1: write_guest(off,4); break;
            case 2: fill_device(off,16); break;
            case 3: tick(); break;
            }
            equivalent();
        }
    }
}
#include "replay.h"
/* The old block policy: first fetch opens a block; any DMA into it resets it.
 * Its periodic tick does nothing without write/execute pinning. */
static unsigned old_fetch(bool *open)
{
    unsigned fault=!*open;
    *open=true;
    return fault;
}
static void replay(void)
{
    unsigned new_faults=0,old_faults=0;
    /* Actual words/offsets, reconstructed mixed-block schedule. Logs do not
     * contain a complete execution/write trace or S1 VA->IPA mappings. */
    for (unsigned i=0;i<sizeof(replay_words)/sizeof(replay_words[0]);i++) {
        reset(); unsigned w=1024+(replay_words[i].ipa & 4095)/4;
        mem[w]=replay_words[i].word; mem[w+1]=replay_words[i].next;
        memcpy(ref,mem,sizeof(mem));
        bool old_open=false;
        new_faults+=execute(0); old_faults+=old_fetch(&old_open);
        new_faults+=execute(2); old_faults+=old_fetch(&old_open);
        tick(); new_faults+=execute(2); old_faults+=old_fetch(&old_open);
        fill_device(4096,4096); old_open=false;
        new_faults+=execute(0); old_faults+=old_fetch(&old_open);
        equivalent();
    }
    printf("replay: %zu logged word pairs, faults old=%u new=%u delta=%u (reconstructed schedule)\n",
           sizeof(replay_words)/sizeof(replay_words[0]),old_faults,new_faults,new_faults-old_faults);
}
int main(void)
{
    boundary(); mixed(); denied(); permissions(); differential(); replay();
    hv_win_pmu_host_reset();
    printf("win-pmu-4k: %u checks; 48000 differential operations; 0 failures\n",checks);
    return 0;
}
