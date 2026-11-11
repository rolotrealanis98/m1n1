#!/usr/bin/env python3
"""Summarise the disp2 resident's FBS/FBS2 console lines (printed every --fb-stats-period seconds
from the EL2 tick, no guest pause) into one row per interval, plus the RUN-SPEC pass/fail checks.

usage: fbs_parse.py primary-console.txt [--from-hex CNTPCT] [--to-hex CNTPCT]
Counter is 1 GHz on T6050; FBS values are cumulative, FBS2 maxima are per interval.
"""
import re
import sys

KV = re.compile(r"(\w+)=([^\s]+)")


def parse(path):
    rows, cur = [], None
    for line in open(path, errors="replace"):
        i = line.find("FBS")
        if i < 0:
            continue
        body = line[i:].strip()
        kind = body.split()[0]
        if kind not in ("FBS", "FBS2"):
            continue
        kv = dict(KV.findall(body))
        t = int(kv["t"], 16)
        if kind == "FBS":
            cur = {**kv, "t": t}
        elif cur is not None and cur["t"] == t:
            cur.update({"2:" + k: v for k, v in kv.items()})
            rows.append(cur)
            cur = None
    return rows


def nums(s):
    return [int(x) for x in s.split("/")]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    lo = int(args[args.index("--from-hex") + 1], 16) if "--from-hex" in args else 0
    hi = int(args[args.index("--to-hex") + 1], 16) if "--to-hex" in args else 1 << 64
    rows = [r for r in parse(args[0]) if lo <= r["t"] <= hi]
    if len(rows) < 2:
        print("need at least two FBS/FBS2 line pairs")
        return 1
    print("  dt_s  busy%  flt/s  spur%  same/sub/alias/rep  pg/pass  wr%   rd%   lat_net/gross_ms  gap_ms(faults)  tick_ms  hist_delta")
    worst = {"lat": 0, "spur": 0.0, "busy": 0.0, "gap": 0, "tick": 0}
    for a, b in zip(rows, rows[1:]):
        dt = (b["t"] - a["t"]) / 1e9
        busy = (int(b["busy_us"]) - int(a["busy_us"])) / 1e6 / dt * 100
        fault_cpu = (int(b["fault_us"]) - int(a["fault_us"])) / 1e6 / dt * 100
        f = int(b["faults"]) - int(a["faults"])
        sa, sb = nums(a["spur"]), nums(b["spur"])
        sp = [y - x for x, y in zip(sa, sb)]
        spur = 100.0 * sp[0] / (f + sp[0]) if f + sp[0] else 0.0
        passes = int(b["passes"]) - int(a["passes"])
        pages = int(b["pages"]) - int(a["pages"])
        cw = [y - x for x, y in zip(nums(a["chunks"]), nums(b["chunks"]))]
        rt = [y - x for x, y in zip(nums(a["rt"]), nums(b["rt"]))]
        lat = nums(b["2:iv_lat_us"])
        gap = b["2:iv_gap_us"].split("@")[0]
        ha = [int(x) for x in a["2:hist"].split(",")]
        hb = [int(x) for x in b["2:hist"].split(",")]
        hd = [y - x for x, y in zip(ha, hb)]
        print(f"{dt:6.1f} {busy:5.1f}+{fault_cpu:3.1f} {f / dt:6.0f} {spur:5.1f}  {sp[1]}/{sp[2]}/{sp[3]}/{sp[4]:<8}"
              f" {pages / passes if passes else 0:7.1f} {100.0 * cw[0] / cw[1] if cw[1] else 0:5.1f}"
              f" {100.0 * rt[1] / rt[2] if rt[2] else 0:5.1f}   {lat[0] / 1e3:7.1f}/{lat[1] / 1e3:7.1f}"
              f"   {int(gap) / 1e3:8.1f}({b['2:gapf']})  {int(b['2:iv_tick_us']) / 1e3:6.2f}  {hd}")
        worst["lat"] = max(worst["lat"], lat[0])
        worst["spur"] = max(worst["spur"], spur)
        worst["busy"] = max(worst["busy"], busy + fault_cpu)
        worst["gap"] = max(worst["gap"], int(gap))
        worst["tick"] = max(worst["tick"], int(b["2:iv_tick_us"]))
    print(f"worst interval: net pass latency {worst['lat'] / 1e3:.1f} ms, spurious {worst['spur']:.2f} %, "
          f"EL2 busy {worst['busy']:.1f} %, longest tick gap {worst['gap'] / 1e3:.1f} ms, longest tick {worst['tick'] / 1e3:.2f} ms")
    print("(rd% = share of a sampled page's convert time spent on a pure NC read of the same page; wr% = 64-byte "
          "chunks actually written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
