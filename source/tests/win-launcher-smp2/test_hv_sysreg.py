#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""smp2: the Python HV shell's shadowed MSRs (debug registers) on a guest-started secondary.
Upstream creates self.sysreg[cpu] in start_secondary(); with PSCI CPU_ON in EL2 the first forwarded
DBGBVR access from CPU 1 used to raise KeyError (SMP-PLAN #19). Needs construct (m1n1 venv)."""
import pathlib, sys, types

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "proxyclient"))
from m1n1.hv import HV  # noqa: E402
from m1n1.sysreg import DBGBVRn_EL1, sysreg_parse  # noqa: E402,F401
from m1n1.hv.types import *  # noqa: E402,F401,F403
from m1n1.proxy import ESR_ISS_MSR, MSR_DIR  # noqa: E402

hv = HV.__new__(HV)
hv.sysreg = {0: {}}
hv.started_cpus = {0: (0, 0, 0)}
hv._bps = [None] * 6
hv._wps = [None] * 4
hv.hook_exceptions = False
hv.log = lambda *a, **k: None

op0, op1, crn, crm, op2 = DBGBVRn_EL1(0)


def msr(cpu, write, rt, val=0):
    regs = [0] * 32
    regs[rt] = val
    ctx = types.SimpleNamespace(cpu_id=cpu, regs=regs, elr=0x1000)
    hv.ctx = ctx
    iss = ESR_ISS_MSR(Op0=op0, Op1=op1, CRn=crn, CRm=crm, Op2=op2, Rt=rt,
                      DIR=MSR_DIR.WRITE if write else MSR_DIR.READ)
    hv.handle_msr(ctx, iss.value if hasattr(iss, "value") else iss)
    return regs[rt]


msr(1, True, 3, 0xfffff80012345678)
got = msr(1, False, 4)
ok = got == 0xfffff80012345678 and msr(0, False, 5) == 0
print(f"secondary DBGBVR0 shadow write/read {'ok' if ok else 'FAILED'} ({got:#x}); boot CPU unaffected")
sys.exit(0 if ok else 1)
