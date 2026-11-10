#!/usr/bin/env python3
"""Native tests of exact smp2h ADT functions; MMU/ADT are mocked, no hardware."""
from pathlib import Path
import subprocess,tempfile,os,json
R=Path(__file__).resolve().parents[2];src=R/'source/src';out=Path(tempfile.mkdtemp(prefix='r46-mcc-'))
s=(src/'mcc.c').read_text();body=s[s.index('static const char *const mcc_adt_tz_names'):s.index('int mcc_unmap_carveouts(void)')]
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef uint64_t u64;typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GENMASK(h,l) (((~0ULL) << (l)) & (~0ULL >> (63-(h))))
#define REGION_RWX_EL0 (1ULL<<60)
#define REGION_RW_EL0 (2ULL<<60)
#define REGION_RX_EL1 (3ULL<<60)
struct prop {char name[32];u32 size;u64 value[2];};
static struct prop props[2];
static void *adt=(void *)1;
static int node_present=1;
static u64 ram_base=0x800000000ULL,mem_size_actual=0x400000000ULL;
static size_t mcc_carveout_count;
static struct {u64 base,size;} mcc_carveouts[8];
static struct {u64 addr,size;} calls[16];static unsigned count;
static unsigned get_page_size(void) {return 16384;}
static int adt_path_offset(void *a,const char *p) {(void)a;assert(!strcmp(p,"/chosen/carveout-memory-map"));return node_present?1:-1;}
static void *adt_getprop(void *a,int n,const char *name,u32 *len) {
 (void)a;assert(n==1);for(unsigned i=0;i<2;i++)if(!strcmp(name,props[i].name)){*len=props[i].size;return props[i].size?props[i].value:NULL;}return NULL;
}
#define ADT_FOREACH_PROPERTY(a,n,p) for(struct prop *p=props;p<props+2;p++)
static void mmu_rm_mapping(u64 a,u64 s) {assert(count<16);calls[count].addr=a;calls[count++].size=s;}
static void reset(void) {
 count=0;mcc_carveout_count=0;node_present=1;ram_base=0x800000000ULL;mem_size_actual=0x400000000ULL;
 strcpy(props[0].name,"region-id-4");strcpy(props[1].name,"region-id-2");
 for(unsigned i=0;i<2;i++){props[i].size=16;props[i].value[0]=ram_base+0x4000+i*0x8000;props[i].value[1]=0x4000;}
}
'''
post=r'''
int main(void) {
 reset();assert(mcc_unmap_adt_tz()==0);assert(count==8 && mcc_carveout_count==2);
 const u64 aliases[]={0,REGION_RWX_EL0,REGION_RW_EL0,REGION_RX_EL1};
 for(unsigned i=0;i<2;i++)for(unsigned j=0;j<4;j++){assert(calls[i*4+j].addr==(props[i].value[0]|aliases[j]));assert(calls[i*4+j].size==0x4000);}
 mcc_describe_l2c_adr(props[1].value[0]);mcc_describe_l2c_adr(ram_base);
 for(unsigned bad=0;bad<8;bad++){
  reset();props[1].size=0;
  switch(bad){case 0:props[0].size=8;break;case 1:props[0].value[1]=0;break;
   case 2:props[0].value[0]=ram_base-0x4000;break;case 3:props[0].value[0]=UINT64_MAX-0x3fff;props[0].value[1]=0x8000;break;
   case 4:props[0].value[0]=ram_base+mem_size_actual;break;case 5:props[0].value[0]++;break;
   case 6:props[0].value[1]++;break;case 7:mcc_carveout_count=7;break;}
  assert(mcc_unmap_adt_tz()==0);assert(count==0);
 }
 reset();node_present=0;assert(mcc_unmap_adt_tz()==-1 && count==0);
 reset();ram_base=UINT64_MAX-0x4000;assert(mcc_unmap_adt_tz()==-1 && count==0);
 puts("PASS exact smp2h ADT functions: 2 regions, 4 aliases, 10 rejection cases, diagnostic");
}
'''
(out/'test.c').write_text(pre+body+post)
x=subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-format','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],capture_output=True,text=True);assert x.returncode==0,x.stderr
x=subprocess.run([str(out/'test')],capture_output=True,text=True);assert x.returncode==0,x.stderr
assert 'region-id-2' in x.stdout and 'in no carveout' in x.stdout
h=Path('/Users/rolo/Work/agents/win-el2/storage/s1-resident-smp2h-20261110/source/src')
identical={n:(src/n).read_bytes()==(h/n).read_bytes() for n in ['mcc.c','mcc.h','exception.c']}
assert all(identical.values()),identical
print(json.dumps({'status':'pass','exact_smp2h_files':identical,'output':x.stdout,'evidence_directory':str(out),'hardware':'unverified'},indent=2))
