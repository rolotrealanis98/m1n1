"""Host-only handoff qualification of frozen6557 actual C, never hardware."""
from pathlib import Path
import ast,hashlib,json,os,shutil,subprocess,tempfile
P=Path(__file__).resolve().parent
F=P.parents[1];S=F/'source/src/hv/win'  # r2: runs against the r2 tree
PIN='b7fadf2113dffc9412dd6f0c75ff426041820d8ac24f0b95e15df0842cc26a33'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
files=['hv_win_storage.c','hv_win_storage.h','hv_win_storage_mem.h']+['s1/'+n for n in ['model.h','hv_nvme.c','hv_pci.c','hv_nvme_queue.c','hv_nvme_queue.h','ram_disk.c','ram_disk.h']]
# Extract only literal platform harness definitions, never execute original script.
constants={}
for node in ast.parse((F/'check-controller.py').read_text()).body:
 if isinstance(node,ast.Assign) and len(node.targets)==1 and isinstance(node.targets[0],ast.Name) and node.targets[0].id in ('pre','post'):
  constants[node.targets[0].id]=ast.literal_eval(node.value)
pre=constants['pre'].replace('guest[32768]','guest[65536]')
pre=pre.replace('irq_level=level;}','if(level&&!irq_level)irq_assertions++;if(!level&&irq_level)irq_deassertions++;irq_level=level;}')
setup=constants['post'].split('static u16 submit(')[0]
body=(P/'handoff-tests.c').read_text().replace('static unsigned scenarios, successful_reads, irq_assertions, irq_deassertions;','static unsigned scenarios, successful_reads;')
actual='\n'.join(x for x in (S/'hv_win_storage.c').read_text().splitlines() if not x.startswith('#include') and x!='extern char _base[], _end[];')
root=Path(tempfile.mkdtemp(prefix='m5w-s1-handoff-'))
source_hashes={n:sha(S/n) for n in files}
def run(name,mutation=None):
 out=root/name;out.mkdir();(out/'s1').mkdir()
 (out/'types.h').write_text('#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\ntypedef uint64_t u64;typedef uint32_t u32;typedef uint16_t u16;typedef uint8_t u8;typedef int64_t s64;\n')
 for n in files:
  if n!='hv_win_storage.c':shutil.copyfile(S/n,out/n)
 if mutation:
  file,old,new=mutation;p=out/file;s=p.read_text();assert s.count(old)==1,(name,s.count(old));p.write_text(s.replace(old,new))
 (out/'test.c').write_text('static unsigned irq_assertions,irq_deassertions;\n'+pre+actual+setup+body)
 cmd=['clang','-std=gnu11','-DVNVME_HOST_TEST','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(out/'s1'),str(out/'test.c')]+[str(out/'s1'/n) for n in ['hv_nvme.c','hv_pci.c','hv_nvme_queue.c','ram_disk.c']]+['-o',str(out/'test')]
 build=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
 (out/'build.stderr').write_text(build.stderr)
 if build.returncode:raise RuntimeError(build.stderr)
 r=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=20,env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
 (out/'stdout').write_text(r.stdout);(out/'stderr').write_text(r.stderr)
 if not mutation and r.returncode:raise RuntimeError(r.stdout+r.stderr)
 if mutation:assert r.returncode!=0 and 'FAIL:' in r.stderr,(name,r.returncode,r.stderr)
 return {'name':name,'returncode':r.returncode,'output_directory':str(out),'last_stdout':r.stdout.splitlines()[-1] if r.stdout else '', 'stderr':r.stderr,'compile_command':cmd}
results=[run('baseline')]
mutations=[('normal_shutdown_without_abort_completion',('s1/hv_nvme_queue.c','!post_completion(ctrl, cqid, qid, sq->sq_head, cmd.cid, 0, VNVME_SC_ABORT_REQUESTED))','true)')),
('retain_old_queues_on_disable',('s1/hv_nvme.c','        vnvme_init(&m->queue,s1_backend_blocks(),&backend_ops,m);\n        s1dg_event(m,S1DG_RESET','        /* deliberately omitted reset */\n        s1dg_event(m,S1DG_RESET')),
('advertise_fake_msi',('s1/hv_pci.c','case 0x34:return 0;','case 0x34:return 0x50;')),
('ignore_intx_disable',('s1/hv_nvme.c','(m->command&S1_BME) && !(m->command&S1_INTX_DISABLE) &&','(m->command&S1_BME) &&')),
('abrupt_keeps_queues',('s1/hv_nvme_queue.c','        memset(ctrl->queues, 0, sizeof(ctrl->queues));\n        update_irq(ctrl);\n        return;','        update_irq(ctrl);\n        return;'))]
for name,mutation in mutations:results.append(run(name,mutation))
receipt={'status':'pass','frozen_manifest_sha256':PIN,'source_sha256':source_hashes,'tests_sha256':sha(P/'handoff-tests.c'),'runner_sha256':sha(P/'run.py'),'actual_controller_model_queue':True,'platform_and_lease_boundary_mocked':True,'hardware_access':False,'windows_stornvme_qualified':False,'results':results}
(root/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
