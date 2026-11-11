# SPDX-License-Identifier: MIT
"""M5WN registration wire for hv_win_m5wn_configure (wireless/n1-20261111).

Same layout and the same range policy as the S1 storage contract (storage_contract.build_contract,
r4.14): guest RAM = the final m5w,memory ranges; protected = resident + future heaps, ADT, boot_args
page, FD + media, Mu handoff, holes, everything below physical RAM, every synthetic frame, the native
framebuffer and the EL2 shadow. Only the 64-byte descriptor differs (M5WN, REG/CTRL/GSIV, backend)."""
import hashlib
import struct

PAGE = 0x4000
HEAP_PAD = 32 << 20
U64 = 1 << 64
WN_MAGIC = 0x4E57354D
REG = 0xF38108000
CTRL = 0xF3810C000
GSIV = 69
FRAME = ("M5WN", REG, 0x8000)  # both 16 KiB granules


def descriptor(backend=1):
    d = struct.pack("<IIQQII", WN_MAGIC, 1, REG, CTRL, GSIV, backend) + bytes(32)
    assert len(d) == 64
    return d


def _int(v):
    if isinstance(v, bool) or not isinstance(v, int):
        raise ValueError("integer required")
    return int(v)


def _extent(a, b):
    a, b = _int(a), _int(b)
    if not 0 <= a < b < U64:
        raise ValueError("invalid address interval")
    return a, b


def _overlap(a, b):
    return a[0] < b[1] and b[0] < a[1]


def build_registration(plan, *, resident_base, heap_top, adt_size, epoch, frames, shadow=None,
                       used_gsivs=(), backend=1):
    epoch, resident_base, heap_top, adt_size = map(_int, (epoch, resident_base, heap_top, adt_size))
    if not 0 < epoch < U64 or GSIV in used_gsivs:
        raise ValueError("invalid epoch or occupied M5WN interrupt")
    ram = [_extent(a, b) for a, b in plan["ranges"]]
    if not 1 <= len(ram) <= 64 or any(a % PAGE or b % PAGE for a, b in ram):
        raise ValueError("RAM geometry")
    if any(a[1] > b[0] for a, b in zip(ram, ram[1:])):
        raise ValueError("RAM ranges must be sorted and disjoint")
    universe = _extent(*plan["universe"])
    if any(not universe[0] <= a < b <= universe[1] for a, b in ram):
        raise ValueError("RAM outside declared physical universe")
    if not universe[0] <= resident_base < heap_top:
        raise ValueError("resident/heap identity outside physical universe")
    resident = _extent(universe[0], (heap_top + HEAP_PAD + PAGE - 1) & -PAGE)
    if any(_overlap(resident, x) for x in ram):
        raise ValueError("resident and future heap allowance overlap guest RAM")
    if not 0 < adt_size <= 4 << 20:
        raise ValueError("invalid allocated ADT size")
    adt_size = (adt_size + PAGE - 1) & -PAGE
    immutable = [
        ("allocated ADT", _extent(plan["adt_base"], plan["adt_base"] + adt_size)),
        ("bootargs page", _extent(plan["bootargs"], plan["bootargs"] + PAGE)),
        ("complete FD and appended media allocation", _extent(plan["fd_base"], plan["fd_end"])),
    ]
    if plan.get("handoff"):
        immutable.append(("Mu handoff", _extent(*plan["handoff"])))
    for i, (_, span) in enumerate(immutable):
        if not any(a <= span[0] < span[1] <= b for a, b in ram):
            raise ValueError("immutable allocation outside one guest RAM range")
        if any(_overlap(span, o) for _, o in immutable[i + 1:]):
            raise ValueError("immutable allocations overlap")
    protected = [("resident and future heaps", resident)] + immutable
    for a, b, reason in plan["holes"]:
        span = _extent(a, b)
        if any(_overlap(span, x) for x in ram):
            raise ValueError("excluded hole overlaps guest RAM")
        protected.append(("hole: " + reason, span))
    if universe[0]:
        protected.append(("below physical RAM, including platform MMIO", (0, universe[0])))
    spans = []
    for name, base, size in frames:
        span = _extent(base, base + size)
        if base % PAGE or any(_overlap(span, x) for x in ram + spans):
            raise ValueError("synthetic frame alignment/overlap")
        spans.append(span)
        protected.append(("synthetic " + name, span))
    if (REG, REG + 0x8000) not in spans:
        raise ValueError("M5WN frame missing from the synthetic frames")
    for name, span in (("native framebuffer", plan.get("fb")), ("EL2 shadow", shadow)):
        if span is not None:
            span = _extent(*span)
            if any(_overlap(span, x) for x in ram):
                raise ValueError("framebuffer/shadow intersects guest RAM")
            protected.append((name, span))
    if not 1 <= len(protected) <= 128:
        raise ValueError("protected range count exceeds resident contract")
    header = descriptor(backend) + struct.pack("<QQQII", epoch, resident[0], resident[1] - resident[0],
                                               len(ram), len(protected))
    assert len(header) == 96
    wire = header + b"".join(struct.pack("<QQ", a, b - a) for a, b in ram + [s for _, s in protected])
    return wire, {
        "schema": 1, "epoch": epoch, "wire_sha256": hashlib.sha256(wire).hexdigest(), "wire_bytes": len(wire),
        "resident": list(resident), "ram": ram,
        "protected": [{"reason": r, "start": a, "end": b} for r, (a, b) in protected],
        "backend": "fake gateway (no Apple hardware)" if backend == 1 else backend,
    }
