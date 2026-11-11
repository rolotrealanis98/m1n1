#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""r4.9 (1 vCPU): smp2g test_hv_sysreg.py on the boot CPU (its CPU-1 case needs the multi-CPU Python fix, not
ported), plus: for every register r4.9 now handles in EL2 C, the host Python handler's branch is the one the C
case mirrors (debug regs: pass-through mrs/msr of the same encoding; ACC_CFG/ACC_OVRD: read pass, write skipped;
DBGB/W 0..4/0..3: shadow). Needs construct (m1n1 venv)."""
import pathlib, re, sys, types

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "proxyclient"))
from m1n1.hv import HV  # noqa: E402
from m1n1.sysreg import DBGBVRn_EL1  # noqa: E402
from m1n1.proxy import ESR_ISS_MSR, MSR_DIR  # noqa: E402

calls = []
class U:
    class cpu_features: apple_sysregs_unlocked = False
    def mrs(self, enc, **k): calls.append(('mrs', tuple(enc))); return 0x5a5a
    def msr(self, enc, v, **k): calls.append(('msr', tuple(enc), v))

hv = HV.__new__(HV)
hv.sysreg = {0: {}}
hv.started_cpus = {0: (0, 0, 0)}
hv._bps = [None] * 5
hv._wps = [None] * 4
hv.hook_exceptions = False
hv.log = lambda *a, **k: None
hv.u = U()

def msr(enc, write, rt, val=0):
    regs = [0] * 32
    regs[rt] = val
    ctx = types.SimpleNamespace(cpu_id=0, regs=regs, elr=0x1000)
    hv.ctx = ctx
    op0, op1, crn, crm, op2 = enc
    iss = ESR_ISS_MSR(Op0=op0, Op1=op1, CRn=crn, CRm=crm, Op2=op2, Rt=rt,
                      DIR=MSR_DIR.WRITE if write else MSR_DIR.READ)
    hv.handle_msr(ctx, iss.value if hasattr(iss, "value") else iss)
    return regs[rt]

fails = 0
def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name); fails += not ok

# smp2g case on CPU 0
e0 = tuple(DBGBVRn_EL1(0))
msr(e0, True, 3, 0xfffff80012345678)
check("DBGBVR0 shadow write/read on the boot CPU", msr(e0, False, 4) == 0xfffff80012345678)

logic = (ROOT / "src/hv/hv_sysreg_logic.h").read_text()
rows = [(tuple(int(x) for x in m.groups()[:5]), m.group(6)) for m in re.finditer(
    r'\{HV_SR_ENC\((\d+), (\d+), (\d+), (\d+), (\d+)\), (HV_SR_PASS(?:_RO)?), "', logic)]
for enc, kind in rows:
    calls.clear(); got = msr(enc, False, 5)
    rd_ok = calls == [('mrs', enc)] and got == 0x5a5a
    calls.clear(); msr(enc, True, 6, 0x1234)
    wr_pass = calls == [('msr', enc, 0x1234)]
    wr_skip = calls == []
    py = 'PASS' if rd_ok and wr_pass else 'PASS_RO' if rd_ok and wr_skip else f'other {calls}'
    # C PASS_RO on an architecturally read-only debug register: writes UNDEF at EL1 before the trap,
    # so the write branch is unreachable; only the read must match.
    arch_ro = enc in {(2, 0, 1, 0, 0), (2, 0, 7, 14, 6), (2, 3, 0, 1, 0)}
    ok = rd_ok and (py == kind[6:] or (arch_ro and kind == 'HV_SR_PASS_RO'))
    check(f"s{enc[0]}_{enc[1]}_c{enc[2]}_c{enc[3]}_{enc[4]}: python {py}, C {kind[6:]}", ok)
print(f"hv_sysreg_1cpu: {fails} failed")
sys.exit(1 if fails else 0)
