#!/usr/bin/env python3
"""Test real (non HOST_TEST) CPU/context predicate and exact proxy lifetime wrapper."""
from pathlib import Path
import tempfile,subprocess,json
R=Path(__file__).resolve().parents[2];S=R/'source/src';out=Path(tempfile.mkdtemp(prefix='r46-context-'))
u=(S/'uartproxy.c').read_text();a=u.index('static int uartproxy_run_inner(');b=u.index('static int uartproxy_run_inner(',a+1)
wrapper=u[a:b]
(out/'smp.h').write_text('#include "types.h"\nextern int cpu,boot_cpu_idx;\nstatic inline int smp_id(void){return cpu;}\n')
(out/'uartproxy.h').write_text('#include "types.h"\nextern u32 uartproxy_storage_exceptions;\n')
code='''#include <assert.h>
#include "types.h"
#include "native_link.h"
#include "proxy.h"
int cpu,boot_cpu_idx;u32 uartproxy_storage_exceptions;
bool hv_win_storage_guest_ram_contains(u64 a,u64 n){(void)a;(void)n;return false;}
static bool smp_ctx; /* smp-storage: hv_win_storage.c (cap > 1 and the storage lock held by this CPU) */
bool hv_win_storage_smp_context(void){return smp_ctx;}
u32 hv_win_storage_smp_cmd_timeout_us(void){return 0;} /* nightly-7: cap 1 (r4.14 bounds) */
enum {START_BOOT,START_EXCEPTION,START_EXCEPTION_LOWER,START_HV};
struct exc_info {u64 regs[46];};
/* r4.9: r4.8 uartproxy.c brackets the session with start->info (s1n_link_exc_ctx); stub carries it */
struct uartproxy_msg_start {u32 reason; u32 code; struct exc_info *info;};
'''+wrapper+'''
static int depth,code;
static int uartproxy_run_inner(struct uartproxy_msg_start *s) {
 if (s && s->reason!=START_BOOT) {
  assert(!s1n_storage_context());
  assert(!s1n_link_proxy(P_HV_WIN_STORAGE_NATIVE_CANARY,false));
  assert(!s1n_link_proxy(P_HV_WIN_STORAGE_NATIVE_QUIESCE,false));
  if (!depth) {depth++; assert(uartproxy_run(s)==code);depth--;assert(uartproxy_storage_exceptions==1);}
 } else assert(s1n_storage_context());
 return code;
}
int main(void){
 s1n_link_lock();assert(s1n_storage_context());
 cpu=1;assert(!s1n_storage_context());assert(!s1n_link_proxy(P_HV_WIN_STORAGE_PREP,false));cpu=0;
 /* smp-storage: a secondary CPU passes only with the SMP context, never inside a host exception session */
 cpu=3;smp_ctx=true;assert(s1n_storage_context());
 uartproxy_storage_exceptions=1;assert(!s1n_storage_context());uartproxy_storage_exceptions=0;
 smp_ctx=false;assert(!s1n_storage_context());cpu=0;assert(s1n_storage_context());
 for(unsigned reason=0;reason<4;reason++)for(code=-1;code<3;code++){
  struct uartproxy_msg_start s={reason,0,0};assert(uartproxy_run(&s)==code);assert(!uartproxy_storage_exceptions);assert(s1n_storage_context());
 }
 assert(uartproxy_run(NULL)==code);assert(!uartproxy_storage_exceptions);
 return 0;
}
'''
(out/'test.c').write_text(code)
cmd=['clang','-std=gnu11','-DS1N_STAGE=2','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',f'-I{out}',f'-I{R/"tests/native/shim"}',f'-I{S}',f'-I{S/"hv/win/s1"}',str(out/'test.c'),str(S/'hv/win/s1/native_link.c'),'-o',str(out/'test')]
x=subprocess.run(cmd,capture_output=True,text=True);assert x.returncode==0,x.stderr
x=subprocess.run([str(out/'test')],capture_output=True,text=True);assert x.returncode==0,x.stderr
print(json.dumps(dict(status='pass',cases='production CPU predicate; boot/native/lower/bark proxy; nested sessions; all return codes',output_directory=str(out))))
