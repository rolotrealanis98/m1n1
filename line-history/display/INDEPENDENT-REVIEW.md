# Independent EL2 delta review: r3.2 + display round 3 (disp3, `19c76cf7…`)

Reviewed 2026-11-10. Offline only: no hardware, serial, USB or SSH. Nothing in this directory was modified except this file.
The rebuild and harness runs were done in a scratch copy, which has since been deleted. Parent review: `../s1-resident-r3.2-disp2-20261110/INDEPENDENT-REVIEW.md`.
Line numbers are disp3 `source/src/hv/win/hv_win_fb.c` unless stated otherwise.

## Verdict: GO-WITH-CONDITIONS for the display test-card run in RUN-SPEC.md

M1, L1 and L2 are fixed correctly. The diff is exactly what the builder claims. Storage is byte-identical, and the build reproduces bit-exact.
I found no new defect that would block the run. The conditions are about running and judging the test (no build change):

- **C1–C5** exactly as written in RUN-SPEC.md. They match the disp2 review and are operationally correct, with the clarifications below.
- **C4a.** After you find the C4 row, apply the steady state with `fbs_parse.py <console> --from-hex <t + 0x2540BE400>`.
  - `fbs_parse` rows print `dt_s` but not `t=`, so take `t=` from the raw `FBS` line of that row.
  - Its "worst interval" line otherwise covers boot and WinPE too, and it reports only **net** latency. Judge gross (C2) and gaps (C1) from the columns by hand.
- **C2a.** If gross − net > 5 ms in an interval with no matching `iv_gap_us`, check the **previous** interval's gap before calling a FAIL.
  A pass that started before the interval boundary carries its stop time into the next interval's maximum.
- **C3a.** Paste the four dump lines as one block. Before `p.reboot()`, check the file sizes:
  - shadow = gs·gh = 6912·1117 = **7,720,704** bytes;
  - native = 13824·2234 = **30,882,816** bytes.

  Stay within the ~100 s HV-shell limit in the disp2x NOTES.
- **C5a.** Each FBS2 interval starts with exactly one print. That print's time lands in the `iv_tick_us` of the line **after** the print, which is that row's own interval in `fbs_parse`.
  For any `iv_tick_us` > 1.2 ms, subtract that row's Δ`print_us` and record both values. RUN-SPEC's "whether it was a print interval" is always true, so it tells you nothing.
- **Watch (M1 cost).** Record Δ`busy_us`/s and Δ`busy_us`/Δ`passes` on the card. Compare them with the r3.2-disp hardware run (`runs/20261110-052123-disp2x`).
  disp2 itself has no hardware baseline. Expect at most about +0.5 % CPU (see Q2). More than that points at serialized broadcast TLBIs: report it, but it is not a FAIL.

Fix in the next build (not a run blocker): **N1** (below).

## Q1 Diff scope, storage, SSD: PASS
- **Source diff.** `diff -rq` of the disp2 and disp3 `source/` trees shows only `hv_win_fb.c` and `run_windows.py`.
  The other entries are files the builder dropped on purpose: `librust.a`, which is staged by hash; the dangling bootlogo PNG links; and the prebuilt `test_vgic3`.
  A fresh `diff -ru` equals `source-vs-r3.2-disp2.patch`.
- **Hunks in `hv_win_fb.c`:**
  - `FB_VERSION` 3 (`:63`) and `ST_PRINT` (`:106-107`);
  - the `print_sum` field (`:133`);
  - L1 (`:523`) and M1 (`:566-569`);
  - comments (`:519-521, :553-558, :575-577`);
  - `print_us` in the FBS line (`:681, :686`);
  - `fb_tick_end(now, start)` (`:699-713`) and its three call sites (`:725, :739, :795`);
  - stat 77 (`:837`).
- **Hunks in `run_windows.py`:**
  - the version check is now `== 3` (`:1109-1110`);
  - the exit record reads `range(78)`, with stat 77 on its own line (`:1318, 1325-1326`).
- **Nothing else.** `hv_win_fb_conv.h`, `hv_vm.c`, `proxy.c` and all 28 storage/NVMe files are byte-identical. My tree diff agrees with `STORAGE-HASHES.txt`.
- **SSD behaviour is the parent's: nothing is sent to the internal SSD on this run.**
  - The guest disk is the `--ramdisk` image in RAM.
  - This launcher has no native-storage, NVMe or write option at all (`run_windows.py` argument list).
  - `disp3-run.sh` differs from `disp2-run.sh` only in the header comment and `RES=`.
  - In the linked image, `nvme_read` is present and `nvme_flush`/`nvme_write` are absent (`llvm-nm` on my rebuild).
  - The read-only r3.2 native path is reachable only on launcher request, and nothing here requests it.

## Q2 Barriers and TLBI: PASS. The untracked-page window is closed
Configuration: HCR_EL2.E2H = 1, TGE = 0, one vCPU, 16 KiB stage-2 leaves, 4 KiB guest stage-1 pages. The shadow is identity-mapped (`hv_map_hw_nc(shadow, shadow, …)`, `:441`), so IPA = PA.

### Tightening: RW→RO re-protect
There are two callers, `fb_begin_pass` (`:588-611`) and `fb_merge_ahead` (`:615-639`). Both run from the 1 kHz FIQ tick with the only vCPU stopped.

The small-pass sequence, end to end:

| Step | Code | Why it is needed |
|---|---|---|
| 1. Move the page to the pass set | `pass |= m; dirty = 0` | — |
| 2. Store the RO leaf | `hv_wp_set(a, false)`, a volatile store (`hv_vm.c:651-658`); an out-of-line call, so the compiler cannot reorder it | — |
| 3. `dsb ishst` | `:567` | The leaf store becomes visible to every table walker in the IS domain **before** the invalidation. This is the M1 fix. |
| 4. `tlbi ipas2e1is` with operand `a>>12` | `:568`; `a` is 16 KiB-aligned, so IPA[13:12] = 0 | Removes stage-2-only entries for that IPA. Any refill after step 3 reads RO. |
| 5. `dsb ish` | `:578` | Completes the per-IPA TLBIs before the stage-1 flush (ARM-required order). |
| 6. `tlbi vmalle1is` | `:580` | Removes stage-1 and combined S1+S2 entries cached under the guest's VAs, including every alias VA. |
| 7. `dsb ish; isb` | `:583-584` | — |
| 8. `dsb sy`, then the shadow is read | `:603` | — |

- **Code generation.** Disassembly of my rebuild shows `bl hv_wp_set; cbz; dsb ishst; ubfx #12,#36; tlbi ipas2e1is` in `fb_wp_word`.
  Both inlined `fb_wp_done` copies are `dsb ish; tlbi vmalle1is | vmalls12e1is; dsb ish; isb`.
  `FB_TLBI` has no memory clobber, but it is `asm volatile` and sits after `sysop("dsb ishst")`, which has one, so the order holds.
- **Large passes.** RO stores, `dsb ish`, `tlbi vmalls12e1is`, `dsb ish; isb`. This is correct and unchanged; it is upstream m1n1's sequence.
- **Conclusion.** After step 7 there is no TLB entry, stage-2-only or combined, that grants write to a re-protected page.
  The next guest store faults and sets `dirty` again. The disp2 M1 window (a TLBI that overtakes the leaf store, leaving a stage-2 RW refill that `vmalle1is` does not remove) is closed.
- **Other ways a page could be left writable but untracked:**
  - Faults cannot race the tick: the abort handler runs with FIQ masked, and there is one vCPU.
  - `hv_win_fb_start` makes every page RO, then `vmalls12e1is` (`:459-462`).
  - The only remaining hole is N1 below. It is not reachable in this configuration.

### Is `vmalle1is` needed, correct, and what does it cost?
- **Needed.** `ipas2e1is` is not required to remove combined entries. The architecture requires a stage-1 invalidation after it, completed by a DSB, which is the KVM `__kvm_tlb_flush_vmid_ipa` shape.
  Without it, a combined RW entry for the guest VA would let stores through with no fault.
- **Correct operation.** At EL2 with {E2H, TGE} = {1, 0}, `TLBI VMALLE1IS` targets the EL1&0 regime for the current VTTBR_EL2.VMID on all PEs in the IS domain. That VMID is the guest's.
  It does not touch EL2&0. That is why PREPARE separately uses `alle2is` (`:323-329`), which is unchanged and only runs before the guest starts.
- **Not new.** The builder did **not** add `vmalle1is` in disp3. disp2 already had it, at disp2 `hv_win_fb.c:566-568`, and the r3.2-disp tree before it.
  The patch only changes the comment there. So it does not undo anything disp2 gained.
- **Cost.** One broadcast VMID-wide stage-1 invalidate per small pass, about 60 passes/s at most, which is low µs/s of EL2 time.
  The real cost is indirect: the guest refills its stage-1 TLB after every small pass. The r3.2-disp hardware run already paid both.
- **What disp3 does add:** up to 32 `dsb ishst` per small pass, one per page, each after the previous page's broadcast `ipas2e1is`.
  - If the core makes a DSB wait for the earlier broadcast TLBI to complete, the 32 TLBIs serialize instead of overlapping.
  - Assuming about 1–2 µs per broadcast completion (my estimate, not measured), that is ≤ 32–64 µs per small pass.
  - That cost lands inside the tick's 500 µs budget (`start` is taken before `fb_begin_pass`). At ≤ 60 passes/s it is ≤ 0.4 % CPU.
  - Card passes average about 52 pages (r3.2-disp run: 498,486 faults over 9,532 passes), so many card passes take the large path and pay nothing extra.
- **Risk:** low. Watch for it as described in the Watch condition.
- **Cheaper alternative** if it ever matters: use one `vmalls12e1is` for every pass (upstream m1n1's only sequence). It needs a rebuild, because `FB_SMALL_PASS` is a compile-time constant.

### Loosening: the fault path makes the page RW (`:512-534`)
- Sequence: `hv_wp_set(…, true)`, then `dsb nshst; tlbi ipas2e1 <aligned>; dsb nsh`, then `vaae1` for the four VAs (or `vmalle1` / `vaae1(far)` in the other modes), then `dsb nsh; isb`.
- Local (non-broadcast) maintenance is enough for one vCPU on one PE. This is KVM's `_nsh` shape.
- Loosening cannot create an untracked page. `dirty` is set in the same exception before the guest re-executes (`:537`).
  A stale RO entry costs only a classified spurious fault followed by a local `vmalls12e1` (`:506`, `hv_vm.c:551-561`).
- Codegen: `lsr #12; and #0xffffffffc; tlbi ipas2e1`, then the `vaae1` loop from `win` to `win + 0x4000` in 0x1000 steps.

### Other sites (all unchanged and correct)
- start: `:459-462`;
- spurious path: `:506`;
- large pass: `:582`;
- PREPARE: `:379-389`, plus `fb_tlbi_el2`.

## Q3 L1 alignment: PASS
- **Stage-2 operand.** `((ipa & ~0x3fff) >> 12) & GENMASK(35,0)` (`:523`).
  - The shadow base is 16 KiB-aligned (`heapblock_alloc_aligned(size, FB_PAGE)`, `:371`), so the IPA page boundaries match the shadow pages.
  - `size` is rounded up to a whole page (`:355`), so the last partial page of the frame is a full stage-2 leaf, and its aligned operand names that leaf.
  - `ipa` from AT carries FAR[11:0] (`hv_vm.c:593`). Alignment removes both those bits and IPA[13:12].
- **Stage-1 operands.** `win = (far − ipa[13:0]) & ~0xfff` (`:491`). FAR and IPA agree in bits 11:0, so `win` is the VA of byte 0 of the IPA page under a linear mapping.
  - The loop `k = 0, 0x1000, 0x2000, 0x3000` (`k < 0x4000`, `:530`) gives exactly four TLBIs, `win … win + 12 KiB`. There is no off-by-one.
  - On the last page, VAs past the end of the guest's mapping are either unmapped or belong to the guest's neighbouring mapping. Invalidating them is harmless.
  - A non-linear guest mapping gives only a classified spurious fault, as in disp2.

## Q4 L2 accounting: PASS
- **Busy time.** `busy` gets the tick work `end − start` (`:793`) and then the print `after − now` (`:705`), with `now = end`. The two intervals are disjoint, so nothing is counted twice.
  - The idle and deferred paths pass `(start, start)`, so only the print is added.
  - All values are u64 differences of monotonic CNTPCT readings with `after ≥ now ≥ start`, so none can go negative.
- **Gaps.** `last_end = after` (`:711`), so print time stays out of the next gap, as before.
- **Tick maxima.** `max_ticks` and the `mx[0]`/`mx[1]` tick maxima get `after − start`.
  - Because `fb_print` clears `mx[1]` first, the print shows up in the **next** FBS2 line (see C5a).
  - `print_us` in line N covers prints up to N−1. So per row, Δ`print_us` and that row's `iv_tick_us` refer to the same print, which is consistent.
- **Version handshake.**

  | Launcher | Resident | Result |
  |---|---|---|
  | new | disp2 (stat 35 = 2) | refused (`:1109-1110`) |
  | new | anything older | refused; stat 35 is `UINT64_MAX` or a different value |
  | disp2 | disp3 | refused, because it requires 2 |

  The refusal happens at PREPARE, before `hv.start()`, and the stat-77 read comes after it.
  Nothing else in `proxyclient` reads 77 stats. `fbs_parse.py` parses `key=value` pairs, so the extra `print_us=` field is ignored safely.
- **Line length.** `debug_printf` uses a 512-byte `vsnprintf` buffer and writes at most 511 bytes (`utils.c:74-86`), so it truncates safely.
  - FBS: 115 literal characters + 21 `%lu` + 1 `%lx`. At realistic magnitudes the line is about 260–380 characters.
  - It could reach 511 only if every counter had ≥ 18 digits.
  - FBS2 is unchanged.

## Q5 Tests and reproduction: verified
- **Build.** A clean container build of `source/` (the pinned `m5pro-windows/mu-build:f43`, librust staged by hash from the parent) gives:
  - `m1n1.bin` **`19c76cf7…a2e8…bed3d8` bit-exact**;
  - `m1n1.macho` `5b44051f…` and `build_tag.h` `2d8adb7c…`, both matching;
  - 0 warnings.
- **Harness.** `harness/run.sh all` in scratch: exit 0. Every expectation held:

  | Tree | `strict` | `print` |
  |---|---|---|
  | disp3 | PASS | PASS |
  | disp2 parent | FAIL 11/11 | FAIL |
  | no-barrier mutant | FAIL 11/11 (re-protect: 27,866 stale pixels, 53,797 untracked stores) | PASS |
  | no-align mutant | FAIL 11/11 (479 unaligned → spurious) | — |
  | no-print mutant | PASS | FAIL (`d_busy 0`) |

  - The existing suite passes with 0 failures.
  - Output matches the shipped `.out` files except embedded paths and the host timing line.
- **Each mutant really removes one fix.** Each `sed` pattern matches exactly one line of the tree: 1 × `            sysop("dsb ishst");`, 1 × the aligned operand, 1 × `busy += after - now`.
- **Limits.** The strict model is adversarial but not architectural:
  - barriers are ordering events, not timing;
  - "refill from the old leaf" is modelled, not observed;
  - there is no AT/HPFAR, no other PEs, and no real broadcast-TLBI cost.

  It proves the code issues the right sequence on every path the tests drive. It cannot prove Apple-core behaviour.

## Q6 RUN-SPEC: PASS with C4a/C2a/C3a/C5a
- C1–C5 are present and match the parent review: gap judged whatever `gapf` says; gross and net latency; teardown C3; counter-defined C4; 1.5 ms tick limit.
- **C3 is runnable as written:**
  - `iface`, `p` and `u` are in the HV-shell namespace (`m1n1/hv/__init__.py:180-184`).
  - The stat indices `(1, 2, 4, 5, 26, 27, 28, 14)` are shadow, native, stride, height, gw, gh, gs and flags; `flags & 4` is `FB_F_SCALE2` (`:59`).
  - The shadow's EL2 aliases are all NC after PREPARE, so `readmem` sees the guest's NC stores.
  - `c3_check.py`'s `px()` matches the converter's documented `(c<<2)|(c>>6)` with X2 = 0 (`hv_win_fb_conv.h:14, 33-36`).
  - Its excluded spans match the **real** `testcard.c` geometry, not just the harness model:

    | Excluded span | Real card (`display/improve-20261109/testcard/testcard.c`) |
    |---|---|
    | band `H*3/4-40`, height `H/4-60` | `:163` |
    | counter rows `{170, H/2+40, H-H/28-60}`, height `H/28` | `:186-187` |

  - Everything else the card draws is static after the first frame: the ruler labels and the mode line.
- **I ran `c3_check.py` on a synthetic dump at the real geometry** (1728×1117, gs 6912, stride 13824, 2x):
  - 4,720,896 pixels checked on 683 of 1117 rows, in about 1 s;
  - one planted stale 16 KiB page at row 37–39 was reported as exactly 16,384 native mismatches, with FAIL.

  So the indexing and the stride/2x handling are right. It has still never seen a real dump.
- **Residual risks for C3:**
  - It assumes the card runs at 100 % DPI (W, H = gw, gh). WinPE defaults to this. If the first mismatch is a whole-row offset rather than an isolated page, suspect the geometry before suspecting tracking.
  - `readmem` of 30 MB is one proxy command; at USB high speed that is seconds.

## Findings
**High / Medium:** none.

**N1 (Low, inherited, next build).** `fb_wp_word` ignores the return value of `hv_wp_set(a, false)` (`:565`).
- If a leaf vanished, the page would stay RW with `dirty` already cleared: untracked, and silent, because `wp_fail` is counted only on the fault path (`:513`).
- It is unreachable today: nothing else rewrites shadow leaves, and `hv_wp_leaf` refuses SW/XT leaves.
- Fix: count the failure into `wp_fail` and keep the page in `dirty`.

**N2 (Low, cost).** The per-page `dsb ishst` may serialize up to 32 broadcast TLBIs per small pass (Q2). It is bounded, and covered by the Watch condition.

**N3 (Low, doc).**
- BUILD-NOTES/RUN-SPEC say print time is in "the printing tick's `iv_tick_us`". It is actually in the next FBS2 line (C5a).
- `fbs_parse.py` tabulates neither `t` nor `print_us` (C4a).

## Not verified
- Real Apple-core behaviour: whether IPA[13:12] is honoured, broadcast TLBI and DSB latency, and speculative stage-2 walks.
- Real print cost over USB CDC.
- `c3_check.py` on a real dump.
- The HV-shell watchdog budget during a 30 MB `readmem`: taken from the disp2x NOTES, not measured.
- Behaviour with more than one vCPU, which is out of scope and gated by `-C 1`.
