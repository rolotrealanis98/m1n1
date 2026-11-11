#!/usr/bin/env python3
"""Build/run two host-only actual-C suites; never opens a target/device/network."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, help='new output directory (must not exist)')
    ap.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    args = ap.parse_args()
    os.umask(0o077)
    if args.out:
        out = args.out.resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix='m5w-storage-s0-'))
    receipt = {'schema': 1, 'status': 'fail', 'hardware_access': False,
               'physical_backend': False, 'started_unix': time.time(),
               'host': {'system': platform.system(), 'machine': platform.machine()},
               'manifest_sha256': sha(HERE/'MANIFEST.json'), 'commands': [], 'output': str(out)}
    def run(argv, name, timeout=60):
        item = {'argv': list(map(str, argv)), 'timeout_seconds': timeout}
        receipt['commands'].append(item)
        env = dict(os.environ)
        env['ASAN_OPTIONS'] = 'abort_on_error=1:detect_leaks=' + ('0' if platform.system() == 'Darwin' else '1')
        item['sanitizer_environment'] = {'ASAN_OPTIONS': env['ASAN_OPTIONS'], 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        result = subprocess.run(item['argv'], capture_output=True, text=True, timeout=timeout, env=env)
        (out/(name+'.stdout')).write_text(result.stdout)
        (out/(name+'.stderr')).write_text(result.stderr)
        item.update(returncode=result.returncode, stdout_sha256=sha(out/(name+'.stdout')),
                    stderr_sha256=sha(out/(name+'.stderr')))
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}): {result.stderr[-2000:]}')
        return result.stdout
    try:
        manifest = json.loads((HERE/'MANIFEST.json').read_text())
        for name, expected in manifest['files'].items():
            if sha(HERE/name) != expected:
                raise RuntimeError('source hash mismatch: '+name)
        for item in manifest['upstream']:
            data=(HERE/item['local_path']).read_bytes()
            blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
            if blob != item['git_blob']:
                raise RuntimeError('upstream Git blob mismatch: '+item['local_path'])
        receipt['source_files'] = manifest['files']
        receipt['compiler'] = run([args.cc,'--version'],'compiler').splitlines()[0]
        flags=['-std=c11','-DVNVME_HOST_TEST','-Wall','-Wextra','-Werror','-O1','-g',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        # Preserve the pristine donor test's original ../src include layout.
        donor=out/'donor'
        (donor/'src').mkdir(parents=True)
        (donor/'tests').mkdir()
        for name in ['hv_nvme_queue.c','hv_nvme_queue.h']:
            shutil.copyfile(HERE/'upstream'/name,donor/'src'/name)
        shutil.copyfile(HERE/'upstream/hv_nvme_queue_test.c',donor/'tests/hv_nvme_queue_test.c')
        run([args.cc,*flags,donor/'src/hv_nvme_queue.c',donor/'tests/hv_nvme_queue_test.c',
             '-o',out/'donor-test'],'build-donor')
        donor_result=run([out/'donor-test'],'test-donor',15)
        if donor_result.strip() != 'hv_nvme_queue_test: ok':
            raise RuntimeError('unexpected donor suite result')
        sources=[HERE/'src'/s for s in ['hv_nvme_queue.c','guest_memory.c','ram_disk.c']]
        run([args.cc,*flags,'-I'+str(HERE/'src'),*sources,HERE/'test_s0.c','-o',out/'s0-test'],'build-s0')
        symbols=run(['nm',out/'s0-test'],'symbols')
        forbidden={'nvme_init','nvme_read','nvme_write','nvme_flush','sart_init','rtkit_init','hv_ipa_to_pa'}
        names={line.split()[-1].lstrip('_') for line in symbols.splitlines() if line.split()}
        if forbidden & names:
            raise RuntimeError('physical/unchecked backend linked: '+str(forbidden & names))
        result=json.loads(run([out/'s0-test'],'test-s0',15))
        if result['status']!='pass' or result['backend_write_calls'] or result['backend_flush_calls'] or result['physical_backend']:
            raise RuntimeError('unexpected S0 receipt')
        receipt.update(status='pass', donor_suite='pass', s0=result,
                       binary_sha256={'donor':sha(out/'donor-test'),'s0':sha(out/'s0-test')},
                       forbidden_backend_symbols_absent=True)
    except Exception as error:
        receipt['error']=str(error)
    finally:
        receipt['finished_unix']=time.time()
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({'status':receipt['status'],'receipt':str(out/'receipt.json'),
                      's0':receipt.get('s0'),'error':receipt.get('error')},indent=2))
    return 0 if receipt['status']=='pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
