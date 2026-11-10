#!/usr/bin/env python3
"""Host-only actual-C PCI/NVMe suite, sanitized mutants and bound receipts."""
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
S0 = HERE.parent / 's0'
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='new private output directory')
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    args = parser.parse_args()
    os.umask(0o077)
    if args.out:
        out = args.out.resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix='m5w-storage-s1-'))
    receipt = {'schema': 1, 'status': 'fail', 'hardware_access': False,
               'physical_backend': False, 'started_unix': time.time(), 'commands': [],
               'manifest_sha256': sha(HERE/'MANIFEST.json'),
               'host': {'system': platform.system(), 'machine': platform.machine()},
               'output': str(out)}
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = 'abort_on_error=1:detect_leaks=' + ('0' if platform.system() == 'Darwin' else '1')
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    receipt['sanitizer_environment'] = {k:env[k] for k in ('ASAN_OPTIONS','UBSAN_OPTIONS')}
    def run(argv, name, timeout=60, reject=False):
        item = {'argv': list(map(str, argv)), 'timeout_seconds': timeout}
        receipt['commands'].append(item)
        result = subprocess.run(item['argv'], capture_output=True, text=True, timeout=timeout, env=env)
        (out/(name+'.stdout')).write_text(result.stdout)
        (out/(name+'.stderr')).write_text(result.stderr)
        item.update(returncode=result.returncode,stdout_sha256=sha(out/(name+'.stdout')),
                    stderr_sha256=sha(out/(name+'.stderr')))
        if reject:
            # A crash alone is not proof the intended contract assertion caught the mutation.
            if result.returncode != 1 or 'FAIL:' not in result.stderr:
                raise RuntimeError('mutation escaped expected assertion: '+name)
        elif result.returncode:
            raise RuntimeError(name+' failed: '+result.stderr[-2000:])
        return result.stdout
    try:
        manifest = json.loads((HERE/'MANIFEST.json').read_text())
        for name, expected in manifest['files'].items():
            if sha(HERE/name) != expected:
                raise RuntimeError('source hash mismatch: '+name)
        for name, expected in manifest['s0_dependencies'].items():
            if sha(S0/name) != expected:
                raise RuntimeError('S0 dependency mismatch: '+name)
        for item in manifest['upstream']:
            data = (HERE/item['local_path']).read_bytes()
            actual = hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
            if actual != item['git_blob']:
                raise RuntimeError('donor Git blob mismatch: '+item['local_path'])
        receipt['source_files'] = manifest['files']
        receipt['s0_dependencies'] = manifest['s0_dependencies']
        receipt['compiler'] = run([args.cc,'--version'],'compiler').splitlines()[0]
        flags = ['-std=c11','-DVNVME_HOST_TEST','-Wall','-Wextra','-Werror','-O1','-g',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                 '-I'+str(HERE/'src'),'-I'+str(S0/'src')]
        shared = [S0/'src'/n for n in ('hv_nvme_queue.c','ram_disk.c','guest_memory.c')]
        def build(src, binary, name):
            run([args.cc,*flags,src/'hv_pci.c',src/'hv_nvme.c',*shared,HERE/'test_s1.c','-o',binary],name)
        build(HERE/'src',out/'s1-test','build')
        symbols = run(['nm',out/'s1-test'],'symbols')
        names = {line.split()[-1].lstrip('_') for line in symbols.splitlines() if line.split()}
        forbidden = {'nvme_init','nvme_read','nvme_write','nvme_read_blocks','nvme_write_blocks',
                     'nvme_flush','sart_init','rtkit_init','hv_ipa_to_pa','hv_translate',
                     'hv_map_hook','hv_nvme_init_backend'}
        if forbidden & names:
            raise RuntimeError('physical/unchecked symbol linked: '+str(forbidden & names))
        result = json.loads(run([out/'s1-test'],'test',15))
        if result['status'] != 'pass' or result['physical_backend']:
            raise RuntimeError('unexpected suite result')
        mutants = [
            ('bar_bounds','hv_pci.c',[(
                'contains(m->window,m->window_size,addr,S1_BAR_SIZE)', 'true')]),
            ('bus_master','hv_nvme.c',[(
                '    else if (!(m->command&S1_BME)) reason=S1DG_REF_NO_BME;\n', ''),(
                '    if (!(m->command&S1_BME)) why|=S1DG_DB_NO_BME;\n', ''),(
                'bool ok=!why && (m->command&S1_BME) && ', 'bool ok=!why && ')]),
            ('interrupt_mask','hv_nvme.c',[(
                ' && !(m->regs.intms&1) && m->queue.irq_asserted;', ' && m->queue.irq_asserted;')]),
            ('doorbell_truncation','hv_nvme.c',[(
                '    if (value>UINT16_MAX) why|=S1DG_DB_VALUE;\n', ''),(
                ' && value<=UINT16_MAX && index', ' && index')]),
            ('reset_irq','hv_nvme.c',[(
                'm->regs.csts=0;m->regs.cc=value;m->regs.intms=0;\n        s1_recompute_irq(m);',
                'm->regs.csts=0;m->regs.cc=value;m->regs.intms=0;')]),
            # r2: shutdown must abort/discard outstanding work; abrupt must drop queues;
            # invalid CC at enable must surface CFS.
            ('shutdown_not_performed','hv_nvme.c',[(
                'vnvme_shutdown(&m->queue,shn==0x8000u);', '(void)shn;')]),
            ('abrupt_as_normal','hv_nvme.c',[(
                'vnvme_shutdown(&m->queue,shn==0x8000u);', 'vnvme_shutdown(&m->queue,false);')]),
            ('invalid_cc_accepted','hv_nvme.c',[(
                'if (!valid_cc(value)) { fatal(m,S1DG_FATAL_ENABLE_CC); return true; }',
                'if (((void)valid_cc,0)) { fatal(m,S1DG_FATAL_ENABLE_CC); return true; }')]),
        ]
        receipt['mutations'] = []
        for name, target, replacements in mutants:
            directory = out/('mutant-'+name)
            directory.mkdir()
            for filename in ('hv_pci.c','hv_nvme.c'):
                shutil.copyfile(HERE/'src'/filename,directory/filename)
            path = directory/target
            text = path.read_text()
            for before, after in replacements:
                if text.count(before) != 1:
                    raise RuntimeError('mutation no longer uniquely matches: '+name)
                text = text.replace(before,after)
            path.write_text(text)
            binary = directory/'test'
            build(directory,binary,'build-mutant-'+name)
            run([binary],'test-mutant-'+name,15,reject=True)
            receipt['mutations'].append({'name':name,'caught_by_assertion':True,
                                         'changed_source_sha256':sha(path)})
        receipt.update(status='pass',s1=result,binary_sha256=sha(out/'s1-test'),
                       forbidden_backend_symbols_absent=True)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['finished_unix'] = time.time()
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({'status':receipt['status'],'receipt':str(out/'receipt.json'),
                      's1':receipt.get('s1'),'error':receipt.get('error')},indent=2))
    return 0 if receipt['status']=='pass' else 1
if __name__ == '__main__':
    raise SystemExit(main())
