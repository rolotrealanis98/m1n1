#!/usr/bin/env python3
"""Run every overlay mutant against r4.6, including independent-backup fuzz.
Only regressions from the r4.6 baseline count as kills; existing failures stay reported.
"""
from pathlib import Path
import json,os,tempfile,shutil,subprocess,concurrent.futures
R=Path(__file__).resolve().parents[2]
O=Path('/Users/rolo/Work/agents/win-el2/storage/s1-resident-r4.5-tests-fix-20261110/tools/mutants_before_after.py')
s=O.read_text();a=s.index('N = "hv/win');b=s.index('\ndef tree(');ns={};exec(s[a:b],ns)
root=Path(tempfile.mkdtemp(prefix='r46-overlay-'))
def run(item):
 name,edits=item;t=root/name
 for f in ['source/src','tests/native','review-tests-r3']:shutil.copytree(R/f,t/f,symlinks=True,ignore=shutil.ignore_patterns('__pycache__','last.json','run.out'))
 for f,a,b in edits:
  p=t/'source/src'/f;s=p.read_text();assert s.count(a)==1;(p).write_text(s.replace(a,b))
 (t/'scratch').mkdir();env=dict(os.environ,TMPDIR=str(t/'scratch'),PYTHONDONTWRITEBYTECODE='1',S1_NO_MUTANTS='1',S1N_STAGE='3')
 n=subprocess.run(['python3','-I',str(t/'tests/native/run.py')],capture_output=True,text=True,env=env,timeout=1800)
 (t/'native.json').write_text(n.stdout+n.stderr)
 try:d=json.loads(n.stdout);nf=[x['scenario'] for x in d['results'] if not x['ok']]
 except Exception:nf=['BUILD/PARSE ERROR']
 v=subprocess.run(['python3','-I',str(t/'review-tests-r3/run.py'),'60000'],capture_output=True,text=True,env=env,cwd=t/'review-tests-r3',timeout=1800)
 (t/'review.log').write_text(v.stdout+v.stderr)
 try:rd=json.loads(v.stdout.strip().splitlines()[-1]);rf=[k for k,val in rd['results'].items() if not val]
 except Exception:rf=['BUILD/PARSE ERROR']
 return dict(name=name,native_rc=n.returncode,native_failures=nf,review_rc=v.returncode,review_failures=rf,evidence_directory=str(t))
results=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
 for x in pool.map(run,ns['MUTANTS'].items()):
  results.append(x);(root/'progress.json').write_text(json.dumps(results,indent=2))
b=next(x for x in results if x['name']=='none')
for x in results:
 x['new_failures']=sorted(set(x['native_failures'])-set(b['native_failures']))+sorted(set(x['review_failures'])-set(b['review_failures']))
 x['caught']=x['name']!='none' and bool(x['new_failures']) and 'BUILD/PARSE ERROR' not in x['new_failures']
print(json.dumps(dict(mutants=results,caught=sum(x['caught'] for x in results),total=len(results)-1,evidence_directory=str(root)),indent=2))
