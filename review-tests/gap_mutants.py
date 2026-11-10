#!/usr/bin/env python3
"""Extra mutants in the r2/r2.1-changed code, run against the builder's tests/r2/test_r2.c
(and tests/storage/s1/test_s1.c, and this directory's test_review.c). Reports which survive. Copies only."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
R = HERE.parent
M = R / 'source/src/hv/win/s1'
S0 = R / 'tests/storage/s0/src'
Q, N = 'hv_nvme_queue.c', 'hv_nvme.c'
MUTANTS = [
    ('abort_cq_space_unchecked', Q, ' &&\n                ctrl->queues[0].cq_size - 1 - ctrl->queues[0].cq_pending >= 2', ''),
    ('sq_doorbell_after_shutdown', Q, '    if (ctrl->shutdown || ctrl->fatal)\n        return false; /* r2: ignored by the caller, nothing fetched */\n', ''),
    ('process_guard_removed', Q, '    if (ctrl->fatal || ctrl->shutdown)\n        return;\n    struct vnvme_queue *sq', '    struct vnvme_queue *sq'),
    ('shutdown_admin_first', Q, 'u16 qid = (u16)((pass + 1) % VNVME_MAX_QUEUES);', 'u16 qid = (u16)pass;'),
    ('abort_cid_unchecked', Q, '                (cmd->cdw10 >> 16) == ctrl->aer_cid && ', '                '),
    ('cc_live_edit_applied', N, 'value=(old&~(S1_CC_SHN|es))|keep|(value&S1_CC_SHN);', '(void)keep;'),
    # r2.1 additions
    ('shutdown_post_after_fatal', Q, '            if (ctrl->fatal) { /* r2.1: nothing more is read or posted after fatal */', '            if (0) {'),
    ('aer_post_after_fatal', Q, '        if (!ctrl->fatal && cq->cq_valid && cq->cq_pending + 1 < cq->cq_size &&', '        if (cq->cq_valid && cq->cq_pending + 1 < cq->cq_size &&'),
    ('cq_ack_blocked_after_cfs', N, 'if (!cq && (m->regs.csts&CSTS_CFS)) why|=S1DG_DB_CFS;', 'if (m->regs.csts&CSTS_CFS) why|=S1DG_DB_CFS;'),
    ('intms_not_reset', N, 'm->regs.csts=0;m->regs.cc=value;m->regs.intms=0;', 'm->regs.csts=0;m->regs.cc=value;'),
    ('shn_clear_not_counted', N, '            g->shn_clear_no_reset++;', '            '),
    ('asq_write_while_enabled', N, 'if (m->regs.cc&S1_CC_EN) return; /* changes while enabled are ignored */', '/* r */'),
    ('aqa_reserved_unmasked', N, 'm->regs.aqa=v&0x0fff0fffu;', 'm->regs.aqa=v;'),
    ('log_offset_unaligned_ok', Q, '|| (offset & 3) ||', '||'),
    # r4: the CFS line also reports a failed shutdown flush; the mutant still hides the fatal CFS.
    ('shutdown_fatal_cfs_hidden', N, '        if (m->queue.fatal || !flushed) m->regs.csts|=CSTS_CFS;\n        if (!was) {', '        if (!flushed) m->regs.csts|=CSTS_CFS;\n        if (!was) {'),
    ('shutdown_twice_reruns', Q, '    if (ctrl->shutdown)\n        return;\n    ctrl->shutdown = true;', '    ctrl->shutdown = true;'),
    ('fatal_after_trace_ignored', Q, '        if (ctrl->fatal)\n            return;\n        if (complete) {', '        if (complete) {'),
    ('temp_under_thsel_ignored', Q, 'u16 *t = thsel ? &ctrl->temp_under : &ctrl->temp_over;', 'u16 *t = &ctrl->temp_over;'),
    ('sv_bit_ignored', Q, '    if (set && (cmd->cdw10 & BIT(31)))\n        return VNVME_SC_FEATURE_NOT_SAVEABLE;\n', ''),
]
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
FL = ['clang', '-std=c11', '-DVNVME_HOST_TEST', '-O0', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
out = Path(tempfile.mkdtemp(prefix='m5w-r2-gap-'))
def build_run(d, which):
    srcs = [str(d / n) for n in ['hv_pci.c', 'hv_nvme.c', 'hv_nvme_queue.c', 'ram_disk.c']]
    if which == 's1':
        cmd = FL + ['-I' + str(d), '-I' + str(S0)] + srcs + [str(S0 / 'guest_memory.c'), str(R / 'tests/storage/s1/test_s1.c'), '-o', str(d / 's1')]
        arg = []
    elif which == 'r2':
        cmd = FL + ['-I' + str(d), '-I' + str(S0)] + srcs + [str(S0 / 'guest_memory.c'), str(R / 'tests/r2/test_r2.c'), '-o', str(d / 'r2')]
        arg = []
    else:
        cmd = FL + ['-I' + str(d)] + srcs + [str(HERE / 'test_review.c'), '-o', str(d / 'review')]
        arg = ['3000']
    subprocess.run(cmd, check=True, capture_output=True)
    r = subprocess.run([str(d / which)] + arg, capture_output=True, text=True, env=env, timeout=600)
    return r.returncode != 0
for name, f, before, after in MUTANTS:
    d = out / name; d.mkdir()
    for n in ['hv_pci.c', 'hv_nvme.c', 'hv_nvme_queue.c', 'hv_nvme_queue.h', 'ram_disk.c', 'ram_disk.h', 'model.h']:
        shutil.copyfile(M / n, d / n)
    t = (d / f).read_text()
    if t.count(before) != 1: sys.exit('mutant does not match uniquely: ' + name)
    (d / f).write_text(t.replace(before, after))
    k1 = build_run(d, 'r2'); k3 = build_run(d, 's1'); k2 = build_run(d, 'review')
    print(f'{name:28s} builder r2: {"killed" if k1 else "survived":8s} builder S1: {"killed" if k3 else "survived":8s} '
          f'review: {"killed" if k2 else "survived":8s} => {"KILLED" if (k1 or k3 or k2) else "SURVIVES ALL"}')
