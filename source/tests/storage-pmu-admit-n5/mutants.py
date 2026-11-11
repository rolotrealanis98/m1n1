#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""nightly-5 storage admission mutants: each edits a copy of hv_win_storage_mem.c (never the tree),
rebuilds test_admit with the real hv_win_pmu.c and must fail at run time. 'nightly4_admission' is
the pre-nightly-5 file's rule (SCANNING refused): the suite must reject it."""
from pathlib import Path
import concurrent.futures, os, shutil, subprocess, sys, tempfile

here = Path(__file__).resolve().parent
win = here.parent.parent / 'src/hv/win'
STORAGE = 'hv_win_storage_mem.c'

M = [
    ('nightly4_admission', '((d & tracked) && leaf.size != HV_SM_PAGE) ||',
     '((d & tracked) && (((d & xn) && !(d & s2ap_w)) || leaf.size != HV_SM_PAGE)) ||'),
    ('admit_untracked_ro', 'u64 g = (d & tracked) ? d | s2ap_w : d;', 'u64 g = d | s2ap_w;'),
    ('drop_tracked_size', '((d & tracked) && leaf.size != HV_SM_PAGE) ||', ''),
    ('tracked_skips_attrs', '(g & ~(address | tracked | flips | xn | BIT(1))) != expected ||',
     '((g & ~(address | tracked | flips | xn | BIT(1))) != expected && !(d & tracked)) ||'),
    ('untracked_flips_ok', '(!(d & tracked) && (d & flips)))', '(false && (d & flips)))'),
    ('no_rearm', '    if (write)\n        hv_win_pmu_dma_write(ipa, length);\n    return (void *)ipa;',
     '    return (void *)ipa;'),
    ('rearm_on_read', '    if (write)\n        hv_win_pmu_dma_write(ipa, length);\n    return (void *)ipa;',
     '    hv_win_pmu_dma_write(ipa, length);\n    return (void *)ipa;'),
    ('rearm_before_checks', '    u64 end = ipa + length;\n',
     '    u64 end = ipa + length;\n    if (write)\n        hv_win_pmu_dma_write(ipa, length);\n'),
    ('rearm_first_leaf_only', '        hv_win_pmu_dma_write(ipa, length);\n    return (void *)ipa;',
     '        hv_win_pmu_dma_write(ipa, 1);\n    return (void *)ipa;'),
    ('skip_protected', '            return sm_refuse(HV_SM_WHY_PROTECTED, ipa, length, m->protected[i].base);',
     '            (void)0;'),
    ('xn_ignored_untracked', 'const u64 tracked = BIT(55), flips = GENMASK(58, 56), xn = BIT(54);',
     'const u64 tracked = BIT(55), flips = GENMASK(58, 56), xn = BIT(54) | BIT(7) | BIT(6);'),
]

CFLAGS = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          '-fno-strict-aliasing', '-DPMU_REWRITE_HOST_TEST', '-fsanitize=address,undefined',
          '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']


def run(d):
    exe = d / 'test_admit'
    c = subprocess.run(['clang', *CFLAGS, '-iquote', str(here / 'shim'), '-iquote', str(here.parent / 'win-pmu-rewrite/shim'),
                        '-iquote', str(d), '-iquote', str(win), str(here / 'test_admit.c'), str(d / STORAGE),
                        str(win / 'hv_win_pmu.c'), str(win / 'hv_win_pmu_rewrite.c'), '-o', str(exe)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if c.returncode:
        return False, False, c.stdout[-300:].replace('\n', ' ')
    r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'), timeout=300)
    lines = [l for l in r.stdout.splitlines() if 'FAIL' in l or 'ERROR' in l] or r.stdout.splitlines()[-1:]
    return True, r.returncode != 0, (lines[0] if lines else '')[:160]


def one(m, root):
    name, old, new = m
    d = root / name
    d.mkdir()
    text = (win / STORAGE).read_text()
    if text.count(old) != 1:
        return name, False, False, f'anchor found {text.count(old)} times'
    (d / STORAGE).write_text(text.replace(old, new))
    return (name, *run(d))


def main():
    root = Path(tempfile.mkdtemp(prefix='admit-mutants-', dir=os.environ.get('FB_OUT') or None))
    base = root / 'baseline'
    base.mkdir()
    shutil.copyfile(win / STORAGE, base / STORAGE)
    ok, failed, ev = run(base)
    if not ok or failed:
        sys.exit(f'baseline must compile and pass: {ev}')
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get('FB_JOBS') or 8)) as ex:
        for name, compiled, killed, ev in ex.map(lambda m: one(m, root), M):
            bad += not killed
            print(f'{name:24s} {"KILLED" if killed else ("NOT COMPILED" if not compiled else "SURVIVED")}  {ev}',
                  flush=True)
    print(f'storage-admit-n5 mutants: {len(M) - bad}/{len(M)} killed')
    shutil.rmtree(root, ignore_errors=True)  # a temporary directory this script created
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
