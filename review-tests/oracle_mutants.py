#!/usr/bin/env python3
"""Prove the ASan-poison oracle in test_review.c detects out-of-grant guest access.
Each mutant makes the r2 model touch bytes it was not granted; the review test must
abort (ASan) for every one. Copies of the sources only; the r2 tree is untouched."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
M = HERE.parent / 'source/src/hv/win/s1'
Q = 'hv_nvme_queue.c'
MUTANTS = [
    # r4: the PRP planner is direction-aware (to_guest); same under-grant mutant.
    ('prp_undergrant', Q, 'u8 *ptr = guest_span(ctrl, addr, n, to_guest);',
     'u8 *ptr = guest_span(ctrl, addr, n > 8 ? n - 8 : n, to_guest);'),
    ('cqe_undergrant', Q, 'void *cqe_dst = guest_span(ctrl, cq->cq_addr + cq->cq_tail * sizeof(cqe), sizeof(cqe), true);',
     'void *cqe_dst = guest_span(ctrl, cq->cq_addr + cq->cq_tail * sizeof(cqe), 8, true);'),
    ('shutdown_sqe_undergrant', Q, '? guest_span(ctrl, sq->sq_addr + sq->sq_head * sizeof(cmd), sizeof(cmd), false)',
     '? guest_span(ctrl, sq->sq_addr + sq->sq_head * sizeof(cmd), 32, false)'),
    ('prp_list_undergrant', Q, 'const u8 *src = guest_span(ctrl, list, n * sizeof(u64), false);',
     'const u8 *src = guest_span(ctrl, list, n * sizeof(u64) - (n > 1 ? 8 : 0), false);'),
]
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
out = Path(tempfile.mkdtemp(prefix='m5w-r2-oracle-'))
caught = 0
for name, f, before, after in MUTANTS:
    d = out / name; d.mkdir()
    for n in ['hv_pci.c', 'hv_nvme.c', 'hv_nvme_queue.c', 'hv_nvme_queue.h', 'ram_disk.c', 'ram_disk.h', 'model.h']:
        shutil.copyfile(M / n, d / n)
    t = (d / f).read_text()
    if t.count(before) != 1: sys.exit('mutant does not match uniquely: ' + name)
    (d / f).write_text(t.replace(before, after))
    subprocess.run(['clang', '-std=c11', '-DVNVME_HOST_TEST', '-O0', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(d), str(d / 'hv_pci.c'), str(d / 'hv_nvme.c'),
                    str(d / 'hv_nvme_queue.c'), str(d / 'ram_disk.c'), str(HERE / 'test_review.c'),
                    '-o', str(d / 't')], check=True, capture_output=True)
    r = subprocess.run([str(d / 't'), '2000'], capture_output=True, text=True, env=env, timeout=600)
    hit = r.returncode != 0 and 'AddressSanitizer' in r.stderr
    caught += hit
    print(f'{name}: {"CAUGHT by ASan" if hit else "ESCAPED"}')
print(f'oracle mutants caught {caught}/4')
sys.exit(0 if caught == 4 else 1)
