# r4.7 = r4.6 + pmu-reads (2026-11-10)

Offline. No MacBook, SSH, USB or serial access. `source/AGENTS.md` is the owner override, byte-identical to r4.6's.
The r4.6 documents that came with the copy (CHECK.md, NOTES.md, REVIEW-GUIDE.md, TEST-RESULTS.json, ...) are in
`r4.6-inherited/`; they describe r4.6 and remain valid for everything except the five files below.

## What changed (and nothing else)

Applied per `tooling/pmu-reads-20261110/NOTES.md` §7: `pmu-reads.patch` (`git apply --check` clean; BSD `patch` prints
the known harmless "No such line 274" for `hv_win.h`, offsets −41/−1/−33) and `check-memory-r4.6.patch`. Full diff:
`source-vs-r4.6.patch` (sha256 `8c38e0c0…6e62`), `check-memory-vs-r4.6.patch` (test + build.sh).

| file (r4.7 line) | change |
|---|---|
| `src/hv/win/hv_win_storage_mem.c:118-120,154,158,161,165` | admission: a tracked leaf is treated as RW (`g = d \| S2AP_W`) unless it is SCANNING (`XN && !S2AP_W` → refused) |
| `src/hv/win/hv_win_storage_mem.c:171-176` | `if (write) hv_win_pmu_dma_write(ipa, length);` after every check, before the pointer is returned (before EL2's copy) |
| `src/hv/win/hv_win_pmu.c:397-426` | new `hv_win_pmu_dma_write`: no-op unless armed; CODE/PINNED → DIRTY via `hv_xt_set` (flips kept), one broadcast `hv_xt_tlbi(true)` if anything changed, stat `HV_WIN_PMU_DMA_RESETS`; `:461` report prints "device refills N" at guest exit |
| `src/hv/win/hv_win.h:237,255` | stat appended last (indices unchanged), prototype |
| `src/hv/win/hv_win_storage_mem.h:44-51` | header contract text |
| `proxyclient/tools/run_windows.py:111,766` | `dma_resets` in the PMU stats tuple / RUN.txt line |
| `tests/win-pmu-rewrite/test_pmu_rewrite.c` (+146), `check-memory.py` | new tests (PMU refill cases; provider CODE/PINNED admitted, SCANNING/foreign/RO refused, reset called only for write admissions) |
| `build.sh` | tag/filenames r4.6 → r4.7 only |

`src/hv/win/s1/` (window, ANS driver, NVMe queue, native link) is byte-identical to r4.6 (`diff -rq`: no output).
`src/mcc.c`, `src/mcc.h`, `src/exception.c` are byte-identical to smp2h: **the carveout-unmapping fix for the
intermittent early EL2 SError is present** (r4.6 ported it; `r46_mcc.py` passes).

## Review of the admission change (`hv_storage_guest_span`, r4.7 `hv_win_storage_mem.c:115-178`)

Can a READ or WRITE now touch anything but guest RAM? **No.** Everything that decides *where* still runs first and is untouched:
- lease state + dispatch context (`:121-126`), non-zero length ≤ `HV_SM_MAX_SPAN`, no wrap (`:127`);
- the whole span inside one registered guest RAM range (`:129-136`); no overlap with the resident interval or any of the
  launcher's protected ranges (resident/heaps, ADT, boot args, FD + RAM disk, Mu handoff, holes, MMIO below RAM, synthetic
  frames, framebuffer, FB shadow) (`:137-142`);
- per 16 KiB leaf: must resolve in stage 2, identity IPA == PA, 16 KiB or 32 MiB leaf, Normal WB inner-shareable with AF,
  no unknown bits (`:161-164`); a tracked bit is only allowed on 16 KiB leaves, flips only on tracked leaves (`:165-166`).

What changed is only *which PMU states of an already-owned RAM page* pass:
- untracked leaves: `g == d`, the old "exactly RW" test is unchanged (untracked RO / none / WO still refused);
- tracked leaves: S2AP RO(01) or RW(11) pass the RW test after `| BIT(7)`; S2AP none(00)/WO(10) become 10 and are refused;
- DIRTY (RW+XN), CODE (RO+X), PINNED (RW+X) admitted; **SCANNING (RO+XN, the in-transition state) refused** (`:165`);
- the call runs under the big HV lock, the same lock as the fault handlers and the rescan, so no page changes state
  between this check and the copy; `hv_win_pmu_dma_write` only edits permission bits of tracked leaves inside the
  admitted span (same output address), then a broadcast TLBI completes before EL2's store.

Window checks are untouched (no file under `s1/` changed): guest WRITE range/MDTS checks in `hv_nvme_queue.c`,
`s1n_write`/`range_ok`/`transfer` pins in `native_window.c`, `xfer_owned` and the locked slot re-read in `ans_ro.c`.

## Tests (logs: `check-20261110/logs/`)

| suite | r4.7 |
|---|---|
| `source/tests/win-pmu-rewrite` (incl. new refill tests) | **862,883 checks, 0 failed** |
| `check-memory.py` (patched) | **449 + 33,226 PASS** |
| `run-all-tests.sh` (SKIP_NATIVE=1; native run separately below) | **rc 0**: controller 9,746; S0 5,719,795; S1 9,346; handoff + 5 mutants; r2 300,414 + 36 mutants; review 15,177,307 + oracle 4/4; confirm + B3 5/5; gap mutants 16 (same 3 survivors as r4.4–r4.6); UEFI e2e 212/178/62; review-r3 pass; s1wc 5/5 |
| native stage 0 / 1 / 2 | **49/49, 55/55, 55/55** scenarios (11,903 / 2,105,637 / 67,006,245 checks) |
| native stage 3 | **230/230**, 32,076,622 checks, **mutants 94/94** |
| `r45_mutants.py` / `r46_run.py` / `r46_mutant_followup.py` / overlay mutants | pass / pass / 9/9 / 7/7 |
| `r46_context_path.py`, `r46_mcc.py`, `stage_inspection.py`, `r46_stage_inspection.py`, `link_path_test.py` | pass |
| twin-v2 `twin_proof.py`, `stage_proof.py` | PASS (broken variants FAIL as required) |
| `review-tests-r4/run.py` | rc 1, identical to r4.6 (`AssertionError ('b_frontend_bound_off', 0)`: superseded harness, r4.6 CHECK §1 row 17) |

Runner: `check-20261110/run-suites.sh` → `logs/run-suites.out`.

## Binaries (pinned container `m5pro-windows/mu-build:f43`, image 66ebcb6da215; librust.a `4ed52ac9…c304` copied with the tree)

`check-20261110/build-all.sh`: `S1N_STAGE=n bash build.sh check-20261110/build/stage<n>-build<k>`, k = 1, 2, each from clean
(build.sh deletes every object). **Build 1 = build 2, bit-identical** (.bin, both ELFs, build_tag.h) for both stages.
Copies in `artifacts/` (+ `SHA256SUMS`).

| stage | m1n1-win-s1-r4.7-stageN.bin sha256 | m1n1-raw.elf | m1n1.elf | size |
|---|---|---|---|---|
| **3** | **`8d8ba1608a76892e95fcaf95a8d14bd28b3d1857f6c90f13222c6253e6802d7f`** | `9778585ee0b7f8fb284abcab3878630faf22907457bf54ac203756c94a1cf1a7` | `ab33293fa4365ff8812f341f795c6ddcd5ac8d4ff5bbc4d44a8b75a9c2f5f4fb` | 8,192,000 |
| 0 | `ef7c86dca70b21e588f07e2a5943e0864b119c9e5dcb308516664844aa6eb2aa` | `debf29aa199350f4ffa8b909e94c2e426a54620a1aff623c932044d80d12f661` | `951b6a8a0188c599a74af9a12127021ea38c7b20076eed165cec5bf748520406` | 7,143,424 |

`build_tag.h` = `v2.0.0-169-g868807f9-dirty-s1r4.7-stageN` (stage 3 `370ca6a6…`, stage 0 `050c3940…`); the inherited
policy marker `M5W-S1N-R4.5-STAGE-N` occurs once with the right N.

### What the launcher needs (from `nm` of the stage-3 ELF; `check-20261110/build/nm-*.txt`)

| symbol | stage 3 | stage 0 | r4.6 stage 3 |
|---|---|---|---|
| `_end` (resident size) | **0x7d0000** | 0x6d0000 | 0x7d0000 (unchanged) |
| `_bss_start` / `_bss_end` | 0x160000 / 0x791bd0 | 0x160000 / 0x691bd0 | same |
| `controller` (S1DG = +0x21088) | 0x426000 | 0x426000 | same |
| `hv_win_storage_prepare` | 0x59600 | 0x59600 | 0x595c0 (+0x40) |
| `hv_storage_guest_span` | 0x59100 | 0x59100 | same |
| `hv_win_pmu_dma_write` | 0x63fa0 | 0x63a20 | absent |
| `.rela.dyn` start (live-image check length) | 0xd44b0 | 0xd44b0 | |

The pmu-reads change adds no data, so the resident size is the r4.6 value: PREP's `resident_size` (launcher:
`heap_top + 32 MiB − universe base`) must cover `_base.._end` = 0x7d0000 bytes, which PREP itself checks
(`hv_win_storage.c:119`). Binary check: `hv_storage_guest_span` calls `hv_win_pmu_dma_write` (`bl 63fa0` at 0x59308,
`check-20261110/build/disasm-guest-span-stage3.txt`).

## Unverified until hardware
The refill path on real page-ins (RUN.txt / console "device refills N" > 0 and no DATA_TRANSFER_ERROR completions),
broadcast-TLBI cost under page-in load, and everything r4.6 CHECK §6/§7 lists (ANS at Windows' write rate, FLUSH latency).
Note: after native select the r4.x proxy lock refuses `P_HV_WIN_PMU_STATS`; the counter is only visible in the console
line the resident prints at guest exit (`hv_win_pmu_report`, on SYSTEM_OFF/RESET).
