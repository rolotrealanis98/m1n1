#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""cpuperf-A: run_windows.py --cpu-classes cppc, cppc_plan() and the m5w,cppc bytes (no device access).
   python3 -I tests/win-launcher-smp2/test_cppc.py
The byte layout checked here is the one Mu's J716sParseCppc reads (J716sAcpiGenLib.h):
<u64 base><u32 stride 0x40><u32 count><u32 flags> then count x 7 u32."""
import contextlib, io, pathlib, struct, sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "proxyclient" / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parent))
import run_windows  # noqa: E402

BASE = ["fd.bin", "--mu-profile", "m1a", "--route", "gic", "--ramdisk", "x.ntasi", "-C", "18"]
fails = 0


def check(name, cond, detail=""):
    global fails
    print(f"{name:66s} {'ok' if cond else 'FAILED ' + detail}")
    fails += not cond


def parse(extra, base=BASE):
    sys.argv = ["run_windows.py"] + base + extra
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            return run_windows.parse_args()[1]
    except SystemExit:
        return None


class Node:
    def __init__(self, name, ctype, state="waiting"):
        self.name = name
        self._properties = {"state": state}
        if ctype is not None:
            self._properties["cluster-type"] = ctype


# J716s: cpu0-11 M in two MPIDR clusters, cpu12-17 P (runs/20261110-213116-smpN c18a4 adt-guest.bin)
MPIDR = [0x80040000 + i for i in range(6)] + [0x80040100 + i for i in range(6)] + [0x80010200 + i for i in range(6)]


def j716s(n=18):
    nodes = [Node(f"cpu{i}", "M" if i < 12 else "P", "running" if i == 0 else "waiting") for i in range(18)]
    vcpus = [(i, f"cpu{i}", MPIDR[i], i == 0) for i in range(n)]
    return {"/cpus": nodes}, nodes, vcpus


# --- argument handling -------------------------------------------------------------------------
check("default stays uniform", parse([]).cpu_classes == "uniform")
a = parse(["--cpu-classes", "cppc"])
check("--cpu-classes cppc accepted", a is not None and a.cpu_classes == "cppc")
check("cppc: default page 0xf3c000000", a.cppc_page == 0xF3C000000)
check("cppc: CPPC frame joins the window frames", ("CPPC", 0xF3C000000, 0x4000) in run_windows.window_frames(a))
check("uniform: no CPPC frame", all(f[0] != "CPPC" for f in run_windows.window_frames(parse([]))))
check("cppc + --route aic refused", parse(["--cpu-classes", "cppc"], [x if x != "gic" else "aic" for x in BASE]) is None)
check("cppc: unaligned page refused", parse(["--cpu-classes", "cppc", "--cppc-page", "0xf3c002000"]) is None)
check("cppc: page on the PL011 refused", parse(["--cpu-classes", "cppc", "--cppc-page", "0xf20000000"]) is None)
check("cppc: page on the GICR refused", parse(["--cpu-classes", "cppc", "--cppc-page", "0xf10000000"]) is None)
check("cppc: page outside the window refused", parse(["--cpu-classes", "cppc", "--cppc-page", "0xf40000000"]) is None)
check("--cppc-perf without cppc refused", parse(["--cppc-perf", "M=1,1,1,1,1,1"]) is None)
a = parse(["--cpu-classes", "cppc", "--cppc-perf", "P=2000,1500,500,300,1000,4000"])
check("--cppc-perf override applied", a is not None and a.cppc_table["P"] == (2000, 1500, 500, 300, 1000, 4000)
      and a.cppc_table["M"] == run_windows.CPPC_PERF_T6050["M"])
for bad in ["P=1,2,1,1,1,1", "P=10,10,10,0,1,1", "P=10,10,10,11,1,1", "P=70000,1,1,1,1,1", "P=10,10,5,5,5,4",
            "X=10,10,5,5,1,1", "P=10,10,5,5,1", "P=a,b"]:
    check(f"--cppc-perf {bad} refused", parse(["--cpu-classes", "cppc", "--cppc-perf", bad]) is None)

# --- the table itself: ordering, P above M ------------------------------------------------------
m, p = run_windows.CPPC_PERF_T6050["M"], run_windows.CPPC_PERF_T6050["P"]
for t, v in run_windows.CPPC_PERF_T6050.items():
    try:
        run_windows.check_cppc_perf(t, v)
        ok = True
    except ValueError:
        ok = False
    check(f"table {t} passes Mu's ordering rules", ok)
check("P highest > M highest (Windows should prefer P)", p[0] > m[0])
check("P nominal > M nominal", p[1] > m[1])
check("formula: P highest = 1024 at 4608 MHz", p[0] == 1024)
check("formula: M highest = round(1024*4308/4608*0.6)", m[0] == round(1024 * 4308 / 4608 * 0.6))
check("formula: P lowest = round(1024*1308/4608)", p[3] == round(1024 * 1308 / 4608))

# --- cppc_plan bytes ----------------------------------------------------------------------------
adt, nodes, vcpus = j716s()
prop, lines = run_windows.cppc_plan(adt, vcpus, 0xF3C000000)
check("m5w,cppc length = 20 + 18*28", len(prop) == 20 + 18 * 28, str(len(prop)))
base, stride, count, flags = struct.unpack_from("<QIII", prop)
check("header: base, stride 0x40, count 18, flags PSD", (base, stride, count, flags) == (0xF3C000000, 0x40, 18, 1))
ent = [struct.unpack_from("<7I", prop, 20 + 28 * i) for i in range(18)]
check("CPUs 0-11 carry the M values", all(e[:6] == m for e in ent[:12]))
check("CPUs 12-17 carry the P values", all(e[:6] == p for e in ent[12:]))
check("domains: 0 x6, 6 x6, 12 x6", [e[6] for e in ent] == [0] * 6 + [6] * 6 + [12] * 6, str([e[6] for e in ent]))
prop2, _ = run_windows.cppc_plan(adt, vcpus, 0xF3C000000, psd=False)
check("--no-cppc-psd clears flag bit 0 only", prop2[:16] == prop[:16] and prop2[16:20] == b"\0\0\0\0"
      and prop2[20:] == prop[20:])
check("cppc mode leaves the guest ADT cluster-types alone",
      [n._properties["cluster-type"] for n in nodes] == ["M"] * 12 + ["P"] * 6)

# 6 vCPUs (cluster 0 only), 2 vCPUs
adt, nodes, vcpus = j716s(6)
prop, _ = run_windows.cppc_plan(adt, vcpus, 0xF3C000000)
check("-C 6: count 6, all M, one domain", struct.unpack_from("<I", prop, 12)[0] == 6 and
      all(struct.unpack_from("<7I", prop, 20 + 28 * i)[6] == 0 for i in range(6)))

# vCPU order other than ADT order: domain = first vCPU index of that cluster
adt, nodes, _ = j716s()
order = [0, 12, 1, 13, 6]
vcpus = [(i, f"cpu{i}", MPIDR[i], i == 0) for i in order]
prop, _ = run_windows.cppc_plan(adt, vcpus, 0xF3C000000)
check("mixed order: domains follow the first vCPU of each cluster",
      [struct.unpack_from("<7I", prop, 20 + 28 * i)[6] for i in range(5)] == [0, 1, 0, 1, 4])

# failures: missing cluster-type, unknown type, bad page
adt, nodes, vcpus = j716s()
del nodes[13]._properties["cluster-type"]
try:
    run_windows.cppc_plan(adt, vcpus, 0xF3C000000)
    check("missing cluster-type refused", False)
except run_windows.PlanError:
    check("missing cluster-type refused", True)
adt, nodes, vcpus = j716s()
nodes[3]._properties["cluster-type"] = "E"
try:
    run_windows.cppc_plan(adt, vcpus, 0xF3C000000)
    check("type without values (E) refused", False)
except run_windows.PlanError:
    check("type without values (E) refused", True)
for page in (0xF3C000040, 0xF3C002000):
    try:
        run_windows.cppc_plan(adt, vcpus, page)
        check(f"page {page:#x} refused", False)
    except run_windows.PlanError:
        check(f"page {page:#x} refused", True)

print(f"{fails} failed")
sys.exit(1 if fails else 0)
