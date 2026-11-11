#!/usr/bin/env python3
"""Additional stage, identity, lock, abort, ordering, counter and cache fault injections."""
import importlib.util,json,os,subprocess,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('native_run',HERE/'run.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
root=Path(tempfile.mkdtemp(prefix='r45-mutants-'))
def m(name,stage,cases,*edits):return (name,stage,cases,list(edits))
A='s1/ans_ro.c';N='s1/native_window.c';L='s1/native_link.c'
mutants=[
 m('disk_guid_unpinned',3,['gpt_12'],(N,'    if (memcmp(header + 56, pinned_disk, 16)) return fail(S1N_E_PIN);','    (void)pinned_disk;'),(N,'        memcmp(header + 56, pinned_disk, 16) || rd64(header, 72) != nsze - 5 ||','        rd64(header, 72) != nsze - 5 ||')),
 m('backup_header_crc_skipped',3,['gpt_13'],(N,'    if (s1n_crc32(tmp, bsize) != bcrc) return fail(S1N_E_BACKUP);','    (void)bcrc;')),
 m('backup_array_skipped',3,['gpt_14'],(N,'        if (memcmp(tmp, scratch + i * SECTOR, bytes)) return fail(S1N_E_BACKUP);','        (void)bytes;')),
 m('first_write_recheck_skipped',3,['r45_recheck_2','r45_recheck_12'],(A,'bool ok = s1n_recheck_before_write();','bool ok = true;')),
 m('mailbox_gap_reintroduced',3,['r45_mailbox'],(A,'    if (!drain()) return -2;',''),(A,'    write32(base + (q->admin ? NVME_DB_LINEAR_ASQ : NVME_DB_LINEAR_IOSQ), tag);','    if (!drain()) return -2;\n    write32(base + (q->admin ? NVME_DB_LINEAR_ASQ : NVME_DB_LINEAR_IOSQ), tag);')),
 m('no_alternating_tags',3,['io_tag_2'],(A,'next_io_tag = tag == ANS_IO_TAG ? ANS_RO_IO_TAG_ALT : ANS_IO_TAG;','next_io_tag = ANS_IO_TAG;')),
 m('duplicate_after_consume_ignored',3,['r45_duplicate'],(A,'    write32(base + (q->admin ? NVME_DB_ACQ : NVME_DB_IOCQ), q->head);\n    dc_ivac_range(&q->cqes[q->head], sizeof(cqe));\n    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */\n    dma_rmb();\n    if ((q->cqes[q->head].status & 1) == q->phase) {','    write32(base + (q->admin ? NVME_DB_ACQ : NVME_DB_IOCQ), q->head);\n    if (0) {')),
 m('no_status_abort',3,['rw_write_error','r45_admin_status'],(A,'        freeze(ANS_RO_E_STATUS);','')),
 m('no_counter_check',3,['r45_counter'],(A,'    if (st.commands != st.cmd_admin + st.cmd_reads + st.cmd_writes + st.cmd_flushes ||\n        st.io_submitted != st.io_completed) {','    if (st.commands != st.cmd_admin + st.cmd_reads + st.cmd_writes + st.cmd_flushes) {')),
 m('no_upper_abort',3,['r45_direct_write_1'],(N,'        st.refused_writes++;\n        ans_ro_abort(ANS_RO_E_ABORT);','        st.refused_writes++;')),
 m('no_proxy_lock',3,['r45_link'],('hv_win_storage_native.c','    s1n_link_lock(); /* one-way even when select fails */','')),
 m('raw_memory_allowed',3,['r45_link'],(L,'    if (!locked) return true;\n    refusals++;','    return true;\n    refusals++;')),
 m('payload_not_discarded',3,['r45_link'],(L,'        while (left) {','        while (left && 0) {')),
 m('discard_sentinel_skipped',3,['r45_link'],(L,'part < 2','part < 1')),
 m('prep_replay',3,['r45_link'],(L,'if (!prep_used) { prep_used = true; return true; }','if (1) { prep_used = true; return true; }')),
 m('start_replay',3,['r45_link'],(L,'if (!start_used) { start_used = true; return true; }','if (1) { start_used = true; return true; }')),
 m('exit_outside_exception',3,['r45_link'],(L,'if (guest_exception) return true;','return true;')),
 m('stage0_flush_cap_removed',0,['r45_stage'],(A,'st.cmd_flushes >= S1N_FLUSH_CAP','st.cmd_flushes > S1N_FLUSH_CAP')),
 m('stage1_guest_writable',1,['r45_stage'],(N,'return S1N_STAGE_GUEST_WRITES && verified && rw;','return verified && rw;')),
 m('stage1_canary_access_unrestricted',1,['r45_stage_reject'],(A,'           && canary_running','           && true')),
 m('stage1_lower_edge',1,['r45_stage_bound_0'],(A,'lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)','lba + 1 >= S1N_SUB_START && lba + 1 - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba + 1 - S1N_SUB_START)')),
 m('stage1_upper_edge',1,['r45_stage_bound_1'],(A,'lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)','lba - S1N_SUB_START <= S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START) + 1')),
 m('stage2_upper_edge',2,['r45_stage_bound_1'],(A,'lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)','lba - S1N_SUB_START <= S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START) + 1')),
 m('canary_compare_removed',1,['r45_stage_corrupt'],(A,'if (memcmp(dma.xfer, expected, sizeof(canary_saved)))','if (0)')),
 m('canary_restore_replaced',1,['r45_stage'],(A,'const u8 *expected = pass ? canary_saved : canary_pattern;','const u8 *expected = canary_pattern;')),
 m('write_cap_removed',1,['r45_stage'],(A,'st.cmd_writes >= S1N_WRITE_CMD_CAP','st.cmd_writes >= S1N_WRITE_CMD_CAP - 1')),
]
for name,call in [('init_tcb','    dc_civac_range(t, QD * sizeof(*t));'),('init_sqe','    dc_civac_range(c, QD * sizeof(*c));'),('init_cqe','    dc_civac_range(p, QD * sizeof(*p));'),('sqe','    dc_civac_range(slot, sizeof(*slot));'),('tcb','    dc_civac_range(tcb, sizeof(*tcb));'),('cqe_poll','        dc_ivac_range(&q->cqes[q->head], sizeof(cqe));'),('cqe_before','    dc_ivac_range(&q->cqes[q->head], sizeof(struct ans_cpl));'),('cqe_after','    dc_ivac_range(&q->cqes[q->head], sizeof(cqe));\n    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */\n    dma_rmb();')]:
 mutants.append(m('cache_'+name,3,['r45_cache'],(A,call,'')))
mutants += [
 m('pinned_selection_removed',3,['r45_pin_args_4'],(N,'    if (w->entry_index != S1N_PIN_ENTRY || w->start_lba != S1N_PIN_START ||\n        w->blocks != S1N_PIN_BLOCKS || memcmp(w->unique_guid, pinned_guid, 16))','    if (0)'),(N,'    win = *w;','    (void)pinned_guid;\n    win = *w;')),
 m('stage1_write_cap_exceeded',1,['r45_write_cap'],(A,'st.cmd_writes >= S1N_WRITE_CMD_CAP','st.cmd_writes > S1N_WRITE_CMD_CAP'),(A,'n > S1N_WRITE_BLOCK_CAP - st.blocks_written','n > S1N_WRITE_BLOCK_CAP - st.blocks_written + 1')),
 m('stage1_block_cap_exceeded',1,['r45_block_cap'],(A,'n > S1N_WRITE_BLOCK_CAP - st.blocks_written','n > S1N_WRITE_BLOCK_CAP - st.blocks_written + 1')),
 m('stage2_lower_edge',2,['r45_stage_bound_0'],(A,'lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)','lba + 1 >= S1N_SUB_START && lba + 1 - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba + 1 - S1N_SUB_START)')),
]
for label,site in [('submit','    dc_civac_range(tcb, sizeof(*tcb));'),('init','    dc_civac_range(p, QD * sizeof(*p));'),('cq_before','    dc_ivac_range(&q->cqes[q->head], sizeof(struct ans_cpl));'),('cq_poll','        dc_ivac_range(&q->cqes[q->head], sizeof(cqe));'),('cq_after','    dc_ivac_range(&q->cqes[q->head], sizeof(cqe));'),('readback','        dc_civac_range(buf, bytes);')]:
 indent=site[:len(site)-len(site.lstrip())]
 anchor=site+'\n'+indent+'sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */'
 if label=='cq_after': anchor += '\n    dma_rmb();'
 if label=='readback': anchor += '\n        dma_rmb();'
 mutants.append(m('cache_barrier_'+label,3,['r45_cache'],(A,anchor,anchor.replace(indent+'sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */\n','') if label in ('cq_after','readback') else site)))
results=[]
if os.environ.get("R45_MUTANTS"):
 mutants = [m for m in mutants if m[0] in os.environ["R45_MUTANTS"].split(",")]
for name,stage,cases,edits in mutants:
 os.environ['S1N_STAGE']=str(stage)
 item={'name':name,'stage':stage,'caught':False}
 try:
  out=r.build(root,name,edits)
  runs=[]
  for case in cases:
   x=subprocess.run([str(out/'test'),case],capture_output=True,text=True,timeout=120,env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0'))
   runs.append({'case':case,'rc':x.returncode,'stderr':x.stderr[-800:]})
  item.update(compiled=True,caught=any(x['rc'] for x in runs),runs=runs)
 except (RuntimeError,AssertionError) as e: item.update(compiled=False,error=str(e))
 results.append(item)
 (root/'progress.json').write_text(json.dumps(results,indent=2))
 print(name, 'CAUGHT' if item['caught'] else 'NOT CAUGHT',file=__import__('sys').stderr,flush=True)
print(json.dumps({'status':'pass' if all(x['caught'] for x in results) else 'fail','mutants':results,'output_directory':str(root)},indent=2))
