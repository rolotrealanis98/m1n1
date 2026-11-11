#!/usr/bin/env python3
"""r3 end-to-end host qualification: guest NVMe frontend -> native window -> ans_ro -> fake ANS.

Compiles the actual r3 sources (S1 model, hv_win_storage.c, hv_win_storage_native.c,
native_window.c, ans_ro.c, and m1n1 asc/rtkit/sart) under ASan/UBSan against the fake T6050
ANS, runs every scenario in its own process, then applies deliberate mutants that must be
caught. Host only: no hardware, no SSH, no serial.
"""
import hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
# r4.3: S1_SOURCE_ROOT=<resident dir> runs THIS harness against another revision (e.g. r4.2);
# S1_NO_MUTANTS=1 skips mutants/probes and prints every scenario result.
R = Path(os.environ.get("S1_SOURCE_ROOT", R)).resolve()
SRC = R / "source/src"
WIN = SRC / "hv/win"
S1 = WIN / "s1"

M1N1 = ["asc.c", "asc.h", "rtkit.c", "rtkit.h", "sart.c", "sart.h"]
S1F = ["s1_stage.h", "native_link.c", "native_link.h", "ans_ro.c", "ans_ro.h", "native_window.c", "native_window.h", "hv_nvme.c", "hv_pci.c",
       "hv_nvme_queue.c", "hv_nvme_queue.h", "model.h", "ram_disk.c", "ram_disk.h"]
SCENARIOS = (["happy", "prep_blocks_mismatch", "e2e_reads", "window_args", "window_args_type",
              "refuse_running", "refuse_sart", "aic_already_routed", "liveness", "allowlist",
              "window_direct", "fixture_default", "late_select",
              "select_no_prep", "quiesce_op", "quiesce_after_prep", "stale_bounce", "scrub_after_gpt_reject",
              "hello_slow", "no_identify_rw", "no_identify_issued", "capacity_mismatch",
              "rw_basic", "rw_power_loss", "rw_fua", "rw_write_through", "rw_guest_shutdown",
              "rw_quiesce_flush", "rw_chunked", "rw_write_error", "rw_defense", "rw_requires_flag",
              "rw_requires_gpt", "rw_random", "rw_slow_flush"]
             + [f"gpt_{i}" for i in range(1, 11)]
             + [f"freeze_init_{i}" for i in range(5)]
             + [f"freeze_runtime_{i}" for i in range(4)]
             # r4.3: alignment / PRP / DMA confinement
             + ["prp_spec_e2e", "prp_mask_e2e", "prp_strict_rw", "prp_spec_rw", "prp_mask_rw",
                "no_stray_writes_strict", "no_stray_writes_spec", "no_stray_writes_mask",
                "no_stray_writes_rw", "no_stray_writes_rw_spec", "m1n1_layout"]
             + [f"dma_guard_{i}" for i in range(14)]
             # r4.4 (= r3.5): RTKit timing, which-wait marker, TCB status register, CC/CQ shape
             + ["rtkit_handshake_3s", "rtkit_handshake_7s", "rtkit_handshake_9s_rw"]
             + [f"rtkit_timeout_{i}" for i in range(5)]
             + ["tcb_alias_nonzero", "tcb_linux_nonzero", "cc_cq_shape"]
             # r4.4: write-path items (I/O tag 2, FLUSH instead of FUA, IOQ register clear)
             + ["io_tag_2", "fua_is_flush", "teardown_ioq_regs_0", "teardown_ioq_regs_1"])

SCENARIOS += ([f"r45_{kind}_{i}" for kind in ("guest_read", "guest_write", "direct_read", "direct_write") for i in range(4)]
              + [f"r45_absolute_{i}" for i in range(3)] + [f"r45_pin_args_{i}" for i in range(5)]
              + [f"gpt_{i}" for i in (12,13,14,15)] + [f"r45_recheck_{i}" for i in (2,3,4,12,13,14)]
              + ["r45_counter", "r45_duplicate", "r45_mailbox", "r45_link", "r45_cache", "r45_raw_ro"])
SCENARIOS += [f"window_direct_{i}" for i in range(1,7)] + [f"rw_defense_{i}" for i in range(1,10)]
# r4.6 (C9/L-1 made refusals terminal): split-out probes, one per process, of checks that used to run
# inside e2e_reads / allowlist / rw_requires_flag after a nonterminal refusal.
SCENARIOS += (["e2e_reads_ro_write", "rw_requires_flag_flush"] + [f"allowlist_io_{i}" for i in range(11)]
              + [f"allowlist_adm_{i}" for i in range(15)])
if os.environ.get("S1N_STAGE", "3") != "3":
    SCENARIOS = ["r45_stage", "r45_stage_reject", "r45_link", "happy", "r45_stage_ro_write"]  # r4.6: + split-out RO write probe
    if os.environ.get("S1N_STAGE") in ("1", "2"): SCENARIOS += ["r45_stage_corrupt", "r45_write_cap", "r45_block_cap"] + [f"r45_stage_bound_{i}" for i in range(3)]

SCENARIOS += ["r45_ro_guest_outside", "r45_admin_status"]
# T1/T2 overlay (r4.5 independent review, secA cases adopted): backup-GPT mismatch cases at every stage,
# GPT-reject freeze, read-only-config guest READ refusals; stage 3 adds the restored write refusals and
# rw_random generator, stages 0/1/2 add guest READ refusals in the stage (rw contract, ro guest) configuration.
SECA_BACKUP = ["secA_backup_control"] + [f"secA_backup_{i}" for i in (13, 14, *range(20, 30))]
SCENARIOS += (SECA_BACKUP + [f"secA_gpt_frozen_{i}" for i in range(1, 16)] + [f"secA_ro_guest_read_{i}" for i in range(6)]
              + ["secA_raw_ro_op_0", "secA_raw_ro_op_1"])
if os.environ.get("S1N_STAGE", "3") == "3":
    # CHECK.md: split differential (flat reference, scattered guest pages), mid-request failure, FUA order
    SCENARIOS += ["chk_split_diff", "chk_split_fua"] + [f"chk_split_error_{i}" for i in (1, 2, 5, 8)]
    SCENARIOS += [f"secA_rw_write_{i}" for i in range(6)] + ["secA_rw_read_max1", "secA_rw_random", "secA_stray_slots", "r414_dup_pages"]
else:
    SCENARIOS += [f"secA_stage_guest_read_{i}" for i in range(6)]

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def strip(text, drop=()):
    return "\n".join(l for l in text.splitlines()
                     if not l.startswith("#include") and l not in drop)

def build(root, name, mutation=None):
    out = root / name
    (out / "src").mkdir(parents=True)
    (out / "s1").mkdir()
    (out / "config.h").write_text("/* host */\n")
    shutil.copyfile(SRC / "proxy.h", out / "src/proxy.h")
    for n in M1N1: shutil.copyfile(SRC / n, out / "src" / n)
    for n in S1F: shutil.copyfile(S1 / n, out / "s1" / n)
    for n in ["hv_win_storage.c", "hv_win_storage.h", "hv_win_storage_mem.h", "hv_win_storage_native.c"]:
        shutil.copyfile(WIN / n, out / n)
    if mutation:
        for f, old, new in (mutation if isinstance(mutation, list) else [mutation]):
            p = out / f
            s = p.read_text()
            assert s.count(old) == 1, (name, f, s.count(old))
            p.write_text(s.replace(old, new))
    test = ((HERE / "prelude.h").read_text()
            + strip((out / "hv_win_storage.c").read_text(), ("extern char _base[], _end[];",))
            + "\n" + strip((out / "hv_win_storage_native.c").read_text())
            + "\n" + (HERE / "body.c").read_text())
    (out / "test.c").write_text(test)
    common = ["clang", "-std=gnu11", "-DVNVME_HOST_TEST", "-DANS_RO_HOST_TEST", "-DS1N_STAGE=" + os.environ.get("S1N_STAGE", "3"), "-Wall", "-Wextra",
              "-Werror", "-Wno-unused-parameter", "-Wno-unused-function", "-O1", "-g",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
              f"-I{HERE/'shim'}", f"-I{HERE}", f"-I{out/'src'}", f"-I{out/'s1'}", f"-I{out}"]
    srcs = [out / "test.c", HERE / "fake_ans.c"] + [out / "src" / n for n in M1N1 if n.endswith(".c")] \
        + [out / "s1" / n for n in S1F if n.endswith(".c")]
    cmd = common + [str(s) for s in srcs] + ["-o", str(out / "test")]
    b = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    (out / "build.stderr").write_text(b.stderr)
    if b.returncode:
        raise RuntimeError(f"{name} build failed:\n{b.stderr[-4000:]}")
    return out

def run_all(out):
    env = dict(os.environ, ASAN_OPTIONS="abort_on_error=1:detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    results, checks = [], 0
    for s in SCENARIOS:
        r = subprocess.run([str(out / "test"), s], capture_output=True, text=True, timeout=120, env=env)
        ok = r.returncode == 0
        if ok: checks += json.loads(r.stdout.strip().splitlines()[-1])["checks"]
        results.append({"scenario": s, "ok": ok,
                        "detail": "" if ok else (r.stderr.strip().splitlines() or ["rc %d" % r.returncode])[0][:200]})
    return results, checks

# Inherited r4.4 mutants retargeted to the pinned r4.5 source, same failure intent.
# T1 overlay: the backup-check mutants that survived the r4.5 suite (review T1) are listed first.
MUTANTS = [
 # CHECK.md: r4.6 32-block split in the emulated controller (hv_nvme_queue.c process_write / READ path)
 ('chk_split_write_same_offset',
  [('s1/hv_nvme_queue.c', '                                     buffer + (size_t)done * VNVME_LBA_SIZE))',
    '                                     buffer))')]),
 ('chk_split_write_gap',
  [('s1/hv_nvme_queue.c', 'if (!ctrl->ops->write_blocks(ctrl->opaque, lba + done, n,',
    'if (!ctrl->ops->write_blocks(ctrl->opaque, lba + done + (done ? 1 : 0), n,')]),
 ('chk_split_write_last_piece_dropped',
  [('s1/hv_nvme_queue.c', '    for (u32 done = 0; done < blocks;) {\n        u32 n = blocks - done;\n        if (n > 32) n = 32;\n        if (!ctrl->ops->write_blocks',
    '    for (u32 done = 0; done + 32 <= blocks || !done;) {\n        u32 n = blocks - done;\n        if (n > 32) n = 32;\n        if (!ctrl->ops->write_blocks')]),
 ('chk_split_write_continue_after_error',
  [('s1/hv_nvme_queue.c', '                                     buffer + (size_t)done * VNVME_LBA_SIZE))\n            return VNVME_SC_INTERNAL;',
    '                                     buffer + (size_t)done * VNVME_LBA_SIZE)) {}')]),
 ('chk_split_cap_64',
  [('s1/hv_nvme_queue.c', '        if (n > 32) n = 32;\n        if (!ctrl->ops->write_blocks', '        if (n > 64) n = 64;\n        if (!ctrl->ops->write_blocks')]),
 ('chk_split_read_same_offset',
  [('s1/hv_nvme_queue.c', '        memcpy(read_result + (size_t)done * VNVME_LBA_SIZE, buffer,', '        memcpy(read_result, buffer,')]),
 ('chk_split_read_gap',
  [('s1/hv_nvme_queue.c', 'if (!ctrl->ops->read_blocks(ctrl->opaque, lba + done, n, buffer))',
    'if (!ctrl->ops->read_blocks(ctrl->opaque, lba + done + (done ? 1 : 0), n, buffer))')]),
 ('chk_read_refusal_reaches_backend',
  [('s1/hv_nvme_queue.c', '        ctrl->refused_reads++;\n        return VNVME_SC_LBA_OUT_OF_RANGE; /* r4.13: was Invalid Field (0x02); NVMe 1.4 Fig. 129 */\n    }',
    '        ctrl->refused_reads++;\n    }')]),

 ('t1_backup_hdr_fields_skipped', [('s1/native_window.c', '        return fail(S1N_E_BACKUP);\n    memcpy(tmp, header, bsize);', '        (void)0;\n    memcpy(tmp, header, bsize);')]),
 ('t1_backup_mylba_unchecked', [('s1/native_window.c', ' rd64(header, 24) != nsze - 1 ||', ' 0 ||')]),
 ('t1_backup_disk_guid_unchecked', [('s1/native_window.c', '        memcmp(header + 56, pinned_disk, 16) || rd64(header, 72) != nsze - 5 ||', '        rd64(header, 72) != nsze - 5 ||')]),
 ('t1_backup_array_first_sector_only', [('s1/native_window.c', '    for (u32 i = 0; i < esectors; i++) {\n        if (!ans_ro_read_gpt(nsze - 5 + i, tmp))', '    for (u32 i = 0; i < 1; i++) {\n        if (!ans_ro_read_gpt(nsze - 5 + i, tmp))')]),
('nw_no_header_crc',
  [('s1/native_window.c', 'if (s1n_crc32(tmp, hsize) != hcrc)', 'if (s1n_crc32(tmp, hsize) != hcrc && 0)')]),
 ('nw_no_entries_crc',
  [('s1/native_window.c',
    'if (s1n_crc32(scratch, ebytes) != ecrc)',
    'if (s1n_crc32(scratch, ebytes) != ecrc && 0)')]),
 ('nw_no_unique_guid', [('s1/native_window.c', 'if (memcmp(e + 16, w->unique_guid, 16))', 'if (0)')]),
 ('nw_no_type_check', [('s1/native_window.c', 'if (memcmp(e, w->type_guid, 16))', 'if (0)')]),
 ('nw_no_range_check',
  [('s1/native_window.c',
    'if (first != w->start_lba || last < first || last - first + 1 != w->blocks)',
    'if (0)')]),
 ('nw_no_overlap_check',
  [('s1/native_window.c', 'if (of <= last && ol >= first)', 'if (of <= last && ol >= first && 0)')]),
 ('nw_read_bounds_all_off_by_one',
  [('s1/native_window.c',
    'blocks <= 256 && lba < S1N_PIN_BLOCKS && blocks <= S1N_PIN_BLOCKS - lba;',
    'blocks <= 256 && lba < S1N_PIN_BLOCKS + 1 && blocks <= S1N_PIN_BLOCKS - lba + 1;'),
   ('s1/native_window.c',
    '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS ||\n'
    '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
    '            ans_ro_abort(ANS_RO_E_ABORT);\n'
    '            return false;\n'
    '        }',
    '        (void)phys;'),
   ('s1/ans_ro.c',
    '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < S1N_PIN_BLOCKS &&\n'
    '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
    '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START <= S1N_PIN_BLOCKS && n <= '
    'S1N_PIN_BLOCKS - (lba - S1N_PIN_START) + 1;')]),
 ('nw_translate_no_start',
  [('s1/native_window.c',
    '        u64 phys = S1N_PIN_START + lba + done;',
    '        u64 phys = S1N_PIN_START + lba;')]),
 ('ans_allow_write',
  [('s1/ans_ro.c',
    '    return opcode == OP_READ || (rw_enabled && (opcode == OP_FLUSH\n'
    '#if S1N_STAGE_WRITES\n'
    '           || opcode == OP_WRITE\n'
    '#endif\n'
    '           ));\n'
    '}',
    '    return opcode == OP_READ || opcode == 0x01;\n}')]),
 ('ans_allow_format',
  [('s1/ans_ro.c',
    'opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ;',
    'opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ || opcode == 0x80;')]),
 ('ans_timeout_no_freeze',
  [('s1/ans_ro.c',
    '            freeze(ANS_RO_E_CMD_TIMEOUT);\n            return -2;',
    '            return -1;')]),
 ('ans_tag_no_freeze',
  [('s1/ans_ro.c',
    '    if (cqe.tag != tag) {\n'
    '        st.cqe_anomalies++;\n'
    '        freeze(ANS_RO_E_CQE);\n'
    '        return -2;\n'
    '    }',
    '    if (cqe.tag != tag) return -1;')]),
 ('ans_skip_linux_tcb_stat',
  [('s1/ans_ro.c',
    '    if (stat_linux) {\n        freeze(ANS_RO_E_TCB);',
    '    if (stat_linux && 0) {\n        freeze(ANS_RO_E_TCB);')]),
 ('ans_dma_outside_xfer',
  [('s1/ans_ro.c',
    '    cmd.prp1 = (u64)buf;\n    if (blocks == 2)',
    '    cmd.prp1 = (u64)dma.bounce;\n    if (blocks == 2)')]),
 ('ans_frozen_still_shuts_down',
  [('s1/ans_ro.c',
    # r4.6 re-anchor: the frozen check in ans_ro_shutdown is now ans_ro_access_ok() (session_frozen)
    # plus state; the mutant un-freezes both, i.e. lets a frozen session still run the shutdown.
    'bool ans_ro_shutdown(void)\n{\n    if (!ans_ro_access_ok()) return false;\n    if (state != ANS_RO_LIVE)',
    'bool ans_ro_shutdown(void)\n'
    '{\n'
    '    if (state == ANS_RO_FROZEN) { __atomic_store_n(&session_frozen, false, __ATOMIC_RELEASE); state = ANS_RO_LIVE; }\n'
    '    if (!ans_ro_access_ok()) return false;\n'
    '    if (state != ANS_RO_LIVE)')]),
 ('ans_no_running_refusal',
  [('s1/ans_ro.c',
    '    if (asc_cpu_running(asc)) {\n        set_state(ANS_RO_REFUSED, ANS_RO_E_RUNNING);',
    '    if (0) {\n        set_state(ANS_RO_REFUSED, ANS_RO_E_RUNNING);')]),
 ('ans_sart_threshold_3', [('s1/ans_ro.c', 'if (st.sart_free < 4) {', 'if (st.sart_free < 3) {')]),
 ('ans_no_pmgr_parity', [('s1/ans_ro.c', '    pmgr_reset(die, "ANS");\n', '')]),
 ('ans_no_drain',
  [('s1/ans_ro.c',
    '    for (unsigned i = 0; i < 64; i++) {\n        if (!rtkit_can_recv(rtk))',
    '    for (unsigned i = 0; i < 0; i++) {\n        if (!rtkit_can_recv(rtk))')]),
 ('storage_tick_noop',
  [('hv_win_storage.c',
    'void hv_win_storage_tick(void)\n{\n    s1_backend_poll();\n}',
    'void hv_win_storage_tick(void)\n{\n}')]),
 ('retire_skips_backend', [('hv_win_storage.c', '    s1_backend_retire();\n}', '}')]),
 ('prep_accepts_zero_blocks',
  [('hv_win_storage.c',
    'd->blocks!=s1_backend_blocks() || !d->blocks ||',
    '(d->blocks!=s1_backend_blocks() && s1_backend_blocks()) ||')]),
 ('native_no_aic_deny',
  [('hv_win_storage_native.c',
    '        if (hv_vgic3_deny_aic_line(ans_lines[i]) < 0)\n            return refuse();',
    '        (void)ans_lines[i];')]),
 ('native_no_poison',
  [('hv_win_storage_native.c',
    '    s1_poison_backend(); /* PREP now refuses every descriptor: expose nothing */\n',
    '')]),
 ('native_late_select_poisons',
  [('hv_win_storage_native.c',
    '    if (hv_win_storage_stat(0) & 4) {\n        last_result = -1;\n        return -1;\n    }',
    '    if (hv_win_storage_stat(0) & 4)\n        return refuse();')]),
 ('r1_retire_needs_prep',
  [('hv_win_storage.c', 'if(!attempted){s1_backend_retire();return;}', 'if(!attempted)return;')]),
 ('r1_tick_needs_attempted',
  [('hv_win_storage.c', '    s1_backend_poll();\n}', '    if(attempted)s1_backend_poll();\n}')]),
 ('r1_quiesce_ignores_guest',
  [('hv_win_storage_native.c',
    '        if (hv_started_cpus[i])\n            return -1;',
    '        (void)hv_started_cpus[i];')]),
 ('r2_read_no_clear',
  [('s1/ans_ro.c',
    '        memset(buf, 0, bytes);\n        dc_civac_range(buf, bytes);',
    '        dc_civac_range(buf, bytes);')]),
 ('r2_no_scrub_on_success',
  [('s1/native_window.c', '    have_snapshot = true;\n    scrub();', '    have_snapshot = true;'),
   ('s1/native_window.c', '    st.verified = 1;\n    scrub();', '    st.verified = 1;')]),
 ('r2_no_scrub_on_fail',
  [('s1/native_window.c',
    'static bool fail(enum s1n_reason r)\n{\n    scrub();',
    'static bool fail(enum s1n_reason r)\n{')]),
 ('rec_hello_default_1s',
  [('s1/ans_ro.c',
    '    rtkit_set_boot_timeout(RTKIT_HELLO_TIMEOUT); /* r3.1: HELLO/EPMAP waits, restored below; r4.4: 10 s */\n',
    '')]),
 ('r32_no_capacity_crosscheck',
  [('s1/native_window.c',
    '    if (alt_lba != nsze - 1)       /* r4.1 (= r3.2): DMA-verified capacity cross-check */\n'
    '        return fail(S1N_E_CAPACITY);',
    '    if (alt_lba == 0 && nsze == 0)\n        return fail(S1N_E_CAPACITY);')]),
 ('r32_capacity_from_identify_ns',
  [('s1/ans_ro.c', '    st.nsze = T6050_NSZE;', '    st.nsze = st.identify_nsze;')]),
 ('r32_wrong_pinned_capacity', [('s1/ans_ro.c', '    st.nsze = T6050_NSZE;', '    st.nsze = T6050_NSZE + 1;')]),
 ('r33_reissue_identify',
  [('s1/ans_ro.c',
    '        return opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||',
    '        return opcode == OP_IDENTIFY || opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||'),
   ('s1/ans_ro.c',
    '    st.identify_nsze = UINT64_MAX;\n    st.identity_confirmed = 0;\n',
    '    st.identify_nsze = UINT64_MAX;\n'
    '    st.identity_confirmed = 0;\n'
    '    memset(&cmd, 0, sizeof(cmd)); cmd.opcode = OP_IDENTIFY; cmd.nsid = 1; cmd.prp1 = (u64)dma.bounce; '
    'clear_bounce(); exec(&adminq, &cmd);\n')]),
 ('r43_allow_identify',
  [('s1/ans_ro.c',
    '        return opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||',
    '        return opcode == OP_IDENTIFY || opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||')]),
 ('r43_guard_off', [('s1/ans_ro.c', '    if (!dma_guard(q, slot, tcb)) {', '    if (0) {')]),
 ('r43_iocq_offset_0x400',
  [('s1/ans_ro.c', '    cmd.prp1 = (u64)ioq.cqes;', '    cmd.prp1 = (u64)ioq.cqes + 0x400;')]),
 ('r43_data_offset_0x800',
  [('s1/ans_ro.c', '    u8 *buf = dma.xfer + offset;', '    u8 *buf = dma.xfer + offset + 0x800;')]),
 ('r43_guard_off_iocq_offset',
  [('s1/ans_ro.c', '    if (!dma_guard(q, slot, tcb)) {', '    if (0) {'),
   ('s1/ans_ro.c', '    cmd.prp1 = (u64)ioq.cqes;', '    cmd.prp1 = (u64)ioq.cqes + 0x400;')]),
 ('r43_guard_and_precheck_off_data_offset',
  [('s1/ans_ro.c', '    if (!dma_guard(q, slot, tcb)) {', '    if (0) {'),
   ('s1/ans_ro.c', '    if (!precheck(q, slot, tcb)) {', '    if (0) {'),
   ('s1/ans_ro.c', '    u8 *buf = dma.xfer + offset;', '    u8 *buf = dma.xfer + offset + 0x800;')]),
 ('r43_guard_and_precheck_off_two_block_no_prp2',
  [('s1/ans_ro.c', '    if (!dma_guard(q, slot, tcb)) {', '    if (0) {'),
   ('s1/ans_ro.c', '    if (!precheck(q, slot, tcb)) {', '    if (0) {'),
   ('s1/ans_ro.c',
    '    if (blocks == 2)\n        cmd.prp2 = (u64)buf + ANS_RO_BLOCK;',
    '    if (blocks == 2)\n        cmd.prp2 = 0;')]),
 ('r43_guard_accepts_any_cq',
  [('s1/ans_ro.c', 'return q == &adminq && buf16k(c->prp1, dma.icpl) &&', 'return q == &adminq && c->prp1 &&')]),
 ('r43_guard_ignores_prp2_one_block',
  [('s1/ans_ro.c',
    '        return !c->prp2 && one_page(c->prp1, bytes);',
    '        return one_page(c->prp1, bytes);')]),
 ('r43_guard_two_block_any_prp2',
  [('s1/ans_ro.c', '        return c->prp2 == c->prp1 + ANS_NVME_PAGE;', '        return true;')]),
 ('r43_guard_list_not_checked',
  [('s1/ans_ro.c', '    if (!buf16k(c->prp2, dma.prp_list))\n        return false;', '')]),
 ('r43_guard_no_xfer_bound',
  [('s1/ans_ro.c',
    '        (c->prp1 & (ANS_NVME_PAGE - 1)) || c->prp1 - x > sizeof(dma.xfer) - bytes)',
    '        (c->prp1 & (ANS_NVME_PAGE - 1)))')]),
 ('r43_guard_flush_prp',
  [('s1/ans_ro.c', '        return !c->prp1 && !c->prp2 && !c->cdw12;', '        return !c->prp2 && !c->cdw12;')]),
 ('r4_rw_without_flag',
  [('s1/native_window.c', '    bool want_rw = w->flags & S1N_RW;', '    bool want_rw = true;')]),
 ('r4_write_allowed_without_rw_both_layers',
  [('s1/ans_ro.c',
    'return opcode == OP_READ || (rw_enabled && (opcode == OP_FLUSH\n'
    '#if S1N_STAGE_WRITES\n'
    '           || opcode == OP_WRITE\n'
    '#endif\n'
    '           ));',
    'return opcode == OP_READ || opcode == OP_WRITE || opcode == OP_FLUSH;'),
   ('s1/ans_ro.c',
    '        if (!rw_enabled || !write_range(lba, n) || t->dma_flags != 2)',
    '        if (t->dma_flags != 2)'),
   ('s1/ans_ro.c', '        return rw_enabled && window_set && !c->prp1', '        return !c->prp1')]),
 ('r4_both_window_checks_off_by_one',
  [('s1/native_window.c',
    'blocks <= 256 && lba < S1N_PIN_BLOCKS && blocks <= S1N_PIN_BLOCKS - lba;',
    'blocks <= 256 && lba < S1N_PIN_BLOCKS + 1 && blocks <= S1N_PIN_BLOCKS - lba + 1;'),
   ('s1/native_window.c',
    '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS ||\n'
    '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
    '            ans_ro_abort(ANS_RO_E_ABORT);\n'
    '            return false;\n'
    '        }',
    '        (void)phys;'),
   ('s1/ans_ro.c',
    '        (write && (!rw_enabled || !write_range(lba, blocks))) ||',
    '        (write && !rw_enabled) ||'),
   ('s1/ans_ro.c',
    '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < S1N_PIN_BLOCKS &&\n'
    '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
    '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START <= S1N_PIN_BLOCKS && n <= '
    'S1N_PIN_BLOCKS - (lba - S1N_PIN_START) + 1;'),
   ('s1/ans_ro.c',
    'lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)',
    'lba - S1N_SUB_START <= S1N_SUB_BLOCKS &&\n           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START) + 1')]),
 ('r4_fua_no_flush',
  [('s1/hv_nvme_queue.c', '    if ((cmd->cdw12 & BIT(30)) || !ctrl->wce) {', '    if (!ctrl->wce) {')]),
 ('r4_wce_ignored',
  [('s1/hv_nvme_queue.c',
    '            if (set)\n                ctrl->wce = cmd->cdw11 & 1;',
    '            (void)set;')]),
 ('r4_shutdown_no_flush',
  [('s1/hv_nvme.c', '            flushed=vnvme_backend_flush(&m->queue);', '            flushed=true;')]),
 ('r4_quiesce_no_flush',
  [('s1/ans_ro.c',
    '    if (rw_enabled && !ans_ro_flush() && state == ANS_RO_FROZEN)\n        return false;',
    '')]),
 ('r4_write_no_cache_clean',
  [('s1/ans_ro.c',
    '        /* Data must be at the point of coherency before ANS reads it. */\n'
    '        dc_civac_range(buf, bytes);',
    '        /* (mutant: no clean) */')]),
 ('r4_prp_list_off_by_page',
  [('s1/ans_ro.c',
    '            dma.prp_list[i - 1] = (u64)buf + (u64)i * ANS_RO_BLOCK;',
    '            dma.prp_list[i - 1] = (u64)buf + (u64)(i + 1) * ANS_RO_BLOCK;')]),
 ('r4_canary_missing',
  [('s1/ans_ro.c', '        if (first < st.w_abs_min_first) st.w_abs_min_first = first;\n', '')]),
 ('r414_write_src_overlap_allowed',
  [('s1/hv_nvme_queue.c', 'if (!to_guest && !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {', 'if (false) {')]),   # nightly-4b: check line changed, same intent (partial WRITE overlap allowed)
 ('r414_read_dup_dest_refused',
  [('s1/hv_nvme_queue.c', 'if (!to_guest && !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {', 'if (to_guest || !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {')]),    # nightly-4b: same intent
 ('n4b_write_dup_refused',
  [('s1/hv_nvme_queue.c', 'if (!to_guest && !(n == VNVME_PAGE_SIZE && plan->len[i] == VNVME_PAGE_SIZE && addr == plan->addr[i])) {', 'if (!to_guest) {')]),
 ('r4_vwc_dishonest', [('s1/hv_nvme_queue.c', '    data[525] = ctrl->caps.vwc ? 1 : 0;', '    data[525] = 0;')]),
 ('r4_nsattr_wp_in_rw',
  [('s1/hv_nvme_queue.c', '    data[99] = ctrl->caps.writable ? 0 : 1;', '    data[99] = 1;')]),
 ('r4_mdts_not_raised',
  [('s1/hv_nvme_queue.c', '    while ((1u << (mdts + 1)) <= ctrl->caps.max_blocks) mdts++;', '    mdts = 5;')]),
 ('r4_write_in_ro_frontend',
  [('s1/hv_nvme_queue.c',
    '        return ctrl->caps.writable ? process_write(ctrl, cmd) : VNVME_SC_NS_WRITE_PROTECTED;',
    '        return process_write(ctrl, cmd);')]),
 ('r4_flush_not_passed',
  [('s1/hv_nvme_queue.c',
    '        if (!ctrl->caps.writable) return VNVME_SC_SUCCESS;\n'
    '        return backend_flush(ctrl) ? VNVME_SC_SUCCESS : VNVME_SC_INTERNAL;',
    '        return VNVME_SC_SUCCESS;')]),
 ('r4_flush_timeout_1s',
  [('s1/ans_ro.c', '#define NVME_FLUSH_TIMEOUT   30000000', '#define NVME_FLUSH_TIMEOUT   1000000')]),  # r4.6 re-anchor: 30 s
 ('r4_ro_default_large_cmds',
  [('s1/native_window.c', '    if (!mx) mx = want_rw ? 256 : 1;', '    if (!mx) mx = 256;')]),
 ('rtkit_power_wait_unbounded',
  [('src/rtkit.c',
    '    return rtkit_power_timeout && timeout_expired(deadline);',
    '    (void)deadline;\n    return false;')]),
 ('r35_hello_back_to_5s',
  [('s1/ans_ro.c', '#define RTKIT_HELLO_TIMEOUT 10000000', '#define RTKIT_HELLO_TIMEOUT 5000000')]),
 ('r35_power_back_to_1s',
  [('s1/ans_ro.c', '#define RTKIT_BOOT_POWER_TIMEOUT 10000000', '#define RTKIT_BOOT_POWER_TIMEOUT 1000000')]),
 ('r35_no_which_wait',
  [('s1/ans_ro.c',
    '    st.rtkit_timeouts |= rtkit_take_timeouts();\n    if (!booted) {',
    '    (void)rtkit_take_timeouts();\n    if (!booted) {')]),
 ('r35_hello_marker_lost', [('src/rtkit.c', '        rtkit_timeouts |= RTKIT_TIMEOUT_HELLO;\n', '')]),
 ('r35_power_marker_lost', [('src/rtkit.c', '            rtkit_timeouts |= RTKIT_TIMEOUT_IOP_POWER_ON;\n', '')]),
 ('r35_alias_freezes_again',
  [('s1/ans_ro.c',
    '    if (stat_linux) {\n        freeze(ANS_RO_E_TCB);',
    '    if (stat_linux || stat_alias) {\n        freeze(ANS_RO_E_TCB);')]),
 ('r35_alias_not_recorded', [('s1/ans_ro.c', '        st.tcb_alias_last = stat_alias;\n', '')]),
 ('r44_io_tag_0', [('s1/ans_ro.c', '#define ANS_IO_TAG    2', '#define ANS_IO_TAG    0')]),
 ('r44_io_tag_1', [('s1/ans_ro.c', '#define ANS_IO_TAG    2', '#define ANS_IO_TAG    1')]),
 ('r44_admin_tag_2',
  [('s1/ans_ro.c',
    '    const u8 tag = q->admin ? ANS_ADMIN_TAG : next_io_tag;',
    '    const u8 tag = ANS_IO_TAG;')]),
 ('r44_guard_ignores_tag',
  [('s1/ans_ro.c',
    '    if (c != want_slot || c->tag != (q->admin ? ANS_ADMIN_TAG : next_io_tag) ||\n'
    '        t != &q->tcbs[c->tag])\n'
    '        return false;',
    '    (void)want_slot;'),
   ('s1/ans_ro.c', '    const u8 tag = q->admin ? ANS_ADMIN_TAG : next_io_tag;', '    const u8 tag = 0;')]),
 ('r44_fua_passed_to_ans',
  [('s1/ans_ro.c',
    '    cmd.cdw12 = blocks - 1;\n    if (write) {',
    '    cmd.cdw12 = (blocks - 1) | (write ? (1u << 30) : 0);\n    if (write) {')]),
 ('r44_no_ioq_clear',
  [('s1/ans_ro.c',
    '        write64_lo_hi(base + NVME_IOQ_CMDS, 0);\n        write64_lo_hi(base + NVME_IOQ_CQES, 0);\n',
    '')]),
 ('r44_ioq_clear_order',
  [('s1/ans_ro.c',
    '        write64_lo_hi(base + NVME_IOQ_CMDS, 0);\n        write64_lo_hi(base + NVME_IOQ_CQES, 0);\n',
    '        write64_lo_hi(base + NVME_IOQ_CQES, 0);\n        write64_lo_hi(base + NVME_IOQ_CMDS, 0);\n')]),
 ('r44_ioq_clear_before_delete',
  [('s1/ans_ro.c',
    '        write64_lo_hi(base + NVME_IOQ_CMDS, 0);\n'
    '        write64_lo_hi(base + NVME_IOQ_CQES, 0);\n'
    '        st.ioq_regs_cleared = 1;\n',
    '        st.ioq_regs_cleared = 1;\n'),
   ('s1/ans_ro.c',
    '    struct ans_cmd cmd;\n    memset(&cmd, 0, sizeof(cmd));\n    cmd.opcode = OP_DELETE_SQ;',
    '    write64_lo_hi(base + NVME_IOQ_CMDS, 0);\n'
    '    write64_lo_hi(base + NVME_IOQ_CQES, 0);\n'
    '    struct ans_cmd cmd;\n'
    '    memset(&cmd, 0, sizeof(cmd));\n'
    '    cmd.opcode = OP_DELETE_SQ;')])]

# Defense-in-depth probes: every check before the SQE is built is corrupted; the pre-doorbell
# check (ans_ro.c precheck) must still refuse, so all scenarios pass AND rw_defense reports
# window_refusals > 0 (with intact code it is 0, because the earlier checks refuse first).
DEFENSE = [('allowlist_corrupted_precheck_catches',
  [('s1/ans_ro.c',
    '    return opcode == OP_READ || (rw_enabled && (opcode == OP_FLUSH\n'
    '#if S1N_STAGE_WRITES\n'
    '           || opcode == OP_WRITE\n'
    '#endif\n'
    '           ));',
    '    return opcode == OP_READ || opcode == OP_WRITE || opcode == OP_FLUSH;')],
  'r45_raw_ro'),
 ('early_ranges_corrupted_precheck_catches',
  [('s1/native_window.c',
    '    if (!S1N_STAGE_GUEST_WRITES || !rw || !data || !range_ok(lba, blocks)) {',
    '    if (!S1N_STAGE_GUEST_WRITES || !rw || !data || !blocks || blocks > 256) {'),
   ('s1/native_window.c',
    '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS ||\n'
    '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
    '            ans_ro_abort(ANS_RO_E_ABORT);\n'
    '            return false;\n'
    '        }',
    '        (void)phys;'),
   ('s1/ans_ro.c',
    '        (write && (!rw_enabled || !write_range(lba, blocks))) ||',
    '        (write && !rw_enabled) ||')],
  'r45_direct_write_1')]

def defense_probe(root):
    results = []
    for name, mut, scenario in DEFENSE:
        out = build(root, "def_" + name, mut)
        r = subprocess.run([str(out / "test"), scenario], capture_output=True, text=True, timeout=120,
                           env=dict(os.environ, ASAN_OPTIONS="abort_on_error=1:detect_leaks=0"))
        # The case itself proves frozen state and zero subsequent I/O. Each probe is a fresh process.
        refs = [json.loads(l)["window_refusals"] for l in r.stdout.splitlines() if "window_refusals" in l]
        results.append({"probe": name, "scenario": scenario, "returncode": r.returncode,
                        "precheck_refusals": refs[-1] if refs else None,
                        "proven": r.returncode == 0 and bool(refs) and refs[-1] > 0})
    return results

def main():
    root = Path(tempfile.mkdtemp(prefix="m5w-r3-native-"))
    out = build(root, "baseline")
    results, checks = run_all(out)
    failed = [r for r in results if not r["ok"]]
    if failed or os.environ.get("S1_NO_MUTANTS"):
        print(json.dumps({"status": "fail" if failed else "pass", "source_root": str(R), "scenarios": len(results),
                          "output_directory": str(root), "passed": len(results) - len(failed), "failed_count": len(failed), "checks": checks,
                          "failed": failed, "results": results}, indent=2))
        sys.exit(1 if failed else 0)
    def run_mutant(m):
        name, mut = m[0], (m[1] if len(m) == 2 else tuple(m[1:]))
        try:
            mo = build(root, "mut_" + name, mut)
            mr, _ = run_all(mo)
            killed = [r["scenario"] for r in mr if not r["ok"]]
        except (RuntimeError, AssertionError) as e:
            return {"mutant": name, "caught": False, "by": ["<invalid mutant: " + str(e)[:200] + ">"]}
        return {"mutant": name, "caught": bool(killed), "by": killed[:4]}
    # Copies and processes are independent; bounded parallelism is optional for host qualification.
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=int(os.environ.get("S1_MUTANT_JOBS", "1"))) as pool:
        muts = list(pool.map(run_mutant, MUTANTS))
    defense = defense_probe(root)
    base_def = subprocess.run([str(out / "test"), "r45_raw_ro"], capture_output=True, text=True, timeout=120)
    base_ref = [json.loads(l)["window_refusals"] for l in base_def.stdout.splitlines() if "window_refusals" in l]
    receipt = {
        "status": "pass" if all(m["caught"] for m in muts) and all(d["proven"] for d in defense) and base_ref == [0]
                  else "mutant_survived_or_defense_unproven",
        "defense_probes": defense, "baseline_precheck_refusals": base_ref,
        "scenarios": len(results), "checks": checks,
        "mutants": muts, "mutants_caught": sum(m["caught"] for m in muts), "mutants_total": len(muts),
        "source_sha256": {str(p.relative_to(R)): sha(p) for p in
                          [S1 / n for n in S1F] + [SRC / n for n in M1N1] +
                          [WIN / n for n in ["hv_win_storage.c", "hv_win_storage.h", "hv_win_storage_native.c"]]},
        "harness_sha256": {n: sha(HERE / n) for n in ["prelude.h", "body.c", "fake_ans.c", "fake_ans.h", "run.py"]},
        "hardware_access": False, "output_directory": str(root),
    }
    (root / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    sys.exit(0 if receipt["status"] == "pass" else 1)

if __name__ == "__main__":
    main()
