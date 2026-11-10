#!/usr/bin/env python3
"""Added r4.6 scenarios and targeted mutants; no inherited assertions removed."""
import importlib.util,json,os,subprocess,tempfile
from pathlib import Path
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('native',H/'run.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
root=Path(tempfile.mkdtemp(prefix='r46-'))
common=['r46_stage_gate','r46_ro_cap','r46_admin_bad_6','r46_admin_bad_128','r46_nested','r46_concurrent','r46_proxy','r46_ro_write']+[f'r46_context_{i}' for i in range(12)]+[f'r46_allowlist_{i}' for i in range(7)]
cases={s:common+(['r46_canary','r46_canary_save','r46_nested_canary','r46_canary_pattern_corrupt'] if s in [1,2] else []) for s in range(4)}
cases[0]+=['r46_stage0_flush_cap']
cases[3]+= [f'r46_raw_range_{i}' for i in range(4)]+['r46_split','r46_read_probe','r46_flush_error']+[f'r46_timeout_{i}' for i in range(3)]+[f'r46_write_refusal_{i}' for i in range(6)]+[f'r46_guest_op_{i}' for i in [4,8,9,128,129,132,255]]
A='s1/ans_ro.c';L='s1/native_link.c';Q='s1/hv_nvme_queue.c'
mutants=[
 ('public_buffer_guard_removed',2,['r46_nested_canary'],[(A,'    if (__atomic_load_n(&exec_busy, __ATOMIC_ACQUIRE)) {','    if (false) {')]),

 ('stage0_flush_cap_removed',0,['r46_stage0_flush_cap'],[(A,'st.cmd_flushes >= S1N_FLUSH_CAP','st.cmd_flushes > S1N_FLUSH_CAP')]),
 ('stage1_guest_writable',1,['r46_stage_gate'],[('s1/native_window.c','return S1N_STAGE_GUEST_WRITES && verified && rw;','return verified && rw;')]),
 ('stage1_canary_gate_removed',1,['r46_stage_gate'],[(A,'           && canary_running','           && true')]),

 ('exact_precheck_removed',3,['r46_raw_range_0'],[(A,'    if (!precheck(q, slot, tcb)) {','    if (false) {')]),
 ('exact_dma_guard_removed',3,['dma_guard_0'],[(A,'    if (!dma_guard(q, slot, tcb)) {','    if (false) {')]),
 ('admin_format_allowed',3,['r46_admin_bad_128'],[(A,'opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ;','opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ || opcode == 0x80;')]),
 ('admin_identify_allowed',3,['r46_admin_bad_6'],[(A,'return opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||','return opcode == OP_IDENTIFY || opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||')]),
 ('ro_cap_changed',3,['r46_ro_cap'],[('s1/native_window.c','    if (!mx) mx = want_rw ? 256 : 1;','    if (!mx) mx = 256;')]),

 ('exec_lock_removed',3,['r46_nested','r46_concurrent'],[(A,'if (__atomic_test_and_set(&exec_busy, __ATOMIC_ACQUIRE))','if (false)')]),
 ('context_cpu_removed',3,['r46_context_1','r46_proxy'],[(L,'s1n_test_cpu == 0 && !s1n_test_exception','!s1n_test_exception')]),
 ('context_exception_removed',3,['r46_context_0','r46_proxy'],[(L,'s1n_test_cpu == 0 && !s1n_test_exception','s1n_test_cpu == 0')]),
 ('exec_context_removed',3,['r46_context_10'],[(A,'    if (!s1n_storage_context()) {\n        freeze(ANS_RO_E_CONTEXT);\n        rc = -2;','    if (false) {\n        freeze(ANS_RO_E_CONTEXT);\n        rc = -2;')]),
 ('proxy_exception_removed',2,['r46_proxy'],[(L,'if (guest_exception || !s1n_storage_context())','if (!s1n_storage_context())')]),
 ('canary_start_removed',2,['r46_proxy'],[(L,'return s1n_storage_context() && !start_used;','return s1n_storage_context();')]),
 ('saved_read_compare_removed',2,['r46_canary_save'],[(A,'if (memcmp(canary_saved, dma.xfer, sizeof(canary_saved)))','if (false)')]),
 ('write_timeout_short',3,['r46_timeout_1'],[(A,'#define NVME_WRITE_TIMEOUT   30000000','#define NVME_WRITE_TIMEOUT   5000000')]),
 ('flush_timeout_short',3,['r46_timeout_0'],[(A,'#define NVME_FLUSH_TIMEOUT   30000000','#define NVME_FLUSH_TIMEOUT   10000000')]),
 ('read_freezes',3,['r46_read_probe'],[(Q,'        ctrl->refused_reads++;','        ctrl->refused_reads++;\n        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);')]),
 ('read_not_counted',3,['r46_read_probe'],[(Q,'        ctrl->refused_reads++;','        (void)0;')]),
 ('write_no_abort',3,['r46_write_refusal_0'],[(Q,'    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba) {\n        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);','    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba) {')]),
 ('write_split_removed',3,['r46_split'],[(Q,'        if (n > 32) n = 32;\n        if (!ctrl->ops->write_blocks','        if (!ctrl->ops->write_blocks')]),
 ('exact_bytes_unchecked',3,['dma_guard_0','r45_raw_ro'],[(A,'    if (!dma_guard(q, slot, tcb)) {','    if (false) {'),(A,'    if (!precheck(q, slot, tcb)) {','    if (false) {')]),
]
def run(out,c):
 x=subprocess.run([str(out/'test'),c],capture_output=True,text=True,timeout=180,env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
 return dict(case=c,rc=x.returncode,stderr=x.stderr[-1200:],stdout=x.stdout[-700:])
report={'stages':{},'mutants':[],'evidence_directory':str(root)}
for s in range(4):
 os.environ['S1N_STAGE']=str(s)
 try:
  out=r.build(root,'stage'+str(s));runs=[run(out,c) for c in cases[s]]
  report['stages'][s]={'runs':runs,'passed':sum(x['rc']==0 for x in runs),'total':len(runs)}
 except Exception as e: report['stages'][s]={'error':str(e)}
 (root/'progress.json').write_text(json.dumps(report,indent=2))
if not os.environ.get('R46_NO_MUTANTS'):
 for name,s,cs,edits in mutants:
  os.environ['S1N_STAGE']=str(s)
  try:
   out=r.build(root,name,edits);runs=[run(out,c) for c in cs]
   report['mutants'].append(dict(name=name,compiled=True,caught=any(x['rc'] for x in runs),runs=runs))
  except Exception as e: report['mutants'].append(dict(name=name,compiled=False,caught=False,error=str(e)))
  (root/'progress.json').write_text(json.dumps(report,indent=2))
report['status']='pass' if all(x.get('passed')==x.get('total') and 'error' not in x for x in report['stages'].values()) and all(x['caught'] for x in report['mutants']) else 'fail'
print(json.dumps(report,indent=2));raise SystemExit(report['status']!='pass')
