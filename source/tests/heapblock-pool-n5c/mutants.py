#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""n5c heapblock pool mutants: edit a copy of src/heapblock.c, rebuild test_pool, require a run-time failure."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile

here = Path(__file__).resolve().parent
src = here.parent.parent / 'src'
M = [
    ('no_pool_fallback', 'if (size && pool_end && pb >= pool_cursor', 'if (0 && size && pool_end && pb >= pool_cursor'),
    ('zero_size_to_pool', 'if (size && pool_end && pb', 'if (pool_end && pb'),
    ('pool_overflow', 'size <= pool_end - pb) {', 'size <= pool_end - pb + 0x1000) {'),
    ('add_twice', 'if (pool_end || !heap_limit ||', 'if (!heap_limit ||'),
    ('add_without_limit', 'if (pool_end || !heap_limit ||', 'if (pool_end ||'),
    ('add_misaligned', '((b | size) & 0x3fff) ||', '0 ||'),
    ('add_over_heap', '(b < hi && lo < e)', '(b < hi && lo < e && 0)'),
    ('total_primary_only', 'return r + (pool_end - pool_cursor);', 'return r;'),
    ('owned_whole_pool', 'pa <= pool_cursor && len <= pool_cursor - pa', 'pa <= pool_end && len <= pool_end - pa'),
    ('remaining_includes_pool', '''               ? (uintptr_t)heap_limit - (uintptr_t)heap_base
               : 0;''', '''               ? (uintptr_t)heap_limit - (uintptr_t)heap_base + (pool_end - pool_cursor)
               : 0;'''),
]


def run(d):
    out = d / 'build'
    out.mkdir()
    shutil.copyfile(d / 'heapblock.c', out / 'heapblock.c')
    shutil.copyfile(src / 'heapblock.h', out / 'heapblock.h')
    exe = out / 'test_pool'
    c = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-iquote', str(here / 'shim'), '-iquote', str(here.parent / 'win-pmu-rewrite/shim'),
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-iquote', str(out),
                        str(here / 'test_pool.c'), str(out / 'heapblock.c'), '-o', str(exe)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if c.returncode:
        return False, False, c.stdout[-200:]
    r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'), timeout=120)
    return True, r.returncode != 0, ([l for l in r.stdout.splitlines() if 'FAIL' in l or 'ERROR' in l] or [''])[0]


def main():
    root = Path(tempfile.mkdtemp(prefix='hbpool-mutants-', dir=os.environ.get('FB_OUT') or None))
    text = (src / 'heapblock.c').read_text()
    b = root / 'baseline'
    b.mkdir()
    (b / 'heapblock.c').write_text(text)
    ok, failed, ev = run(b)
    if not ok or failed:
        sys.exit('baseline must pass: ' + ev)
    bad = 0
    for name, old, new in M:
        d = root / name
        d.mkdir()
        if text.count(old) != 1:
            print(f'{name:24s} ANCHOR x{text.count(old)}')
            bad += 1
            continue
        (d / 'heapblock.c').write_text(text.replace(old, new))
        compiled, killed, ev = run(d)
        bad += not killed
        print(f'{name:24s} {"KILLED" if killed else ("NOT COMPILED" if not compiled else "SURVIVED")}  {ev}')
    print(f'heapblock-pool-n5c mutants: {len(M) - bad}/{len(M)} killed')
    shutil.rmtree(root, ignore_errors=True)  # a temporary directory this script created
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
