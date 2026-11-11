#!/usr/bin/env python3
"""Host-only r2 behaviour suite + mutants on the actual r2 resident model C."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, sys, tempfile
HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
M = R/'source/src/hv/win/s1'
S0 = R/'tests/storage/s0/src'
out = Path(tempfile.mkdtemp(prefix='m5w-s1-r2-'))
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
FLAGS = ['clang','-std=c11','-DVNVME_HOST_TEST','-Wall','-Wextra','-Werror','-O1','-g',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all']
def build(src, binary):
    cmd = FLAGS+['-I'+str(src),'-I'+str(S0),str(src/'hv_pci.c'),str(src/'hv_nvme.c'),str(src/'hv_nvme_queue.c'),
                 str(src/'ram_disk.c'),str(S0/'guest_memory.c'),str(HERE/'test_r2.c'),'-o',str(binary)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return cmd
def copy_model(d):
    d.mkdir()
    for n in ['hv_pci.c','hv_nvme.c','hv_nvme_queue.c','hv_nvme_queue.h','ram_disk.c','ram_disk.h','model.h']:
        shutil.copyfile(M/n, d/n)
base = out/'base'; copy_model(base)
cmd = build(base, out/'test')
dump = out/'s1dg.dump'
r = subprocess.run([str(out/'test')], capture_output=True, text=True, env=dict(env, S1DG_DUMP=str(dump)), timeout=60)
if r.returncode: sys.exit('base failed: '+r.stderr)
result = json.loads(r.stdout)
dec = subprocess.run([sys.executable, '-I', str(R/'tests/s1dg/test_decode.py'), str(dump)], capture_output=True, text=True, timeout=60)
if dec.returncode: sys.exit('S1DG decoder tests failed: '+dec.stderr[-2000:])
decoder_tests = [l for l in dec.stderr.splitlines() if l.startswith('Ran ')]
Q, N = 'hv_nvme_queue.c', 'hv_nvme.c'
mutants = [
 ('d1_cfs_does_not_stop_engine', N, '    m->queue.fatal=true;\n    m->regs.csts|=CSTS_CFS;', '    m->regs.csts|=CSTS_CFS;'),
 ('cfg_failed_read_logs_buffer', 'hv_pci.c', '(ok||write)?*value:0', '*value'),
 ('aer_post_after_fatal', Q, '        if (!ctrl->fatal && cq->cq_valid && cq->cq_pending + 1 < cq->cq_size &&', '        if (cq->cq_valid && cq->cq_pending + 1 < cq->cq_size &&'),
 ('cq_ack_blocked_after_cfs', N, 'if (!cq && (m->regs.csts&CSTS_CFS)) why|=S1DG_DB_CFS;', 'if (m->regs.csts&CSTS_CFS) why|=S1DG_DB_CFS;'),
 ('prp_list_offset_rejected', Q, 'if (!list || (list & 7) || frags == VNVME_MAX_LIST_FRAGS) return false;', 'if (!list || (list & (VNVME_PAGE_SIZE - 1)) || frags == VNVME_MAX_LIST_FRAGS) return false;'),
 ('prp_chain_offset_accepted', Q, '                if (frags && (list & (VNVME_PAGE_SIZE - 1))) return false; /* chained: page aligned */\n', ''),
 # nightly-4: the overlap check is now `if (!to_guest) {` (READ: any overlap accepted); same intent.
 ('r414_read_dup_dest_refused', Q, 'if (!to_guest) {', 'if (true) {'),
 ('n4_read_partial_refused', Q, 'if (!to_guest) {', 'if (!to_guest || n != VNVME_PAGE_SIZE || plan->len[i] != VNVME_PAGE_SIZE || addr != plan->addr[i]) {'),
 ('n4_part_counter_missing', Q, '                ctrl->prp_part_dest_reads++;\n', ''),
 ('r414_rule_not_recorded', Q, '    ctrl->prp_refusals[rule]++;', '    ctrl->prp_refusals[rule]++; rule = 0;'),
 ('prp_fragment_overlap_unchecked', Q, '        for (size_t i = 0; i < frags; i++)\n            if (overlaps(addr, n, frag_addr[i], frag_len[i])) return false;\n', ''),
 ('intms_not_reset', N, 'm->regs.csts=0;m->regs.cc=value;m->regs.intms=0;', 'm->regs.csts=0;m->regs.cc=value;'),
 ('iosqes_strict_at_enable', N, 'return (sq==0||sq==6) && (cq==0||cq==4);', 'return sq==6 && cq==4;'),
 ('iocqes_unchecked_at_create', Q, '    if (ctrl->iocqes != 4) /* r2.1: CC.IOCQES must be valid before CQ creation */\n        return VNVME_SC_INVALID_FIELD;\n', ''),
 ('abort_cid_unchecked', Q, '                (cmd->cdw10 >> 16) == ctrl->aer_cid && ', '                '),
 ('abort_cq_space_unchecked', Q, ' &&\n                ctrl->queues[0].cq_size - 1 - ctrl->queues[0].cq_pending >= 2) {', ') {'),
 ('shutdown_admin_first', Q, 'u16 qid = (u16)((pass + 1) % VNVME_MAX_QUEUES);', 'u16 qid = (u16)pass;'),
 ('admin_size1_accepted', Q, 'sq_size < 2 || cq_size < 2 ||', '!sq_size || !cq_size ||'),
 ('diag_admin_first_missing', N, '                memcpy(x->sqe,c,sizeof(x->sqe));x->status=0;x->completed=0;x->dw0=0;', '                (void)x;'),
 ('diag_refusal_missing', N, '        m->diag.refusals[reason]++;\n', ''),
 ('diag_fatal_reason_missing', N, '    if (!g->first_fatal_reason) g->first_fatal_reason=reason;\n', ''),
 ('diag_ring_not_advanced', N, '    g->write_seq++;\n', ''),
 ('diag_write_counter_missing', N, '                g->writes++;', '                '),
 ('aer_not_tracked', Q, '    ctrl->aer_outstanding = true;\n', ''),
 ('nsid_ffffffff_accepted', Q, '            if (cmd->nsid != 1)\n                return VNVME_SC_INVALID_NAMESPACE;\n            identify_namespace', '            if (cmd->nsid != 1 && cmd->nsid != 0xffffffffu)\n                return VNVME_SC_INVALID_NAMESPACE;\n            identify_namespace'),
 ('fua_rejected', Q, '~(0xffffu | BIT(30) | BIT(31))', '~0xffffu'),
 # r4: the fixture FLUSH line moved; same intent (FLUSH wrongly rejected on a valid nsid).
 ('flush_rejected', Q, 'if (!ctrl->caps.writable) return VNVME_SC_SUCCESS;', 'if (!ctrl->caps.writable) return VNVME_SC_INVALID_OPCODE;'),
 ('fw_slot_missing', Q, '        case VNVME_LID_FW_SLOT: log_size = 512; break;\n', ''),
 ('temp_not_stored', Q, '                *t = cmd->cdw11 & 0xffff;', '                (void)t;'),
 # r4: the VWC case gained the caps.vwc guard; same intent (VWC Set wrongly accepted on the fixture).
 ('vwc_set_accepted', Q, '            if (!ctrl->caps.vwc)\n                return VNVME_SC_INVALID_FIELD;', '            if (0) /* mutant */\n                return VNVME_SC_INVALID_FIELD;'),
 # r4: WRITE on the read-only fixture goes through caps.writable; same intent (not WP-rejected).
 ('write_not_protected', Q, 'return ctrl->caps.writable ? process_write(ctrl, cmd) : VNVME_SC_NS_WRITE_PROTECTED;', 'return ctrl->caps.writable ? process_write(ctrl, cmd) : VNVME_SC_INVALID_OPCODE;'),
 ('descriptors_empty', Q, '    data[0] = 1; data[1] = 8;   /* NIDT EUI-64 */', '    return;'),
 ('nguid_unstable', Q, '    memcpy(data + 104, ns1_nguid, sizeof(ns1_nguid)); /* r2 NGUID */', '    data[104] = (u8)ctrl->stats.commands;'),
 ('fatal_not_latched', Q, '        set_fatal(ctrl, VNVME_FATAL_QSPAN_CQ);\n        return;', '        return;'),
 ('aer_not_aborted_on_shutdown', Q, '            post_completion(ctrl, 0, 0, ctrl->queues[0].sq_head, ctrl->aer_cid, 0, VNVME_SC_ABORT_REQUESTED))', '            false)'),
 ('cfs_not_reported', N, '    if (m->queue.fatal) m->regs.csts|=CSTS_CFS;\n    s1_recompute_irq(m);\n}', '    s1_recompute_irq(m);\n}'),
 ('enable_admission_not_fatal', N, '            fatal(m,S1DG_FATAL_ENABLE_ADMQ); return true;', '            return true;'),
 ('narrow_reads_zero', N, '        v|=(u64)((read32(m,a&~3u)>>((a&3)*8))&0xff)<<(8*i);', '        (void)a;'),
 ('qword_high_dropped', N, '            if (bytes==8) write32(m,off+4,(u32)(*value>>32));', ''),
 ('reserved_write_effect', N, '        return; /* NSSR unsupported; reserved */', '        m->regs.intms=v&1;return;'),
]
caught = []
for name, f, before, after in mutants:
    d = out/('mutant-'+name); copy_model(d); p = d/f; t = p.read_text()
    if t.count(before) != 1: sys.exit('mutant no longer matches uniquely: '+name)
    p.write_text(t.replace(before, after))
    try: build(d, d/'test')
    except subprocess.CalledProcessError as e: sys.exit('mutant build failed '+name+': '+e.stderr[-800:])
    x = subprocess.run([str(d/'test')], capture_output=True, text=True, env=env, timeout=30)
    if x.returncode == 0 or 'FAIL:' not in x.stderr: sys.exit('mutant escaped: '+name)
    caught.append({'name': name, 'assertion': x.stderr.strip().splitlines()[-1]})
sources = {n: hashlib.sha256((M/n).read_bytes()).hexdigest() for n in ['hv_pci.c','hv_nvme.c','hv_nvme_queue.c','hv_nvme_queue.h','model.h']}
receipt = {'status': 'pass', 'suite': result, 'decoder_tests': decoder_tests, 'mutants_caught': caught, 'compile': cmd, 'sources_sha256': sources,
           'test_sha256': hashlib.sha256((HERE/'test_r2.c').read_bytes()).hexdigest(), 'hardware_access': False,
           'stornvme_trace_is_reconstruction_not_capture': True, 'output_directory': str(out)}
print(json.dumps(receipt, indent=1))
