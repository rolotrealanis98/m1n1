#!/usr/bin/env python3
"""Run inherited scenarios and mutants without changing their assertions.
A mutant is caught only by a test that PASSED against unmutated r4.6.
"""
import importlib.util,json,os,subprocess,tempfile,concurrent.futures
from pathlib import Path
H=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('native',H/'run.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
root=Path(tempfile.mkdtemp(prefix='r46-inherited-'))
report={'stages':{},'native_mutants':[],'r45_mutants':[],'output_directory':str(root)}
# Preserve original scenario selection per stage (module import reads S1N_STAGE).
for stage in range(4):
 os.environ['S1N_STAGE']=str(stage);spec.loader.exec_module(r)
 out=r.build(root,'stage'+str(stage));rows,checks=r.run_all(out)
 report['stages'][stage]={'scenarios':len(rows),'passed':sum(x['ok'] for x in rows),'checks':checks,'results':rows}
 (root/'progress.json').write_text(json.dumps(report,indent=2))
os.environ['S1N_STAGE']='3';spec.loader.exec_module(r)
# New additive scenarios supply coverage when an old assertion specifies the former policy.
extras=['r46_nested','r46_concurrent','r46_read_probe','r46_split','r46_ro_write']+[f'r46_allowlist_{i}' for i in range(7)]
r.SCENARIOS+=extras
base,_=r.run_all(root/'stage3');passing={x['scenario'] for x in base if x['ok']}
def native(m):
 name,mut=m[0],m[1] if len(m)==2 else tuple(m[1:])
 try:
  out=r.build(root,'mut_'+name,mut);rows,_=r.run_all(out)
  kills=[x['scenario'] for x in rows if not x['ok'] and x['scenario'] in passing]
  return dict(name=name,compiled=True,caught=bool(kills),by=kills,all_failures=[x for x in rows if not x['ok']])
 except Exception as e:return dict(name=name,compiled=False,caught=False,error=str(e))
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
 for result in pool.map(native,r.MUTANTS):
  report['native_mutants'].append(result);(root/'progress.json').write_text(json.dumps(report,indent=2))
# Evaluate original r45 targeted mutants. Do not count pre-existing baseline failures as kills.
ns={'__file__':str(H/'r45_mutants.py'),'__name__':'r45_import'}
exec((H/'r45_mutants.py').read_text().split('results=[]')[0],ns)
for name,stage,cases,edits in ns['mutants']:
 os.environ['S1N_STAGE']=str(stage)
 item=dict(name=name,stage=stage,caught=False)
 try:
  out=r.build(root,'r45_'+name,edits);rows=[]
  for case in cases:
   b=subprocess.run([str(root/('stage'+str(stage))/'test'),case],capture_output=True,text=True,timeout=180)
   x=subprocess.run([str(out/'test'),case],capture_output=True,text=True,timeout=180)
   rows.append(dict(case=case,baseline_rc=b.returncode,mutant_rc=x.returncode,stderr=x.stderr[-800:]))
  item.update(compiled=True,runs=rows,caught=any(x['baseline_rc']==0 and x['mutant_rc']!=0 for x in rows))
 except Exception as e:item.update(compiled=False,error=str(e))
 report['r45_mutants'].append(item);(root/'progress.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
