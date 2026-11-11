#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""sched1: run_windows.py --cpu-classes and uniform_cluster_types() (no device access).
   python3 -I tests/win-launcher-smp2/test_cpu_classes.py"""
import contextlib, io, pathlib, sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "proxyclient" / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parent))
import run_windows  # noqa: E402

BASE = ["fd.bin", "--mu-profile", "m1a", "--route", "gic", "--ramdisk", "x.ntasi", "-C", "18"]
fails = 0


def check(name, cond, detail=""):
    global fails
    print(f"{name:60s} {'ok' if cond else 'FAILED ' + detail}")
    fails += not cond


def parse(extra):
    sys.argv = ["run_windows.py"] + BASE + extra
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


class Adt(dict):
    pass


def j716s():
    """The J716s /cpus layout the hardware ADT has (runs/20261110-213116-smpN c18a4 adt-guest.bin):
    cpu0-11 cluster-type M (cpu0 running), cpu12-17 P."""
    nodes = [Node(f"cpu{i}", "M" if i < 12 else "P", "running" if i == 0 else "waiting") for i in range(18)]
    return Adt({"/cpus": nodes}), nodes


a = parse([])
check("default --cpu-classes is uniform", a is not None and a.cpu_classes == "uniform")
a = parse(["--cpu-classes", "adt"])
check("--cpu-classes adt accepted", a is not None and a.cpu_classes == "adt")
check("--cpu-classes bogus refused", parse(["--cpu-classes", "bogus"]) is None)

adt, nodes = j716s()
changed = run_windows.uniform_cluster_types(adt)
check("J716s: 6 nodes changed (cpu12-17)", [n for n, _, _ in changed] == [f"cpu{i}" for i in range(12, 18)],
      str(changed))
check("J716s: every node now M", all(n._properties["cluster-type"] == "M" for n in nodes))
check("J716s: P->M recorded", all(o == "P" and w == "M" for _, o, w in changed))
check("J716s: second call changes nothing", run_windows.uniform_cluster_types(adt) == [])

# the boot CPU's type wins even when it is not cpu0 / not first
nodes = [Node("cpu0", "M"), Node("cpu1", "P", "running"), Node("cpu2", "M")]
changed = run_windows.uniform_cluster_types(Adt({"/cpus": nodes}))
check("boot CPU (state running) type is used", [n._properties["cluster-type"] for n in nodes] == ["P"] * 3,
      str(changed))

# nodes without the property are left alone (Mu then reports homogeneous anyway)
nodes = [Node("cpu0", "M", "running"), Node("cpu1", None)]
run_windows.uniform_cluster_types(Adt({"/cpus": nodes}))
check("node without cluster-type is not given one", "cluster-type" not in nodes[1]._properties)

# boot CPU without a usable type: nothing changes
nodes = [Node("cpu0", None, "running"), Node("cpu1", "P")]
check("no boot type: no change", run_windows.uniform_cluster_types(Adt({"/cpus": nodes})) == [] and
      nodes[1]._properties["cluster-type"] == "P")

# Mu's ranking (J716sHandoffLib.c ClusterTypeRank + dense renumbering) on the result: one class
rank = {"E": 0, "M": 1, "P": 2}
adt, nodes = j716s()


def mu_classes(ns):
    r = [rank[n._properties["cluster-type"]] for n in ns]
    dense = {v: i for i, v in enumerate(sorted(set(r)))}
    return [dense[v] for v in r]


check("Mu classes before: 12 x 0 then 6 x 1", mu_classes(nodes) == [0] * 12 + [1] * 6)
run_windows.uniform_cluster_types(adt)
check("Mu classes after: 18 x 0", mu_classes(nodes) == [0] * 18)

print(f"{fails} failed")
sys.exit(1 if fails else 0)
