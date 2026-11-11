#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""smp2/smpN: run_windows.py argument gates for -C (no device access; parse_args only).
   python3 -I tests/win-launcher-smp2/test_args.py"""
import contextlib, io, pathlib, sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "proxyclient" / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parent))
import run_windows  # noqa: E402

BASE = ["fd.bin", "--mu-profile", "m1a", "--route", "gic", "--ramdisk", "x.ntasi"]


def parse(extra):
    sys.argv = ["run_windows.py"] + BASE + extra
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            _, args = run_windows.parse_args()
        return args, None
    except SystemExit:
        return None, err.getvalue().strip().splitlines()[-1]


fails = 0


def check(name, cond, detail=""):
    global fails
    print(f"{name:55s} {'ok' if cond else 'FAILED ' + detail}")
    fails += not cond


a, e = parse([])
check("default stays 1 vCPU", a is not None and a.cpus == 1, str(e))
a, e = parse(["-C", "2"])
check("-C 2 accepted on the gic route", a is not None and a.cpus == 2, str(e))
a, e = parse(["-C", "2", "--pmu-rewrite", "el2"])
check("-C 2 with --pmu-rewrite el2", a is not None, str(e))
for n in (3, 6, 18, 24):
    a, e = parse(["-C", str(n)])
    check(f"-C {n} accepted on the gic route (smpN)", a is not None and a.cpus == n, str(e))
a, e = parse(["-C", "25"])
check("-C 25 refused (MAX_CPUS 24)", a is None and "1..24" in e, str(e))
a, e = parse(["-C", "6", "--fb-shadow"])
check("-C 6 --fb-shadow refused", a is None and "-C 1" in e, str(e))
sys.argv = ["run_windows.py", "fd.bin", "--mu-profile", "m0", "-C", "6"]
try:
    with contextlib.redirect_stderr(io.StringIO()):
        run_windows.parse_args()
    check("aic route -C 6 refused", False)
except SystemExit:
    check("aic route -C 6 refused", True)
a, e = parse(["-C", "0"])
check("-C 0 refused", a is None, str(e))
a, e = parse(["-C", "2", "--fb-shadow"])
check("-C 2 --fb-shadow refused", a is None and "-C 1" in e, str(e))
a, e = parse(["-C", "1", "--fb-shadow", "--fb-dirty"])
check("-C 1 --fb-shadow --fb-dirty still accepted", a is not None, str(e))
sys.argv = ["run_windows.py", "fd.bin", "--mu-profile", "m0", "-C", "2"]
try:
    with contextlib.redirect_stderr(io.StringIO()):
        run_windows.parse_args()
    check("aic route -C 2 refused", False)
except SystemExit:
    check("aic route -C 2 refused", True)
print(f"{fails} failed")
sys.exit(1 if fails else 0)
