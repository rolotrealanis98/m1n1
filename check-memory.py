#!/usr/bin/env python3
"""Compile the actual resolver and provider with fake sysregs/locks and real S2 tables."""
from pathlib import Path
import subprocess
import tempfile
import json
import hashlib
ROOT=Path(__file__).resolve().parent
SRC=ROOT/'source'
def noincludes(s):
    return '\n'.join(l for l in s.splitlines() if not l.startswith('#include'))+'\n'
def function(s,signature):
    start=s.index(signature); brace=s.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}'); end+=1
    return s[start:end]
vm=(SRC/'src/hv/hv_vm.c').read_text()
header=(SRC/'src/hv/win/hv_win_storage_mem.h').read_text()
provider=(SRC/'src/hv/win/hv_win_storage_mem.c').read_text()
raw=function(vm,'bool hv_pt_resolve_leaf(')
macros=vm[vm.index('#define PAGE_SIZE '):vm.index('uint64_t vaddr_bits;')]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef uint64_t u64; typedef int64_t s64;
#define BIT(n) (1UL<<(n))
#define MASK(n) (BIT(n)-1)
#define GENMASK(h,l) ((BIT((h)+1-(l))-1)<<(l))
#define FIELD_GET(m,v) (((v)&(m))/((m)&-(m)))
#define MAX_CPUS 4
struct hv_s2_leaf {u64 ipa_base,pa_base,size,descriptor;};
typedef struct {s64 lock; int count;} spinlock_t;
spinlock_t bhl;
u64 hv_cpus_in_guest;
bool hv_started_cpus[MAX_CPUS];
int boot_cpu_idx=0, fake_cpu;
u64 fake_daif;
struct {int nr_vcpus, vcpu_cpu[MAX_CPUS]; bool vgic;} hv_win;
static int smp_id(void){return fake_cpu;}
static bool hv_win_vgic_enabled(void){return hv_win.vgic;}
#define mrs(x) fake_daif
static u64 *hv_Ltop;
u64 vaddr_bits;
/* pmu-reads: the provider re-arms the PMU rewrite for write spans (hv_win_pmu.c, tested there) */
static u64 dma_calls, dma_ipa, dma_len;
__attribute__((unused)) static unsigned hv_win_pmu_dma_write(u64 ipa, u64 size){dma_calls++;dma_ipa=ipa;dma_len=size;return 0;}
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);abort();}}while(0)
'''
tests=r'''
static u64 *l1,*l2,*l3;
#define IPA 0x10000000000UL
#define ATTR (BIT(10)|(3UL<<8)|(3UL<<6)|(15UL<<2)|3)
static struct hv_storage_mem mem;
static struct hv_sm_range ram[2],protect[4];
static struct hv_sm_config cfg;
static void tables(void){
 memset(l1,0,0x4000);memset(l2,0,0x4000);memset(l3,0,0x4000);
 hv_Ltop=l1;vaddr_bits=44;l1[IPA>>36]=(u64)l2|3;
 l2[(IPA>>25)&2047]=(u64)l3|3;
 for(unsigned i=0;i<2048;i++)l3[i]=(IPA+i*0x4000UL)|ATTR;
}
static void setup(void){
 memset(&mem,0,sizeof(mem));memset(hv_started_cpus,0,sizeof(hv_started_cpus));
 hv_cpus_in_guest=0;fake_cpu=boot_cpu_idx=0;fake_daif=0xc0;
 hv_win.nr_vcpus=1;hv_win.vcpu_cpu[0]=0;hv_win.vgic=true;bhl=(spinlock_t){0,1};
 ram[0]=(struct hv_sm_range){IPA,0x2000000};ram[1]=(struct hv_sm_range){IPA+0x3000000,0x4000};
 protect[0]=(struct hv_sm_range){IPA+0x100000,0x4000};
 protect[1]=(struct hv_sm_range){IPA+0x110000,17};
 cfg=(struct hv_sm_config){.epoch=55,.resident={IPA-0x100000,0x100000},.ram=ram,.protected=protect,.nr_ram=2,.nr_protected=2};
 tables();
}
static void registered(void){setup();CHECK(hv_storage_mem_register(&mem,&cfg));hv_started_cpus[0]=true;CHECK(hv_storage_mem_begin(&mem,55));}
static void reject_leaf(u64 descriptor){registered();l3[1]=descriptor;CHECK(!hv_storage_guest_span(&mem,IPA+0x4000,32,true));CHECK(!hv_storage_guest_span(&mem,IPA+0x4000,32,false));}
int main(void){
 CHECK(!posix_memalign((void**)&l1,0x4000,0x4000));CHECK(!posix_memalign((void**)&l2,0x4000,0x4000));CHECK(!posix_memalign((void**)&l3,0x4000,0x4000));
 struct hv_s2_leaf out;
 setup();CHECK(hv_pt_resolve_leaf(IPA+123,&out));CHECK(out.ipa_base==IPA&&out.pa_base==IPA&&out.size==0x4000&&out.descriptor==(IPA|ATTR));
 CHECK(!hv_pt_resolve_leaf(IPA,NULL));CHECK(!hv_pt_resolve_leaf(1UL<<44,&out));CHECK(!out.size);
 for(unsigned i=0;i<64;i++){setup();u64 bit=1UL<<i;if(bit&~(PTE_TARGET_MASK|3UL)){l1[IPA>>36]|=bit;CHECK(!hv_pt_resolve_leaf(IPA,&out));setup();l2[(IPA>>25)&2047]|=bit;CHECK(!hv_pt_resolve_leaf(IPA,&out));}}
 setup();l1[IPA>>36]&=~1UL;CHECK(!hv_pt_resolve_leaf(IPA,&out));
 setup();l2[(IPA>>25)&2047]&=~1UL;CHECK(!hv_pt_resolve_leaf(IPA,&out));
 setup();l3[0]&=~1UL;CHECK(!hv_pt_resolve_leaf(IPA,&out)); /* software subpage table rejected, not followed */
 setup();l3[0]&=~2UL;CHECK(!hv_pt_resolve_leaf(IPA,&out));
 setup();l2[(IPA>>25)&2047]=IPA|(ATTR&~2UL);CHECK(hv_pt_resolve_leaf(IPA+0x123456,&out));CHECK(out.size==0x2000000&&out.pa_base==IPA&&out.descriptor==(IPA|(ATTR&~2UL)));
 l2[(IPA>>25)&2047]|=0x4000;CHECK(!hv_pt_resolve_leaf(IPA,&out));
 setup();vaddr_bits=36;hv_Ltop=l2;l2[0]=(u64)l3|3;l3[0]=ATTR;CHECK(hv_pt_resolve_leaf(23,&out));CHECK(out.ipa_base==0&&out.pa_base==0);
 vaddr_bits=31;CHECK(!hv_pt_resolve_leaf(0,&out));vaddr_bits=45;CHECK(!hv_pt_resolve_leaf(0,&out));
 setup();hv_Ltop=NULL;CHECK(!hv_pt_resolve_leaf(IPA,&out));
 registered();dma_calls=0;CHECK(hv_storage_guest_span(&mem,IPA+1,0x20000,true)==(void*)(IPA+1));CHECK(dma_calls==1&&dma_ipa==IPA+1&&dma_len==0x20000);
 CHECK(hv_storage_guest_span(&mem,IPA+1,0x20000,false)==(void*)(IPA+1));CHECK(dma_calls==1); /* reads never re-arm */
 CHECK(!hv_storage_guest_span(&mem,IPA,0x20001,true));CHECK(!hv_storage_guest_span(&mem,IPA,0,true));CHECK(!hv_storage_guest_span(&mem,0,1,true));CHECK(!hv_storage_guest_span(&mem,UINT64_MAX-4,8,true));
 CHECK(!hv_storage_guest_span(&mem,IPA-1,2,true));CHECK(!hv_storage_guest_span(&mem,IPA+0x2000000-1,2,true));
 CHECK(!hv_storage_guest_span(&mem,IPA+0x100000,1,true));CHECK(!hv_storage_guest_span(&mem,IPA+0xfffff,2,true));
 CHECK(hv_storage_guest_span(&mem,IPA+0xfffff,1,true));CHECK(!hv_storage_guest_span(&mem,IPA+0x110010,2,true));CHECK(hv_storage_guest_span(&mem,IPA+0x110011,1,true));
 CHECK(hv_storage_guest_span(&mem,IPA+0x1fff000,4096,true)); /* Mu-like high workspace remains admitted */
 CHECK(!hv_storage_mem_begin(&mem,55));CHECK(hv_storage_mem_end(&mem));CHECK(!hv_storage_guest_span(&mem,IPA,1,true));CHECK(!hv_storage_mem_begin(&mem,54));CHECK(hv_storage_mem_begin(&mem,55));
 hv_storage_mem_retire(&mem);CHECK(!hv_storage_guest_span(&mem,IPA,1,true));CHECK(!hv_storage_mem_end(&mem));CHECK(!hv_storage_mem_register(&mem,&cfg));
 /* Reject each unknown/unsupported attribute, device/NC, AF/SH/AP restrictions, aliases. */
 dma_calls=0;
 for(unsigned i=0;i<64;i++){u64 d=(IPA+0x4000)|ATTR;u64 bit=1UL<<i;
   if(bit&~(PTE_TARGET_MASK|ATTR|BIT(54)|BIT(55)|GENMASK(58,56)))reject_leaf(d|bit);
 }
 for(unsigned ap=0;ap<3;ap++)reject_leaf(((IPA+0x4000)|ATTR)&~(3UL<<6)|(u64)ap<<6);
 for(unsigned attr=0;attr<15;attr++)reject_leaf(((IPA+0x4000)|ATTR)&~(15UL<<2)|(u64)attr<<2);
 reject_leaf(((IPA+0x4000)|ATTR)&~BIT(10));reject_leaf(((IPA+0x4000)|ATTR)&~BIT(8));
 reject_leaf((IPA+0x8000)|ATTR);reject_leaf(((IPA+0x4000)|ATTR)|BIT(56));
 reject_leaf((((IPA+0x4000)|ATTR)|BIT(55)|BIT(54))&~BIT(7)); /* SCANNING RO+XN */
 reject_leaf((((IPA+0x4000)|ATTR)|BIT(55)|BIT(54))&~(3UL<<6)); /* tracked, S2AP none */
 reject_leaf((((IPA+0x4000)|ATTR)|BIT(55))&~(3UL<<6)|BIT(7)); /* tracked, S2AP WO */
 reject_leaf(((((IPA+0x4000)|ATTR)|BIT(55))&~BIT(7))|BIT(52)); /* CODE with the contiguous bit */
 reject_leaf(((IPA+0x4000)|ATTR)&~BIT(7)); /* untracked RO: still refused both ways */
 CHECK(dma_calls==0); /* a refused span never reaches the PMU reset */
 /* pmu-reads: PINNED RW+X and CODE RO+X are admitted both ways (only RW pages are ever tracked;
  * CODE's RO is the rewrite's write trap). A write re-arms exactly the admitted span, after every
  * check; the provider itself leaves the descriptor alone (the PMU helper owns that change). */
 for(int st=0;st<2;st++){
  registered();l3[1]=st?(((IPA+0x4000)|ATTR)|BIT(55)):((((IPA+0x4000)|ATTR)|BIT(55))&~BIT(7));
  u64 saved=l3[1];dma_calls=0;
  CHECK(hv_storage_guest_span(&mem,IPA+0x4000,32,false));CHECK(dma_calls==0);
  CHECK(hv_storage_guest_span(&mem,IPA+0x3ff0,32,true));CHECK(dma_calls==1&&dma_ipa==IPA+0x3ff0&&dma_len==32);
  CHECK(!hv_storage_guest_span(&mem,IPA+0x3ff0,0x20001,true));CHECK(dma_calls==1);
  CHECK(l3[1]==saved);
 }
 registered();l3[1]|=BIT(55)|BIT(54)|GENMASK(58,56);u64 saved=l3[1];CHECK(hv_storage_guest_span(&mem,IPA+0x4000,32,true));CHECK(l3[1]==saved);CHECK(hv_storage_guest_span(&mem,IPA+0x4000,32,false));
 registered();l3[8]&=~BIT(7);CHECK(!hv_storage_guest_span(&mem,IPA+1,0x20000,true));
 registered();l2[(IPA>>25)&2047]=IPA|(ATTR&~2UL);CHECK(hv_storage_guest_span(&mem,IPA+1,0x20000,true));
 l2[(IPA>>25)&2047]|=BIT(55)|BIT(54);CHECK(!hv_storage_guest_span(&mem,IPA,32,true));
 /* Copies input arrays; reject bad registration once and remain dead. */
 registered();ram[0].base=0;protect[0].base=0;CHECK(hv_storage_guest_span(&mem,IPA,32,true));CHECK(!hv_storage_guest_span(&mem,IPA+0x100000,32,true));
 for(int scenario=0;scenario<15;scenario++){
  setup();switch(scenario){case 0:cfg.epoch=0;break;case 1:cfg.nr_ram=65;break;case 2:cfg.nr_protected=129;break;case 3:ram[0].size=0;break;case 4:ram[0].base++;break;case 5:ram[1]=ram[0];break;case 6:cfg.resident.size++;break;case 7:protect[0]=(struct hv_sm_range){UINT64_MAX,2};break;case 8:hv_started_cpus[0]=true;break;case 9:hv_cpus_in_guest=1;break;case 10:hv_win.nr_vcpus=2;break;case 11:fake_cpu=1;break;case 12:cfg.ram=NULL;break;case 13:cfg.protected=NULL;break;case 14:hv_win.vgic=false;break;}
  CHECK(!hv_storage_mem_register(&mem,&cfg));CHECK(mem.state==HV_SM_DEAD);
 }
 /* Context failure at begin/span/end permanently retires. */
 for(int where=0;where<3;where++)for(int scenario=0;scenario<10;scenario++){
  registered();if(where==0)CHECK(hv_storage_mem_end(&mem));
  switch(scenario){case 0:bhl.lock=-1;break;case 1:bhl.count=0;break;case 2:hv_cpus_in_guest=1;break;case 3:fake_daif&=~BIT(7);break;case 4:fake_daif&=~BIT(6);break;case 5:hv_started_cpus[1]=true;break;case 6:hv_started_cpus[0]=false;break;case 7:hv_win.nr_vcpus=2;break;case 8:fake_cpu=1;break;case 9:hv_win.vcpu_cpu[0]=1;break;}
  CHECK(where==0?!hv_storage_mem_begin(&mem,55):where==1?!hv_storage_guest_span(&mem,IPA,1,true):!hv_storage_mem_end(&mem));CHECK(mem.state==HV_SM_DEAD);
 }

#ifdef REAL_IDENTITY_TEST
 /* Real identity-backed ordinary host allocation: returned pointer is usable;
  * actual provider never dereferences payload itself or mutates PTEs. */
 unsigned char *arena=mmap((void*)IPA,0x8000,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);CHECK(arena!=MAP_FAILED);
 setup();u64 real=(u64)arena;CHECK(real<(1UL<<44));
 memset(l1,0,0x4000);memset(l2,0,0x4000);memset(l3,0,0x4000);
 l1[real>>36]=(u64)l2|3;l2[(real>>25)&2047]=(u64)l3|3;
 CHECK(((real>>25)&2047)==(((real+0x7fff)>>25)&2047));
 l3[(real>>14)&2047]=real|ATTR|BIT(55)|BIT(54);
 l3[((real>>14)+1)&2047]=(real+0x4000)|ATTR|BIT(55)|BIT(54);
 ram[0]=(struct hv_sm_range){real,0x8000};cfg.nr_ram=1;
 cfg.resident=(struct hv_sm_range){real-0x4000,0x4000};
 protect[0]=(struct hv_sm_range){real+0x7fff,1};cfg.nr_protected=1;
 CHECK(hv_storage_mem_register(&mem,&cfg));hv_started_cpus[0]=true;CHECK(hv_storage_mem_begin(&mem,55));
 memset(arena,0x5a,0x8000);unsigned char *ptr=hv_storage_guest_span(&mem,real+0x3ff0,32,true);
 CHECK(ptr==arena+0x3ff0);memset(ptr,0xa5,32);
 for(unsigned i=0;i<0x8000;i++)CHECK(arena[i]==(i>=0x3ff0&&i<0x4010?0xa5:0x5a));
 CHECK(!hv_storage_guest_span(&mem,real+0x7ffe,2,true));CHECK(hv_storage_mem_end(&mem));CHECK(munmap(arena,0x8000)==0);
#endif
 free(l1);free(l2);free(l3);printf("%u actual-C resolver/provider checks PASS\n",checks);return 0;
}
'''
out=Path(tempfile.mkdtemp(prefix='m5w-s1-resident-mem-'));c=out/'actual.c';c.write_text(shim+macros+noincludes(header)+raw+noincludes(provider)+tests)
subprocess.run(['clang','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-Wno-bitwise-op-parentheses','-g','-fsanitize=address,undefined',str(c),'-o',str(out/'test')],check=True,timeout=60)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=30)
subprocess.run(['clang','-std=c11','-D_DARWIN_C_SOURCE','-DREAL_IDENTITY_TEST','-Wall','-Wextra','-Werror','-Wno-bitwise-op-parentheses',str(c),'-o',str(out/'test-identity')],check=True,timeout=60)
identity=subprocess.run([str(out/'test-identity')],check=True,capture_output=True,text=True,timeout=30)
receipt={'output':r.stdout.strip(),'identity_output':identity.stdout.strip(),'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [SRC/'src/hv/hv_vm.c',SRC/'src/hv/hv.h',SRC/'src/hv/win/hv_win_storage_mem.c',SRC/'src/hv/win/hv_win_storage_mem.h']},'host_only':True,'output_directory':str(out)}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
