#!/usr/bin/env python3
"""Stage-0 host compiler evidence. This does not claim a resident ELF was built."""
import json,os,re,subprocess,tempfile,hashlib
from pathlib import Path
R=Path(__file__).resolve().parents[2];S=R/'source/src/hv/win/s1';out=Path(tempfile.mkdtemp(prefix='r45-stage-inspect-'))
base=['clang','-std=gnu11','-DANS_RO_HOST_TEST',f'-I{R/"tests/native/shim"}',f'-I{R/"tests/native"}',f'-I{R/"source/src"}',f'-I{S}']
results={}
for stage in range(4):
 pp=subprocess.run(base+[f'-DS1N_STAGE={stage}','-E','-P',str(S/'ans_ro.c')],capture_output=True,text=True,check=True).stdout
 (out/f'stage{stage}.i').write_text(pp)
 ir=out/f'stage{stage}.ll';subprocess.run(base+[f'-DS1N_STAGE={stage}','-O0','-S','-emit-llvm',str(S/'ans_ro.c'),'-o',str(ir)],check=True,capture_output=True)
 obj=out/f'stage{stage}.o';subprocess.run(base+[f'-DS1N_STAGE={stage}','-O0','-c',str(S/'ans_ro.c'),'-o',str(obj)],check=True,capture_output=True)
 nm=subprocess.run(['nm',str(obj)],check=True,capture_output=True,text=True).stdout
 (out/f'stage{stage}.nm').write_text(nm)
 if stage==0:
  allowed=pp[pp.index('static _Bool allowed('):pp.index('static _Bool in_window(')]
  # Opcode 1 remains legitimate admin CREATE_SQ. It must not be allowed in the I/O return.
  io=allowed[allowed.index('return opcode == 0x02'):]
  assert '0x01' not in io,io
  # r4.6 re-anchor: command construction moved from ans_ro_xfer (now a guard wrapper) to xfer_owned
  xfer=ir.read_text().split('define internal zeroext i1 @xfer_owned(',1)[1].split('\n}',1)[0]
  assert 'store i8 2,' in xfer and 'store i8 1,' not in xfer
  write_range=ir.read_text().split('define internal zeroext i1 @write_range(',1)[1].split('\n}',1)[0]
  assert 'ret i1 false' in write_range
  assert 'canary_saved' not in nm and 'canary_pattern' not in nm and 'recheck_saved' not in nm
 results[stage]={'host_object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),'stage_marker':f'M5W-S1N-R4.5-STAGE-{stage}','scope':'host object, not resident binary'}
for stage in (None,'-1','4'):
 cmd=base+([] if stage is None else ['-DS1N_STAGE='+stage])+['-fsyntax-only',str(S/'ans_ro.c')]
 x=subprocess.run(cmd,capture_output=True,text=True);assert x.returncode!=0
 (out/f'rejected-stage-{stage}.stderr').write_text(x.stderr)
print(json.dumps({'status':'pass','stage0':'host IR: READ opcode 2 only in xfer, write_range returns false; no I/O WRITE allowlist or canary buffers','invalid_stage_compiles_rejected':3,'stages':results,'evidence_directory':str(out),'resident_ELF_inspection':'unverified: not built'},indent=2))
