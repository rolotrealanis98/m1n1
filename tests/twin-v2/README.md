# Twin v2: a write-confinement proof that can fail (review H3 / C5)

This is a versioned copy of the offline twin's write audit. `offline-twin-20261109/` is used read-only: QEMU, ACPI tables, `run-qemu-twin.sh`, batches and the feeder.

## What was wrong in v1, and the fix

| v1 (H3) | v2 |
|---|---|
| The fake ANS refused out-of-window WRITEs (status 0x80, nothing stored), so the image could not change | `patch_fake_v2.py`: the fake **accepts and stores** WRITE (also when rw is off), WRITE ZEROES, WRITE UNCORRECTABLE and DSM-deallocate at any LBA below NSZE. Only past-the-end writes are refused, and they are still counted. |
| `disk-audit.py` ignored zero-content extents outside the window | The C2 verifier (`../verifier/m5w_outside_verify.py --sparse`) compares **every** outside chunk of the 1 TB image, content **and** allocation, so a zero write into a hole is seen. 256 MiB around the window edges, p1 and the backup GPT are LBA-stamped, so zero writes there are content changes too. |
| `writes_outside_window` went only to stderr | It is in the receipt (`twin_native_v2.c` → `$S1TWIN_RECEIPT`; the harness prints it) and asserted. |
| No per-command record | Every command reaching the fake is logged to `$S1TWIN_WLOG` (JSONL: queue, op, nsid, slba, nlb, status, FNV-1a of written data). The gate recomputes the outside writes from the log. |
| S1WC decoded with the r2.2 decoder | `rw-gate-v2.sh` uses the resident's own `proxyclient/tools/s1dg_decode.py` (r4.4). |

## Files

- `twin_proof.py` — adversarial proof. It compiles r4.4's real storage stack plus the v2 fake (ASan/UBSan) and runs `scenario_twin_proof.c`:
  - legitimate edge writes and 64 random writes, with read-back;
  - 14 adversarial guest commands;
  - 9 frontend-bypass backend calls.

  It then judges with D1 (image), D2 (log) and D3 (fake counter), plus positive controls. It also builds the broken variants M1–M4 in scratch from the same source, and replays M1 with the v1 fake and v1 audit to show the old blind spot.
- `rw-gate-v2.sh`, `twin-winpe47-v2.sh`, `build-native-model-v2.sh`, `twin_native_v2.c`, `mkimg_v2.c` — the real-WinPE QEMU gate on the v2 model. `models/` holds the r4.4 v2 dylib and image maker.
- `results-r4.4-20261110.json` — the adversarial proof result. `runs/gatev2-r4.4-mx{256,1}/gate-v2.json` — the WinPE gate results. The 5 MB before/after manifests were deleted after the run; the compare result is in `d1-compare.json`.

## Results (2026-11-10)

| Variant | `max_xfer` | D1 image outside identical | D2 outside writes in log | D3 fake counter | Verdict |
|---|---|---|---|---|---|
| r4.4 unmodified | 256 | yes | 0 | 0 | **PASS** |
| r4.4 unmodified | 1 | yes | 0 | 0 | **PASS** |
| M1 window check disabled (all layers) | 256 | no: PMBR, p2 tail, gap, p3 head, backup GPT (GPT refused) | 18 | 18 | **FAIL** |
| M2 off-by-one at the end (all layers) | 256 | no: LBA 198637574 | 7 | 7 | **FAIL** |
| M3 off-by-one at the start (translation + checks shifted) | 256 | no: LBA 174213119 | 2 | 2 | **FAIL** |
| M4 wrapped LBA (additive bound checks, no lower bound) | 256 | no: PMBR, p2 tail 174212920+ | 7 | 7 | **FAIL** |
| M1 with the **v1** fake + v1 `disk-audit.py` | 256 | v1 audit says **pass** (blind) | 18 | 18 | v2: FAIL |

WinPE gate on r4.4 v2:
- `max_xfer` 256: 20 in-window writes; `max_xfer` 1: 39.
- Both runs: D1 PASS, D2 0, D3 0, S1WC `all_writes_inside_window`, and the persistence canary file present after reboot. **PASS.**

Re-run: `python3 -I twin_proof.py > results.json` takes about 25 s. `S1N_MAXXFER=1 ./rw-gate-v2.sh TAG r4.4` takes about 2 min per gate. Set `DISK_DIR` to put the sparse image in scratch.
