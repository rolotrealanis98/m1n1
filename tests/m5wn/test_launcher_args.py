# SPDX-License-Identifier: MIT
"""run_windows.py argument path for the M5WN/N1 options, exactly as hw/m5wn-run.sh passes them
(parse_args only: this runs the synthetic-window frame check that failed run m1a2). No device access."""
import contextlib, io, pathlib, sys
TOOLS = pathlib.Path(__file__).resolve().parents[2] / "source/proxyclient/tools"
sys.path.insert(0, str(TOOLS)); sys.path.insert(0, str(TOOLS.parent))
import run_windows, m5wn_contract  # noqa: E402

M5WN_RUN = ["--mu-profile", "m1a", "--route", "gic", "-C", "1", "-l", "m5wn.log", "--label", "m5wn-m1",
            "--ramdisk", "x.ntasi", "--sha256", "0" * 64, "--pmu-rewrite", "el2", "--m5wn-fake", "-S",
            "--run-dir", "/nonexistent", "fd.bin"]
fails = 0
def parse(argv):
    sys.argv = ["run_windows.py"] + argv
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            return run_windows.parse_args()[1], None
    except SystemExit:
        return None, (err.getvalue().strip().splitlines() or ["?"])[-1]
def check(name, cond, detail=""):
    global fails
    print(f"{name:60s} {'ok' if cond else 'FAILED ' + detail}"); fails += not cond

a, e = parse(M5WN_RUN)
check("hw/m5wn-run.sh argument list parses (frame check)", a is not None, str(e))
fr = run_windows.window_frames(a) if a else []
check("M5WN frame is 64 KiB aligned and covers both pages", ("M5WN", 0xF38100000, 0x10000) in fr, str(fr))
a, e = parse(M5WN_RUN + ["--builtin-keyboard"])
check("with --builtin-keyboard: one shared frame", a is not None and ("HIDM+M5WN", 0xF38100000, 0x10000) in run_windows.window_frames(a), str(e))
a, e = parse([x for x in M5WN_RUN if x != "--m5wn-fake"] + ["--builtin-keyboard"])
check("HIDM alone unchanged", a is not None and ("HIDM", 0xF38100000, 0x8000) in run_windows.window_frames(a), str(e))
a, e = parse([x for x in M5WN_RUN if x != "--m5wn-fake"] + ["--m5wn-n1", "ftab.bin"])
check("--m5wn-n1 parses, backend 2", a is not None and a.m5wn_backend == 2 and not a.m5wn_fake, str(e))
a, e = parse(M5WN_RUN + ["--m5wn-n1", "ftab.bin"])
check("--m5wn-n1 with --m5wn-fake refused", a is None and "exclusive" in (e or ""), str(e))
a, e = parse([x if x != "1" else "2" for x in M5WN_RUN])
check("--m5wn-fake with -C 2 refused", a is None, str(e))
# the contract accepts the launcher's frame list
a, _ = parse(M5WN_RUN)
G = 1 << 30
plan = dict(universe=(0x10000000000, 0x10000000000 + 48 * G), ranges=[(0x10000000000 + G, 0x10000000000 + 40 * G)],
            holes=[(0x10000000000, 0x10000000000 + G, "m1n1")], adt_base=0x10000000000 + 2 * G,
            bootargs=0x10000000000 + 2 * G + 0x400000, fd_base=0x10000000000 + 3 * G, fd_end=0x10000000000 + 3 * G + 0x1000000,
            handoff=None, fb=None)
try:
    m5wn_contract.build_registration(plan, resident_base=0x10000800000, heap_top=0x10020000000, adt_size=0x200000,
                                     epoch=1, frames=run_windows.window_frames(a)); ok = True
except ValueError as x:
    ok = False; print(x)
check("contract accepts the launcher's frames", ok)
print(f"m5wn-launcher-args: {'PASS' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
