#!/usr/bin/env python3
"""r4.8: the locked-link memory gate (s1n_link_mem_ok) against the REAL native_link.c.

Proves: before select nothing is refused; after lock, host memory access is refused EXCEPT, inside a
guest-exception session, the exact exception-context block and registered guest RAM; the SSD controller
MMIO, the resident/storage structures and the image are refused even inside an exception session; and the
window must lie entirely inside one permitted region.
"""
from pathlib import Path
import json, os, subprocess, tempfile

R = Path(__file__).resolve().parents[2]
S = R / 'source/src'
out = Path(tempfile.mkdtemp(prefix='r48-link-mem-'))

# guard: the production provider reads memory.ram[]; the lock must call it, not open-code ranges
nl = (S / 'hv/win/s1/native_link.c').read_text()
assert 'hv_win_storage_guest_ram_contains(addr, size)' in nl
assert 'within(exc_ctx_addr, exc_ctx_size, addr, size)' in nl
hs = (S / 'hv/win/hv_win_storage.c').read_text()
assert 'bool hv_win_storage_guest_ram_contains(u64 addr, u64 size)' in hs
assert 'for(size_t i=0;i<memory.nr_ram;i++)' in hs

code = r'''
#include <assert.h>
#include <stdio.h>
#include "types.h"
#include "native_link.h"

/* model one registered guest-RAM range (the launcher registers these at PREP) */
static u64 gram_base = 0x800000000ULL, gram_size = 0x40000000ULL;
bool hv_win_storage_guest_ram_contains(u64 a, u64 n) {
    return gram_size && n && a >= gram_base && n <= gram_size && a - gram_base <= gram_size - n;
}

#define CTX   0x10b67000000ULL        /* an exc_info block the resident passed (EL2 stack PA) */
#define CTXSZ 512ULL                  /* SIZEOF_EXC_INFO */
#define BAR   0xf30000000ULL          /* synthetic NVMe BAR (controller MMIO) */
#define ECAM  0xf28000000ULL
#define IMG   0x10005e5c000ULL        /* resident image base */

int main(void) {
    /* before select: everything allowed (normal upload path) */
    assert(s1n_link_mem_ok(BAR, 8));
    assert(s1n_link_mem_ok(IMG, 0x1000));
    assert(s1n_link_mem_ok(CTX, CTXSZ));

    s1n_link_lock();

    /* locked, NO exception session: refuse everything (size 0 is a no-op) */
    assert(s1n_link_mem_ok(0, 0));
    assert(!s1n_link_mem_ok(CTX, CTXSZ));
    assert(!s1n_link_mem_ok(gram_base, 0x1000));
    assert(!s1n_link_mem_ok(BAR, 8));

    /* enter a guest-exception session: context + guest RAM admitted */
    s1n_link_exc_ctx(CTX, CTXSZ);
    assert(s1n_link_mem_ok(CTX, CTXSZ));              /* full context */
    assert(s1n_link_mem_ok(CTX, 368));               /* ExcInfo.sizeof() <= block */
    assert(s1n_link_mem_ok(CTX + 8, 16));            /* a field */
    assert(s1n_link_mem_ok(gram_base, 0x1000));      /* guest code / RAM */
    assert(s1n_link_mem_ok(gram_base + gram_size - 4, 4));

    /* still refused inside the session: controller MMIO, resident/image, out-of-range context */
    assert(!s1n_link_mem_ok(BAR, 8));
    assert(!s1n_link_mem_ok(ECAM, 4));
    assert(!s1n_link_mem_ok(IMG, 0x1000));
    assert(!s1n_link_mem_ok(CTX, CTXSZ + 8));         /* overruns the context block */
    assert(!s1n_link_mem_ok(CTX - 8, 16));           /* starts before it */
    assert(!s1n_link_mem_ok(gram_base + gram_size - 4, 8));  /* straddles the RAM end */
    assert(!s1n_link_mem_ok(gram_base - 8, 16));      /* just below guest RAM */
    assert(!s1n_link_mem_ok(0xfffffffffffffff0ULL, 0x100)); /* address wrap */
    assert(!s1n_link_mem_ok(CTX, 0) == 0);            /* size 0 always ok, even here */
    assert(s1n_link_mem_ok(CTX, 0));

    /* leave the session: context no longer admitted */
    s1n_link_exc_ctx(0, 0);
    assert(!s1n_link_mem_ok(CTX, CTXSZ));
    assert(!s1n_link_mem_ok(gram_base, 0x1000));

    puts("PASS r4.8 locked memory gate: ctx+guest-RAM in an exception session; MMIO/resident/image refused");
    return 0;
}
'''
(out / 'test.c').write_text(code)
cmd = ['clang', '-std=gnu11', '-DS1N_STAGE=3', '-DANS_RO_HOST_TEST', '-fsanitize=address,undefined',
       '-fno-sanitize-recover=all', f'-I{R/"tests/native/shim"}', f'-I{S}', f'-I{S/"hv/win/s1"}',
       str(out / 'test.c'), str(S / 'hv/win/s1/native_link.c'), '-o', str(out / 'test')]
x = subprocess.run(cmd, capture_output=True, text=True)
(out / 'build.stderr').write_text(x.stderr)
assert x.returncode == 0, x.stderr
x = subprocess.run([str(out / 'test')], capture_output=True, text=True,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
assert x.returncode == 0, x.stderr + x.stdout
print(json.dumps({'status': 'pass', 'output': x.stdout.strip(), 'evidence_directory': str(out)}))
