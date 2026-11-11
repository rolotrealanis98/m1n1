#!/usr/bin/env python3
"""Independent r4 write-safety review tests. Builds the ACTUAL r4 stack (S1 model,
hv_win_storage*.c, native_window, ans_ro, m1n1 asc/rtkit/sart) + the builder's fake ANS +
prelude + body.c (main renamed) + body_review.c, under ASan/UBSan. Also runs two mutants
that disable a first-layer write-window check and proves precheck() still confines the write
(no physical write outside the window; window_refusals > 0). Copies only."""
from pathlib import Path
import json, os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
R = HERE.parent
NAT = R / 'tests/native'
SRC = R / 'source/src'; WIN = SRC / 'hv/win'; S1 = WIN / 's1'
M1N1 = ['asc.c', 'asc.h', 'rtkit.c', 'rtkit.h', 'sart.c', 'sart.h']
S1F = ['s1_stage.h', 'native_link.c', 'native_link.h', 'ans_ro.c', 'ans_ro.h', 'native_window.c', 'native_window.h', 'hv_nvme.c', 'hv_pci.c', 'hv_nvme_queue.c',
       'hv_nvme_queue.h', 'model.h', 'ram_disk.c', 'ram_disk.h']
SCEN = ['rw_fuzz', 'canary_faithful', 'direct_ans', 'ro_regression', 'oob_probe']
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

def strip(t, drop=()):
    return '\n'.join(l for l in t.splitlines() if not l.startswith('#include') and l not in drop)

def body_text():
    # CHECK.md: r4.5+ body.c includes its case files; inline them verbatim (they carry their own
    # system includes and the access->posix_access rename), strip only body.c's own #includes.
    out = []
    for l in (NAT / 'body.c').read_text().splitlines():
        if l.startswith('#include "') and l.endswith('_cases.h"'):
            out.append((NAT / l.split('"')[1]).read_text())
        elif not l.startswith('#include'):
            out.append(l)
    return '\n'.join(out)

def build(root, name, mutation=None):
    out = root / name; (out / 'src').mkdir(parents=True); (out / 's1').mkdir()
    (out / 'config.h').write_text('/* host */\n')
    shutil.copyfile(SRC / 'proxy.h', out / 'src' / 'proxy.h')  # CHECK.md: r4.5+ link gate needs proxy.h
    for n in M1N1: shutil.copyfile(SRC / n, out / 'src' / n)
    for n in S1F: shutil.copyfile(S1 / n, out / 's1' / n)
    for n in ['hv_win_storage.c', 'hv_win_storage.h', 'hv_win_storage_mem.h', 'hv_win_storage_native.c']:
        shutil.copyfile(WIN / n, out / n)
    if mutation:
        f, old, new = mutation; p = out / f; s = p.read_text()
        assert s.count(old) == 1, (name, s.count(old)); p.write_text(s.replace(old, new))
    body = body_text()  # body.c has its own main -> rename it
    body = body.replace('int main(int argc, char **argv)', 'int builder_main(int argc, char **argv)', 1)
    test = ((NAT / 'prelude.h').read_text()
            + strip((out / 'hv_win_storage.c').read_text(), ('extern char _base[], _end[];',))
            + '\n' + strip((out / 'hv_win_storage_native.c').read_text())
            + '\n' + body + '\n' + (HERE / 'body_review.c').read_text())
    (out / 'test.c').write_text(test)
    common = ['clang', '-std=gnu11', '-DVNVME_HOST_TEST', '-DANS_RO_HOST_TEST', '-DS1N_STAGE=3', '-Wall', '-Wextra',
              '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-O1', '-g',
              '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              f"-I{NAT/'shim'}", f'-I{NAT}', f"-I{out/'src'}", f"-I{out/'s1'}", f'-I{out}']
    srcs = [out / 'test.c', NAT / 'fake_ans.c'] + [out / 'src' / n for n in M1N1 if n.endswith('.c')] \
        + [out / 's1' / n for n in S1F if n.endswith('.c')]
    b = subprocess.run(common + [str(s) for s in srcs] + ['-o', str(out / 'test')], capture_output=True, text=True, timeout=240)
    if b.returncode: raise RuntimeError(b.stderr[-3000:])
    return out

def run_all(out):
    res = {}
    for s in SCEN:
        r = subprocess.run([str(out / 'test'), s], capture_output=True, text=True, env=env, timeout=600)
        res[s] = r.returncode == 0
        line = (r.stdout.strip().replace('\n', ' | ') or r.stderr.strip()[-300:])
        print(f'  {s}: {"ok" if res[s] else "FAIL"} | {line}')
    return res

root = Path(tempfile.mkdtemp(prefix='m5w-r4-review-'))
print('baseline:')
base = build(root, 'baseline'); res = run_all(base)
# Defense-in-depth mutants: progressively remove the earlier write-window checks and prove the
# pre-doorbell precheck() still confines every physical write (oob_probe => writes_outside_window==0).
# M1 removes the frontend bound; M2 ALSO removes the backend's ans_ro_xfer in_window re-check, so
# only precheck() stands between an out-of-window guest write and ANS.
FRONT = '    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba)\n        return VNVME_SC_INVALID_FIELD;\n    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n    u8 *buffer = data_buffer(ctrl, &capacity);\n    static struct prp_plan plan;\n    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false)) return VNVME_SC_DATA_TRANSFER_ERROR;'
FRONT_MUT = '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n    u8 *buffer = data_buffer(ctrl, &capacity);\n    static struct prp_plan plan;\n    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false)) return VNVME_SC_DATA_TRANSFER_ERROR;'
XFER = '(write && (!rw_enabled || !in_window(lba, blocks))) ||'
XFER_MUT = '(write && (!rw_enabled || (0 && !in_window(lba, blocks)))) ||'
MUT = [
    ('frontend_bound_off', [('s1/hv_nvme_queue.c', FRONT, FRONT_MUT)]),
    ('frontend_and_xfer_off_precheck_only', [('s1/hv_nvme_queue.c', FRONT, FRONT_MUT),
                                             ('s1/ans_ro.c', XFER, XFER_MUT)]),
]
print('defense mutants (precheck must still confine; oob_probe => no physical OOB write):')
mres = {}
for name, muts in MUT:
    try:
        mo = root / ('mut_' + name); mo.mkdir()
        # build with multiple edits by writing a tiny wrapper around build()
        mo2 = build(root, 'b_' + name, muts[0])
        if len(muts) > 1:
            for f, old, new in muts[1:]:
                p = mo2 / f; s = p.read_text(); assert s.count(old) == 1, (name, f, s.count(old)); p.write_text(s.replace(old, new))
            # rebuild test.c + binary
            body = body_text().replace('int main(int argc, char **argv)', 'int builder_main(int argc, char **argv)', 1)
            test = ((NAT / 'prelude.h').read_text()
                    + strip((mo2 / 'hv_win_storage.c').read_text(), ('extern char _base[], _end[];',))
                    + '\n' + strip((mo2 / 'hv_win_storage_native.c').read_text())
                    + '\n' + body + '\n' + (HERE / 'body_review.c').read_text())
            (mo2 / 'test.c').write_text(test)
            common = ['clang', '-std=gnu11', '-DVNVME_HOST_TEST', '-DANS_RO_HOST_TEST', '-DS1N_STAGE=3', '-Wall', '-Wextra',
                      '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-O1', '-g',
                      '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                      f"-I{NAT/'shim'}", f'-I{NAT}', f"-I{mo2/'src'}", f"-I{mo2/'s1'}", f'-I{mo2}']
            srcs = [mo2 / 'test.c', NAT / 'fake_ans.c'] + [mo2 / 'src' / n for n in M1N1 if n.endswith('.c')] \
                + [mo2 / 's1' / n for n in S1F if n.endswith('.c')]
            b = subprocess.run(common + [str(s) for s in srcs] + ['-o', str(mo2 / 'test')], capture_output=True, text=True, timeout=240)
            if b.returncode: raise RuntimeError(b.stderr[-2000:])
        r = subprocess.run([str(mo2 / 'test'), 'oob_probe'], capture_output=True, text=True, env=env, timeout=600)
        confined = r.returncode == 0
        mres[name] = confined
        info = (r.stdout.strip().splitlines() or [''])[0]
        print(f'  {name}: {"CONFINED (no OOB physical write) " + info if confined else "ESCAPED: " + r.stderr.strip()[-200:]}')
    except RuntimeError as e:
        mres[name] = False; print(f'  {name}: build failed {str(e)[-200:]}')
ok = all(res.values()) and all(mres.values())
print(json.dumps({'status': 'pass' if ok else 'fail', 'scenarios': res, 'mutants_confined': mres}))
sys.exit(0 if ok else 1)
