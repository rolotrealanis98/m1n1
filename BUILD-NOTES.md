# r4.14 = r4.13 + READ may repeat a whole destination page; PRP refusals logged by rule (2026-11-10)

Offline. No MacBook/SSH/USB. Lean copy of r4.13 (its notes: `BUILD-NOTES-r4.13.md`). Owner-override
`source/AGENTS.md` unchanged, librust.a `4ed52ac9…c304` reused. Disk-window confinement, LBA checks, the WRITE
path's checks, guest-memory admission (`hv_win_storage_mem.c`), pins, stage logic and the link lock are unchanged.

## The refused READs (r4.13 RUN B `20261110-211000`: 31 of ~51,000; `20261110-212026`: 248)

Every one is an I/O READ (op 0x02, nlb 4..48) completed 0x4. In both sessions the model's refusal counter and the
admission counters stay **0** (`STORAGE EVENT ... refusals 0`, NV `admission refusals` empty, `refused 0`), and the
backend reports `rerr 0`. Every guest-memory admission call from the planner goes through `hv_nvme.c:114`, which
counts a refusal, so **admission did not refuse these**. 0x4 for a READ has one source,
`hv_nvme_queue.c:765` (r4.13 `:702`): `plan_prps_dir` returned false on one of its non-admission rules: PRP2
zero, PRP2/list/entry alignment, more than 2 list fragments, a data page over a queue, over the PRP list, or
**over an earlier data page of the same command** (r4.13 `hv_nvme_queue.c:304-305`).

**Root cause (inferred, not proven from captured PRP lists):** the last rule. Windows' memory manager builds
clustered in-page reads with the pages of the cluster that are already resident pointed at **one system-wide
dummy page**, so one PRP list names that page several times. r2.1 required disjoint destinations and refused it.
Evidence: (1) all are multi-page reads with odd cluster lengths (12, 14, 21, 25, 30 blocks), reissued once with
the same SLBA (Windows retry); (2) one PRP1 recurs across *different* commands: `0x10b599b0000` for #33 (slba
696061) and #35 (slba 1522707), and `0x1001f600000` for **20 of the 50** logged errors in `212026`. That fits a
read that starts on the dummy page and names it again later. The other rules would need stornvme to build broken
PRPs, or point data at its own queue or list, while ~51,000 similar reads succeed. This explains all 31 (and the
248) only as an inference. The PRP lists were not captured. The next run's `PRP-REFUSE` lines and the
`dup-dest reads` counter will confirm it or name the rule that fires.

## Fix (`source-vs-r4.13.patch`, 4 files +168/−16, sha256 `0d1d325b…`)

| file:line | change |
|---|---|
| `src/hv/win/s1/hv_nvme_queue.c:325-343` | in the device-to-guest direction (READ, Identify, logs) a data segment may be an **exact repeat of an earlier whole, page-aligned segment** (same address, both 4096 bytes). The last copy wins. Each segment is still admitted on its own (`guest_span`, write: resident, protected, holes, MMIO, s2 attributes, PMU re-arm). It still may not overlap a queue or a PRP list fragment. Partial overlaps (unaligned PRP1) and **every overlap between WRITE source segments** keep the 0x4 refusal. |
| `hv_nvme_queue.c:247-322`, `:349-375` | `PRP_AT` notes the rule before each planner check (no existing check line changed). `plan_prps_dir` (now a wrapper around `plan_prps_body`) counts refusals by rule. It also keeps the last 16 in `ctrl->prp_log` (rule, segment, address, length, conflicting base, cid, opcode, direction) and counts READs with repeated destinations (`prp_dup_dest_reads`, last PRP1). |
| `hv_nvme_queue.h:214-243, 275-280` | `enum vnvme_prp_rule`, `struct vnvme_prp_refusal`, the counters. `_end` unchanged at 0x7d0000. |
| `src/hv/win/hv_win_status.c` | the `NVME-ERR` line appends `prp rule <name> seg addr len other`. A new line, `HV/win r4.14 PRP-REFUSE #n cmd cid op dir rule seg addr len other`, is printed for the first 64 refusals and then every 256th; refusals that wrap out of the log are counted. The NV line appends `prp refusals <rule>:<n>` and `dup-dest reads N (last prp1)`. |
| `hv_exc.c`, `hv_win_status.c` | the console tag is now `HV/win r4.14`. |

## Tests (`tests-vs-r4.13.patch`, sha256 `a78e5b61…`)
- **Full tier (`fbtest.py full`): 28/28 PASS** (`full-tier.out`). It includes S0 5,719,797 checks, S1 9,346, handoff
  63,306, review 15.2 M, confirm 3.5 M, r2 + mutants, native stage 0-3, r46-run and uefi-e2e.
- **Mutants tier: 11/11 PASS** (`mutants-tier.out`): native 231 scenarios, 32.1 M checks, mutants **96/96** (r4.13: 94 + the 2 new), twin-proof and stage-proof pass, b3 5/5, oracle 4/4, gap 16.
- **New tests.**
  - r2 `test_r2.c`: the refusal log records rule, address, segment and order for 4 existing refusals.
  - r2 `test_r2.c`: a repeated destination page succeeds (last copy wins, the skipped page is untouched), including
    PRP1 repeated in the list.
  - r2 `test_r2.c`: a repeated page outside RAM is still refused (data-admission), and a partial overlap is still
    refused (data-overlap).
  - native `r414_dup_pages` (stage 3, rw backend): the READ with a repeated page succeeds. A WRITE with a repeated
    source page gets 0x4 with rule data-overlap, nothing reaches ANS, and media and the outside of the window are
    unchanged.
  - 4 new mutants (r2: dup refused, rule not recorded; native: WRITE overlap allowed, READ dup refused).
- **Changed oracles (2, not deleted).** Both asserted that an exact repeat of a READ destination page is refused:
  - S0 `test_s0.c:300` (entries `{DATA+PAGE, DATA+PAGE}`);
  - handoff `handoff-tests.c:106` (PRP1 repeated in the list).

  Both now assert success with the data placement checked. The handoff case also adds the unaligned/partial
  variant, which is still refused. The neighbouring partial-overlap case (S0 `:302`) and every list, queue and
  admission refusal are unchanged. Reviewers should know this is a deliberate policy change: duplicate READ
  destinations are now accepted.

## Build (`S1N_STAGE=3 ./build.sh`, same container/make/tag scheme as r4.13; two builds bit-identical)
| file | sha256 |
|---|---|
| `artifacts/m1n1-win-s1-r4.14-stage3.bin` | **`80450014e74230c0f1f804d0b03362e36d1f08a25ca1b6fefff66653fb779df3`** |
| `artifacts/m1n1-raw.elf` | `6f45d63e5d4e69809106842e3fc24e555e43c2685e58a593a58cdb129338b4a0` |
| `artifacts/m1n1.elf` | `5063f2882ffb363757f952876feee3a0b0afc8aaa86061faf9cc15504b1e45e8` |
| `artifacts/build_tag.h` | `3c734e9b9e338d375257aa97b85b74ec9ed234f35a3a1c0c7b7a001a98ad15ff` |

Tag `v2.0.0-169-g868807f9-dirty-s1r4.14-stage3`. Run B layout: check_len 0xd53d0, 249 relocs.

## Run-B rebind (`runB-rebind/`: `before/` = r4.13 files + `BEFORE-SHA256`, `after/`, `rebind.patch`)
- `bin/m1n1-win-s1-r4.14-stage3.bin` was added. `bin/image-layout.json` was regenerated with
  `launcher/mk_image_layout_b.py`; r4.13's copy is kept as `bin/image-layout-r4.13.json`.
- `run_b.py`:
  - PIN bin/sha/version now point at r4.14 (were r4.13 `bd448da4…0c34`, `…s1r4.13-stage3`);
  - proxyclient and the doc string point at the r4.14 directory;
  - `STATUS_ST`/`STATUS_TAGS` now match `HV/win r4.14`.
- `launcher/run_windows_b.py:100` `SOURCE_ROOT` and the `:1415` record string now say r4.14.
- `tests/test_run_b.py`, `tests/fake_launcher.py`: the r4.14 strings and prefix.
- `SHA256SUMS` was updated (all OK) and a note was added to `RUNB-READY.md`. `run-b.sh` is unchanged (`2d18f8ed`),
  so the command is the same.
- `test_run_b` 19 OK, `test_storage_contract_b` 16 OK, `test_os_memory_b` 4 OK, `test_resilient_upload` 9 OK.
- **Revert:** copy `runB-rebind/before/*` back to their runB paths (`image-layout.json` → `bin/`;
  `run_windows_b.py`, `storage_contract.py` → `launcher/`; `test_*.py`, `fake_launcher.py` → `tests/`).

## The 0x1A bugcheck in `20261110-212026` (lead, not established)
`MEMORY_MANAGEMENT (0x404, 0xFFFFF03FFE092370, 0x00200100D2800003, …)` at t+67 s came after 248 refused READs (40
in the last minute). A user-mode heap corruption (`c0000374`) appeared at t+38 s. The same session shows the same
signature: model/admission refusals 0, and PRP1 `0x1001f600000` repeated 20 times. So the refusals there have the
same cause as above.

**Lead:** the 0x1A is a consequence of in-page I/O failures on the memory manager's paging path (failed clustered
reads, retries and error paths), not the resident altering guest memory. A refused command copies nothing. The
resident writes guest memory only through admitted plan segments and CQEs. PA 0x100D2800000 appears in no logged
command. The exact meaning of subcode 0x404 is unverified. Rerun r4.14: if the refusals vanish and 0x1A does not
recur, the lead holds.

## Unverified
- The dummy-page root cause is inferred (no captured PRP lists).
- Not run on hardware. Not independently reviewed: the change touches only the READ/guest-destination direction;
  the WRITE checks are unchanged and covered by the new test and mutant.
