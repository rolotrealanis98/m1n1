# r4.6 build and host verification

**Firmware binaries: not built**, at all stages. No container command was attempted, as instructed. Firmware linkage, final BSS layout, image hashes, physical cache coherence, persistence, Windows operation and hardware recovery are **unverified**. All executed compiles are native host tests or host LLVM inspection. No network, install, hardware, SSH, USB, serial or upstream action was used.

The source was copied from r4.5 without `source/build/`, `.git`, caches, prior verification directories or compiled outputs. Font/logo `.bin` files are source assets, retained. `source/AGENTS.md` is the exact owner override. The smp2h port includes its required `mcc.h` declaration. The stage policy/pins and existing `M5W-S1N-R4.5-STAGE-N` policy marker are retained; the build script's version/output filenames identify r4.6. A future launcher must bind the actual built image hash and stage stat, not use the inherited policy-marker string as an image attestation.

## Future firmware builds (not executed)

The wrapper requires the trusted offline Rust archive omitted by the no-binaries copying rule. In a separately authorised build session, prepare the local prerequisite inside this package:

```sh
mkdir -p source/build scratch
cp ../s1-resident-r4.5-native-rw-20261110/source/build/librust.a source/build/librust.a
# Expected SHA256: 4ed52ac9ef8456ba088671c64531479d71cc6705d82ec231cc406a7c96efc304
```

With the existing pinned `m5pro-windows/mu-build:f43` image and permitted container runtime, build each stage (no download):

```sh
export TMPDIR="$PWD/scratch"
S1N_STAGE=0 env -u CDPATH bash build.sh "$PWD/artifacts/stage0"
S1N_STAGE=1 env -u CDPATH bash build.sh "$PWD/artifacts/stage1"
S1N_STAGE=2 env -u CDPATH bash build.sh "$PWD/artifacts/stage2"
S1N_STAGE=3 env -u CDPATH bash build.sh "$PWD/artifacts/stage3"
```

Repeat each build into a distinct directory and compare hashes before any hardware qualification. The wrapper preserves the existing bring-up build flags and stage enforcement. These are instructions, not claimed build results. Binaries remain **not built**.

## Host reproduction

All commands write only inside this package, including temporary compiler outputs:

```sh
mkdir -p scratch
export TMPDIR="$PWD/scratch" PYTHONDONTWRITEBYTECODE=1
for stage in 0 1 2 3; do
  S1N_STAGE=$stage S1_NO_MUTANTS=1 python3 -I tests/native/run.py > "scratch/stage$stage.json" 2>&1
done
python3 -I tests/native/r46_inherited.py > scratch/inherited.json
python3 -I tests/native/r46_run.py > scratch/r46.json
python3 -I tests/native/r46_mutant_followup.py > scratch/mutant-followup.json
python3 -I tests/native/r46_overlay_mutants.py > scratch/overlay-mutants.json
python3 -I tests/native/r46_context_path.py
python3 -I tests/native/r46_mcc.py
python3 -I tests/native/r46_stage_inspection.py
SKIP_NATIVE=1 bash run-all-tests.sh
python3 -I tests/twin-v2/twin_proof.py "$PWD" > scratch/twin.json
python3 -I tests/twin-v2/stage_proof.py "$PWD" > scratch/twin-stages.json
```

The inherited native suite exits nonzero on preserved incompatible expectations. `r46_inherited.py` continues through all scenarios and mutants and records regressions relative to each unmutated baseline; its process exit alone is not a PASS gate. The twin uses sparse synthetic images and the read-only verifier under `tooling/write-safety-20261110/`; it needs no firmware binary. Its inherited labels say “r4.5 (unmodified)”; the `resident` field and source hashes identify this r4.6 tree.

See NOTES.md for every changed expectation, REVIEW-GUIDE.md for source/test mapping, TEST-RESULTS.json for counts and failures, source-vs-r4.5.patch for source edits, and CHANGED-FILES.tsv for provenance and hashes.
