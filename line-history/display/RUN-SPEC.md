# Display round-3 (disp3) hardware run spec (not executed)

Same run shape as the disp2 spec (`../s1-resident-r3.2-disp2-20261110/RUN-SPEC.md`): read-only storage line, no USB passthrough,
no guest keyboard, the test card auto-run from the WinPE ramdisk, **counters read from the console without pausing the guest**.
What changes: the resident, the launcher tree, and the pass criteria now carry the disp2 review's run conditions **C1–C5**
(`../s1-resident-r3.2-disp2-20261110/INDEPENDENT-REVIEW.md`).

## Components
- **Resident:** `artifacts/m1n1-win-s1-r3.2-disp3.bin`. Bind its raw SHA256
  `19c76cf767071759206167a11212472d37b43a3263c227ef2b54634bcabed3d8`. Banner tag `…-s1r3.2-disp3` is informative only.
- **Firmware / media:** unchanged: FD `912a9527…` (s1-mu-windows-bindfix); media `display-testcard.ntasi` `c7061b4e…` (auto-runs `testcard.exe 1`).
- **Launcher:** `./disp3-run.sh` = `disp2-run.sh` with the resident tree switched to this directory. Same arguments.
  The launcher refuses a resident whose stat 35 is not 3 (disp3 counter layout) or whose stat 36 does not match the tuning.
- **SSD:** nothing is sent to the SSD. Storage sources are byte-identical to disp2 (`STORAGE-HASHES.txt`); the run passes no storage arguments.

## Launcher arguments (in `disp3-run.sh`, unchanged)

    --mu-profile m1a --route gic -C 1 --ramdisk <media> --sha256 <FD sha> --pmu-rewrite el2 \
      --fb-shadow --fb-dirty --fb-scale2 --fb-bench --fb-stats-period 2 [FB_EXTRA] -S --run-dir <dir> <FD>

Defaults: `--fb-tlbi sub`, write elision on, merge-ahead off, budget 500 µs base / 1000 µs escalated after 8 ms / 8 ms defer.

## Reading counters: do NOT pause the guest during the measurement window
- Every 2 s the resident prints `FBS …` and `FBS2 …` from the EL2 tick (fields as in the disp2 spec). Summarise with
  `python3 fbs_parse.py <run>/primary-console.txt`. **No `kill -INT` / FBSTAT read until teardown.**
- New in disp3: FBS field **`print_us`** = cumulative time spent printing FBS/FBS2 lines, up to the previous print.
  It is also included in `busy_us` and in the printing tick's `iv_tick_us` (it used to be in no counter). Stat 77 holds it in ticks.
- `run_windows.py` records stats 0..77 at exit.

## Sequence
1. Boot and start the guest as in disp2. Record the PREPARE lines (`FB guest mode …`, `bench …`, `FB disp2 bench 2x/rd32/rd64/diff …`).
2. WinPE console (~20 s): those FBS rows are the **idle** check.
3. **C4 — steady-state start:** in the FBS rows, find the first interval after the WinPE console where faults/s (fbs_parse `flt/s`)
   exceeds 1000. Steady state starts at that row's `t=` + 10 s (10·10⁹ counter ticks). **Write that `t=` down** in the run notes.
4. Card mode 1 runs ≥ 90 s with no host interaction. Owner films 10 s of slow motion (counters and bar) and takes the whole-panel photo.
5. **Teardown:** one `kill -INT` → HV shell. One FBSTAT read. Then **C3** (below), then `p.reboot()`.
6. **Only if a criterion fails, one A/B boot**, named by the failing counter (as in disp2):
   spurious > 1 % with `sub`+`alias` dominant → `FB_EXTRA="--fb-tlbi all"`; CPU high with `rt` read share > 70 % → `FB_EXTRA="--fb-no-diff"`.

## C3 — stale-pixel check at teardown (in the HV shell, before `p.reboot()`)

    g = [p.hv_win_fb_stat(i) for i in (1, 2, 4, 5, 26, 27, 28, 14)]
    open("/tmp/c3_geom.txt", "w").write(" ".join(map(str, g)))
    open("/tmp/c3_shadow.bin", "wb").write(iface.readmem(g[0], g[6] * g[5]))   # ~7.7 MB
    open("/tmp/c3_native.bin", "wb").write(iface.readmem(g[1], g[2] * g[3]))   # ~30 MB, seconds; inside the ~150 s WDT

Then on the host: `python3 c3_check.py /tmp/c3_geom.txt /tmp/c3_shadow.bin /tmp/c3_native.bin` (copy the files into the run dir).
It checks native == 2x-convert(shadow) on every guest row outside the card's band (bar), the three counter rows, ±16 rows margin
(`--margin`, `--exclude A:B` to widen). **Any mismatch is a FAIL.** `c3_check.py --selftest` checks only its own formula/indexing.
Not covered even with C3: a stale page in rows the card redraws (it heals within a frame); only the photo/video can show that.

## Pass criteria
Steady state = FBS intervals from the C4 `t=` on.

| Check | Source | Pass |
|---|---|---|
| Max latency (**C2**) | FBS2 `iv_lat_us` **net and gross**, every steady-state interval | net < 50 ms **and gross < 50 ms**; ≥ 99 % of new `hist` entries < 32 ms. Any interval with gross − net > 5 ms must show an `iv_gap_us` at least as large in the same interval, else FAIL |
| Spurious share | FBS `spur` / (`faults` + `spur`), per interval | < 1 %, `repeat` = 0, and `same` = 0 (see L1 below) |
| CPU (EL2) | Δ`busy_us` + Δ`fault_us` per interval (busy now includes print time) | ≤ 30 % on the card, < 2 % on the idle console; read as a lower bound (exception entry/exit not counted) |
| Longest guest stop (**C1**) | FBS2 `iv_gap_us`, **whatever `gapf` says** | < 5 ms in every steady-state interval; FBS `gaps` > 50 ms = 0 after boot. Do not filter or excuse a gap by `gapf` (a real stop normally shows `gapf` > 0) |
| Longest FB tick (**C5**) | FBS2 `iv_tick_us` | ≤ **1.5 ms** (escalated 1000 µs budget + one `rt`-sampled page ≈ 1.4 ms). Anything over 1.2 ms: record which interval and whether it was a print interval |
| Stale pixels (**C3**) | `c3_check.py` at teardown | 0 mismatches |
| Guest smoothness (visual) | owner's slow-motion video | no counter freeze > 50 ms; bar unsheared |
| Integrity | stats 29, 63, 62; SError / USB / boot | `ipa_fail` = 0, `wp_fail` = 0, `sp_repeat` = 0; no new SError, no storage or boot regression |

Required records (diagnostic, no threshold), as in disp2: Δ`rt` read/convert split; `chunks` written/examined; bench v2 (stats 22–25, 70–73);
faults/s and `fault_us`/s; and now `print_us`/s.

## Which conditions this build's fixes affect
- **M1 (barrier before the small-pass `ipas2e1is`) → C3.** M1 was the one known way to get a writable-but-untracked page, i.e. exactly
  the silent stale pixels C3 looks for. With disp3 a C3 mismatch has no known cause in the converter and must be reported as a new defect,
  not attributed to M1. Cost: up to 32 extra `dsb ishst` per small pass; no expected effect on C1/C2/C5.
- **L1 (16 KiB-aligned fault-path `ipas2e1` operand) → spurious criterion.** If a core honoured IPA[13:12], disp2 could take a
  `sp_same` spurious fault + full local flush after a real fault. disp3 should show `same` = 0; a non-zero `same` now means the targeted
  stage-2 TLBI does not take for another reason (report it). It also removes that confound from any `--fb-tlbi` A/B.
- **L2 (print time counted) → C5 and CPU.** Print time is now inside `iv_tick_us` for the tick that prints (once per 2 s) and inside
  `busy_us`. Expect tens of µs; C5's 1.5 ms has room. If a C5 excursion coincides with a print interval, subtract `print_us`'s delta
  and record both. Gaps (C1) are unchanged: print time was already outside `iv_gap_us` and stays outside it.
- **C1, C2, C4:** not affected by this build's fixes (measurement/judging rules only).

## Abort / fallback
Unchanged: faults climbing on a static screen or a guest hang at the first store → relaunch without `--fb-dirty`;
`FB_EXTRA="--fb-no-diff --fb-tlbi far"` gives r3.2-disp's conversion and TLB behaviour with the new counters.
