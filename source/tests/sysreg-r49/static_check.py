#!/usr/bin/env python3
"""r4.9: every hv_sr_extra[] row (hv_sysreg_logic.h) has exactly one matching SYSREG_PASS / SYSREG_PASS_RO case in
hv_exc.c (resolved through the SYS_* defines), the smp2g port is present (OSLSR case, hv_dbg_shadow call after the
switch), the fail-soft option defaults ON only for stage 3 and is gated by s1n_link_locked(), and the pre-escalation
log runs at the top of hv_exc_proxy before any proxy call."""
import re, sys, pathlib
S = pathlib.Path(__file__).resolve().parents[2] / 'src'
exc = (S / 'hv/hv_exc.c').read_text()
logic = (S / 'hv/hv_sysreg_logic.h').read_text()
defs = {}
for f in ('arm_cpu_regs.h', 'cpu_regs.h'):
    for m in re.finditer(r'#define\s+(SYS_\w+)\s+sys_reg\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)', (S / f).read_text()):
        defs[m.group(1)] = tuple(int(x) for x in m.groups()[1:])
rows = [(tuple(int(x) for x in m.groups()[:5]), m.group(6)) for m in re.finditer(
    r'\{HV_SR_ENC\((\d+), (\d+), (\d+), (\d+), (\d+)\), (HV_SR_PASS(?:_RO)?), "', logic)]
unl = exc[exc.index('static bool hv_handle_msr_unlocked'):exc.index('static bool hv_handle_nested_eret')]
cases = {}
for m in re.finditer(r'\b(SYSREG_PASS(?:_RO)?)\((SYS_\w+)\)', unl):
    enc = defs.get(m.group(2))
    if enc:
        cases.setdefault(enc, []).append(m.group(1))
fails = 0
def check(name, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name)
    fails += not ok
check(f'{len(rows)} table rows parsed', len(rows) >= 15)
for enc, kind in rows:
    got = cases.get(enc, [])
    check(f'row s{enc[0]}_{enc[1]}_c{enc[2]}_c{enc[3]}_{enc[4]} {kind} -> hv_exc.c {got}', got == [kind.replace('HV_SR_', 'SYSREG_')])
check('hv_dbg_shadow called after the switch in hv_handle_msr_unlocked',
      re.search(r'if \(hv_dbg_shadow\(reg, rt, is_read, regs\)\)\s+return true;\s+return false;\s+\}', unl) is not None)
check('fail-soft default: ON for S1N_STAGE 3 only',
      re.search(r'#if S1N_STAGE == 3\s+#define HV_SYSREG_FAILSOFT 1\s+#else\s+#define HV_SYSREG_FAILSOFT 0', exc) is not None)
check('r4.10: unhandled action gated by HV_SYSREG_FAILSOFT and s1n_link_locked()',
      'hv_unhandled_action(HV_SYSREG_FAILSOFT, s1n_link_locked(), &e)' in exc)
check('r4.10: reflection writes the guest EL1 regs through the _EL12 accessors',
      all(f'msr(SYS_{r}_EL12, r.' in exc for r in ('ELR', 'SPSR', 'ESR', 'FAR')) and 'mrs(SYS_VBAR_EL12)' in exc)
check('r4.10: a reflected exception keeps its new ELR (protect_elr) and SMC advances ELR',
      'protect_elr = true; /* ELR/SPSR now point' in exc and 'protect_elr = e.ec == ESR_EC_HVC;' in exc)
px = exc[exc.index('void hv_exc_proxy('):]
px = px[:px.index('\n}\n')]
check('pre-escalation log precedes every proxy call in hv_exc_proxy',
      px.index('hv_esc_log(') < px.index('_hv_exc_proxy('))
check('log flushes the console', 'iodev_console_flush();\n}\n\nvoid hv_exc_proxy' in exc)
st = exc[exc.index('void hv_status_tick('):]
check('r4.11: status tick runs the storage watch every tick and reports on the 5 s cadence',
      'if (hv_win_storage_watch())\n        iodev_console_flush();' in st and 'hv_status_due(now, st_last_t, freq, HV_STATUS_PERIOD_S)' in st)
hvc = (S / 'hv/hv.c').read_text()
tick = hvc[hvc.index('void hv_tick(struct exc_info *ctx)'):]
tick = tick[:tick.index('\n}\n')]
check('r4.11: hv_tick calls hv_status_tick(ctx)', 'hv_status_tick(ctx);' in tick)
check('r4.11: reflection log prints every NEW-SITE (no blind spot)', 'if (n > HV_CLASS_LOG_FIRST && !fresh)' in exc)
print(f'static: {fails} failed')
sys.exit(1 if fails else 0)
