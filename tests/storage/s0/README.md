# S0: read-only synthetic NVMe queue experiment

This host-only experiment reuses the actual NVMe command/queue implementation from
`paulsmir/m1n1_windows@9cd80ac652ac404e92ae279deeaec8c629d7d184`. It exercises admin
and I/O submission/completion rings, Identify, Create/Delete SQ/CQ, PRPs and IRQ
callbacks against an immutable 40-sector RAM fixture. It neither builds firmware
nor opens a device. Physical ANS, SART, RTKit, PCI/MMIO emulation and target transport
are not linked. It does not expose the MacBook's disk or qualify Windows storage.

Run from the repository root (Clang with ASan/UBSan and Python 3 required):

```sh
python3 -I tools/storage/s0/run.py
# Or retain evidence in a chosen NEW directory:
python3 -I tools/storage/s0/run.py --out /tmp/m5w-s0-new-result
```

The runner verifies every pinned source SHA256 and original Git blob identity,
compiles/runs the unmodified donor suite, then compiles/runs the adapted frontend
and our actual RAM backend/memory-span validator. Both suites use ASan+UBSan,
`-Werror`, and failure-on-first-sanitizer-error. LeakSanitizer is disabled on macOS
where that option is unsupported; AddressSanitizer and UndefinedBehaviorSanitizer remain enabled. It records commands, compiler,
source/binary/log hashes and result in `receipt.json`; failures also leave a receipt.
Compilation has a 60-second per-process limit; each C suite has 15 seconds.
The output directory is private and never reused. Tests are offline; no download
or package installation occurs in the runner.

## Code and provenance

- `upstream/`: byte-exact pinned donor sources, original test and MIT LICENSE.
  These files are references; only the original test run compiles these copies.
- `src/hv_nvme_queue.c/.h`: adapted donor queue implementation. `adaptation.patch`
  is the exact diff against upstream, with `src/` paths for review/application to
  a donor-layout scratch tree. Existing MIT notices are preserved.
- `src/guest_memory.c/.h`: complete-span RAM permission validator. No identity
  translation or physical mapping fallback. A zero permission page models a hole,
  protected memory or MMIO exclusion; read-only pages reject output DMA copies.
- `src/ram_disk.c/.h`: const fixture and const descriptor, with independently
  checked sector/byte geometry and subtraction-based range checks. No write API.
  Each sector has a distinct first32-byte pattern and zero remainder; this is a
  protocol test fixture, not a valid GPT or installable disk image.
- `test_s0.c`: actual queued commands and completions, exact byte comparisons,
  malformed descriptors and callback tripwires. The large assertion count includes
  byte-by-byte unchanged-buffer/pattern checks; it is not a count of independent
  scenarios. `reset_cases` counts fixture resets, not a test-case coverage metric.
- `MANIFEST.json`: pinned repository/commit/blob identities, hashes for every
  source/document/test input, and fixture geometry/digest. `receipts/` retains the
  completed local run's receipt; binaries/logs live in its referenced output dir.

The adaptation removes raw `hv_ipa_to_pa`, validates whole queues at admission and
again at dispatch, snapshots PRP metadata, validates all destinations before any
backend read or output copy, and rejects unsupported flags/metadata. Queue spans
must be disjoint on admission. Payload spans must be disjoint from all active
SQ/CQ spans, each other, and the exact consumed PRP-list bytes (not the whole
metadata page). Half-open adjacent spans are allowed. Metadata is snapshotted
before validating/copying payloads. Rejected commands still post their normal
error CQE; the no-output guarantee applies to payload, not completion writes. S0 only accepts
READ on the I/O queue. WRITE, FLUSH, DSM/trim, write-zeroes, write-uncorrectable and
unknown I/O opcodes are rejected; administrative format/sanitize/firmware/vendor
commands also fail. S0 advertises neither DSM nor a volatile write cache. Explicit
FLUSH is deliberately unsupported for this immutable fixture; it is not a model
of persistent-device flush success. An accidentally supplied write/flush callback
is never called. The backend independently rejects an out-of-range request even
if frontend capacity is deliberately made larger in a regression case.

## Exercised gates

| Area | Actual checks |
|---|---|
| Discovery and queues | Controller/namespace/list Identify, capacity40 and4096-byte sectors, SQ/CQ setup/deletion, completion identity and IRQ publication |
| Data | First/last sectors, two-block read, unaligned first PRP, noncontiguous guest pages, maximum32-block/33-segment request, exact full output |
| Disk boundary | Wrong NSID, last-LBA crossing, UINT64_MAX LBA, oversized transfer, independently narrower backend, invalid/overflowing descriptor geometry |
| Guest boundary | Null/high/wrapped/out-of-arena spans, full queue extent crossing a hole, later queue-permission revocation, read-only destinations, unreadable list, zero/misaligned/out-of-range list entries, unsupported metadata/SGL/FUA flags |
| No mutation | Rejected commands leave output buffer unchanged, late bad PRP leaves all earlier destinations untouched, fixture and arena sentinels unchanged, zero backend write/flush calls |
| Alias policy | READ/Identify/GetLog reject every active queue as payload; late queue/list/duplicate-output aliases leave payload untouched and skip backend; exact metadata/output/queue adjacency is allowed; overlapping queues fail admission without state change |
| Queue lifecycle | Delete/recreate SQ resets both indices and cannot replay a stale READ from a CQ doorbell; successful admin-queue reset deasserts the external IRQ callback, while rejected reset preserves pending state |

This validates actual C logic with host-owned stable RAM mappings, not a live
stage-2 page table walk or DMA isolation. Existing admin-feature/AER compatibility
behavior is inherited and not broadly qualified. No claim is made about concurrent
vCPUs, async backend lifetime, interrupt hardware, volatile caches or power failure.

## Before S1

1. Independently review this bounded adaptation and test failures, including a
   mutation check if useful. Keep a narrow new experimental build, not the live
   resident source, for any later port.
2. Implement a real pinned guest-RAM-span provider from the current resident's
   memory plan and stage-2 permissions; solve mapping lifetime/concurrent guest
   mutation. The S0 interval policy compares guest IPAs; distinct IPAs aliasing the
   same physical bytes require an injective provider or additional physical-span
   overlap rejection. The host validator is not that
   provider, and a callback does not by itself establish hardware isolation.
3. Audit/port synthetic PCI BAR/ECAM lifecycle and level-SPI delivery to our vGIC;
   ensure Mu and Windows observe exactly one synthetic disk with no ANS mappings.
4. Specify the required NVMe controller/reset/features/namespace read-only
   presentation for inbox StorNVMe, including failure status and FLUSH semantics.
   The present read-only policy intentionally prioritizes rejection over a complete
   Windows-compatible controller. Do not infer PE discovery from the host suite.
5. Only then test Mu read followed by PE capacity/known-sector hash via the existing
   direct-KD channel. Physical backend stays absent. Native T6050 ANS ownership and
   split NVMe/NVMMU register banks are a separate S2 contract; `nvme_init()` reuse
   alone is not sufficient. No partitioning or write qualification belongs to S0/S1.

The alias policy is a conservative experimental admission rule, not a claim that
NVMe requires these exact restrictions. It may reject unusual host-driver layouts;
Windows StorNVMe compatibility remains unqualified. Independent historical receipts
are retained; the alias receipt binds the revised source and tests.
