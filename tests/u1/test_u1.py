#!/usr/bin/env python3
"""u1 unified-resident checks (host only):
 1. every storage/confinement source file is byte-identical to r4.14 (git blob ids pinned from u1-import-r4.14);
 2. the u1 SMP storage gate: extracted from hv_win.c and compiled against a stub; refuses with > 1 vCPU, admits
    0/1 vCPU, and a later CONFIGURE with > 1 vCPU is refused once storage was touched;
 3. proxy.c calls the gate before hv_win_storage_prepare / hv_win_storage_native_select;
 4. no proxy opcode number is used twice (the smp line's 0xc25 clash with the storage CANARY)."""
import re, subprocess, sys, tempfile, hashlib
from pathlib import Path
R = Path(__file__).resolve().parents[2]
S = R / 'source/src'
fails = 0
def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what)
    fails += 0 if ok else 1

def blob(p):
    d = p.read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(d) + d).hexdigest()
pins = [l.split() for l in (R / 'tests/u1/r414-storage-blobs.txt').read_text().splitlines() if l.strip()]
check(len(pins) >= 20, '%d pinned storage files' % len(pins))
for sha, path in pins:
    check(blob(R / path) == sha, 'r4.14 byte-identical: ' + path)
listed = {p for _, p in pins}
for p in list((S / 'hv/win/s1').glob('*')) + list((S / 'hv/win').glob('hv_win_storage*')):
    check(str(p.relative_to(R)) in listed, 'no storage file added outside the pin list: ' + str(p.relative_to(R)))

w = (S / 'hv/win/hv_win.c').read_text()
a = w.index('static bool u1_storage_touched;'); b = w.index('/* smp2d: EL2 is emulating the guest', a)
gate = w[a:b]
c = w.index('    if (nr_vcpus > 1 && u1_storage_touched) {'); d = w.index('    }\n', c) + 6
cfg = w[c:d]
code = r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32; typedef int64_t s64;
static struct { u32 nr_vcpus; } hv_win;
static int lines;
static bool n1;                                  /* nightly: N1 probe/bring-up ran */
static bool hv_win_n1_touched(void) { return n1; }
#define printf(...) (lines++, fprintf(stderr, __VA_ARGS__))
''' + gate + r'''
static int configure(long nr_vcpus) {
''' + cfg + r'''
    hv_win.nr_vcpus = (u32)nr_vcpus; return 0;
}
int main(void) {
    int bad = 0;
    hv_win.nr_vcpus = 18; bad |= hv_win_storage_smp_gate("t") != false; bad |= lines != 1;
    bad |= u1_storage_touched;                         /* refused: storage not touched */
    hv_win.nr_vcpus = 2;  bad |= hv_win_storage_smp_gate("t") != false;
    hv_win.nr_vcpus = 0;  bad |= hv_win_storage_smp_gate("t") != true;   /* before CONFIGURE: r4.14 behaviour */
    u1_storage_touched = false;
    hv_win.nr_vcpus = 1;  bad |= hv_win_storage_smp_gate("t") != true; bad |= !u1_storage_touched;
    int l = lines; bad |= configure(18) != -1; bad |= lines != l + 1;     /* storage touched: SMP refused */
    bad |= configure(1) != 0;
    u1_storage_touched = false; bad |= configure(6) != 0;                 /* no storage: SMP allowed */
    /* nightly: after the N1 probe/bring-up every storage call is refused, whatever the vCPU count */
    n1 = true; u1_storage_touched = false;
    hv_win.nr_vcpus = 1; l = lines; bad |= hv_win_storage_smp_gate("t") != false; bad |= lines != l + 1;
    bad |= u1_storage_touched;
    hv_win.nr_vcpus = 0; bad |= hv_win_storage_smp_gate("t") != false;
    n1 = false;
    printf("gate harness %s\n", bad ? "FAILED" : "passed");
    return bad;
}
'''
with tempfile.TemporaryDirectory() as t:
    (Path(t) / 'g.c').write_text(code)
    r = subprocess.run(['cc', '-Wall', '-Werror', '-o', t + '/g', t + '/g.c'], capture_output=True, text=True)
    check(r.returncode == 0, 'gate harness compiles ' + r.stderr[-300:])
    if r.returncode == 0:
        r = subprocess.run([t + '/g'], capture_output=True, text=True)
        check(r.returncode == 0, 'gate harness: ' + r.stderr.strip().splitlines()[-1])

p = (S / 'proxy.c').read_text()
for case, call in [('P_HV_WIN_STORAGE_PREP', 'hv_win_storage_prepare('), ('P_HV_WIN_STORAGE_NATIVE:', 'hv_win_storage_native_select(')]:
    i = p.index('case ' + case); j = p.index(call, i); k = p.index('hv_win_storage_smp_gate(', i)
    check(i < k < j and 'break;' in p[k:j], 'proxy.c gates %s before the storage call' % case)

h = (S / 'proxy.h').read_text()
body = h[h.index('typedef enum {'):h.index('} ProxyOp;') if '} ProxyOp;' in h else None]
val, seen, dup = -1, {}, []
for m in re.finditer(r'^\s*(P_[A-Z0-9_]+)\s*(?:=\s*(0x[0-9a-fA-F]+|\d+))?\s*,', body, re.M):
    val = int(m.group(2), 0) if m.group(2) else val + 1
    if val in seen: dup.append((hex(val), seen[val], m.group(1)))
    seen[val] = m.group(1)
check(not dup, 'proxy opcodes unique %s' % dup)
check(seen.get(0xc25) == 'P_HV_WIN_STORAGE_NATIVE_CANARY' and seen.get(0xc29) == 'P_HV_WIN_SMP_DUMP'
      and seen.get(0xc28) == 'P_HV_WIN_HIDM_CONFIGURE', '0xc25 CANARY, 0xc28 HIDM, 0xc29 SMP_DUMP')
py = (R / 'source/proxyclient/m1n1/proxy.py').read_text()
check('P_HV_WIN_SMP_DUMP = 0xc29' in py and 'P_HV_WIN_HIDM_CONFIGURE = 0xc28' in py, 'proxy.py numbers agree')
print('u1: %s' % ('PASS' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
