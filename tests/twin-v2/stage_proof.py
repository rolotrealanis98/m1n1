#!/usr/bin/env python3
"""Stage-specific twin-v2: full canary restoration and falsifiable sub-window bounds."""
import importlib.util,json,os,subprocess,tempfile,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('twin',HERE/'twin_proof.py');t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
wd=Path(tempfile.mkdtemp(prefix='r45-stage-twin-'));results=[];builds={}
try:
 for stage in (0,1,2):
  os.environ['TWIN_STAGE']=str(stage)
  builds[stage]=t.build(wd,f'stage{stage}',[])
 tmpl=wd/'template.img';env=dict(os.environ,S1TWIN_DISK=str(tmpl),ASAN_OPTIONS='detect_leaks=0')
 x=subprocess.run([str(builds[0]),'twin_mkimg'],capture_output=True,text=True,env=env,timeout=300);assert x.returncode==0,x.stderr
 t.write_backup_gpt(tmpl);pins=wd/'pins.json';t.make_pins(tmpl,pins)
 before=wd/'before.json';t.verifier('hash',str(tmpl),'--image','--sparse','--pins',str(pins),'-o',str(before),'--jobs','8')
 bound='lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)'
 for stage in (0,1,2):
  zone=t.W_START+4096;blocks=512 if stage==1 else 16384
  variants=[('baseline',builds[stage],list(range(-1,2)) if stage else [-1,0])]
  if stage:
   os.environ['TWIN_STAGE']=str(stage)
   for which in (0,1):
    bad=('lba + 1 >= S1N_SUB_START && lba + 1 - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba + 1 - S1N_SUB_START)' if which==0 else
         'lba >= S1N_SUB_START && lba - S1N_SUB_START <= S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START) + 1')
    binary=t.build(wd,f'stage{stage}-edge{which}',[('s1/ans_ro.c',bound,bad)])
    variants.append((f'broken-edge-{which}',binary,[which]))
  for variant,binary,cases in variants:
   for case in cases:
    tag=f'stage{stage}-{variant}-case{case}';img=wd/(tag+'.img');wlog=wd/(tag+'.wlog');after=wd/(tag+'.after.json')
    subprocess.run(['cp','-c',str(tmpl),str(img)],check=True)
    env=dict(os.environ,S1TWIN_DISK=str(img),S1TWIN_WLOG=str(wlog),S1TWIN_CASE=str(case),ASAN_OPTIONS='detect_leaks=0')
    x=subprocess.run([str(binary),'twin_proof'],capture_output=True,text=True,env=env,timeout=300)
    assert x.returncode==0,x.stderr
    rec=json.loads([l for l in x.stdout.splitlines() if l.startswith('{"stage"')][0])
    log=[json.loads(l) for l in wlog.read_text().splitlines()]
    writes=[e for e in log if e['q']=='io' and e['op']==1]
    violations=[e for e in writes if stage==0 or e['slba']<zone or e['slba']>=zone+blocks or e['nlb']>zone+blocks-e['slba'] or (stage==1 and e['nlb']!=1)]
    t.verifier('hash',str(img),'--image','--sparse','--pins',str(pins),'-o',str(after),'--jobs','8')
    _,so=t.verifier('compare',str(before),str(after),'--max-list','8');cmp=json.loads(so)
    restored=True
    if stage and case==-1:
     with open(img,'rb') as f,open(tmpl,'rb') as g:
      f.seek(zone*4096);g.seek(zone*4096)
      for _ in range(blocks//128): restored &= f.read(128*4096)==g.read(128*4096)
    counts=(rec['writes']==(0 if stage==0 else 1024 if stage==1 else 256) and rec['blocks']==(0 if stage==0 else 1024 if stage==1 else 32768)) if case==-1 else not rec['writes']
    positive=(bool(rec['operation_ok']) if case==-1 else not rec['operation_ok']) and counts and restored
    ok=cmp['verdict']=='PASS' and not violations and not rec['outside'] and not rec['file_errors'] and positive
    result={'stage':stage,'variant':variant,'case':case,'verdict':'PASS' if ok else 'FAIL','D1_outside':cmp['verdict'],'D2_stage_violations':len(violations),'D3_outside':rec['outside'],'restored':restored,'receipt':rec}
    results.append(result);(wd/(tag+'.json')).write_text(json.dumps(result,indent=2));img.unlink();after.unlink()
    print(tag,result['verdict'],file=sys.stderr,flush=True)
finally:
 for p in wd.glob('*.img'):p.unlink()
 for p in wd.glob('*.after.json'):p.unlink()
 if (wd/'before.json').exists():(wd/'before.json').unlink()
ok=len(results)==12 and all(r['verdict']==('PASS' if r['variant']=='baseline' else 'FAIL') for r in results)
print(json.dumps({'status':'pass' if ok else 'fail','results':results,'evidence_directory':str(wd),'scope':'host model using actual driver; test-only entry simulates canary origin for adversarial zone writes'},indent=2))
sys.exit(0 if ok else 1)
