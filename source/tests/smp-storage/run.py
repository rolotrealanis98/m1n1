#!/usr/bin/env python3
"""smp-storage: multi-threaded stress of the REAL disk-path frontend (hv_win_storage.c: MMIO hooks + big storage
lock; hv_win_storage_mem.c: the lease in its SMP mode; the S1 model and PRP planner) against a flat fake disk,
under ThreadSanitizer and under AddressSanitizer+UBSan, plus mutants (each must make the stress fail).

  run.py            stress: {tsan, asan} x {2, 6, 18 vCPUs} x {main, latch, retire, retire_nested}
  run.py mutants    lock removed at each concurrent site, broken lock, SQE re-read after validation (TOCTOU),
                    CQE phase stored first
Env: SMPS_ITERS (commands per worker, default 1500), SMPS_JOBS (default 8), SMPS_SEED (default 11). Host only, no hardware.
"""
import json, os, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / 'src'
WIN = SRC / 'hv/win'
S1 = WIN / 's1'
S1F = ['hv_nvme.c', 'hv_pci.c', 'hv_nvme_queue.c', 'ram_disk.c', 'hv_nvme_queue.h', 'model.h', 'ram_disk.h']
ITERS = int(os.environ.get('SMPS_ITERS', '1500'))
JOBS = int(os.environ.get('SMPS_JOBS', '8'))
SEED = int(os.environ.get('SMPS_SEED', '11'))


def strip(text):
    return '\n'.join(l for l in text.splitlines() if not l.startswith('#include') and l != 'extern char _base[], _end[];')


def build(root, name, san, mutation=()):
    out = root / name
    (out / 's1').mkdir(parents=True)
    for n in S1F:
        shutil.copyfile(S1 / n, out / 's1' / n)
    for n in ['hv_win_storage.c', 'hv_win_storage.h', 'hv_win_storage_mem.c', 'hv_win_storage_mem.h']:
        shutil.copyfile(WIN / n, out / n)
    for f, old, new in mutation:
        p = out / f
        s = p.read_text()
        assert s.count(old) == 1, (name, f, s.count(old), old[:60])
        p.write_text(s.replace(old, new))
    test = ((HERE / 'stress_prelude.h').read_text() + '\n' + strip((out / 'hv_win_storage.c').read_text()) + '\n'
            + strip((out / 'hv_win_storage_mem.c').read_text()) + '\n' + (HERE / 'stress_body.c').read_text())
    (out / 'test.c').write_text(test)
    flags = {'tsan': ['-fsanitize=thread', '-O1'], 'asan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-O1']}[san]
    cmd = (['clang', '-std=gnu11', '-DVNVME_HOST_TEST', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-unused-function', '-g', '-pthread'] + flags
           + [f'-I{HERE / "shim"}', f'-I{out / "s1"}', f'-I{out}', str(out / 'test.c')]
           + [str(out / 's1' / n) for n in S1F if n.endswith('.c')] + ['-o', str(out / 'test')])
    b = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    (out / 'build.stderr').write_text(b.stderr)
    if b.returncode:
        raise RuntimeError(f'{name} build failed:\n{b.stderr[-3000:]}')
    return out / 'test'


ENV = dict(os.environ, TSAN_OPTIONS='halt_on_error=1:exitcode=66:second_deadlock_stack=1',
           ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')


def run(exe, nv, iters, seed, mode, timeout=900, env=None):
    r = subprocess.run([str(exe), str(nv), str(iters), str(seed), mode], capture_output=True, text=True,
                       timeout=timeout, env=dict(ENV, **(env or {})))
    res = {'nvcpus': nv, 'mode': mode, 'seed': seed, 'rc': r.returncode}
    if r.returncode == 0:
        res.update(json.loads(r.stdout.strip().splitlines()[-1]))
    else:
        err = r.stderr.strip().splitlines()
        res['detail'] = next((l for l in err if 'FAIL' in l or 'ThreadSanitizer' in l or 'ERROR' in l), (err or ['?'])[0])[:300]
    return res


L = 'hv_win_storage.c'
Q = 's1/hv_nvme_queue.c'
MUTANTS = [
    ('access_lock_removed', [(L, '''    hv_storage_lock_take();
    smps_enter(HV_SMPS_ACCESS); /* nightly-7 */
    u64 t0=SMPS_NOW();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);
    hv_storage_lock_release();''', '''    smps_enter(HV_SMPS_ACCESS); /* nightly-7 */
    u64 t0=SMPS_NOW();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);''')], 'main'),
    ('tick_lock_removed', [(L, '''    hv_storage_lock_take(); /* smp-storage */
    smps_enter(HV_SMPS_TICK); /* nightly-7 */
    u64 t0 = SMPS_NOW();
    s1_backend_poll();
    smps_hold(HV_SMPS_TICK, t0, 0);
    hv_storage_lock_release();''', '''    smps_enter(HV_SMPS_TICK); /* nightly-7 */
    u64 t0 = SMPS_NOW();
    s1_backend_poll();
    smps_hold(HV_SMPS_TICK, t0, 0);''')], 'main'),  # nightly-7: retargeted past the breadcrumbs (same intent)
    ('retire_lock_removed', [(L, '''    hv_storage_lock_take(); /* smp-storage */
    storage_retire_locked();
    hv_storage_lock_release();''', '''    storage_retire_locked();''')], 'retire'),
    ('lock_not_exclusive', [(L, '''        if (__atomic_compare_exchange_n(&storage_lock.owner, &none, me, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;''', '''        __atomic_store_n(&storage_lock.owner, me, __ATOMIC_RELAXED); (void)none;
        break;''')], 'main'),
    ('lock_release_early', [(L, '''    if (!--storage_lock.depth)
        __atomic_store_n(&storage_lock.owner, -1, __ATOMIC_RELEASE);''', '''    --storage_lock.depth;
    __atomic_store_n(&storage_lock.owner, -1, __ATOMIC_RELEASE);''')], 'retire_nested'),
    ('access_lock_and_lease_lock_check_removed', [(L, '''    hv_storage_lock_take();
    smps_enter(HV_SMPS_ACCESS); /* nightly-7 */
    u64 t0=SMPS_NOW();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);
    hv_storage_lock_release();''', '''    smps_enter(HV_SMPS_ACCESS); /* nightly-7 */
    u64 t0=SMPS_NOW();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);'''),
        ('hv_win_storage_mem.c', 'bhl.count > 0 && hv_storage_lock_held() &&', 'bhl.count > 0 &&')], 'main'),
    # nightly-7: a model callback outside the storage lock must show in the ops_unlocked breadcrumb
    ('n7_access_runs_after_release', [(L, '''    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);
    hv_storage_lock_release();''', '''    hv_storage_lock_release();
    bool ok=access_locked(ctx,address,value,write,width);
    smps_hold(HV_SMPS_ACCESS,t0,address);''')], 'main'),
    ('sqe_reread_after_validation', [
        (Q, 'static u16 process_write(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)\n{',
         'static const void *mut_live_sqe;\nstatic u16 process_write(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)\n{'),
        (Q, '''    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;
    u8 *buffer = data_buffer(ctrl, &capacity);
    static struct prp_plan plan;
    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))''', '''    if (mut_live_sqe) { const volatile struct vnvme_command *l = mut_live_sqe; lba = ((u64)l->cdw11 << 32) | l->cdw10; }
    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;
    u8 *buffer = data_buffer(ctrl, &capacity);
    static struct prp_plan plan;
    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))'''),
        (Q, '        memcpy(&cmd, sqe_src, sizeof(cmd));\n        sq->sq_head++;',
         '        memcpy(&cmd, sqe_src, sizeof(cmd));\n        mut_live_sqe = sqe_src;\n        sq->sq_head++;')], 'main'),
    ('cqe_phase_first', [(Q, '''    memcpy(cqe_dst, &cqe, 12);
    memcpy(&cqe_dw3, (const u8 *)&cqe + 12, sizeof(cqe_dw3));
    __atomic_store_n((u32 *)((u8 *)cqe_dst + 12), cqe_dw3, __ATOMIC_RELEASE);''', '''    memcpy(&cqe_dw3, (const u8 *)&cqe + 12, sizeof(cqe_dw3));
    __atomic_store_n((u32 *)((u8 *)cqe_dst + 12), cqe_dw3, __ATOMIC_RELAXED);
    for (volatile int spin = 0; spin < 200; spin++) {}
    memcpy(cqe_dst, &cqe, 12);''')], 'main'),
]


def main():
    root = Path(tempfile.mkdtemp(prefix='smp-storage-'))
    if len(sys.argv) > 1 and sys.argv[1] == 'mutants':
        results = []
        def one(m):
            name, mut, mode = m
            caught, runs = False, []
            for san in ('tsan', 'asan'):
                exe = build(root, f'{name}-{san}', san, mut)
                for seed in (1, 2, 3, 4, 5):
                    r = run(exe, 6, max(ITERS // 3, 300), seed, mode, timeout=600, env={'SMPS_HOSTILE': '50'})
                    runs.append(r)
                    if r['rc'] != 0:
                        caught = True
                        break
                if caught:
                    break
            return {'name': name, 'caught': caught, 'runs': runs}
        with ThreadPoolExecutor(max_workers=max(1, JOBS // 2)) as ex:
            results = list(ex.map(one, MUTANTS))
        killed = sum(r['caught'] for r in results)
        print(json.dumps({'mutants': results, 'killed': killed, 'total': len(results), 'dir': str(root)}, indent=1))
        print(f'smp-storage mutants: {killed}/{len(results)} killed')
        sys.exit(0 if killed == len(results) else 1)
    exes = {san: build(root, san, san) for san in ('tsan', 'asan')}
    jobs = [(san, nv, mode, seed) for san in ('tsan', 'asan') for nv in (2, 6, 18) for mode in ('main', 'latch', 'retire', 'retire_nested')
            for seed in ((SEED,) if mode != 'main' else (SEED, SEED + 1))]
    def go(j):
        san, nv, mode, seed = j
        r = run(exes[san], nv, ITERS if mode == 'main' else max(ITERS // 3, 200), seed, mode)
        r['san'] = san
        return r
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        results = list(ex.map(go, jobs))
    bad = [r for r in results if r['rc'] != 0]
    print(json.dumps({'runs': results, 'dir': str(root)}, indent=1))
    tot = lambda k, san: sum(r.get(k, 0) for r in results if r['san'] == san)
    for san in ('tsan', 'asan'):
        print(f'smp-storage {san}: {sum(r["san"] == san and r["rc"] == 0 for r in results)}/{sum(r["san"] == san for r in results)} runs pass, '
              f'{tot("completions", san)} completions, {tot("writes", san)} writes verified ({tot("words_exact", san)} words exact, '
              f'{tot("words_torn", san)} torn-but-valid, {tot("words_unbounded", san)} unbounded), {tot("hostile_writes", san) + tot("hostile_reads", san)} hostile')
    print('smp-storage stress: %s' % ('PASS' if not bad else '%d FAILED' % len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
