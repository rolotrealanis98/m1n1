# smp2h: smp2g + T6050 TZ carveout unmap (EL2 SError fix). Build notes, 2026-11-10

Offline only; nothing touched the MacBook. Parent: `../s1-resident-smp2g-20261110` (bin `40779d71…419a`).
Analysis: `tooling/serror-20261110/` (check_adr.py / check_adr.out).

| sha256 | file |
|---|---|
| `5f7d6c38ad9b6471e81b3beed780e2dab0e005cda3ce668ca84ddf6c088239f9` | `artifacts/m1n1-win-s1-r3.2-smp2h.bin` (4 980 736 bytes = 0x4C0000, same as smp2g) |
| `9b7278ce2dc2cf76f575eb3e168ed1ba4e4de693f463427f3c8283c795d73d21` | `artifacts/m1n1.elf` |
| `04bb5ef326fd368916e09401b170ed66952bad66cbbc96a9e84f4cc77ae74c33` | `artifacts/build_tag.h` (`v2.0.0-169-g868807f9-dirty-s1r3.2-smp2h`) |
| `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | `artifacts/build_cfg.h` (unchanged) |
| `22798338ba13a9791918b0f14bcfa1b5289fd3ce22fa3a5dbc8da2d807310083` | `source-vs-smp2g.patch` (3 files, +93 / -2) |

Changes (nothing else differs from smp2g; librust.a by hash from r3.2-disp as before; AGENTS.md owner-override, byte-identical):
- `src/mcc.c:236-286` `mcc_unmap_adt_tz()`: unmap `region-id-4`, `region-id-2` from `/chosen/carveout-memory-map` in all four EL2 aliases (port of upstream d34014b1), bounds/alignment checked, bad entries logged and left mapped, recorded in `mcc_carveouts`.
- `src/mcc.c:316-325`: `mcc_unmap_carveouts()` calls it when the MCC is not initialised and `chip_id == T6050` (was: return -1).
- `src/mcc.c:288-314` + `src/mcc.h:19` + `src/exception.c:9,231-236`: on SError, print the carveout region holding `L2C_ERR_ADR[41:0]`.
- `build.sh` tag `-smp2h`; `compile-verify.sh` now covers mcc.c, exception.c, memory.c.

Verification: two clean builds in mu-build:f43 BIT-IDENTICAL (build1.txt/build2.txt); compile-verify clean; run-tests.sh all suites pass (test-results.txt).
