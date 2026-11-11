import contextlib, importlib.util, io, pathlib, sys
p=pathlib.Path('../../proxyclient/tools/run_windows.py')
spec=importlib.util.spec_from_file_location('hidm_launcher',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
checks=0
def check(ok):
 global checks
 checks+=1
 assert ok

def parse(extra):
 sys.argv=['run_windows.py','fd.bin','--mu-profile','m1a','--route','gic','--ramdisk','x.ntasi']+extra
 try:
  with contextlib.redirect_stderr(io.StringIO()):return m.parse_args()[1]
 except SystemExit:return None
x=parse([]);check(x is not None and not x.builtin_keyboard)
x=parse(['--builtin-keyboard']);check(x is not None)
check(('HIDM',0xF38100000,0x8000) in m.window_frames(x))
x=parse(['--builtin-keyboard','-C','2']);check(x is not None and x.cpus==2)
check(parse(['--builtin-keyboard','--pl011-gsiv','68']) is None)
check(parse(['--builtin-keyboard','--irq-map','660:68']) is None)
check(parse(['--builtin-keyboard','--route','aic']) is None)
check(parse(['--builtin-keyboard','--raw-guest']) is None)
check(parse(['--builtin-keyboard','--synthetic-size','0x10000000']) is None)
x=parse(['--builtin-keyboard']);frames=m.window_frames(x)
try:m.check_frames((m.SYNTH_WINDOW_BASE,m.SYNTH_WINDOW_BASE+m.SYNTH_WINDOW_SIZE),frames,[(0xF38104000,0xF38108000)])
except m.PlanError:check(True)
else:check(False)
# Full updated Python files compile without importing the hardware libraries.
for p in [p,pathlib.Path('../../proxyclient/m1n1/proxy.py')]:compile(p.read_text(),str(p),'exec');check(True)
print(f'launcher: {checks} checks PASS')
