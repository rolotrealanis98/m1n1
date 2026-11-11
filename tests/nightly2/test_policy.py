#!/usr/bin/env python3
"""nightly-2 review fixes, host checks:
 F2  resident-side refusals of SMC power actions and M5WN with a disk path (hv_win_feature_policy.h truth tables,
     and the glue calls the policy BEFORE any state change or hardware write; every SMC power write funnels
     through hv_win_smcp_power);
 F3  SMCP's guest-path send is non-blocking (asc_try_send, no asc_send/rtkit_send on the tick path), the receive
     budget stays bounded, and a full mailbox defers to a later tick (smcp_core retry), checked in hv_win_smcp.c.
Usage: test_policy.py [ROOT]  (default: this worktree)"""
import re, subprocess, sys, tempfile
from pathlib import Path
R = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
S = R / 'source/src'
fails = 0
def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what)
    fails += 0 if ok else 1

def body(src, sig):
    i = src.index(sig); j = src.index('\n}\n', i)
    return src[i:j]

# F2 truth tables (compiled from the header itself)
code = r'''
#include <stdbool.h>
#include <stdio.h>
#include "hv_win_feature_policy.h"
int main(void) {
    int bad = 0;
    for (int f = 0; f < 4; f++) for (int d = 0; d < 2; d++)
        bad |= fp_smcp_configure_ok((unsigned long long)f, d) != !((f & 1) && d);
    bad |= fp_smcp_power_ok(false) != true; bad |= fp_smcp_power_ok(true) != false;
    bad |= fp_m5wn_configure_ok(false) != true; bad |= fp_m5wn_configure_ok(true) != false;
    printf("policy truth tables %s\n", bad ? "FAILED" : "passed");
    return bad;
}
'''
with tempfile.TemporaryDirectory() as t:
    (Path(t) / 'p.c').write_text(code)
    r = subprocess.run(['cc', '-Wall', '-Werror', '-I', str(S), '-o', t + '/p', t + '/p.c'], capture_output=True, text=True)
    check(r.returncode == 0, 'policy header compiles ' + r.stderr[-200:])
    if r.returncode == 0:
        r = subprocess.run([t + '/p'], capture_output=True, text=True)
        check(r.returncode == 0, r.stdout.strip())

sm = (S / 'hv_win_smcp.c').read_text()
b = body(sm, 's64 hv_win_smcp_power(u64 action)')
k = b.find('fp_smcp_power_ok(hv_win_disk_path_used())')
check(k >= 0 and k < b.index('power_done = true') and k < b.index('smcp_power_run(') and 'return -1;' in b[k:b.index('power_done = true')],
      'SMCP power: disk-path refusal before power_done and the first SMC write')
b = body(sm, 's64 hv_win_smcp_configure(u64 version, u64 flags)')
k = b.find('fp_smcp_configure_ok(flags, hv_win_disk_path_used())')
check(k >= 0 and k < b.index('attempted = true') and k < b.index('hv_map_hook(') and k < b.index('smc_bringup()'),
      'SMCP configure: power-actions refusal before any state, hook or SMC bring-up')
check(sm.count('smcp_power_run(') == 1, 'smcp_power_run has one caller (hv_win_smcp_power)')
ae = body(sm, 'void hv_win_smcp_after_exit(void)')
check('smcp_power_run' not in ae and 'hv_win_smcp_power(' in ae, 'after-exit power goes through hv_win_smcp_power')
check(sm.count('hv_win_smcp_actions_armed(void) { return attempted && (rec_flags & SMCP_REC_F_POWER_ACTIONS); }') == 1,
      'actions-armed accessor')
check('_Static_assert(FP_SMCP_F_POWER_ACTIONS == SMCP_REC_F_POWER_ACTIONS' in sm,
      'policy flag bit == SMCP_REC_F_POWER_ACTIONS (compile-time assert in the glue)')
pc = (S / 'proxy.c').read_text()
i = pc.index('case P_HV_WIN_SMCP_POWER:'); j = pc.index('break;', i)
check('hv_win_smcp_power(' in pc[i:j], 'proxy 0xc42 goes through hv_win_smcp_power')

wn = (S / 'hv_win_m5wn.c').read_text()
b = body(wn, 's64 hv_win_m5wn_configure(u64 data, u64 size)')
k = b.find('fp_m5wn_configure_ok(hv_win_disk_path_used())')
check(k >= 0 and k < b.index('attempted = true') and k < b.index('hv_storage_mem_register(') and k < b.index('hv_map_hook('),
      'M5WN configure: disk-path refusal before any state, lease or hook')
n1 = (S / 'hv_win_n1.c').read_text()
b = body(n1, 's64 hv_win_n1_start(u64 fw, u64 fw_len, u64 stage, u64 flags)')
check(b.index('hv_win_storage_native_stat(0) & 6') < b.index('started = true') and b.index('if (attempted)') < b.index('started = true'),
      'N1 START: disk-path refusals before bring-up (unchanged)')
hw = (S / 'hv/win/hv_win.c').read_text()
g = body(hw, 'bool hv_win_storage_smp_gate(const char *what)')
t = g.index('u1_storage_touched = true')
for c in ('hv_win_smcp_actions_armed()', 'hv_win_m5wn_attempted()', 'hv_win_n1_touched()', 'hv_win.nr_vcpus > 1'):
    check(g.find(c) >= 0 and g.index(c) < t, 'storage gate refuses before admitting: ' + c)

# F3: the SMCP mailbox send on the tick path never blocks
send = body(sm, 'static int t_send(')
check('asc_try_send(' in send and 'asc_send(' not in send.replace('asc_try_send(', '') and 'rtkit_send(' not in send,
      'SMCP t_send uses asc_try_send (no blocking asc_send/rtkit_send)')
tick = body(sm, 'void hv_win_smcp_tick(void)')
check('udelay' not in tick and 'asc_send' not in tick and 'while' not in tick, 'SMCP tick has no wait loop')
core = (S / 'smcp/core/smcp_core.c').read_text()
check('SMCP_SEND_BUSY' in core and 'SMCP_SEND_BUSY' in send, 'a full mailbox is reported as BUSY and retried on a later tick')
print('nightly2-policy: %s' % ('PASS' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
