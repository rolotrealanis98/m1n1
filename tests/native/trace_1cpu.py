#!/usr/bin/env python3
"""smp-storage: 1-vCPU behaviour equivalence by trace comparison. Builds the native end-to-end harness (real
storage stack + fake ANS, run.py's build) twice, from this tree and from a base revision (default nightly-4b
7e803d6, exported with git archive), runs every stage-3 scenario of run.py on both with TRACE_1CPU set, and
compares, per scenario: exit code, stdout, and the trace (every MMIO/config hook call: address, direction, width,
value in/out, result, guest-memory hash after each write; then the final controller/S1DG/canary/queue/PRP/fake-ANS
state). Any difference fails. Usage: trace_1cpu.py [BASE_REV]"""
import json, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
BASE = sys.argv[1] if len(sys.argv) > 1 else '7e803d6'
GIT = ['git', '-C', str(TREE)]


def build_in(root, out_root, name):
    code = (f"import sys,json;sys.argv=['run.py'];import importlib.util as u;"
            f"s=u.spec_from_file_location('r',{str(HERE / 'run.py')!r});m=u.module_from_spec(s);s.loader.exec_module(m);"
            f"from pathlib import Path;o=m.build(Path({str(out_root)!r}),{name!r});print(json.dumps({{'exe':str(o/'test'),'scen':m.SCENARIOS}}))")
    r = subprocess.run([sys.executable, '-I', '-c', code], env=dict(os.environ, S1_SOURCE_ROOT=str(root), S1N_STAGE='3'),
                       capture_output=True, text=True, timeout=600)
    if r.returncode:
        raise SystemExit(f'build {name} failed: {r.stderr[-2000:]}')
    return json.loads(r.stdout.strip().splitlines()[-1])


def main():
    tmp = Path(tempfile.mkdtemp(prefix='trace-1cpu-'))
    base = tmp / 'base'
    base.mkdir()
    a = subprocess.run(GIT + ['archive', BASE, 'source'], capture_output=True, check=True)
    subprocess.run(['tar', '-x', '-C', str(base)], input=a.stdout, check=True)
    new = build_in(TREE, tmp, 'new')
    old = build_in(base, tmp, 'old')
    scen = new['scen']
    assert scen == old['scen']
    env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')

    def one(s):
        res = []
        for tag, exe in (('old', old['exe']), ('new', new['exe'])):
            tf = tmp / f'{tag}-{s}.trace'
            r = subprocess.run([exe, s], capture_output=True, text=True, timeout=300, env=dict(env, TRACE_1CPU=str(tf)))
            res.append((r.returncode, r.stdout, tf.read_text() if tf.exists() else ''))
        (o_rc, o_out, o_tr), (n_rc, n_out, n_tr) = res
        same = o_rc == n_rc and o_out == n_out and o_tr == n_tr
        return {'scenario': s, 'same': same, 'rc': n_rc, 'hook_calls': n_tr.count('\n') - (1 if 'END ' in n_tr else 0),
                'traced': bool(n_tr), 'detail': '' if same else f'rc {o_rc}/{n_rc}, stdout {o_out == n_out}, trace {o_tr == n_tr}'}

    with ThreadPoolExecutor(max_workers=int(os.environ.get('FB_JOBS', '12'))) as ex:
        results = list(ex.map(one, scen))
    diff = [r for r in results if not r['same']]
    calls = sum(r['hook_calls'] for r in results)
    print(json.dumps({'base': BASE, 'scenarios': len(results), 'traced': sum(r['traced'] for r in results),
                      'hook_calls_compared': calls, 'differences': diff, 'dir': str(tmp)}, indent=1))
    print(f'trace-1cpu: {len(results) - len(diff)}/{len(results)} scenarios identical to {BASE} ({calls} hook calls compared)')
    sys.exit(1 if diff else 0)


if __name__ == '__main__':
    main()
