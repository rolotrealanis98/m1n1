#!/usr/bin/env python3
"""Independent r3 review tests (host only, ASan+UBSan). Builds copies only.
 1. gpt_fuzz: actual native_window.c vs malformed GPTs + window confinement oracle.
 2. e2e_review: actual r3 stack (hv_win_storage*.c, S1 model, native_window, ans_ro, m1n1
    asc/rtkit/sart) against a COPY of the builder's fake ANS patched to log every physical
    READ LBA and optionally to complete a READ without DMA."""
from pathlib import Path
import json, os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
R = HERE.parent
NAT = R / 'tests/native'
SRC = R / 'source/src'; WIN = SRC / 'hv/win'; S1 = WIN / 's1'
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
SAN = ['-DS1N_STAGE=3', '-O0', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
out = Path(tempfile.mkdtemp(prefix='m5w-r3-review-'))
res = {}

# 1. GPT fuzz
subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-variable', *SAN,
                f'-I{NAT/"shim"}', f'-I{S1}', str(HERE / 'gpt_fuzz.c'), '-o', str(out / 'gpt_fuzz')], check=True)
r = subprocess.run([str(out / 'gpt_fuzz'), sys.argv[1] if len(sys.argv) > 1 else '300000'],
                   capture_output=True, text=True, env=env, timeout=1800)
print(r.stdout.strip(), r.stderr.strip()[-2000:])
res['gpt_fuzz'] = r.returncode == 0

# 2. end-to-end
b = out / 'e2e'; (b / 'src').mkdir(parents=True); (b / 's1').mkdir()
(b / 'config.h').write_text('/* host */\n')
for n in ['proxy.h', 'asc.c', 'asc.h', 'rtkit.c', 'rtkit.h', 'sart.c', 'sart.h']: shutil.copyfile(SRC / n, b / 'src' / n)
for n in ['s1_stage.h', 'native_link.c', 'native_link.h', 'ans_ro.c', 'ans_ro.h', 'native_window.c', 'native_window.h', 'hv_nvme.c', 'hv_pci.c', 'hv_nvme_queue.c',
          'hv_nvme_queue.h', 'model.h', 'ram_disk.c', 'ram_disk.h']: shutil.copyfile(S1 / n, b / 's1' / n)
for n in ['hv_win_storage.c', 'hv_win_storage.h', 'hv_win_storage_mem.h', 'hv_win_storage_native.c']: shutil.copyfile(WIN / n, b / n)
fake = (NAT / 'fake_ans.c').read_text()
# r3.1: the builder's fake gained read_no_dma; the review patch keeps its own switch.
# r3.4: the builder's fake now DMAs through data_dma (PRP model); same patch semantics.
old_read = '                for (unsigned b = 0; b < nlb; b++) fake_disk_block(slba + b, data + (u64)b * 4096);\n                if (!F.cfg.read_no_dma) data_dma(prp1, prp2, data, (u64)nlb * 4096);'
assert fake.count(old_read) == 1
fake = fake.replace(old_read, '                review_log_read(slba, nlb);\n                for (unsigned b = 0; b < nlb; b++) fake_disk_block(slba + b, data + (u64)b * 4096);\n'
                    '                if (!review_skip_dma && !F.cfg.read_no_dma) data_dma(prp1, prp2, data, (u64)nlb * 4096);')
fake = fake.replace('#include "fake_ans.h"', '#include "fake_ans.h"\nextern void review_log_read(u64 slba, unsigned nlb);\nextern bool review_skip_dma;', 1)
(b / 'fake_ans.c').write_text(fake)
def strip(t, drop=()): return '\n'.join(l for l in t.splitlines() if not l.startswith('#include') and l not in drop)
test = ((NAT / 'prelude.h').read_text()
        + strip((b / 'hv_win_storage.c').read_text(), ('extern char _base[], _end[];',))
        + '\n' + strip((b / 'hv_win_storage_native.c').read_text())
        + '\n#define main builder_main\n' + (NAT / 'body.c').read_text() + '\n#undef main\n'
        + (HERE / 'e2e_review.c').read_text())
(b / 'test.c').write_text(test)
cmd = ['clang', '-std=gnu11', '-DVNVME_HOST_TEST', '-DANS_RO_HOST_TEST', '-Wall', '-Wextra', '-Wno-unused-parameter',
       '-Wno-unused-function', *SAN, f'-I{NAT/"shim"}', f'-I{NAT}', f'-I{b/"src"}', f'-I{b/"s1"}', f'-I{b}',
       str(b / 'test.c'), str(b / 'fake_ans.c')] + [str(b / 'src' / n) for n in ['asc.c', 'rtkit.c', 'sart.c']] \
      + [str(b / 's1' / n) for n in ['native_link.c', 'ans_ro.c', 'native_window.c', 'hv_nvme.c', 'hv_pci.c', 'hv_nvme_queue.c', 'ram_disk.c']] \
      + ['-o', str(b / 'test')]
c = subprocess.run(cmd, capture_output=True, text=True)
if c.returncode: print(c.stderr[-4000:]); sys.exit(1)
for s in ['guest_fuzz', 'freeze_total', 'select_no_prep', 'stale_bounce', 'stale_bounce_after_data', 'stale_bounce_after_recheck']:
    r = subprocess.run([str(b / 'test'), s], capture_output=True, text=True, env=env, timeout=600)
    print(s, 'rc', r.returncode, '|', r.stdout.strip().replace('\n', ' | '), r.stderr.strip()[-600:])
    res[s] = r.returncode == 0
print(json.dumps({'status': 'pass' if all(res.values()) else 'fail', 'results': res, 'out': str(out)}))
sys.exit(0 if all(res.values()) else 1)
