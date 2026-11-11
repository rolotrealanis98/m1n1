#!/usr/bin/env python3
"""Compile the actual raw memory switch cases and proxy gate from production sources."""
from pathlib import Path
import json,os,subprocess,tempfile
R=Path(__file__).resolve().parents[2];S=R/'source/src';out=Path(tempfile.mkdtemp(prefix='r45-link-path-'))
u=(S/'uartproxy.c').read_text();p=(S/'proxy.c').read_text()
raw=u[u.index('            case REQ_MEMREAD:'):u.index('            default:',u.index('            case REQ_MEMREAD:'))]
gate=p[p.index('    if (!s1n_link_proxy'):p.index('    switch (request->opcode)',p.index('    if (!s1n_link_proxy'))]
for needle in ['#include "hv/win/s1/native_link.h"','s1n_link_discard(request.mrequest.size, disable_data_csums, discard_read)']:
 assert needle in u
assert raw.index('!s1n_link_mem_ok(') < raw.index('data_checksum')
assert raw.index('s1n_link_discard') < raw.index('write8')
assert 'uartproxy_guest_exception = false;' in u
assert 'START_EXCEPTION_LOWER || start->reason == START_HV' in u
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "types.h"
#include "proxy.h"
#include "native_link.h"
#define REQ_MEMREAD 1
#define REQ_MEMWRITE 2
#define ST_BADCMD -1
#define ST_XFRERR -3
#define GUARD_RETURN 1
#define GUARD_SKIP 2
#define GUARD_OFF 0
#define DATA_END_SENTINEL 0xdecafbad
static int exc_count,exc_guard, touched, sent, remaining, consumed;
bool hv_win_storage_guest_ram_contains(u64 a,u64 n){(void)a;(void)n;return false;}
static bool disable_data_csums, uartproxy_guest_exception;
static u8 stream[1024];
static ssize_t iodev_read(int device,void *b,size_t n) {
 (void)device; if(n>(size_t)remaining)n=remaining; if(n>11)n=11;
 memcpy(b,stream+consumed,n); remaining-=n;consumed+=n;return n;
}
static ssize_t discard_read(void *b,size_t n) { return iodev_read(0,b,n); }
static u64 data_checksum(void *p,size_t n) {(void)p;(void)n;touched++;return 0;}
static void write8(u64 p,u8 v) {(void)p;(void)v;touched++;}
struct request {int type;struct {u64 addr,size,dchecksum;} mrequest;};
struct reply {int status;struct {u64 dchecksum;} mreply;};
static int handle(struct request request) {
 struct reply reply={0};size_t bytes;u64 checksum_val;int iodev=0;
 switch(request.type) {
''' +raw+r'''
 default:return -9;
 }
 if (request.type == REQ_MEMREAD && reply.status == 0) sent++;
 return reply.status;
}
static int proxy_gate(ProxyRequest *request,ProxyReply *reply) {
''' +gate+r'''
 touched++; return 1;
}
int main(void) {
 s1n_link_lock();
 for (int sc=0;sc<2;sc++) {
  disable_data_csums=sc;remaining=513+4*sc;consumed=0;
  assert(handle((struct request){REQ_MEMWRITE,{1,513,0}})==ST_BADCMD);
  assert(!remaining && consumed==513+4*sc && !touched);
  /* Following request remains parseable; raw read does not queue its pointer. */
  assert(handle((struct request){REQ_MEMREAD,{1,4096,0}})==ST_BADCMD);
  assert(!touched && !sent);
 }
 ProxyRequest request={.opcode=P_WRITE32};ProxyReply reply={0};
 assert(proxy_gate(&request,&reply)==0 && reply.status==S_BADCMD && !touched);
 request.opcode=P_CALL; assert(proxy_gate(&request,&reply)==0 && !touched);
 assert(s1n_link_refusals()==6);
 puts("PASS raw handlers, discard, sentinel, next request, proxy dispatch gate");
}
'''
(out/'test.c').write_text(code)
cmd=['clang','-std=gnu11','-DS1N_STAGE=3','-fsanitize=address,undefined','-fno-sanitize-recover=all',f'-I{R/"tests/native/shim"}',f'-I{S}',f'-I{S/"hv/win/s1"}',str(out/'test.c'),str(S/'hv/win/s1/native_link.c'),'-o',str(out/'test')]
x=subprocess.run(cmd,capture_output=True,text=True);(out/'build.stderr').write_text(x.stderr);assert x.returncode==0,x.stderr
x=subprocess.run([str(out/'test')],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'));assert x.returncode==0,x.stderr
print(json.dumps({'status':'pass','output':x.stdout.strip(),'evidence_directory':str(out),'limitation':'actual handler bodies compiled with transport mocks; full UART transport runtime unverified'}))
