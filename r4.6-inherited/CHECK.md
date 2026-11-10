# r4.6 independent check (2026-11-10)

Offline. No SSH, USB, serial or MacBook access. Evidence: `check-20261110/` (build logs and binaries, disassembly, suite logs, WinPE twin gate). All paths below are relative to this directory; source lines refer to `source/src/hv/win/s1/` unless a directory is given.

## Verdict

| Stage | Verdict |
|---|---|
| 0 (no write) | **READY** (unchanged scope; no write path, confirmed in the binary) |
| 1 (resident canary, zone C) | **READY** |
| 2 (resident canary, zone S) | **READY** — M-A is closed in code (it was the only stage-2 blocker) |
| 3 (Windows writes inside the window) | **READY for a first stage-3 hardware run**, with the procedure items in §7 |

No confinement defect was found in r4.6. No source file was changed by this check. All inherited failures — 17 native scenario failures (1/3/3/10 at stages 0–3, 13 distinct tests), 2 invalid mutant anchors and 2 stale harnesses — are category (a): policy changes the r4.6 brief asked for, or refactor anchors. Category (b), real defects: 0. Both mutants were re-anchored and are killed.

Headline evidence: native suites 49/55/55/230 scenarios pass at stages 0/1/2/3 (0 failures); stage-3 mutants 94/94 (86 inherited incl. the 2 re-anchored + 8 new split mutants), 2/2 defense probes; r4.5 mutants 44/44; overlay mutants 7/7; r4.6 follow-ups 9/9; twin-v2 confinement proof PASS with all broken variants failing; stage twin PASS; WinPE twin gate on r4.6 PASS (D1/D2/D3 clean, persistence canary). Builds bit-identical at all stages.

## 1. Inherited test failures and how they were resolved

r4.6 as delivered failed 1/3/3/10 inherited native scenarios at stages 0/1/2/3, plus `stage_inspection.py` (old function anchor), `review-tests-r4/run.py` (harness never updated for r4.5) and 2 invalid mutant anchors. Every one is category **(a)**: it asserts behaviour that the r4.6 brief changed on purpose. The brief (`briefs/r46.md`, item 2) asked for three things beyond the READ policy and splitting: C9 applied uniformly (L-1), CANARY only before `HV_START` (M-A), and the 30 s timeouts (item 3). Each failure maps to exactly one of these. In every case the original refusal is still asserted; where a refusal is now terminal and used to be followed by more checks in the same process, those checks moved to their own process (the pattern the r4.5 review used for section A).

| # | Test (stage) | Why it failed | Policy | Change (intent kept) |
|---|---|---|---|---|
| 1 | `r45_stage` (0,1,2) | guest WRITE to the read-only namespace before the stage checks now freezes (C9), so the canary/flush checks after it never ran | C9 / L-1 | probe removed from `r45_stage` (`tests/native/r45_cases.h:147`); new `r45_stage_ro_write` (`:261`) asserts NS_WRITE_PROTECTED, no ANS command, reason ABORT, frozen, no further I/O, canary refused |
| 2 | `r45_stage_corrupt` (1,2) | same probe | C9 | same split |
| 3 | `r45_link` (1,2) | the op loop consumes `P_HV_START` (0xc02) before it reaches CANARY (0xc25); CANARY after HV_START is now refused | M-A | CANARY expected refused in this loop (`r45_cases.h:103`); allowed-before-HV_START is asserted by `r46_proxy` |
| 4 | `e2e_reads` (3) | in-range WRITE to the read-only namespace now freezes, so the clean-retire checks failed | C9 | probe moved to new `e2e_reads_ro_write` (`body.c:215`): NS_WRITE_PROTECTED, no ANS command, reason ABORT, frozen, retire issues no teardown I/O |
| 5 | `allowlist` (3) | a refused physical opcode is now terminal (-2, NOT_ALLOWED) instead of -1/LIVE | C9 | each of the 11 I/O and 15 admin opcodes in its own process: `allowlist_io_0..10`, `allowlist_adm_0..14` (`body.c:355`): -2, no command, no MMIO, FROZEN/NOT_ALLOWED, nothing after; `allowlist` keeps the LIVE+READ-works half |
| 6 | `rw_requires_flag` (3) | same | C9 | 0x01 → -2 FROZEN/NOT_ALLOWED, write/flush refused, 0 writes; FLUSH (0x00) case in new `rw_requires_flag_flush` |
| 7 | `rw_basic` (3) | expected 4 ANS writes for guest writes of 1+1+256+256 blocks | split | `RW4_WRITES` = 2 + 2·⌈256/32⌉ = 18 (`tests/native/fake_ans.h:72`) |
| 8–13 | `secA_rw_write_0..5` (3) | same count after the same 4 writes | split | 4 → `RW4_WRITES`, 5 → `RW4_WRITES + 1` (`secA_cases.h:71,76,81`); refusal status, nothing-reached-ANS and outside-window checks unchanged |
| 14 | `ans_frozen_still_shuts_down` mutant | anchor text gone (shutdown now begins with `ans_ro_access_ok()`) | M-A | re-anchored to the new frozen check; the mutant un-freezes both `session_frozen` and `state` (`tests/native/run.py`). **Killed** (`capacity_mismatch`, `gpt_1`) |
| 15 | `r4_flush_timeout_1s` mutant | anchor was the old 10 s constant | item 3 | re-anchored to the 30 s constant. **Killed** (`rw_slow_flush`) |
| 16 | `stage_inspection.py` | looked for command construction in `ans_ro_xfer`, now a guard wrapper | M-A refactor | anchor moved to `xfer_owned` (same assertions). PASS |
| 17 | `review-tests-r4/run.py` | never compiled since r4.5 (missing `s1_stage.h`, `native_link.*`, `proxy.h`, `-DS1N_STAGE`, and body.c's case includes) | harness | copy list, define and include-inlining fixed; assertions untouched. It now builds and runs: 2/5 pass (`canary_faithful`, `ro_regression`); `rw_fuzz`, `direct_ans`, `oob_probe` fail **identically on r4.5 + overlay** (checked against a copy of that tree), because they assert the r4.4 nonterminal-refusal policy that r4.5 replaced and that section A re-covers. Superseded, not a defect; left failing, not edited |

The READ-refusal expectation edits r4.6 itself made (`r45_guest_read_0..3`, the `secA_rw_random` READ child, reviewer `guest_fuzz`) were re-read: each still asserts the error status, zero ANS commands, the counter, and that the session stays LIVE and serves the next READ. Every WRITE-class refusal still expects FROZEN.

**No inherited confinement check was lost.** All section-A cases pass at every stage (`secA_*`: 42 per stage in the native lists, plus the reviewer-r3 `gpt_fuzz`/`e2e_review` parts): backup-GPT cases `secA_backup_control/13/14/20..29`, GPT-reject freezes `secA_gpt_frozen_1..15`, read-only guest READ refusals, raw read-only opcodes, the six restored guest WRITE refusals, `(MAX,1)` READ, `secA_rw_random` and stray slots. The stale-bounce checks (`stale_bounce`, `stale_bounce_after_data`, `stale_bounce_after_recheck`) pass in review-r3.

## 2. Audit (code, file:line)

### M-A: submission guard — closed
- One doorbell writer: `exec_owned` (`ans_ro.c:537`). Its only callers are `exec` (`:420`) and two owned paths: the first-write re-call (`:509`) and `xfer_owned(..., recheck=true)` from `ans_ro_read_gpt` (`:951`), which is reachable only while `recheck_active` is set inside `exec_owned` under the lock. Confirmed in all four binaries (callers of `exec_owned`: `exec`, `xfer_owned` only).
- `exec` takes `exec_busy` with an atomic test-and-set (`:411`); a concurrent or nested public entry freezes with reason 29 before touching the slot. Every public entry (`ans_ro_xfer`, `flush`, `set_window`, `xfer_buffer`, `poll`, `shutdown`, `run_canary`) first calls `ans_ro_access_ok` (`:838`), which freezes on a held lock, wrong CPU or open exception session.
- Context: `s1n_storage_context` (`native_link.c:15`) = boot CPU and no exception/bark proxy session anywhere. The counter is taken around the whole `uartproxy_run` for every non-boot start (`uartproxy.c:136-138`), so it covers `hv_exc_proxy`, user interrupts and the watchdog bark on another CPU. Storage proxy ops are refused in guest-exception sessions or off the boot CPU before any dispatch (`native_link.c:44`), CANARY additionally after `HV_START` (`:66`). select/quiesce re-check it (`hv_win_storage_native.c:69,110`).
- Bytes validated under the guard: the slot and TCB are written, cleaned, then `dma_guard` (`:475`) and `precheck` (`:479`) re-read the queue memory and check opcode, NSID, LBA/NLB against the compile-time pins (`in_window :265`, `write_range :276`), PRP layout and length, all while `exec_busy` is held; nothing runs between the checks and the doorbell except counters.
- Guest traps are not proxy sessions: the BAR hook runs in the data-abort handler (`hv_win_storage.c:79`) on the single vCPU, which PREP pins to the boot CPU (`hv_win_storage.c:117`).
- Note (no defect): `xfer_owned` builds the PRP list before `exec` takes the lock. A same-CPU nested entry in that gap is impossible (the trap and proxy paths run with interrupts masked; the tick only calls `poll`) and would be caught by `dma_guard`'s PRP-list check anyway. The LBA is only ever trusted from the locked re-read.

### Stage-3 guest write path — correct
Guest WRITE (`hv_nvme_queue.c:624`): reserved fields (`:628`), NLB ≤ MDTS 256 else abort (`:631`), `lba < nsblocks && blocks ≤ nsblocks − lba` else abort (`:636`, no overflow: subtraction after the first test), PRPs planned once against guest RAM (`plan_prps_dir :246`, every page through `hv_storage_guest_span`, no queue/PRP-list overlap) and copied **once** into the private transfer buffer (`:645`); the pieces are written from that buffer, the guest is never re-read. Then per piece: `s1n_write` range check (`native_window.c:254`, `range_ok :211`), translation and re-check against `S1N_PIN_*` (`transfer :218-224`), `xfer_owned` check (`ans_ro.c:855`), and the locked slot re-read (`precheck`). Namespace size equals the pinned window (`S1N_PIN_BLOCKS`), checked at PREP.

Other write-class commands: WRITE ZEROES (0x08), DSM/trim (0x09), WRITE UNCORRECTABLE (0x04), vendor opcodes → Invalid Opcode at `hv_nvme_queue.c:684`, nothing forwarded (ONCS = 0). FLUSH (`:666`) → canonical NSID-1 FLUSH with no address fields (checked in `precheck`). FUA / WCE=0 → the checked writes then FLUSH (`:655`). Guest admin commands are emulated, never forwarded. Physical admin is limited to the exact Create/Delete queue shapes (`dma_guard`).

**Splitting (`hv_nvme_queue.c:646-653` write, `:705-713` read).** Pieces are `[lba+done, lba+done+n)` with `n = min(32, blocks−done)` and `done += n`: they tile the request exactly — no gap, no overlap, last partial piece = `blocks mod 32`. Every piece is inside the already-checked request, and each is independently re-checked three more times against the pins, so no piece can leave the window. The guest page list is resolved once for the whole command into a linear private buffer; pieces index that buffer (`done·4096`), so 4 KiB guest pages vs 16 KiB host pages and PRP1 offsets do not interact with the split. `s1n_write` moves piece k to the DMA buffer start with a non-overlapping `memcpy` (source offset ≥ 32 blocks ≥ n). Reads land in the DMA buffer and are assembled in a private 1 MiB `read_result`; the guest sees data only after every piece succeeded.
- **Mid-request error:** any failed piece freezes the session (timeout `ans_ro.c:546`, any nonzero status `:597`, any refusal → `ans_ro_abort`), the guest command completes with Internal Error, and every later command fails with no ANS access. Pieces before the failure are in the device write cache, never flushed by the resident (no FLUSH can follow a freeze); they may or may not reach the medium, which is the NVMe-allowed "indeterminate" state for a failed WRITE. All of them are inside the window.
- **Flush ordering:** synchronous — every piece has completed (CQE consumed) before the next command or FLUSH is issued; FUA issues exactly one FLUSH after the last piece.
- **New tests** (`tests/native/chk_cases.h`, stage 3): `chk_split_diff` — 1,500 random operations against a flat byte-array reference (two 8 MiB regions at the window head and tail, random data, random scattered guest pages with random PRP1 offsets, lengths biased to 1/31/32/33/63/64/65/255/256, FUA, flushes), asserts exact read-back, ⌈n/32⌉ ANS writes per command, blocks sum = n, max NLB 32, final medium equal to the reference, nothing outside. `chk_split_error_{1,2,5,8}` — the k-th piece of a 256-block write fails: Internal Error, exactly k ANS writes, FROZEN/E_STATUS, cache holds exactly 32(k−1) blocks, nothing durable, no later ANS access. `chk_split_fua` — 8 writes then one FLUSH. 8 split mutants (offset, gap, dropped last piece, continue-after-error, 64-block cap, read offset, read gap, READ refusal reaching the backend) are all killed.

### READ refusal — correct, read-only
`hv_nvme_queue.c:691-694`: only in the READ branch, before the MDTS check, counts `refused_reads` (`storage_stat(12)`, `hv_win_storage.c:172`), returns before any plan or backend call, so nothing is read from anywhere. WRITE range checks (`:631,:636`) are unchanged and terminal; the lower-layer READ guards (`s1n_read`, `xfer_owned`, `precheck`) stay terminal. Status is Invalid Field in Command (SCT 0 / 0x02), not the more specific LBA Out of Range (0x80). Both map to a non-retried ILLEGAL REQUEST in stornvme, so the probe fails cleanly either way; it was left as is (the inherited suites assert 0x02, and r3.4 used it on hardware for the same probe). Low.

### 30 s timeouts
`ans_ro.c:60-61`; the wait loop (`:544-556`) is bounded by `timeout_expired` on the architectural counter and drains the mailbox each turn, so it cannot hang. Expiry freezes (reason CMD_TIMEOUT), a failed WRITE/FLUSH status freezes (`:597`). The wait runs inside the guest trap with interrupts masked: the HV tick (`hv.c:582`) does not run, so the guest's single vCPU and the KD/PL011 poll stall for up to 30 s per command. The hardware WDT is disabled at boot (`main.c:168`). The HV watchdog (1 s) would bark during any wait over 1 s, but it is only armed by `P_HV_WDT_START`, which the launcher refuses (rule L8) and which the lock refuses after select.

### smp2h crash fix — exact
`src/mcc.c`, `src/mcc.h`, `src/exception.c` are byte-identical to `storage/s1-resident-smp2h-20261110/source/` (cmp), and r4.5's copies equal smp2g's, so the port is exactly the smp2h delta. smp2h's patch touches no other file.

## 3. Changes made by this check

Tests and harness only (no source change):
- `tests/native/r45_cases.h`, `body.c`, `secA_cases.h`, `fake_ans.h`: §1 rows 1–13.
- `tests/native/run.py`: new scenario names (§1), 2 re-anchored mutants, 8 new split mutants, `chk_*` scenarios.
- `tests/native/chk_cases.h` (new), `fake_ans.c`: opt-in `chk_fail_write_n` (fail the n-th ANS WRITE) and `chk_max_write_nlb` (default 0 = old behaviour; the twin's `patch_fake_v2.py` still applies).
- `tests/native/stage_inspection.py`, `review-tests-r4/run.py`: harness re-anchoring (§1 rows 16–17).
- `check-20261110/`: build and suite scripts, logs, binaries, disassembly, WinPE twin copy (`winpe-gate/twin-v2`: r4.5+ build flags, `proxy.h`, heap stubs, absolute batch paths; NTFS batches).

## 4. Binaries

`S1N_STAGE=n bash build.sh check-20261110/build/stage<n>-build<k>` (k = 1, 2), the documented `container run … m5pro-windows/mu-build:f43` (image 66ebcb6da215), each from clean; librust.a `4ed52ac9…efc304` copied from r4.5. Only make's clock-skew warning (librust.a mtime 2030). **Build 1 = build 2, bit-identical, for the .bin, both ELFs and build_tag.h at every stage.**

| Stage | m1n1-win-s1-r4.6-stageN.bin sha256 | m1n1-raw.elf | m1n1.elf | size (r4.5) |
|---|---|---|---|---|
| 0 | `c9ec224097d5fc4374a0fcecced303f39e820f361888253b16bfbe4921d95e54` | `2f09ff552735a1a308efa69c94a915b1460cfdf4cb06e71f6e2cb2e6ad8798cb` | `883d04dd6f63236154bb412786740f890e2536c3697d3a466815ee49bf31f4af` | 7,143,424 (6.09 MB) |
| 1 | `4ec93fb025ddd84cf040cf1e1540df38ce386eaafb2badd9087f0e6c38590086` | `9e822f5fdd71df1bcccc8eeae50b2ad41995c91d6ee7607451580f047ea0bae6` | `ac990f3bfdd63a5e68c7394690b25cfa77a51bc1e99bff7b83a3bf7d87119426` | 9,240,576 (8.19 MB) |
| 2 | `3a57e0315955f12238f8fec06c956ced98f6bfaa5e643a5a5b6d3a1960672aef` | `e7290e605e8a7418d1f20a13f6e224918b837446ff7c6b3da899b32ac1b0658b` | `3c730fca298358576ae66952d34d96db48487a028241b63bfbb7e8f861bd0884` | 9,240,576 (8.19 MB) |
| 3 | `b9a43b72a2a743f8e2194c244ac44754489f918e8cad56151dc329e4227b0311` | `fccb023dff8dc70c9e4e0c9c16c332753fdbb785a2e46eaeb7d07e7edfc963db` | `a50cff0d1f409b513151cac99b5c0761e72c67c069718968c18485cb910213f7` | 8,192,000 (7.14 MB) |

Every image is exactly 1 MiB larger than r4.5 (the split READ assembly buffer). `_end` = 0x6d0000 / 0x8d0000 / 0x8d0000 / 0x7d0000. `build_tag.h` = `v2.0.0-169-g868807f9-dirty-s1r4.6-stageN`.

**Binary checks** (binutils objdump/nm from the build image; Homebrew llvm-objdump 23 crashes on these ELFs):
- Stage 0: `write_range`, canary and recheck buffers absent; `s1n_writable` = `mov w0,#0; ret`; `s1n_write` only calls `ans_ro_abort(23)`; `ans_ro_run_canary` = access check then return 0; `xfer_owned` stores only opcode 2; `exec_owned` admits I/O opcode 2, or 0 when `rw_enabled`, everything else → `freeze(15)` before the slot is filled; admin opcodes 0/1/4/5 only. No WRITE path.
- Stage 1 (and 2): the only `write=1` call of `ans_ro_xfer` is in `ans_ro_run_canary`; `s1n_read`, `ans_ro_read_block` and `ans_ro_read_gpt` pass 0. `s1n_write`/`s1n_writable` are the refusing stubs. Stage-1 `write_range` = `window_set && lba−174217216 ≤ 0x1ff && n == 1 && canary_running`. Canary only.
- Stage 3: `ans_ro_run_canary` is a stub, no canary buffers; `transfer` carries 174213120 / 24424453 / 198637574.
- All stages: callers of `exec_owned` are `exec` and `xfer_owned` only; `exec` callers are flush, init×2, shutdown×2, `xfer_owned`; partition and disk GUID occur once; the stage marker `M5W-S1N-R4.5-STAGE-N` (inherited string) occurs once with the right N.

## 5. Test results

| Suite | Result |
|---|---|
| Native stage 0 / 1 / 2 | **49/49, 55/55, 55/55** scenarios; 11,903 / 2,105,637 / 67,006,245 checks (r4.6 as delivered: 47/48, 51/54, 51/54) |
| Native stage 3 | **230/230**, 32,076,621 checks; **mutants 94/94**; defense probes 2/2 proven; baseline precheck refusals [0] (as delivered: 186/196, 84/86 with 2 invalid) |
| Split mutants (new, in the 94) | 8/8 killed: by `rw_basic`, `rw_chunked`, `rw_random`, `chk_split_diff`, `rw_write_error`, `r45_guest_read_*` |
| `r45_mutants.py` (r4.5 targeted, stages 0–3) | **44/44** |
| `r46_run.py` (r4.6 added, 140 scenarios / 23 mutants) | pass |
| `r46_mutant_followup.py` | 9/9 |
| Overlay mutants (`r46_overlay_mutants.py`) | **7/7** |
| `r46_context_path.py`, `r46_mcc.py`, `link_path_test.py`, `stage_inspection.py`, `r46_stage_inspection.py` | pass |
| `run-all-tests.sh` (non-native part, after the test edits) | rc 0: controller 9,746; memory 420 + 33,197; S0 5,719,795; S1 9,346; handoff + 5 mutants; r2 300,414 + 36 mutants; review 15,177,307 + oracle 4/4; confirm + B3 5/5; gap mutants 16/19 (the same 3 survivors as r4.4/r4.5); UEFI e2e 212/178/62; review-r3 (60,000 fuzz, stale-bounce ×3) pass; decoders pass |
| `review-tests-r4/run.py` | builds now; 2/5, same 3 failures as r4.5 (superseded, §1 row 17) |
| twin-v2 `twin_proof.py` (local r4.5/4.6 adaptation) | **PASS**: unmodified resident passes, all broken variants (M1–M4 retargeted) FAIL on D1/D2/D3 |
| twin-v2 `stage_proof.py` | **PASS** (stage 1/2 canaries restore zone C/S, 4 loosened stage edges detected, stage 0 write refused) |
| **WinPE twin gate** (`check-20261110/winpe-gate`, real WinPE on QEMU/HVF, NVMe = r4.6 storage stack compiled natively + the v2 fake that stores any write anywhere; 1 TB sparse image) | FAT batch (as the r4.4 gate): diskpart `clean` + `convert gpt` + partition + format + file write; reboot, file read back. **PASS**: D1 (every block outside the window identical, 219,851,811 blocks hashed), D2 0 outside writes in the command log, D3 0, S1WC all writes inside, persistence canary found. Big-copy variant (11.4 MB file, `mountvol /p`): D1/D2/D3 clean, canary persisted, 32-block split writes and reads seen; **bulk data did not persist** — see below |

Notes on the twin:
- The twin runs the C source natively, not the aarch64 binary; it is the same tree the binaries were built from (no source change since the build).
- NTFS could not be tested: `format fs=ntfs` stalls in the twin, and the big-file copy cannot be flushed. Cause, from the decoded S1DG ring: the **twin's** guest-memory provider (QEMU `guest_span`) refuses a 4 KiB page of a 256-block transfer, the frontend then latches CFS (fatal reason CQE), and Windows resets the controller in a loop. **r4.4 shows exactly the same thing** (same NTFS batch: 12 writes, 6 PROVIDER refusals, CQE fatal), so it is a twin limitation that predates r4.6, not a regression. Windows-scale bulk I/O integrity therefore rests on the native differential test (§2) and, on hardware, on RUN 2's large reads having worked with the real provider. Watch `fatal_count` / CFS in S1DG on the first run (§7).
- `fc`, `certutil` are absent from this WinPE, so in-guest byte comparison of the big file was not possible.

## 6. Remaining / open
- **L-5 image self-hash at select: open** (unchanged from r4.5). The launcher's trust up to select remains; the launcher must bind the exact stage-3 image hash above.
- READ refusal status 0x02 instead of 0x80 (Low, §2).
- No physical evidence yet for: ANS behaviour with 32-block WRITEs at Windows' rate, FLUSH latency (the 30 s bound), MMU/SError behaviour of the smp2h unmapping.

## 7. First stage-3 hardware run: what to watch

Procedure prerequisites:
- Load the stage-3 image with the hash above; take symbol addresses from `nm` of the **stage-3** ELF (they moved: +1 MiB). PREP's `resident_size` must cover `_end` = 0x7d0000.
- No HV watchdog (`P_HV_WDT_START`), no `P_HV_PIN_CPU` to a non-boot CPU (the tick would call `poll` off the boot CPU and freeze with reason 30), single vCPU on the boot CPU (PREP enforces).
- Storage STAT ops are now refused inside a guest-exception session (e.g. after a Ctrl-C user interrupt). Read stats only from the boot proxy before HV_START or after the guest exits; a refused STAT is not a fault.

Live signals:
- `native_stat(2)` state/reason: any FROZEN ends the run. New reasons: 29 REENTRY (should never happen), 30 CONTEXT (procedure error), 22/E_CMD_TIMEOUT (a WRITE/FLUSH > 30 s: the medium state of that command is indeterminate).
- S1DG `fatal_count` / CSTS.CFS: must stay 0. A provider refusal followed by a CQE fatal is what the twin shows for its own memory provider (§5); on hardware it would mean the real guest-memory provider refused a guest page.
- `storage_stat(12)` refused READs: small and stable is normal (partition/VDS probes past the end); a steadily climbing count means Windows' view of the disk size is wrong.
- S1WC `abs_min_first`/`abs_max_last` must stay within 174213120 … 198637573; `ans_writes` ≈ `blocks_written`/32 or more.
- Windows: watch for a DPC/clock watchdog bugcheck or storport timeouts when a FLUSH is slow, because the vCPU is stalled for the whole synchronous wait.

**Before/after whole-disk verification.** With Windows legitimately writing, the window's contents will change arbitrarily, so a whole-disk hash must be split: (1) every block **outside** 174213120 … 198637573 must be bit-identical before/after (the C2 verifier's outside mode) — this is the confinement proof and is unaffected by Windows' activity; (2) the primary and backup GPT (LBAs 1–5 and NSZE−5 … NSZE−1) must be unchanged, and partition entry 7 must still match the pins; (3) the window itself can only be checked for plausibility (Windows' own GPT at window LBA 0–5 and its backup at the window end, NTFS boot sector), not for equality. What it will **not** show: a wrong write that landed inside the window (data corruption of Windows' own files, e.g. from a split or PRP bug) — that shows up only as Windows/chkdsk errors; and anything about writes that were still in the SSD's volatile cache if the run ends in a freeze or power cut (do a clean guest shutdown so the final FLUSH runs, then quiesce). SMART "data units written" will be large and is not a useful confinement signal in stage 3.
