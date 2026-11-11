#!/usr/bin/env python3
"""nightly-4: each mutant breaks one check of the PRP planner (or the in-order copy); test_prp_overlap must fail
for every one. Copies of the sources only (temp dir); the tree is untouched. Usage: mutants.py [iterations]"""
import os, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / 'src/hv/win/s1'
ITER = sys.argv[1] if len(sys.argv) > 1 else '20000'
CC = '/opt/homebrew/opt/llvm/bin/clang' if Path('/opt/homebrew/opt/llvm/bin/clang').exists() else 'clang'
L = 'if (!to_guest && !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {'
MUTANTS = [
    # data segment admission: READ destinations admitted for read only (a read-only page becomes a target)
    ('data_admission_read_only', 'u8 *ptr = guest_span(ctrl, addr, n, to_guest);', 'u8 *ptr = guest_span(ctrl, addr, n, false);'),
    # data segment admission dropped for repeated pages: only the first occurrence of a page is admitted
    ('repeat_not_admitted', 'u8 *ptr = guest_span(ctrl, addr, n, to_guest);',
     'u8 *ptr = NULL; for (size_t j = 0; j < plan->count; j++) if ((plan->addr[j] & ~(u64)(VNVME_PAGE_SIZE - 1)) == (addr & ~(u64)(VNVME_PAGE_SIZE - 1))) ptr = plan->ptr[j] + (addr & (VNVME_PAGE_SIZE - 1)) - (plan->addr[j] & (VNVME_PAGE_SIZE - 1)); if (!ptr) ptr = guest_span(ctrl, addr, n, to_guest);'),
    # queue-page check
    ('queue_check_dropped', 'if (!ptr || overlaps_queue(ctrl, addr, n)) return false;', 'if (!ptr) return false;'),
    # PRP-list-page check
    ('list_check_dropped', '            if (overlaps(addr, n, frag_addr[i], frag_len[i])) return false;', '            (void)0;'),
    # list fragment admission
    ('list_admission_dropped', 'const u8 *src = guest_span(ctrl, list, n * sizeof(u64), false);',
     'const u8 *src = guest_span(ctrl, list, n * sizeof(u64), false); if (!src && list >= 0x80000000ull && list < 0x80200000ull) src = (const u8 *)ctrl->ops->guest_span(ctrl->opaque, 0x80000000ull, 1, false) + (list - 0x80000000ull);'),
    # entry alignment
    ('entry_align_dropped', 'if (!addr || (addr & (VNVME_PAGE_SIZE - 1))) return false;', 'if (!addr) return false;'),
    # nightly-4b: the overlap check line is
    #   if (!to_guest && !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {
    # WRITE overlap allowed (all shapes, incl. partial)
    ('write_overlap_allowed', L, 'if (false) {'),
    # WRITE exact whole-page repeat refused again (nightly-4 behaviour)
    ('write_exact_repeat_refused', L, 'if (!to_guest) {'),
    # WRITE short segment repeating an earlier whole page at the same address allowed (W4)
    ('write_short_repeat_allowed', L, 'if (!to_guest && !(plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {'),
    # WRITE any overlap at the same start address allowed (length checks dropped)
    ('write_same_addr_allowed', L, 'if (!to_guest && !(addr == plan->addr[i])) {'),
    # WRITE: queue-page check dropped for WRITE sources (W7)
    ('write_queue_check_dropped', 'if (!ptr || overlaps_queue(ctrl, addr, n)) return false;',
     'if (!ptr || (to_guest && overlaps_queue(ctrl, addr, n))) return false;'),
    # WRITE: list-page check dropped for WRITE sources (W6)
    ('write_list_check_dropped', '            if (overlaps(addr, n, frag_addr[i], frag_len[i])) return false;',
     '            if (to_guest && overlaps(addr, n, frag_addr[i], frag_len[i])) return false;'),
    # WRITE sources admitted for write instead of read (W9: a write-only page)
    ('write_admission_wrong_perm', 'u8 *ptr = guest_span(ctrl, addr, n, to_guest);', 'u8 *ptr = guest_span(ctrl, addr, n, true);'),
    # WRITE: a repeated source page is not copied again (stale bounce bytes submitted)
    ('write_repeat_not_copied', """    for (size_t i = 0; i < plan->count; i++) {
        memcpy(buffer, plan->ptr[i], plan->len[i]);""", """    for (size_t i = 0; i < plan->count; i++) {
        if (i && plan->addr[i] == plan->addr[0]) { buffer += plan->len[i]; continue; }
        memcpy(buffer, plan->ptr[i], plan->len[i]);"""),
    # WRITE: bounce offset advanced a whole page per segment
    ('write_copy_stride_page', """        memcpy(buffer, plan->ptr[i], plan->len[i]);
        buffer += plan->len[i];""", """        memcpy(buffer, plan->ptr[i], plan->len[i]);
        buffer += VNVME_PAGE_SIZE;"""),
    # WRITE repeat counter missing
    ('dup_src_counter_missing', '            ctrl->prp_dup_src_writes++;\n', ''),
    # r4.14 behaviour: READ partial overlap refused
    ('read_partial_refused', L,
     'if (!to_guest || n != VNVME_PAGE_SIZE || plan->len[i] != VNVME_PAGE_SIZE || addr != plan->addr[i]) {'),
    # every READ overlap refused
    ('read_overlap_refused', L, 'if (to_guest || !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {'),
    # copies in reverse order (earlier bytes would win)
    ('copy_reverse_order', '''    for (size_t i = 0; i < plan->count; i++) {
        memcpy(plan->ptr[i], buffer, plan->len[i]);
        buffer += plan->len[i];
    }''', '''    size_t off = 0;
    for (size_t i = 0; i < plan->count; i++) off += plan->len[i];
    for (size_t i = plan->count; i-- > 0;) {
        off -= plan->len[i];
        memcpy(plan->ptr[i], buffer + off, plan->len[i]);
    }'''),
    # wrong per-segment length (stream advanced a whole page per segment)
    ('copy_stride_page', '''        memcpy(plan->ptr[i], buffer, plan->len[i]);
        buffer += plan->len[i];''', '''        memcpy(plan->ptr[i], buffer, plan->len[i]);
        buffer += VNVME_PAGE_SIZE;'''),
    # partial-overlap counter
    ('part_counter_missing', '                ctrl->prp_part_dest_reads++;\n', ''),
    # partial classification wrong (exact repeats counted as partial)
    ('part_flag_always', '                    plan->part_dest = true;', '                    plan->part_dest = true; else plan->part_dest = true;'),
]
out = Path(tempfile.mkdtemp(prefix='m5w-n4-prp-mutants-'))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')

def run(m):
    name, before, after = m
    d = out / name; d.mkdir()
    for n in ['hv_nvme_queue.c', 'hv_nvme_queue.h']: shutil.copyfile(SRC / n, d / n)
    t = (d / 'hv_nvme_queue.c').read_text()
    if t.count(before) != 1: return name, 'NO-MATCH'
    (d / 'hv_nvme_queue.c').write_text(t.replace(before, after))
    c = subprocess.run([CC, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-iquote', str(HERE / 'shim'),
                        '-I', str(HERE / 'shim'), '-iquote', str(d), '-o', str(d / 't'), str(HERE / 'test_prp_overlap.c'),
                        str(d / 'hv_nvme_queue.c')], capture_output=True, text=True)
    if c.returncode: return name, 'COMPILE-FAIL ' + c.stderr[-300:]
    r = subprocess.run([str(d / 't'), ITER], capture_output=True, text=True, env=env, timeout=900)
    return name, 'KILLED' if r.returncode else 'SURVIVED'

with ThreadPoolExecutor(max_workers=int(os.environ.get('FB_JOBS', '8'))) as ex:
    res = list(ex.map(run, MUTANTS))
killed = sum(1 for _, s in res if s == 'KILLED')
for n, s in res: print(f'{n}: {s}')
print(f'prp-overlap-n4 mutants killed {killed}/{len(MUTANTS)}')
shutil.rmtree(out, ignore_errors=True)
sys.exit(0 if killed == len(MUTANTS) else 1)
