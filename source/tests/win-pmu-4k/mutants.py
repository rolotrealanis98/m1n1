#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""nightly-5 mutants of the PMU rewrite (4 KiB granule, the granule switch, the walker, the frame
scanner). Each mutant edits a copy of the production sources (never the tree), rebuilds test_4k
(ASan/UBSan, PMU_MAX_OPEN=4) and the legacy win-pmu-rewrite suite (16 KiB + 4 KiB groups), and must
make one of them fail at run time. A mutant that does not compile is an error, not a kill.
"""
from pathlib import Path
import concurrent.futures, os, shutil, subprocess, sys, tempfile

here = Path(__file__).resolve().parent
source = here.parent.parent
win = source / 'src/hv/win'
legacy = source / 'tests/win-pmu-rewrite'
FILES = ['hv_win_pmu.c', 'hv_win_pmu_rewrite.c', 'hv_win_pmu_rewrite.h', 'hv_win_pmu_4k.h', 'hv_win.h']

M = [
    # proof of execution
    ('no_exec_proof', 'hv_win_pmu.c', 'if (pmu_exec_proof(ctx, ipa, hint)) {', 'if (pmu_exec_proof(ctx, ipa, hint) || true) {'),
    ('accept_mismatched_ipa', 'hv_win_pmu.c', '&& walked == ipa;', '&& true;'),
    ('ignore_fault_class', 'hv_win_pmu.c', '(ctx->esr >> 26) != EC_IABORT_LOWER ||', 'false ||'),
    ('ignore_fnv', 'hv_win_pmu.c', '(ctx->esr & ESR_ISS_ABORT_FNV) ||', 'false ||'),
    ('ignore_pc', 'hv_win_pmu.c', '        ctx->far != ctx->elr)\n        return false;\n    struct pmu_s1 s;',
     '        false)\n        return false;\n    struct pmu_s1 s;'),
    ('hint_is_proof', 'hv_win_pmu.c', 'if (hint || (ctx->esr', 'if (false || (ctx->esr'),
    ('no_execution_mark', 'hv_win_pmu.c', 'pmu_scan_frame(ctx, block, pa, frame, fr, true);', 'pmu_scan_frame(ctx, block, pa, frame, fr, false);'),
    # scan extent
    ('scan_two_frames', 'hv_win_pmu_rewrite.c',
     'for (u32 i = 0; i < 1024; i++) {\n        u32 old = words[i];\n        enum pmu_rw_kind kind = pmu_rw_classify(old);\n        if (kind == PMU_RW_NONE) {\n            counts[PMU_RW_SKIP_OTHER]',
     'for (u32 i = 0; i < 2048; i++) {\n        u32 old = words[i];\n        enum pmu_rw_kind kind = pmu_rw_classify(old);\n        if (kind == PMU_RW_NONE) {\n            counts[PMU_RW_SKIP_OTHER]'),
    ('wrong_frame_pa', 'hv_win_pmu.c', 'fpa = pa + frame * PMU_FRAME_SIZE;', 'fpa = pa;'),
    ('ignore_guard_frame', 'hv_win_pmu_rewrite.c', 'if (pmu_rw_next_is_data(following)) {', 'if (false) {'),
    ('boundary_always_skip', 'hv_win_pmu_rewrite.c', 'if (i == 1023 && !next) {', 'if (i == 1023) {'),
    ('boundary_ignore_null', 'hv_win_pmu_rewrite.c', 'if (i == 1023 && !next) {', 'if (false) {'),
    ('next_block_dropped', 'hv_win_pmu.c', '        next = (const u32 *)npa;', '        next = NULL;'),
    # frame state lifetime
    ('no_clear_on_flip', 'hv_win_pmu.c', '    if (fr)\n        pmu_clear_frames(fr);\n\n    flips++;',
     '    (void)fr;\n\n    flips++;'),
    ('scanning_write_spurious', 'hv_win_pmu.c', '            if (!pmu.mode16k)\n                break;',
     '            if (false)\n                break;'),
    ('rescan_on_reopen', 'hv_win_pmu.c',
     '            /* re-protected: read-only and XN on every CPU already, frame states valid */\n            break;',
     '            pmu_clear_frames(fr);\n            break;'),
    # open list / re-protection
    ('no_reprotect', 'hv_win_pmu.c',
     '            hv_xt_set(block, HV_XT_SCANNING, flips);\n            pmu.stats[HV_WIN_PMU_REPROTECTS]++;',
     '            hv_xt_set(block, HV_XT_CODE, flips);\n            pmu.stats[HV_WIN_PMU_REPROTECTS]++;'),
    ('never_list', 'hv_win_pmu.c', '    if (!pmu_all_scanned(fr))\n        pmu_open_add(block, fr);',
     '    if (false)\n        pmu_open_add(block, fr);'),
    ('close_ticks_11', 'hv_win_pmu.c', '#define PMU_CLOSE_TICKS  10', '#define PMU_CLOSE_TICKS  11'),
    ('reprotect_full_blocks', 'hv_win_pmu.c', '== HV_XT_CODE && !pmu_all_scanned(fr)) {', '== HV_XT_CODE) {'),
    ('tick_never_closes', 'hv_win_pmu.c', 'if (pmu.nr_open && ++pmu.close_ticks >= PMU_CLOSE_TICKS)',
     'if (false && pmu.nr_open)'),
    ('list_full_no_close', 'hv_win_pmu.c', '    if (pmu.nr_open >= PMU_MAX_OPEN)\n        pmu_close_open();',
     '    if (pmu.nr_open >= 2 * PMU_MAX_OPEN)\n        pmu_close_open();'),
    ('listed_dedup_off', 'hv_win_pmu.c', '    if (*fr & PMU_BS_LISTED)\n        return;', '    (void)0;'),
    # device fills
    ('dma_no_frame_reset', 'hv_win_pmu.c', '*fr &= ~PMU_BS_SCANNED(f);', '*fr &= 0xff;'),
    ('state_bit_shared', 'hv_win_pmu.c', '#define PMU_BS_SCANNED(frame) (1U << (frame))', '#define PMU_BS_SCANNED(frame) (1U << ((frame) & 1))'),
    ('state_per_32k', 'hv_win_pmu.c', 'return &pmu.ranges[i].frames[(ipa - pmu.ranges[i].base) / PMU_PAGE];', 'return &pmu.ranges[i].frames[(ipa - pmu.ranges[i].base) / (2 * PMU_PAGE)];'),
    ('heap_need_frames', 'hv_win_pmu.c', 'u64 frame_bytes = pmu.mode16k ? 0 : size / PMU_PAGE;', 'u64 frame_bytes = pmu.mode16k ? 0 : size / PMU_FRAME_SIZE;'),
    ('heap_need_no_margin', 'hv_win_pmu.c', 'if (tables * PMU_PAGE + PMU_HEAP_MARGIN + frame_bytes > heapblock_remaining()) {', 'if (tables * PMU_PAGE + frame_bytes > heapblock_remaining()) {'),
    ('dma_keep_x', 'hv_win_pmu.c',
     '            if (st == HV_XT_CODE) {\n                hv_xt_set(page, HV_XT_SCANNING, flips);',
     '            if (st == HV_XT_CODE) {\n                hv_xt_set(page, HV_XT_CODE, flips);'),
    ('dma_reset_whole_block', 'hv_win_pmu.c', 'u64 lo = ipa > page ? ipa : page;', 'u64 lo = page;'),
    ('dma_miss_last_frame', 'hv_win_pmu.c', 'f <= (hi - page) / PMU_FRAME_SIZE; f++)',
     'f < (hi - page) / PMU_FRAME_SIZE; f++)'),
    ('dma_pinned_kept', 'hv_win_pmu.c', '        } else if (st == HV_XT_CODE || st == HV_XT_PINNED) {',
     '        } else if (st == HV_XT_CODE) {'),
    # walker
    ('ignore_hpd_bit', 'hv_win_pmu_4k.h', 'bool hpd = !!(s->tcr & BIT(upper ? 42 : 41));', 'bool hpd = false;'),
    ('hpd_always', 'hv_win_pmu_4k.h', 'bool hpd = !!(s->tcr & BIT(upper ? 42 : 41));', 'bool hpd = true;'),
    ('root_page_aligned', 'hv_win_pmu_4k.h', 'u32 root_align = bits - (12 + 9 * (3 - start)) + 3;',
     'u32 root_align = 12;'),
    ('accept_misaligned_root', 'hv_win_pmu_4k.h',
     '    if (s->ttbr[upper] & ((1UL << root_align) - 1) & ~1UL)\n        return false;', '    (void)0;'),
    ('allow_pxn_leaf', 'hv_win_pmu_4k.h', '(!user && (pxn || (d & BIT(53))))', '(!user && pxn)'),
    ('allow_el0_writable', 'hv_win_pmu_4k.h', '(!user && el0 && !ro)', 'false'),
    ('ignore_wxn', 'hv_win_pmu_4k.h', '((s->sctlr & BIT(19)) && !ro)', 'false'),
    ('ignore_af', 'hv_win_pmu_4k.h', '|| !(d & BIT(10)))', ')'),
    ('ignore_endian', 'hv_win_pmu_4k.h', 's->sctlr & (BIT(25) | BIT(24))', 'false'),
    ('ignore_dbm', 'hv_win_pmu_4k.h', '(s->tcr & BIT(40)) && (d & BIT(51))', 'false'),
    ('accept_lpa2', 'hv_win_pmu_4k.h', '(BIT(59) | BIT(55) | BIT(56))', '(BIT(55) | BIT(56))'),
    ('ignore_el0_access', 'hv_win_pmu_4k.h', '(user && (!el0 || uxn || (d & BIT(54))))', '(user && (uxn || (d & BIT(54))))'),
    ('allow_uxn_leaf', 'hv_win_pmu_4k.h', '(user && (!el0 || uxn || (d & BIT(54))))', '(user && (!el0 || uxn))'),
    # granule switch
    ('mode16k_ignored', 'hv_win_pmu.c',
     'return pmu.mode16k ? pmu_exec_fault_16k(ctx, ipa) : pmu_exec_fault_4k(ctx, ipa, hint);',
     'return false ? pmu_exec_fault_16k(ctx, ipa) : pmu_exec_fault_4k(ctx, ipa, hint);'),
    ('mode_change_after_arm', 'hv_win_pmu.c', 'if (pmu.armed && mode16k != pmu.mode16k) {', 'if (false) {'),
    ('id_bit_missing', 'hv_win_pmu.c', '(pmu.mode16k ? 0 : HV_WIN_PMU_ID_4K)', '0'),
    ('unknown_flag_accepted', 'hv_win_pmu.c', '    if (flags & ~(u64)HV_WIN_PMU_MODE_16K)\n        return -1;',
     '    (void)0;'),
]

CFLAGS = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          '-fno-strict-aliasing', '-DPMU_REWRITE_HOST_TEST', '-fsanitize=address,undefined',
          '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']


def build_and_run(d):
    """Returns (compiled, failed_at_runtime, evidence)."""
    t4, lg = d / 'test_4k', d / 'test_legacy'
    c1 = subprocess.run(['clang', *CFLAGS, '-DPMU_MAX_OPEN=4', '-iquote', str(legacy / 'shim'), '-iquote', str(d),
                         str(here / 'test_4k.c'), str(d / 'hv_win_pmu.c'), str(d / 'hv_win_pmu_rewrite.c'),
                         '-o', str(t4)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c2 = subprocess.run(['clang', *CFLAGS, '-Wundef', '-Wsign-compare', '-iquote', str(legacy / 'shim'),
                         '-iquote', str(d), str(legacy / 'test_pmu_rewrite.c'), str(d / 'hv_win_pmu_rewrite.c'),
                         str(d / 'hv_win_pmu.c'), '-o', str(lg)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c3 = subprocess.run(['clang', *CFLAGS, '-iquote', str(legacy / 'shim'), '-iquote', str(d), str(here / 'test_sizing.c'),
                         str(d / 'hv_win_pmu.c'), str(d / 'hv_win_pmu_rewrite.c'), '-o', str(d / 'test_sizing')],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if c1.returncode or c2.returncode or c3.returncode:
        return False, False, (c1.stdout + c2.stdout + c3.stdout)[-400:].replace('\n', ' ')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    r0 = subprocess.run([str(d / 'test_sizing')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    if r0.returncode:
        lines = [l for l in r0.stdout.splitlines() if 'FAIL' in l or 'ERROR' in l] or r0.stdout.splitlines()[-1:]
        return True, True, 'sizing: ' + (lines[0] if lines else '')[:160]
    r1 = subprocess.run([str(t4)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
                        timeout=900)
    if r1.returncode:
        lines = [l for l in r1.stdout.splitlines() if 'FAIL' in l or 'ERROR' in l] or r1.stdout.splitlines()[-1:]
        return True, True, 'test_4k: ' + (lines[0] if lines else '')[:160]
    r2 = subprocess.run([str(lg)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
                        timeout=900)
    if r2.returncode:
        lines = [l for l in r2.stdout.splitlines() if 'FAIL' in l or 'ERROR' in l] or r2.stdout.splitlines()[-1:]
        return True, True, 'legacy: ' + (lines[0] if lines else '')[:160]
    return True, False, 'survived'


def one(m, root):
    name, file, old, new = m
    d = root / name
    d.mkdir()
    for f in FILES:
        shutil.copyfile(win / f, d / f)
    p = d / file
    text = p.read_text()
    if text.count(old) != 1:
        return name, False, False, f'anchor found {text.count(old)} times'
    p.write_text(text.replace(old, new))
    return (name, *build_and_run(d))


def main():
    root = Path(tempfile.mkdtemp(prefix='pmu4k-mutants-', dir=os.environ.get('FB_OUT') or None))
    base = root / 'baseline'
    base.mkdir()
    for f in FILES:
        shutil.copyfile(win / f, base / f)
    ok, failed, ev = build_and_run(base)
    if not ok or failed:
        sys.exit(f'baseline must compile and pass: {ev}')
    jobs = int(os.environ.get('FB_JOBS') or os.cpu_count() or 4)
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for name, compiled, killed, ev in ex.map(lambda m: one(m, root), M):
            status = 'KILLED' if killed else ('NOT COMPILED' if not compiled else 'SURVIVED')
            bad += not killed
            print(f'{name:26s} {status}  {ev}', flush=True)
    print(f'pmu-4k mutants: {len(M) - bad}/{len(M)} killed')
    shutil.rmtree(root, ignore_errors=True)  # a temporary directory this script created
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
