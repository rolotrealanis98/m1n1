# r4.6 execution notes

Host-only. Firmware binaries: **not built**. Hardware behavior: **unverified**. No network, installation, container, hardware or upstream action. All writes are inside this package; temporary files use scratch/.

Overlay applied after verifying OVERLAY-BASE.sha256 by copying overlay/. over the parent copy. Build outputs, caches and prior logs excluded. Font/logo .bin source assets retained; compiled librust.a excluded. Owner override source/AGENTS.md copied byte-for-byte. Initial copy encountered dangling image symlinks; fresh copy preserved symlinks without following them.

**Deliberate policy change:** out-of-range guest READ returns normal guest error status and is counted, without freezing. Out-of-range WRITEs and other confinement violations remain terminal.

Global skills/memories outside win-el2 were not read due to the explicit read boundary.

## Decisions and limits

- **READ policy exception (prominent):** a well-formed guest READ outside the virtual namespace returns `VNVME_SC_INVALID_FIELD`, increments `controller.queue.refused_reads` (exported as `storage_stat(12)`), performs no backend command, and leaves the session LIVE. Range checking precedes the READ MDTS check, including a past-end request with NLB=65535. Direct backend/absolute READ violations remain terminal: only guest disk probes receive this exception. Guest out-of-range WRITEs remain terminal.
- The requested 32-block cap is interpreted as a cap on each backend request produced by a guest command. Valid guest requests up to the existing advertised 256-block MDTS are accepted and split by the emulated controller. Stages 1/2 retain their 128-block resident canary chunks and exact write caps. The frontend assembles a split READ in a private 1 MiB static buffer so later backend reads cannot overwrite earlier chunks. This adds 1 MiB BSS; final firmware layout is **unverified** because binaries are **not built**.
- C9 is applied to the three local L-1 paths: physical opcode allowlist refusal, above-cap frontend transfers, and any WRITE to a read-only native namespace. A valid-range oversized READ still freezes; an out-of-range guest READ is the explicit exception above. Unsupported optional guest commands still return Invalid Opcode and never reach ANS.
- M-A uses an atomic submission lock and a new terminal reason (29, REENTRY); wrong CPU or exception-proxy context freezes with reason 30. All proxy storage operations are gated before dispatch, including select/PREP/stat/quiesce/CANARY. CANARY additionally requires no prior HV_START. Normal guest traps are not proxy sessions. The proxy context counter covers the entire native/lower-exception/watchdog session, nesting and every return path; it conservatively excludes storage on all CPUs while any such session exists.
- The first-write GPT recheck is an explicit private operation under the same submission ownership. Only its GPT reads call the private owned transfer/submission path; public nested entry freezes before touching the shared DMA buffer. After the recheck the pending command is rebuilt, cleaned and checked again under the retained lock. A failed recheck does not restore the DMA buffer while frozen.
- L-3: each canary chunk is saved, read again and compared before any pattern WRITE. This is the requested two-read consistency check; it does not prove the medium returned correct data if both reads are identically wrong.
- L-5 image self-hash at select remains **open/unverified**. The review gives no complete digest placement/trust scheme. With no linked image or firmware build, adding a trusted expected digest, immutable covered ranges and relocation/exclusion rules is not a small local change. Pre-select launcher/image trust remains an explicit boundary.
- smp2h was ported exactly for `mcc.c`, `mcc.h`, `exception.c`; all three match the supplied smp2h versions byte-for-byte. Native mock tests exercise the exact ADT carveout functions, not hardware MMU/SError behavior.

## Conflicting acceptance instructions — no assertions weakened

The task requires new C9 semantics and 32-block splitting, but also forbids changing any existing test expectation except guest READ refusal. These requirements conflict with tests explicitly asserting the old nonterminal C9 behavior or four physical writes for two 256-block plus two one-block requests. The old expectations were preserved, failures recorded, and new tests added for the requested behavior. Therefore this package is **not an all-green stage-3 release**. The coordinator must reconcile those assertions before claiming acceptance; this execution does not silently treat them as passing.

The inherited native suites currently retain 1/3/3/10 failures at stages 0/1/2/3. The original stage-inspection script also fails because it looks for command construction in `ans_ro_xfer`, now a guarded wrapper; an additional inspection checks the new `xfer_owned` body with the same assertions. The legacy `review-tests-r4/run.py` fails to compile on both the copied r4.5-with-overlay baseline and r4.6 because its copy list omits `s1_stage.h`; it is not claimed as passed.

## Existing test expectation edits (only the authorised READ change)

1. `tests/native/r45_cases.h`, `r45_boundary`: only `r45_guest_read_0..3` now assert LIVE, a counted refusal and a successful following READ. Direct reads and all writes retain frozen assertions.
2. `tests/native/secA_cases.h`, the invalid-READ child in `secA_rw_random`: retain refusal/no-command/outside-window assertions; replace frozen expectation with LIVE, counter and successful service. Invalid WRITE children are unchanged.
3. `review-tests-r3/e2e_review.c`, `guest_fuzz` invalid-READ branch: retain error/no-backend-command checks; expect LIVE and a succeeding READ instead of FROZEN.

All other changes under tests are additions or infrastructure: `body.c` includes/dispatches the added r46 cases; `fake_ans.c` adds opt-in fault hooks and a virtual clock step (zero/default preserves its old behavior); two shim headers support the new platform predicate; `r46_*.py`/`r46_cases.h` are additional checks. `tests/sync-model.py` (unchanged) refreshed the S0/S1 source mirrors and their hash manifests to test the actual implementation. The mandatory overlay replacements were applied unchanged before implementation; they are separately identifiable in CHANGED-FILES.tsv. No existing assertion was removed to obtain a pass.

Mutants are counted as caught only when a passing unmutated test newly fails. A pre-existing incompatible test failure is never counted as a mutant kill. Invalid text-replacement anchors are reported separately, not counted as killed mutants. Receipts preserve every failure and additive follow-up result.

## Final packaging audit

A source-tree executable scan found an inherited `source/tests/win-vgic3/test_vgic3` host binary and its dSYM inside the copied source tree. They were not executed and have been removed from the deliverable and baseline/patch-check copies. Generated UEFI host-test outputs were relocated to `scratch/uefi-out-flag0` and `scratch/uefi-out-flag1`; source/test caches were removed. Font/logo binary assets remain. The delivered source contains no ELF/Mach-O/archive executable artifacts.

Final inherited native mutation accounting: 84/86 are caught by passing original or added cases. `ans_frozen_still_shuts_down` and `r4_flush_timeout_1s` have stale exact-text anchors and are invalid, not killed. The new 30-second timeout tests/mutants cover the changed timeout policy; frozen-session tests verify no subsequent MMIO. All 44 additional r4.5 mutants are caught with the added passing policy cases. The independent overlay matrix catches 7/7. The original three gap-mutant survivors are unchanged (16/19 caught).

During development, the added canary-restoration oracle initially compared two views of the post-write fake medium. A surviving restore mutant exposed that weakness. The added test was strengthened to snapshot the medium before the canary; the restore mutant is now caught. No inherited test was relaxed.
