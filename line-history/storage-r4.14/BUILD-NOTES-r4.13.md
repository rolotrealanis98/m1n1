# r4.13 = r4.12 + guest-memory lease fix (launcher contract), NVMe error log, LBA Out of Range (2026-11-10)

Offline. No MacBook/SSH/USB. Lean copy of r4.12 (`BUILD-NOTES-r4.12.md`, older notes alongside). The
owner-override `source/AGENTS.md` is unchanged, and librust.a `4ed52ac9…c304` is reused by hash. Disk-window
confinement, pins, stage logic and the link lock are unchanged.

## The 102 errors (r4.12 run `runB/runs/20261110-203520-runB`, hv.log STORAGE EVENT lines 641-739)

| # | what | status |
|---|---|---|
| 6 | admin, the known optional features: Identify CNS 06h, Get Features D0h/0Ch/7Fh (0x2); Get Log Page C0h (0x109); firmware Create CQ qid 2 | 0x2 / 0x109 |
| **96** | **I/O queue READs, all status 0x4 (Data Transfer Error)** | 0x4 |

The 96 I/O errors begin at the first Windows I/O (20:38:17.699, `refusals 1 nvme-err 7 c34`). The model's
`refusals` counter rises **exactly 1:1** with them (`refusals 96 nvme-err 102`), while the backend reports `rerr 0
werr 0 wref 0`. So every failing READ had a guest buffer that the resident's guest-memory admission
(`hv_storage_guest_span`, provider refusal) rejected. The PRP planner then failed with 0x4. Nothing reached the
SSD.

## Root cause: the lease protected memory the firmware gives to Windows

The storage contract (launcher `storage_contract.py`) protected the four boot-time allocations whole. Mu's EFI
memory map in this run (kd-bridge `[con] Memory Range` lines) hands most of them to the OS:

| contract "immutable" span | Mu memory type | OS-owned |
|---|---|---|
| allocated ADT 0x1010e274000..0x1010e32c000, bootargs page ..0x1010e330000 | 7 Conventional (J813 EARLY copied them to the handoff block) | yes |
| FD 0x1010e400000..0x10110200000 (30 MiB) | 4 BootServicesData (free after ExitBootServices) | yes |
| appended RAM disk 0x10110200000..0x10114601000 | 0 Reserved | no |
| "appended media" tail 0x10114601000..0x10114604000 | 7 | yes |
| Mu handoff 0x10400000000..0x104000bc000 | 0 Reserved | no |
| Mu handoff tail 0x104000bc000..0x10400404000 | 7 | yes |

Once Windows reused those pages (about 34 MiB, freed boot-services memory that is hot on its free lists) for NTFS
metadata buffers, the resident refused the DMA. The volume mount failed, giving UNMOUNTABLE_BOOT_VOLUME.

The twin admitted all guest RAM (`glue_fi.c`), so it never hit this. The firmware and boot loader never placed
buffers there.

Protecting guest-owned RAM from guest-programmed DMA has no isolation value: the guest CPU can write those pages
anyway. The resident, its heaps, the holes, MMIO, the framebuffer/shadow and the synthetic frames stay protected.

## Fix

| file:line | change |
|---|---|
| `install/.../runB/launcher/storage_contract.py:93-112` | `build_contract(..., media=)`: when given, the boot-time allocations are released to the guest OS. Only what Mu keeps from the OS stays protected: the RAM-disk payload (exact, `[fd_base+PcdFdSize, +len(ramdisk))`) and the handoff's boot_args+ADT copy (`[h0, h0+PAGE+adt_size)` = Mu's type-0 0xBC000). The receipt lists `released_to_guest_os`. Same wire format; validation of both spans |
| `install/.../runB/launcher/run_windows_b.py:1243` | passes `media=` for a Mu run with the appended RAM disk |
| `src/hv/win/s1/hv_nvme_queue.c:693`, `.h` | a past-end READ completes **LBA Out of Range (0x80)** instead of Invalid Field (0x02); no backend I/O, as before |

**Diagnostics.**
- **`src/hv/win/hv_win_storage_mem.c`**: every admission refusal records its exact reason (lease state, context,
  args/span, not-guest-RAM, resident, protected (with the range base), s2-walk, s2-attr (with the leaf),
  pmu-scanning), the address and length, plus per-reason counts in `hv_sm_last_refusal`. No behaviour change.
- **`src/hv/win/hv_win_status.c`**:
  - an `HV/win r4.13 NVME-ERR #n` line for each of the first 50 non-zero completions, with queue, cid, status,
    opcode, SLBA, NLB, PRP1, NSID, the model's refused span (direction, address, length, reason) and the
    admission reason;
  - an extra NV line with errors by status, admission refusals by reason, and out-of-range reads.

**Launcher.** `LINGER_S` is now 3 (was 60): after a bugcheck, FROZEN or EL2 exception the run captures about 3 s
more of KD and console, then resets. The watcher polls every 2 s, so the reset comes ≤ 5 s after the bugcheck.

## Build (`S1N_STAGE=3 ./build.sh`)
There were two clean builds. The first preceded a one-line type fix (`u32` → `u64` in the refusal record, which
the host memory shim lacks); the second is the artifact.

| file | sha256 |
|---|---|
| `artifacts/m1n1-win-s1-r4.13-stage3.bin` | **`bd448da49a55fe03e13746574c350137acb9805d372244120053f6241a090c34`** |
| `artifacts/m1n1-raw.elf` | `1ed488e032471a63641afcf63c89da6c539ce2db297f2fe96290c200d552ff75` |
| `artifacts/m1n1.elf` | `202787763fbe1526abdfdc7267ba4f17d3538286f6d8e8677fa8a7ed22da27cf` |
| `artifacts/build_tag.h` | `aa152e5d142fe5a5b7ad1e1ea14bd95665e916d42d5defc3c0d8cb9d0f0c2a12` |

- `source-vs-r4.12.patch`: 7 files +128/−20, sha256 `d0a2f9f2…`.
- `_end` 0x7d0000 (unchanged), `controller` 0x42a000, `hv_win_storage_prepare` 0x5a900, `s1n_link_mem_ok`
  0x646c0, `hv_sm_last_refusal` 0x428918.
- Run B pins: check_len 0xd51e0, 249 relocs.

## Tests
| suite | result |
|---|---|
| `runB/tests/test_os_memory_b.py` (new; fixtures = this run's contract + Mu memory map) | The r4.12 contract **refuses >32 MiB of OS-usable memory** in exactly the four spans above. The r4.13 contract **admits every OS-usable range** (types 1/2/3/4/7) and keeps every type-0 page and the resident refused. It validates the media and handoff arguments, and without `media` the r4.12 protections are unchanged. 4/4 OK |
| `tests/nvme-intx-r412` (+ past-end READ) | 2032 checks, 0 failed. **r4.12's model fails** the 0x80 checks (it returns 0x2) |
| storage quick tier | **23/23 PASS**. Past-end READ oracles moved 0x02 → 0x80 in s0, s1, native secA/r45/r46, review-r3 e2e and the native mutant pattern; >MDTS (33 blocks) stays 0x02 (`tests-vs-r4.12.patch`) |
| `tests/native/r46_run.py` | pass |
| `link_mem_lock_test.py`, `link_path_test.py` | PASS, PASS |

## Run-B rebind (`runB-rebind/`)
- r4.13 bin added, layout regenerated (r4.12's kept as `image-layout-r4.12.json`).
- PIN and SOURCE_ROOT point at r4.13, and the progress lines read `r4.13-stage3 bd448da4`.
- `storage_contract.py` is now in `SHA256SUMS` (all OK), and a note was added to `RUNB-READY.md`.
- `test_run_b.py` **19/19 OK**; storage contract 16 OK; resilient upload OK; test_os_memory_b 4 OK.
- `run-b.sh` is unchanged (2d18f8ed): same command.
