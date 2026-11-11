# Local r4.5 adaptation of twin-v2

The parent `tooling/write-safety-20261110/twin-v2` remains read-only. This local copy retargets
its four original faults to r4.5's constant bounds, stage-3 bound, and terminal aborts.
Every adversarial case runs in a separate process against a separate APFS-cloned sparse image.
The same D1 outside-disk verifier, D2 command log and D3 independent fake counter are used.
The fake accepts/stores out-of-window writes; none is hidden by the model.

Run from the resident root (all temporary writes remain in this directory):

```sh
TMPDIR="$PWD/scratch" python3 -I tests/twin-v2/twin_proof.py "$PWD"
TMPDIR="$PWD/scratch" python3 -I tests/twin-v2/stage_proof.py "$PWD"
```

`stage_proof.py` runs complete resident canaries on stages 1/2 and verifies restored bytes in
zone C/S against the original file. The adversarial zone calls enter through a host-test-only
helper simulating the canary caller; that helper is absent from production builds. Broken stage
edges are detected by D2's independent stage-range rule even though they remain inside p7.
Stage 0 permits flush but rejects a simulated write. Source hashes accompany compiled models.

These are native C-stack model tests. No resident firmware binary, QEMU/WinPE gate, launcher,
capsule, or physical device was executed. The original README is retained as provenance and
describes additional historical files/runs that are not copied here.
