# S1 host PCI/NVMe protocol model

This is an offline implementation checkpoint, **not firmware or Windows qualification**.
It adapts the actual `hv_pci.c`/`hv_nvme.c` protocol models from
`paulsmir/m1n1_windows@9cd80ac652ac404e92ae279deeaec8c629d7d184`, preserves exact
originals and MIT license under `upstream/`, and connects them to the reviewed S0
queue engine and immutable40-sector RAM fixture. There is no native ANS backend,
physical initialization, target mapping, serial transport or hardware operation.

Run from the repository root:

```sh
python3 -I tools/storage/s1/run.py
# Optional new private output directory:
python3 -I tools/storage/s1/run.py --out /tmp/m5w-s1-new-result
```

The runner verifies all source/dependency hashes and donor Git blob identities,
compiles the actual C protocol/queue/backend code with ASan/UBSan and `-Werror`,
runs the suite, then compiles six narrowly mutated copies and requires each to
fail a test assertion. A sanitizer crash alone does not count as detecting the
intended mutation. It records commands, compiler, binary/source/log hashes and
pass/fail JSON. Builds have60-second per-process limits and tests15 seconds.
LeakSanitizer is disabled on macOS because that option is unsupported; ASan and
UBSan remain enabled. Source inputs and immutable backing storage are never modified.

## What is implemented

- One PCI function00:00.0, class010802, with the donor's config identity and layout.
  Other functions/devices return all ones. ECAM permits aligned1/2/4-byte accesses
  inside one explicit bus window; it rejects unsupported/cross-dword/out-of-window
  accesses. No MSI/MSI-X capability or ITS is claimed.
- BAR0 is64-bit,16MiB, non-prefetchable. Sizing reads preserve type bits. Decoding
  requires MSE and an entire BAR within the registered aperture. Relocation first
  disables/resets the old decode, then calls checked unmap/map callbacks. Failed
  callbacks latch a fault, disable service/IRQ and prohibit automatic retry.
  These are host mocks, not evidence that a live stage-2 mapping is atomic.
- BME gates guest-memory callbacks, queue doorbells and controller enable. Clearing
  BME preserves queue state but suppresses the external IRQ and prevents queue
  progress; setting it again can deliver the existing pending level. MSE removal,
  BAR relocation and CC disable reset queue/controller state as documented in code.
- CAP/VS, CC/CSTS, AQA,64-bit ASQ/ACQ, INTMS/INTMC and four doorbells are adapted
  from the donor. NVMe register accesses support aligned dwords and aligned64-bit
  accesses for the64-bit registers; unsupported widths fail. Reserved dword writes
  below doorbells are ignored; accesses beyond the four supported doorbells fail.
  Unsupported CC geometry and bad admin queues report CFS. Live queue-register
  edits fail. Doorbell values are checked before conversion to the16-bit queue API.
- Actual S0 admin/IO commands and CQEs run behind that MMIO interface. The backend
  is hardwired to `s0_fixture`; write/flush backend callbacks do not exist. READ
  succeeds within bounds; write/destructive commands remain rejected by S0.
- Effective INTx level combines queue pending state, controller readiness, vector0
  mask, PCI MSE/BME and INTx-disable. Tests verify assertion/deassertion and pending
  level reappearance after masks clear. This does not exercise physical GIC EOI.
- The constructor requires a zero-initialized object and refuses live re-init.
  There is no unbounded loop, asynchronous request or pending native DMA. Immutable
  synchronous SHN completion does not claim persistent storage flush durability.
  Normal shutdown is rejected without changing state while any submitted SQ entry
  remains unprocessed behind CQ backpressure. The caller must drain that work first;
  a driver-compatible shutdown-in-progress state machine is not implemented.

## Test evidence and limits

The actual-C suite configures BARs/CC/admin and I/O queues through ECAM/MMIO;
checks Identify, known-sector bytes, completion identity, read-only failures,
invalid spans, width/doorbell rejection, reset while IRQ pending, mask/unmask,
BME admission, BAR resizing/relocation and mapping failure. Assertions include
byte-by-byte patterns, so the assertion count is not an independent scenario count.
Mutation tests remove BAR bounds, BME gates, interrupt masking, wide-doorbell
checks or reset IRQ deassertion and must be caught.
The sixth mutation removes pending-SQ shutdown admission.
An independent review reproduced CQ acknowledgment dispatching a deferred READ
after an early SHST_COMPLETE. The model now refuses that premature transition;
the two-command regression verifies that the second READ precedes accepted shutdown
and that subsequent CQ acknowledgment performs no backend operation.

`src/model.h` is a host adapter contract. The guest-span callback must provide a
complete contiguous readable/writable RAM span and stable lifetime. The S0 test
provider is **not** a live resident page-table implementation. The separate S1
memory work remains necessary: current `hv_translate()` translates guest VAs via
AT S12E1R/W rather than validating a PRP IPA; `hv_pt_walk()`/`hv_pt_is_ram()` alone
lose/omit the permissions and RAM provenance needed here. PMU page protection and
write-fault dirty tracking also need a deliberate policy before EL2 copies.

Remaining gates: integration of that stable permission provider and physical-alias
checks; a resident synthetic-window registration/hook adapter; real level-SPI
routing; ACPI/PCI root bridge and Mu NVMe modules; driver feature/read-only/reset
compatibility; timing and firmware-to-Windows takeover. Keep native ANS mappings
and Mu ANS flags excluded. This model has no bootable GPT, filesystem, persistence,
Windows guest test or internal SSD access. Its QEMU-style PCI identity is inherited
for protocol testing, not an independently allocated production vendor/device ID.
