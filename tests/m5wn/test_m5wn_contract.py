# SPDX-License-Identifier: MIT
"""m5wn_contract wire layout and refusals; the resident parser's fixed header checks mirrored."""
import pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "source/proxyclient/tools"))
import m5wn_contract as c

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1; print("FAIL", what)

G = 1 << 30
plan = dict(universe=(0x10000000000, 0x10000000000 + 48 * G),
            ranges=[(0x10000000000 + 1 * G, 0x10000000000 + 40 * G)],
            holes=[(0x10000000000, 0x10000000000 + 1 * G, "iBoot/m1n1/heaps")],
            adt_base=0x10000000000 + 2 * G, bootargs=0x10000000000 + 2 * G + 0x400000,
            fd_base=0x10000000000 + 3 * G, fd_end=0x10000000000 + 3 * G + 0x30000000,
            handoff=(0x10000000000 + 4 * G, 0x10000000000 + 4 * G + 0x4000), fb=(0x9f0000000, 0x9f8000000))
frames = [("GICD", 0xF00000000, 0x10000), ("GICR", 0xF00100000, 0x20000), c.FRAME]
kw = dict(resident_base=0x10000000000 + 0x800000, heap_top=0x10000000000 + 0x20000000, adt_size=0x200000,
          epoch=123456789, frames=frames)
wire, rec = c.build_registration(plan, **kw)
magic, ver, reg, ctrl, gsiv, backend = struct.unpack_from("<IIQQII", wire, 0)
check((magic, ver, reg, ctrl, gsiv, backend) == (0x4E57354D, 1, 0xF38108000, 0xF3810C000, 69, 1), "descriptor")
check(wire[32:64] == bytes(32), "descriptor zero tail")
epoch, rb, rs, nr, np = struct.unpack_from("<QQQII", wire, 64)
check(epoch == 123456789 and nr == 1 and len(wire) == 96 + 16 * (nr + np), "header counts")
check(rb == plan["universe"][0] and rb + rs >= kw["heap_top"] + c.HEAP_PAD, "resident covers heap pad")
prot = [struct.unpack_from("<QQ", wire, 96 + 16 * (nr + i)) for i in range(np)]
check((0xF38108000, 0x8000) in prot, "M5WN frame protected")
check(any(b == plan["fd_base"] for b, _ in prot), "FD protected")
for bad, what in [(dict(frames=frames[:2]), "missing frame"), (dict(epoch=0), "zero epoch")]:
    try:
        c.build_registration(plan, **{**kw, **bad}); check(False, what)
    except ValueError:
        pass
try:
    c.build_registration(plan, **kw, used_gsivs=[69]); check(False, "gsiv clash")
except ValueError:
    pass
p2 = dict(plan, ranges=[(0x10000000000 + 1 * G, 0xF38110000 + 0x10000000000)])
try:
    c.build_registration(dict(plan, holes=[(plan["ranges"][0][0], plan["ranges"][0][0] + 0x4000, "x")]), **kw)
    check(False, "hole overlapping RAM")
except ValueError:
    pass
print(f"m5wn-contract: {'PASS' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
