#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutate copies of production sources, rebuild with ASan/UBSan, require runtime failure.
No compile-error kills. Neither the live source nor an existing test is modified.
"""
from pathlib import Path
import json, os, shutil, subprocess, sys
here=Path(__file__).resolve().parent
source=here.parent.parent
out=Path(os.environ.get('FB_OUT',str(here/'build'/'mutants'))).resolve()
out.mkdir(parents=True,exist_ok=True)
win=source/'src/hv/win'
mutants=[
 ('ignore_endian','hv_win_pmu_4k.h','s->sctlr & (BIT(25) | BIT(24))','false'),
 ('ignore_hpd','hv_win_pmu_4k.h','(0x3ffUL << 41)','0'),
 ('ignore_dbm','hv_win_pmu_4k.h','(s->tcr & BIT(40)) && (d & BIT(51))','false'),
 ('ignore_fault_class','hv_win_pmu.c','(ctx->esr >> 26) != 0x20','false'),
 ('guess_after_at_failure','hv_win_pmu.c',
  '/* HPFAR is architecturally UNKNOWN here: no speculative scan fallback. */\n        return pmu_unproductive(ctx);',
  'if (!hv_xt_fault_ipa(ctx, true, false, &ipa)) return false;\n        ipa = ctx->far - 0x300000UL;'),
 ('scan_siblings' ,'hv_win_pmu_rewrite.c','i < 1024','i < 4096'),
 ('wrong_subframe','hv_win_pmu.c','pa += page - block;','pa += 0;'),
 ('no_exec_proof','hv_win_pmu.c','if (!pmu_exec_proof(ctx, ipa))','if (false && !pmu_exec_proof(ctx, ipa))'),
 ('no_execution_mark','hv_win_pmu.c','*frame |= PMU_FRAME_EXECUTED;','*frame |= 0;'),
 ('accept_mismatched_ipa','hv_win_pmu.c','&& walked == ipa','&& true'),
 ('scan_with_other_cpu','hv_win_pmu.c','__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE) || ctx->far','false || ctx->far'),
 ('ignore_pc','hv_win_pmu.c','ctx->far != ctx->elr ||','false ||'),
 ('ignore_fnv','hv_win_pmu.c','(ctx->esr & BIT(10))','false'),
 ('allow_pxn_leaf','hv_win_pmu_4k.h','pxn || (d & BIT(53))','pxn'),
 ('allow_uxn_leaf','hv_win_pmu_4k.h','!el0 || uxn || (d & BIT(54))','!el0 || uxn'),
 ('allow_pxn_table','hv_win_pmu_4k.h','pxn |= !!(d & BIT(59));','pxn |= false;'),
 ('allow_uxn_table','hv_win_pmu_4k.h','uxn |= !!(d & BIT(60));','uxn |= false;'),
 ('ignore_aptable','hv_win_pmu_4k.h','no_user |= !!(d & BIT(61));','no_user |= false;'),
 ('ignore_wxn','hv_win_pmu_4k.h','(s->sctlr & BIT(19)) && !ro','false && !ro'),
 ('ignore_af','hv_win_pmu_4k.h','|| !(d & BIT(10))','|| false'),
 ('ignore_guard','hv_win_pmu_rewrite.c','if (pmu_rw_next_is_data(words[i + 1]))','if (false && pmu_rw_next_is_data(words[i + 1]))'),
 ('ignore_boundary','hv_win_pmu_rewrite.c','if (i == 1023)','if (false)'),
 ('write_no_reset','hv_win_pmu.c','pmu_invalidate(ipa, bytes);','pmu_invalidate(ipa, 0);'),
 ('write_reset_siblings','hv_win_pmu.c','pmu_invalidate(ipa, bytes);','pmu_invalidate(ipa & ~(PMU_PAGE - 1), PMU_PAGE);'),
 ('dma_reset_siblings','hv_win_pmu.c','u32 n = pmu_invalidate(ipa, size);','u32 n = pmu_invalidate(ipa & ~(PMU_PAGE - 1), size ? PMU_PAGE : 0);'),
 ('dma_miss_end','hv_win_pmu.c','if (page == stop)','if (page == first || page == stop)'),
 ('retain_proof_after_fill','hv_win_pmu.c','*frame = 0;','*frame |= 0;'),
 ('no_tick_reprotect','hv_win_pmu.c','if (pmu.armed && pmu.open)','if (false && pmu.armed && pmu.open)'),
 ('no_block_close','hv_win_pmu.c','hv_xt_set(pmu.open_block, HV_XT_SCANNING, 0);','hv_xt_set(pmu.open_block, HV_XT_CODE, 0);'),
 ('reopen_writes','hv_win_pmu.c','hv_xt_set(block, HV_XT_CODE, 0)','hv_xt_set(block, HV_XT_PINNED, 0)'),
]
cmd=['make','-s','-C',str(here),'check','OUT='+str(out/'baseline')]
r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(out/'baseline.log').write_text(r.stdout)
if r.returncode: sys.exit('baseline failed')
results=[]
for name,file,old,new in mutants:
    dest=out/name; dest.mkdir(exist_ok=True)
    for f in ['hv_win_pmu.c','hv_win_pmu_rewrite.c','hv_win_pmu_rewrite.h','hv_win_pmu_4k.h','hv_win.h']:
        shutil.copyfile(win/f,dest/f)
    p=dest/file; text=p.read_text()
    if text.count(old)!=1: raise RuntimeError((name,'anchor count',text.count(old)))
    p.write_text(text.replace(old,new))
    exe=dest/'test'
    compile_cmd=['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                 '-Wno-unused-parameter','-fno-strict-aliasing','-DPMU_REWRITE_HOST_TEST',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                 '-iquote',str(source/'tests/win-pmu-rewrite/shim'),'-iquote',str(dest),
                 str(here/'test_4k.c'),str(dest/'hv_win_pmu.c'),str(dest/'hv_win_pmu_rewrite.c'),'-o',str(exe)]
    c=subprocess.run(compile_cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (dest/'compile.log').write_text(c.stdout)
    row={'name':name,'compile_rc':c.returncode}
    if not c.returncode:
        r=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (dest/'run.log').write_text(r.stdout)
        row.update(run_rc=r.returncode,killed=r.returncode!=0,evidence=r.stdout.splitlines()[:3])
    else: row['killed']=False
    results.append(row)
    print(name, 'KILLED' if row['killed'] else 'FAILED',flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
killed=sum(r['killed'] for r in results)
print(f'mutants: {killed}/{len(results)} runtime kills')
sys.exit(killed!=len(results))
