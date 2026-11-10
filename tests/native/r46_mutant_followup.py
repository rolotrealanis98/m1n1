#!/usr/bin/env python3
"""Additional passing tests for inherited mutants obscured by policy-incompatible baselines."""
from pathlib import Path
import importlib.util,tempfile,os,subprocess,json
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('native',H/'run.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
ns={'__file__':str(H/'r45_mutants.py'),'__name__':'r45_import'}
exec((H/'r45_mutants.py').read_text().split('results=[]')[0],ns)
cs={'stage0_flush_cap_removed':['r46_stage0_flush_cap'],'stage1_guest_writable':['r46_stage_gate'],'stage1_canary_access_unrestricted':['r46_stage_gate'],'canary_compare_removed':['r46_canary_pattern_corrupt'],'canary_restore_replaced':['r46_canary'],'write_cap_removed':['r46_canary']}
mut=[(n,s,cs[n],e) for n,s,c,e in ns['mutants'] if n in cs]
cs={'ans_allow_format':['r46_admin_bad_128'],'r43_allow_identify':['r46_admin_bad_6'],'r4_ro_default_large_cmds':['r46_ro_cap']}
mut += [(m[0],3,cs[m[0]],m[1] if len(m)==2 else tuple(m[1:])) for m in r.MUTANTS if m[0] in cs]
root=Path(tempfile.mkdtemp(prefix='r46-followup-'));results=[]
for n,s,cases,edits in mut:
 os.environ['S1N_STAGE']=str(s)
 baseline=r.build(root,n+'-baseline');out=r.build(root,n,edits);runs=[]
 for case in cases:
  b=subprocess.run([str(baseline/'test'),case],capture_output=True,text=True,timeout=180)
  x=subprocess.run([str(out/'test'),case],capture_output=True,text=True,timeout=180)
  runs.append(dict(case=case,baseline_rc=b.returncode,mutant_rc=x.returncode,stderr=x.stderr[-600:]))
 results.append(dict(name=n,caught=any(x['baseline_rc']==0 and x['mutant_rc']!=0 for x in runs),runs=runs))
 (root/'progress.json').write_text(json.dumps(results,indent=2))
print(json.dumps(dict(mutants=results,caught=sum(x['caught'] for x in results),total=len(results),evidence_directory=str(root)),indent=2))
