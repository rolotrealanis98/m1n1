#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""r4.10: the firmware's hv_reflect_compute() (C, compiled with ASan/UBSan) against the host Python HV._lower()
as the reference, on edge + random cases. Equal: ELR/SPSR/ESR/FAR written to the guest's EL1 registers via the
_EL12 accessors (the Python writes ELR_EL12/SPSR_EL12/ESR_EL12/FAR_EL12 and reads VBAR_EL12), the vector offset
(EL0t +0x400 / EL1t +0 / EL1h +0x200), the return PC and the return PSTATE M/DAIF; refusal for EL2/AArch32 origins.
Documented differences (architectural, hv_sysreg_logic.h): ESR EC lower->current for EL1 origin
(0x20/0x24/0x30/0x32/0x34 -> +1; BRK 0x3c unchanged), return PSTATE SS/IL/BTYPE cleared and PAN set when
SCTLR_EL1.SPAN = 0. Needs the m1n1 venv (construct)."""
import pathlib, random, subprocess, sys, tempfile, types

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "proxyclient"))
from m1n1.hv import HV  # noqa: E402
from m1n1.sysreg import SPSR, ESR, ELR_EL12, SPSR_EL12, ESR_EL12, FAR_EL12, VBAR_EL12  # noqa: E402

out = pathlib.Path(tempfile.mkdtemp(prefix="reflect-r410-"))
exe = out / "drv"
cc = "/opt/homebrew/opt/llvm/bin/clang"
cc = cc if pathlib.Path(cc).exists() else "clang"
subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=undefined", f"-I{ROOT / 'src'}", str(pathlib.Path(__file__).with_name("reflect_driver.c")),
                "-o", str(exe)], check=True)

def py_lower(elr, spsr, esr, far, vbar):
    w = {}
    class U:
        def msr(self, r, v, **k): w[tuple(r)] = v
        def mrs(self, r, **k):
            assert tuple(r) == tuple(VBAR_EL12); return vbar
    hv = HV.__new__(HV); hv.u = U(); hv.is_fault = True; hv.exc_code = 0
    try:
        sp = SPSR(spsr)
    except ValueError:
        return None  # Python cannot even represent this origin (AArch32 M[4]=1): no reflection
    hv.ctx = types.SimpleNamespace(elr=elr, spsr=sp, esr=ESR(esr), far=far)
    import contextlib, io
    with contextlib.redirect_stdout(io.StringIO()):
        ok = hv._lower()
    if not ok:
        return None
    return dict(elr_el1=w[tuple(ELR_EL12)], spsr_el1=w[tuple(SPSR_EL12)], esr_el1=w[tuple(ESR_EL12)],
                far_el1=w[tuple(FAR_EL12)], new_elr=hv.ctx.elr, new_spsr=hv.ctx.spsr.value)

rnd = random.Random(410)
cases = []
for m in (0x0, 0x4, 0x5, 0x8, 0x9, 0x10, 0x13):
    for ec in (0x3c, 0x30, 0x32, 0x34, 0x24, 0x20, 0x00, 0x16):
        for sctlr in (0, 1 << 23):
            spsr = (rnd.getrandbits(32) & ~0x1f) | m
            esr = (ec << 26) | (1 << 25) | rnd.getrandbits(25)
            if ec == 0x3c:
                esr = (ec << 26) | (1 << 25) | 0xf002
            cases.append((0xfffff802b24013c4, spsr, esr, rnd.getrandbits(64), 0xfffff802b2400000 & ~0x7ff, sctlr))
for _ in range(3000):
    m = rnd.choice((0, 4, 5, 9, 0x10))
    ec = rnd.choice((0x00, 0x16, 0x17, 0x18, 0x20, 0x21, 0x24, 0x25, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x3c))
    cases.append((rnd.getrandbits(64), (rnd.getrandbits(32) & ~0x1f) | m, (ec << 26) | rnd.getrandbits(26),
                  rnd.getrandbits(64), rnd.getrandbits(64) & ~0x7ff & ((1 << 64) - 0x1000), rnd.choice((0, 1 << 23))))
inp = "".join("%x %x %x %x %x %x\n" % c for c in cases)
res = subprocess.run([str(exe)], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()

fails = checks = 0
M64 = (1 << 64) - 1
for c, line in zip(cases, res):
    elr, spsr, esr, far, vbar, sctlr = c
    py = py_lower(elr, spsr, esr, far, vbar)
    checks += 1
    if py is None or line == "refused":
        if not (py is None and line == "refused"):
            fails += 1; print("FAIL refusal mismatch", [hex(x) for x in c], py, line)
        continue
    f = [int(x, 16) for x in line.split()]
    cr = dict(zip(("elr_el1", "spsr_el1", "esr_el1", "far_el1", "new_elr", "new_spsr", "offset"), f))
    from_el1 = (spsr & 0x1f) in (4, 5)
    ec = (esr >> 26) & 0x3f
    exp_esr = py["esr_el1"]
    if from_el1 and ec in (0x20, 0x24, 0x30, 0x32, 0x34):
        exp_esr = (exp_esr & ~(0x3f << 26)) | ((ec + 1) << 26)
    exp_spsr = py["new_spsr"] & ~((1 << 21) | (1 << 20) | (3 << 10))
    if not sctlr & (1 << 23):
        exp_spsr |= 1 << 22
    want = dict(elr_el1=py["elr_el1"], spsr_el1=py["spsr_el1"], esr_el1=exp_esr, far_el1=py["far_el1"],
                new_elr=py["new_elr"] & M64, new_spsr=exp_spsr)
    bad = {k: (hex(cr[k]), hex(v)) for k, v in want.items() if cr[k] != v}
    if (py["new_spsr"] & 0x3df) != 0x3c5 or (cr["new_spsr"] & 0x3df) != 0x3c5:
        bad["M/DAIF"] = (hex(cr["new_spsr"]), hex(py["new_spsr"]))
    if bad:
        fails += 1
        if fails < 10:
            print("FAIL", [hex(x) for x in c], bad)
named = {
    "BRK #0xf002 from EL1h -> VBAR+0x200, ESR unchanged": cases[0],
}
c = (0xfffff802b24013c4, 0x60000005, (0x3c << 26) | (1 << 25) | 0xf002, 0, 0xfffff802b2400000, 1 << 23)
line = subprocess.run([str(exe)], input="%x %x %x %x %x %x\n" % c, capture_output=True, text=True).stdout.split()
ok = int(line[4], 16) == 0xfffff802b2400200 and int(line[2], 16) == c[2] and int(line[5], 16) & 0x3ff == 0x3c5
checks += 1; fails += not ok
print(("ok   " if ok else "FAIL ") + "hardware case: BRK #0xf002 at 0xfffff802b24013c4 from EL1h -> VBAR_EL1+0x200, ESR_EL1 = ESR_EL2")
c = (0x7ff6000, 0x0, (0x3c << 26) | (1 << 25) | 0xf000, 0, 0xfffff802b2400000, 1 << 23)
line = subprocess.run([str(exe)], input="%x %x %x %x %x %x\n" % c, capture_output=True, text=True).stdout.split()
ok = int(line[4], 16) == 0xfffff802b2400400; checks += 1; fails += not ok
print(("ok   " if ok else "FAIL ") + "BRK from EL0t -> VBAR_EL1+0x400")
c = (0x1000, 0x5, (0x32 << 26) | (1 << 25), 0, 0x8000, 0)
line = subprocess.run([str(exe)], input="%x %x %x %x %x %x\n" % c, capture_output=True, text=True).stdout.split()
ok = (int(line[2], 16) >> 26) == 0x33 and int(line[5], 16) & (1 << 22); checks += 1; fails += not ok
print(("ok   " if ok else "FAIL ") + "software step from EL1h: ESR_EL1.EC 0x33 (current EL), PAN set with SPAN=0")
print(f"reflect-vs-python: {checks} cases, {fails} failed")
sys.exit(1 if fails else 0)
