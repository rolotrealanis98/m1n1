#!/usr/bin/env python3
"""Mutants of the r2.1 B3 PRP-list walk; each must be caught by test_confirm.c
(ASan poisoned-arena oracle or assertions). Works on copies only."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
M = HERE.parent / 'source/src/hv/win/s1'
Q = 'hv_nvme_queue.c'
MUT = [
    ('fragment_undergrant', 'const u8 *src = guest_span(ctrl, list, n * sizeof(u64), false);',
     'const u8 *src = guest_span(ctrl, list, n > 1 ? (n - 1) * sizeof(u64) : 8, false);'),
    ('chain_alignment_dropped', 'if (frags && (list & (VNVME_PAGE_SIZE - 1))) return false; /* chained: page aligned */', ''),
    ('fragment_overlap_unchecked', '            if (overlaps(addr, n, frag_addr[i], frag_len[i])) return false;', '            (void)0;'),
    ('in_page_off_by_one', 'size_t in_page = (VNVME_PAGE_SIZE - (size_t)(list & (VNVME_PAGE_SIZE - 1))) / sizeof(u64);',
     'size_t in_page = (VNVME_PAGE_SIZE - (size_t)(list & (VNVME_PAGE_SIZE - 1))) / sizeof(u64) + 1;'),
    ('qword_alignment_dropped', 'if (!list || (list & 7) || frags == VNVME_MAX_LIST_FRAGS) return false;',
     'if (!list || frags == VNVME_MAX_LIST_FRAGS) return false;'),
]
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
out = Path(tempfile.mkdtemp(prefix='m5w-r21-b3-'))
caught = 0
for name, before, after in MUT:
    d = out / name; d.mkdir()
    for n in ['hv_pci.c', 'hv_nvme.c', 'hv_nvme_queue.c', 'hv_nvme_queue.h', 'ram_disk.c', 'ram_disk.h', 'model.h']:
        shutil.copyfile(M / n, d / n)
    t = (d / Q).read_text()
    if t.count(before) != 1: sys.exit('mutant does not match uniquely: ' + name)
    (d / Q).write_text(t.replace(before, after))
    subprocess.run(['clang', '-std=c11', '-DVNVME_HOST_TEST', '-Wno-unused-function', '-O0', '-g',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + str(d),
                    *[str(d / f) for f in ('hv_pci.c', 'hv_nvme.c', 'hv_nvme_queue.c', 'ram_disk.c')],
                    str(HERE / 'test_confirm.c'), '-o', str(d / 't')], check=True, capture_output=True)
    r = subprocess.run([str(d / 't'), '3000'], capture_output=True, text=True, env=env, timeout=900)
    hit = r.returncode != 0
    caught += hit
    how = 'ASan' if 'AddressSanitizer' in r.stderr else ('assert' if 'FAIL:' in r.stderr else '')
    print(f'{name:28s} {"CAUGHT (" + how + ")" if hit else "ESCAPED"}')
print(f'B3 mutants caught {caught}/{len(MUT)}')
