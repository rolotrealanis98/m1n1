#!/usr/bin/env python3
"""perf-exits static checks against the parent commit (nightly-5 8018846; perf-exits was on nightly-4b 7e803d6):
 - hv_asm.S: purely additive; the default sync path is the old one with a 3-instruction probe in front, and
   the fast vector falls back to the unchanged _hv_entry barriers + hv_exc_sync on the same frame;
 - hv_exc.c: hv_exc_sync/irq/fiq bodies are byte-identical to the old functions (renamed); the wrappers only
   add timing/statistics behind HV_PERF_STATS; the fast path uses the slow path's shadow (PERCPU(mdscr));
 - hv_win.c: hv_win_perf_configure refusals (unknown flags, unconfigured, guest running, no FGT) and the
   per-CPU apply hook; proxy.c/h + proxy.py agree on P_HV_WIN_PERF = 0xc58 and no other command uses it;
 - storage files (s1/*, hv_win_storage*, hv_nvme*, native_*) unchanged from the parent."""
import re, subprocess, sys, pathlib
SRC = pathlib.Path(__file__).resolve().parents[2]          # source/
WT = SRC.parent
PARENT = 'af1bb98'  # nightly-5c (perf-exits was on nightly-4b 7e803d6)
fails = 0
def check(name, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name)
    fails += not ok
def old(path):
    return subprocess.run(['git', '-C', str(WT), 'show', f'{PARENT}:source/{path}'], capture_output=True, text=True, check=True).stdout
def new(path):
    return (SRC / path).read_text()

# hv_asm.S
oa, na = old('src/hv/hv_asm.S').splitlines(), new('src/hv/hv_asm.S').splitlines()
it = iter(na)
check('hv_asm.S: every old line still present, in order (additive change)', all(any(l == m for m in it) for l in oa))
def block(lines, start, end):
    i = lines.index(start); j = lines.index(end, i + 1)
    return lines[i:j]
check('hv_asm.S: _hv_entry unchanged (dsb sy; isb kept)', block(oa, '_hv_entry:', '.globl _hv_return') == block(na, '_hv_entry:', '.globl _hv_return'))
vs = block(na, '_v_hv_sync:', '/* perf-exits: try the fast path; if it declines, run the unchanged slow path on the same frame */')
code = [l.strip() for l in vs if l.strip() and not l.strip().startswith('/*')]
check('hv_asm.S: default path = old _v_hv_sync + probe', code == ['_v_hv_sync:', 'msr pan, #0', 'sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)',
      'str x30, [sp, #-16]!', 'adrp x30, hv_perf_fast_sync', 'ldrb w30, [x30, :lo12:hv_perf_fast_sync]', 'cbnz w30, _v_hv_sync_fast',
      'bl _hv_entry', 'bl hv_exc_sync', 'cbz x0, 1f', 'b _hv_gexit', '1:', 'b _hv_return'])
fv = block(na, '_v_hv_sync_fast:', '.globl _v_hv_irq')
fcode = [l.strip() for l in fv if l.strip()]
check('hv_asm.S: fast vector falls back with dsb sy; isb; mov x0, sp; bl hv_exc_sync', fcode == ['_v_hv_sync_fast:', 'bl _hv_entry_nb',
      'bl hv_exc_sync_fast', 'cbnz x0, 1f', 'dsb sy', 'isb', 'mov x0, sp', 'bl hv_exc_sync', 'cbz x0, 1f', 'b _hv_gexit', '1:', 'b _hv_return'])
ent = block(na, '_hv_entry:', '    dsb sy')[1:]
nb = block(na, '_hv_entry_nb:', '    mov x0, sp')[1:]
check('hv_asm.S: _hv_entry_nb saves the same frame as _hv_entry', [l for l in ent if l.strip()] == [l for l in nb if l.strip()] and len(nb) == 15)

# hv_exc.c
oe, ne = old('src/hv/hv_exc.c'), new('src/hv/hv_exc.c')
def fn(src, sig):
    i = src.index(sig); j = src.index('\n}\n', i)
    return src[src.index('{', i):j]
for o, n in [('bool hv_exc_sync(struct exc_info *ctx)\n{', 'static bool hv_exc_sync_body(struct exc_info *ctx)\n{'),
             ('void hv_exc_irq(struct exc_info *ctx)\n{', 'static void hv_exc_irq_body(struct exc_info *ctx)\n{'),
             ('void hv_exc_fiq(struct exc_info *ctx)\n{', 'static void hv_exc_fiq_body(struct exc_info *ctx)\n{')]:
    check(f'hv_exc.c: {n.split("(")[0].split()[-1]} == old {o.split("(")[0].split()[-1]} byte for byte', fn(oe, o) == fn(ne, n))
check('hv_exc.c: slow-path MDSCR case unchanged (PERCPU(mdscr) shadow)',
      fn(oe, 'static bool hv_handle_msr_unlocked(struct exc_info *ctx, u64 iss)\n{') == fn(ne, 'static bool hv_handle_msr_unlocked(struct exc_info *ctx, u64 iss)\n{'))
fast = fn(ne, 'u64 hv_exc_sync_fast(struct exc_info *ctx)\n{')
check('hv_exc.c: fast path emulates on the slow path shadow', 'hv_perf_mdscr_emulate(esr, ctx->regs, &PERCPU(mdscr))' in fast)
check('hv_exc.c: fast path advances ELR_EL2 by 4 only when handled',
      fast.index('return 0;') < fast.index('msr(ELR_EL2, mrs(ELR_EL2) + 4);'))
w = fn(ne, 'bool hv_exc_sync(struct exc_info *ctx)\n{')
check('hv_exc.c: hv_exc_sync wrapper calls the body once and only adds stats', w.count('hv_exc_sync_body(ctx)') == 1 and 'hv_perf_t0()' in w)
check('hv_exc.c: PF line only with an option set', 'if (hv_perf_flags)\n        hv_perf_report(P, now, freq);' in ne)
check('hv_exc.c: hv_exc_perf_set drives the vector flag from FAST_MDSCR only',
      'hv_perf_fast_sync = !!(flags & HV_PERF_FAST_MDSCR);' in ne)

# hv_win.c
hw = new('src/hv/win/hv_win.c')
cfg = fn(hw, 's64 hv_win_perf_configure(u64 flags)\n{')
check('hv_win.c: unknown flags refused', 'if (flags & ~(u64)HV_PERF_ALL)' in cfg)
check('hv_win.c: refused before CONFIGURE', 'if (!hv_win_enabled())' in cfg)
check('hv_win.c: refused while a guest runs', 'hv_started_cpus[cpu]' in cfg)
check('hv_win.c: mdscr-pass needs FGT', 'HV_PERF_MDSCR_PASS) && !FIELD_GET(ID_AA64MMFR0_FGT' in cfg)
check('hv_win.c: fast-mdscr dropped with SPTM emulation or GXF', 'hv_sptm_mode == SPTM_EMULATION || gxf_enabled()' in cfg)
ci = fn(hw, 'void hv_win_cpu_init(void)\n{')
check('hv_win.c: per-CPU apply in hv_win_cpu_init (secondaries)', 'hv_win_perf_cpu_apply();' in ci)
ap = fn(hw, 'static void hv_win_perf_cpu_apply(void)\n{')
check('hv_win.c: apply is a no-op without mdscr-pass', ap.lstrip('{').lstrip().startswith('if (!(hv_win_perf & HV_PERF_MDSCR_PASS))\n        return;'))
check('hv_win.c: FGT bits set before MDCR traps are dropped', ap.index('SYS_HDFGWTR_EL2') < ap.index('msr(MDCR_EL2'))

# proxy
ph, pc, pp = new('src/proxy.h'), new('src/proxy.c'), new('proxyclient/m1n1/proxy.py')
check('proxy.h: P_HV_WIN_PERF = 0xc58, unique', 'P_HV_WIN_PERF = 0xc58' in ph and ph.count('0xc58') == 1)
check('proxy.c: dispatch', 'case P_HV_WIN_PERF:\n            reply->retval = hv_win_perf_configure(request->args[0]);' in pc)
check('proxy.py: constant + method', 'P_HV_WIN_PERF = 0xc58' in pp and 'def hv_win_perf_configure(self, flags):' in pp)

# storage untouched
paths = ['source/src/hv/win/s1', 'source/src/hv/win/hv_win_storage.c', 'source/src/hv/win/hv_win_storage.h',
         'source/src/hv/win/hv_win_storage_mem.c', 'source/src/hv/win/hv_win_storage_mem.h',
         'source/src/hv/win/hv_win_storage_native.c', 'source/src/hv/win/hv_win_status.c']
# nightly-7 (replaces "storage files identical to the parent", which cannot hold once the SMP disk path is merged):
# (a) at the merge commit, every storage-pattern file differs from the parent by exactly the reviewed smp-storage
#     diff 7e803d6..efb8883 (the same changed lines, file by file, nothing else);
# (b) after the merge, only the nightly-7 SMP files listed in N7_STORAGE may differ from the merge commit.
N7_MERGE, SMP_BASE, SMP_HEAD = '78c0567', '7e803d6', 'efb8883'
N7_STORAGE = {'source/src/hv/win/hv_win_storage.c', 'source/src/hv/win/hv_win_storage.h',
              'source/src/hv/win/hv_win_status.c', 'source/src/hv/win/s1/ans_ro.c', 'source/src/hv/win/s1/native_link.c'}  # native_link.c: + the session-end allowlist
def changed_lines(a, b, f):
    out = subprocess.run(['git', '-C', str(WT), 'diff', '-U0', a, b, '--', f], capture_output=True, text=True, check=True).stdout
    return sorted(l for l in out.splitlines() if l[:1] in '+-' and not l.startswith(('+++', '---')))
def storage_files(rev):
    out = subprocess.run(['git', '-C', str(WT), 'ls-tree', '-r', '--name-only', rev, '--', 'source/src'],
                         capture_output=True, text=True, check=True).stdout.split()
    return [p for p in out if re.search(r'/s1/|hv_win_storage|hv_nvme|native_', p) or p in paths]
files = sorted(set(storage_files(PARENT)) | set(storage_files(N7_MERGE)))
bad = [f for f in files if changed_lines(PARENT, N7_MERGE, f) != changed_lines(SMP_BASE, SMP_HEAD, f)]
check(f'storage files at the nightly-7 merge = parent + the smp-storage diff exactly ({len(files)} files)', not bad)
r = subprocess.run(['git', '-C', str(WT), 'diff', '--name-only', N7_MERGE, '--', 'source/src'], capture_output=True, text=True)
bad = [p for p in r.stdout.split() if (re.search(r'/s1/|hv_win_storage|hv_nvme|native_', p) or p in paths) and p not in N7_STORAGE]
check(f'after the merge only the nightly-7 SMP storage files changed ({len(r.stdout.split())} source files changed)', not bad)
# nightly-6c: the nightly-5c review's conditions for builds rebased on it (REVIEW-5c.md): these resident files unchanged
r = subprocess.run(['git', '-C', str(WT), 'diff', '--quiet', PARENT, '--', 'source/src/heapblock.c', 'source/src/heapblock.h',
                    'source/src/usb.c', 'source/src/mtp_dma.c'])
check('heapblock.c/.h, usb.c, mtp_dma.c identical to the parent (review-5c condition)', r.returncode == 0)
print(f'win-perf static: {fails} failed')
sys.exit(1 if fails else 0)
