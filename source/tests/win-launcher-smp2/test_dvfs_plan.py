#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""cpuperf-B: run_windows.py --cpu-dvfs / --dvfs-max and dvfs_plan() (no device access).
   python3 -I tests/win-launcher-smp2/test_dvfs_plan.py [--write OUT]   (OUT: the 18-vCPU config, for the C cross test)
Layout = hv_win_dvfs_core.h struct dvfs_cfg (632 bytes)."""
import contextlib, io, pathlib, struct, sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "proxyclient" / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parent))
import run_windows  # noqa: E402

ARGV = list(sys.argv)  # parse() below replaces sys.argv
BASE = ["fd.bin", "--mu-profile", "m1a", "--route", "gic", "--ramdisk", "x.ntasi", "-C", "18"]
fails = 0


def check(name, cond, detail=""):
    global fails
    print(f"{name:66s} {'ok' if cond else 'FAILED ' + detail}")
    fails += not cond


def parse(extra):
    sys.argv = ["run_windows.py"] + BASE + extra
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            return run_windows.parse_args()[1]
    except SystemExit:
        return None


class Node:
    def __init__(self, name, ctype):
        self.name = name
        self._properties = {"cluster-type": ctype}


MPIDR = [0x80040000 + i for i in range(6)] + [0x80040100 + i for i in range(6)] + [0x80010200 + i for i in range(6)]


def j716s(n=18):
    adt = {"/cpus": [Node(f"cpu{i}", "M" if i < 12 else "P") for i in range(18)]}
    return adt, [(i, f"cpu{i}", MPIDR[i], i == 0) for i in range(n)]


# arguments
check("default --cpu-dvfs off", parse([]).cpu_dvfs == "off")
check("--cpu-dvfs el2 without cppc refused", parse(["--cpu-dvfs", "el2"]) is None)
a = parse(["--cpu-classes", "cppc", "--cpu-dvfs", "el2"])
check("--cpu-classes cppc --cpu-dvfs el2 accepted", a is not None and a.cpu_dvfs == "el2" and a.dvfs_max == {})
check("--dvfs-max without cppc refused", parse(["--dvfs-max", "P=17"]) is None)
for bad in ["P=22", "M=17", "M=0", "X=3", "P=x", "P"]:
    check(f"--dvfs-max {bad} refused", parse(["--cpu-classes", "cppc", "--dvfs-max", bad]) is None)
a = parse(["--cpu-classes", "cppc", "--cpu-dvfs", "el2", "--dvfs-max", "M=12"])
check("--dvfs-max M=12 sets clamp and _CPC", a.dvfs_max == {"M": 12} and a.cppc_table["M"][:2] ==
      (run_windows.t6050_perf("M", 12),) * 2)
check("--dvfs-interval-us 999 refused", parse(["--cpu-classes", "cppc", "--dvfs-interval-us", "999"]) is None)

# the config
adt, vcpus = j716s()
cfg, lines = run_windows.dvfs_plan(adt, vcpus)
check("config is 632 bytes", len(cfg) == 632)
magic, ver, ncl, ncpu, interval = struct.unpack_from("<IIIIQ", cfg)
check("header: magic DVFS, v1, 3 clusters, 18 CPUs, 1 ms", (magic, ver, ncl, ncpu, interval) ==
      (0x53465644, 1, 3, 18, 1000000))
check("cluster_of: 0 x6, 1 x6, 2 x6", list(cfg[24:42]) == [0] * 6 + [1] * 6 + [2] * 6 and cfg[42:56] == bytes(14))
cl = [struct.unpack_from("<QII32I", cfg, 56 + 144 * i) for i in range(4)]
check("bases mc0 / mc1 / pc", [c[0] for c in cl[:3]] == [0x210e20000, 0x211e20000, 0x212e20000])
check("clamps M p1..p15, P p1..p17", [(c[1], c[2]) for c in cl[:3]] == [(1, 15), (1, 15), (1, 17)])
check("unused 4th cluster all zero", cl[3] == (0,) * 35)
mperf, pperf = list(cl[0][3:]), list(cl[2][3:])
check("M perf: p0 0, p1..p16 from the OPP table, p17+ 0", mperf[0] == 0 and all(mperf[1:17]) and not any(mperf[17:]))
check("P perf: p1..p21, p22+ 0", all(pperf[1:22]) and not any(pperf[22:]))
check("perf strictly increasing", all(x < y for x, y in zip(mperf[1:17], mperf[2:17])) and
      all(x < y for x, y in zip(pperf[1:22], pperf[2:22])))
check("M p15 perf = _CPC highest 796", mperf[15] == 796 == run_windows.CPPC_PERF_T6050["M"][0])
check("P p17 perf = _CPC highest 957", pperf[17] == 957 == run_windows.CPPC_PERF_T6050["P"][0])
check("P p21 perf = 1024", pperf[21] == 1024)
cfg2, _ = run_windows.dvfs_plan(adt, vcpus, {"P": 21}, 5000000)
check("--dvfs-max P=21 / 5 ms", struct.unpack_from("<II", cfg2, 56 + 144 * 2 + 8) == (1, 21) and
      struct.unpack_from("<Q", cfg2, 16)[0] == 5000000)

# -C 6: one cluster; order follows the vCPUs
adt, vcpus = j716s(6)
cfg, _ = run_windows.dvfs_plan(adt, vcpus)
check("-C 6: 1 cluster mc0", struct.unpack_from("<I", cfg, 8)[0] == 1 and struct.unpack_from("<Q", cfg, 56)[0] == 0x210e20000)
adt, _ = j716s()
vcpus = [(i, f"cpu{i}", MPIDR[i], i == 0) for i in [0, 12, 6]]
cfg, _ = run_windows.dvfs_plan(adt, vcpus)
check("mixed order: clusters mc0, pc, mc1", [struct.unpack_from("<Q", cfg, 56 + 144 * i)[0] for i in range(3)] ==
      [0x210e20000, 0x212e20000, 0x211e20000] and list(cfg[24:27]) == [0, 1, 2])

# refusals
adt, vcpus = j716s()
for desc, mut in [("wrong cluster-type", lambda a, v: a["/cpus"][13]._properties.update({"cluster-type": "M"})),
                  ("MPIDR outside the clusters", lambda a, v: v.__setitem__(3, (3, "cpu3", 0x80050000, False)))]:
    a2, v2 = j716s()
    mut(a2, v2)
    try:
        run_windows.dvfs_plan(a2, v2)
        check(f"{desc} refused", False)
    except run_windows.PlanError:
        check(f"{desc} refused", True)
try:
    run_windows.dvfs_plan(adt, vcpus, interval_ns=999999)
    check("interval < 1 ms refused", False)
except run_windows.PlanError:
    check("interval < 1 ms refused", True)

if len(ARGV) == 3 and ARGV[1] == "--write":
    adt, vcpus = j716s()
    pathlib.Path(ARGV[2]).write_bytes(run_windows.dvfs_plan(adt, vcpus)[0])

print(f"{fails} failed")
sys.exit(1 if fails else 0)
