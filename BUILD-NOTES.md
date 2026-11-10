# r3.2 + display round 3 (disp3): build notes, 2026-11-10

Parent (read-only, not modified): `../s1-resident-r3.2-disp2-20261110/` (bin `ed252158…`), plus the three findings M1, L1, L2 of its
`INDEPENDENT-REVIEW.md` (GO-WITH-CONDITIONS) and its run conditions C1–C5. Offline only: no device, SSH, USB or serial access.
**Not reviewed.** This build has had no independent review.

## Directory and what was done with the abandoned copy
The directory held a partial `source/` from another tool (a failed `copytree`) plus stub `REPORT.md`/`BUILD-NOTES.md`. All three were
deleted and the tree was rebuilt with `rsync -a` from the parent's `source/`. Deliberately left out:

| Parent path | Why | How it is handled |
|---|---|---|
| `source/.buildshim/librust.a` (11 MB, sha256 `4ed52ac9…c304`) | large, unchanged | **referenced by hash**: `build.sh` checks the parent's copy, stages it into `source/.buildshim/` for the build, deletes it on exit |
| `source/data/bootlogo_{48,128,256}.png` | dangling symlinks to `../artwork/logos/…` (artwork submodule absent) | **dropped**. This target builds the logos from `data/bootlogo_*.bin` (`Makefile:317`); the PNGs are only used by the `.rgba` rule, not by `m1n1.bin` |
| `source/GEMINI.md`, `source/CLAUDE.md` | valid in-tree symlinks to `AGENTS.md` | **kept as symlinks** (`rsync -a`) |
| `__pycache__/`, `*.pyc`, `tests/win-vgic3/test_vgic3{,.dSYM}` | host caches / prebuilt test binary | dropped |
| parent `artifacts/`, `build-verify/`, logs, `*.out` | parent outputs | not copied; cited by hash below |

**Proof the lean copy is build-equivalent:** before any edit, this tree built with the parent's tag gave `m1n1.bin`
`ed252158d14d…2628`, `m1n1.macho` `b4db3f3a…`, `build_tag.h` `68f13862…`, i.e. the parent's artifacts bit-exact (`logs/baseline-parent-tag.log`).
Whole-tree compare after the edits: 591 files compared with the parent, 2 differ (`STORAGE-HASHES.txt`).
`source/build/` and `harness/build/` were removed after the runs (regenerable). Size: `source/` 7.4 MB, `artifacts/` 18 MB.

## Artifacts (`artifacts/`, `SHA256-artifacts.txt`)

    19c76cf767071759206167a11212472d37b43a3263c227ef2b54634bcabed3d8  m1n1-win-s1-r3.2-disp3.bin   (bind this raw SHA)
    b5dc6cc22f240fe6c86307bf227e4d1d81ff6faf75a5d1770102aa083eec4835  m1n1-raw.elf
    77442b827dc73736c2cafec53db2955cd53531aca61c1def38b41eaea1a2e8ca  m1n1.elf
    5b44051f72573352c246129ca59a18ecf6127e7d8be1740845206abddba6e197  m1n1.macho
    2d8adb7cb9db2fddcb06e0121034e93cce7a17fa71539a40f29f0faf90e00747  build_tag.h  (…-s1r3.2-disp3)
    e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  build_cfg.h  (identical to disp2)

- `./build.sh`: the parent's pinned container command (`m5pro-windows/mu-build:f43`, clang 21.1.8); only the tag and the librust.a staging differ.
- `./two-build-hash.sh`: two `make clean` builds → **BIT-IDENTICAL** (`build1.txt` == `build2.txt`; `build1.log`/`build2.log`, 0 warnings).
- `./compile-verify.sh` (homebrew LLVM, m1n1 flags): `hv_win_fb.c`, `hv_vm.c`, `proxy.c` compile with 0 warnings; 0 SIMD registers in `hv_win_fb.c.o`.
- `m1n1.elf`: `_end` = `0x4c0000` (unchanged); `win_fb` BSS grows 8 bytes (`0x8b30` → `0x8b38`); `nvme_flush` absent, `nvme_read` present.
  Disassembly of the linked image: the single `tlbi ipas2e1is` (in `fb_wp_word`) is preceded by `dsb ishst`; the fault-path
  `tlbi ipas2e1` operand is masked with `0xffffffffc` (IPA[13:12] cleared).

## Diff vs parent (`source-vs-r3.2-disp2.patch`, sha256 `6e91ff38…41c5`)
Exactly two source files differ. `hv_win_fb_conv.h` is unchanged (`911e5bc7…`). New hashes: `hv_win_fb.c` `fbec3de7…50c1`, `run_windows.py` `b6b40ddc…6d15`.

| Change | Where (disp3 file:line) |
|---|---|
| **M1** `dsb ishst` between each small-pass leaf store (`hv_wp_set(a, false)`) and its `tlbi ipas2e1is`; `fb_wp_done` then gives `dsb ish; tlbi vmalle1is; dsb ish; isb` | `hv_win_fb.c:565-569` (comment `:553-558`, `:575-577`) |
| **L1** fault-path `ipas2e1` operand aligned down to the 16 KiB stage-2 page: `((ipa & ~(FB_PAGE - 1)) >> 12)` | `hv_win_fb.c:523` (comment `:519-521`) |
| **L2** print time measured in `fb_tick_end(now, start)`: added to `busy`, to `max_ticks`/`iv_tick` of the printing tick, and to a new `print_sum`; gap accounting starts after the print (unchanged) | `hv_win_fb.c:697-712`; call sites `:725, :739, :795`; field `:133` |
| L2 counter exposed: FBS line `print_us=`; stat 77 `ST_PRINT` (ticks); `FB_VERSION` 2 → 3; `ST_NR` 77 → 78 | `hv_win_fb.c:681, :686, :106-107, :837, :63` |
| Launcher: requires stat 35 == 3; records stats 0..77 (`print_ticks` on its own line) | `run_windows.py:1109-1110, 1318, 1325-1326` |

L1 check of "invalidate all four 4 KiB guest pages": `win = (far − ipa[13:0]) & ~0xfff` (`:491`) is the guest VA of byte 0 of the faulting
16 KiB IPA page, and the loop `k = 0, 4, 8, 12 KiB` (`:530-531`) issues `vaae1` for `win + k`. Under a linear guest mapping these are exactly
the four stage-1 pages of that IPA page, no more, no fewer. The strict harness checks this per real fault with a VA−IPA offset of 0x5000
(4 KiB- but not 16 KiB-aligned). Aligned `ipas2e1` operand: 0 mismatches in 103,798 real faults (all modes).
Exactly four `vaae1` covering that page: 0 mismatches in the 80,001 faults that ran in `sub` mode.

## Audit: every stage-2 permission change followed by a TLBI in `hv_win_fb.c`
Reference sequence: upstream m1n1 (AsahiLinux/m1n1 `origin/main` `70757dc7`, 2026-11-04) after stage-2 changes:
`src/hv/hv.c:94-97` and `src/hv/hv_cpu_power.c:136-139`: `dsb ishst; tlbi vmalls12e1is; dsb ish; isb`.
KVM `__kvm_tlb_flush_vmid_ipa`: `dsb ishst; tlbi ipas2e1is; dsb ish; tlbi vmalle1is; dsb ish; isb` (as quoted in the disp2 review; not re-read from kernel source here).

| Site | Stage-2 change | Sequence (disp3) | Status |
|---|---|---|---|
| `hv_win_fb_start` `:441, :447` → `:459-462` | `hv_map_hw_nc`, `hv_wp_prepare` (all RO; ends `dsb ishst`, `hv_vm.c:642`) | `dsb ishst; tlbi vmalls12e1is; dsb ish; isb` | confirmed, unchanged |
| fault path `:512` → `:522-534` | `hv_wp_set(…, true)` (RO→RW) | `dsb nshst; tlbi ipas2e1 <16K-aligned>; dsb nsh; vmalle1 / vaae1(far) / vaae1×4; dsb nsh; isb` (local; single vCPU) | confirmed; **operand fixed (L1)** |
| spurious path `:506` | none (dirty page already RW) | `hv_xt_tlbi(false)` = `dsb ishst; tlbi vmalls12e1; dsb nsh; isb` (`hv_vm.c:551-561`) | confirmed, unchanged |
| small pass (≤ 32 pages) `fb_wp_word`/`fb_wp_done` `:565-569, :578-584`, from `fb_begin_pass` `:599-602` and `fb_merge_ahead` `:631-633` | `hv_wp_set(…, false)` (RW→RO) | per page `dsb ishst; tlbi ipas2e1is`; then `dsb ish; tlbi vmalle1is; dsb ish; isb` | **fixed (M1)** |
| large pass (> 32 pages), same functions `:565, :578-584` | `hv_wp_set(…, false)` ×N | `dsb ish; tlbi vmalls12e1is; dsb ish; isb` | confirmed, unchanged |
| PREPARE `:379-389` (`fb_tlbi_el2` `:323-329`) | none (EL2 stage-1 mapping of the shadow) | `dsb ishst; tlbi vmalle1is / alle2is; dsb ish; isb` | confirmed (not stage 2), unchanged |

## Storage / NVMe byte identity (`STORAGE-HASHES.txt`)
28 listed files by sha256 vs the parent: **28 identical, 0 differ**: `hv_win_storage{,_mem,_native}.{c,h}`, `nvme.{c,h}`, all 12 files of
`src/hv/win/s1/` (ans_ro, native_window, hv_nvme, hv_nvme_queue, hv_pci, ram_disk, model, LICENSE), `hv_vm.c`, `hv.c`, `hv_exc.c`, `hv.h`,
`hv_win.{c,h}`, `proxy.c`, `proxy.py`, `hv_win_fb_conv.h`. Their values equal the parent's `STORAGE-UNTOUCHED.md`.
Linked image: every storage/NVMe data and BSS symbol address is identical to disp2. Storage `.text` that links after `hv_win_fb.o`
moves by +160 bytes (same code). Behaviour toward the SSD is unchanged: the read-only r3.2 line, used only on launcher request, and the RUN-SPEC passes no storage arguments.

## Tests (`harness/run.sh`; outputs `harness/test.out`, `strict.out`, `print.out`, `work.out`, `card.out`)
The harness compiles the real converters (orig `c5f57839`, r3.2-disp `8598bd01`, parent disp2 `c43fc76d`, this tree `fbec3de7`) against stubs,
plus three mutants of this tree. Each mutant reverts exactly one fix by `sed`, checked to change one line.

- **Existing suite (`test`), all PASS, 0 failures:** colour bit-exact; diff spans; full-frame coverage; 3000-rect random dirty in every
  tlbi/diff/merge mode with and without prefetch (0 stale, 0 lost); pause accounting; read-reset; reserved bits rejected.
- **`work`/`card` model outputs are identical to the parent's** except the new `print_us=0` field and the host compute-floor timing line.
  Performance numbers are unchanged; they are model estimates, as in disp2.
- **New `strict` mode (M1, L1):** an adversarial stage-2 model (see `harness/sim.c` header). A leaf store is invisible to the walker
  until a DSB. There are stage-2-only TLB entries that `vmalle1*` does not remove. A stage-2 TLBI issued before the store is visible
  refills from the old leaf. An `ipas2e1` operand with IPA[13:12] ≠ 0 matches nothing. VA = IPA + 0x5000. A sequence check runs at every guest re-entry.
  11 scenarios: small-pass re-protect then rewrite ×300 in sub/all/far/2x/merge, plus 3000-rect random.

  | Tree | Result |
  |---|---|
  | disp3 | **PASS**: 0 stale, 0 untracked stores, 0 ordering/sequence violations, 0 unaligned operands, 0 spurious (sub/all) |
  | disp2 parent | FAIL 11/11 (e.g. re-protect sub: 4122 stale pixels, 29168 untracked stores, 234 spurious from the unaligned operand) |
  | mutant no-barrier | FAIL 11/11 (re-protect: ~27.9k stale pixels, ~54k untracked stores) |
  | mutant no-align | FAIL 11/11 (unaligned `ipas2e1` → `sp_same` spurious faults, e.g. 479 of 2280) |
  | mutant no-print | PASS (expected: unrelated) |
- **New `print` mode (L2):** 40 µs per console line, stats every 1 s, idle guest.
  - disp3 **PASS**: print 400 µs = busy +400 µs, `tick_max` 80 µs, gap max 1.000 ms (print not counted as a gap), FBS `print_us` correct.
  - disp2 parent FAIL (busy +0, no stat 77). Mutant no-print FAIL (busy +0). Mutant no-barrier PASS.
- `run.sh strict`/`print` exit non-zero unless the expected pass/fail pattern holds; both exited 0.

## What the harness can and cannot check
- **Can:**
  - That the code issues the right ordering events (DSB before the per-IPA TLBI, DSB + ISB after it) on every path the tests drive.
  - That a missing barrier causes stale pixels under a worst-case walker model.
  - That fault-path operands name exactly the 16 KiB IPA page and its four 4 KiB VAs.
  - Print-time accounting.
- **Cannot:**
  - Real hardware reordering or speculation. A barrier is an ordering event, not a cycle-accurate effect, and the refill-from-old-leaf
    rule is an assumption that represents what the architecture permits.
  - Whether Apple cores honour or ignore IPA[13:12]. The model picks "honour" so that the bug is visible.
  - Other CPUs, AT/HPFAR, non-linear guest mappings, the EL2&0 regime (`alle2is`), real NC timing, and real print cost
    (the USB CDC path). The tests prove that the counters include whatever the print costs, not how much it costs.

## Not done
- No hardware run, so C1–C5 are written into `RUN-SPEC.md` but not exercised. `c3_check.py` has only its `--selftest`
  (formula and indexing on synthetic data). It has not been run on a real dump.
- No independent review of this build.
- `fbs_parse.py` is the parent's, byte-identical. It does not tabulate `print_us` or apply C1–C5 automatically; those are judged from
  its columns by hand as `RUN-SPEC.md` describes.
- Review items outside M1/L1/L2 were not touched: L3 (latency bound), L4 (CPU undercount), L5 nits, multi-vCPU locking.
