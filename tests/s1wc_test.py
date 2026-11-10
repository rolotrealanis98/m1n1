#!/usr/bin/env python3
"""r4: S1WC decoder unit test (host only)."""
import importlib.util, struct, sys
from pathlib import Path
R = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('s1dg_decode', R / 'source/proxyclient/tools/s1dg_decode.py')
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
W0, WB = 174213120, 24424454
def blk(mn, mx): return struct.pack('<II10Q', 0x43573153, 1, 3, 10, mn, mx, 3, 2, 1, 1, 0, 0)
r = d.decode_s1wc(blk(W0, W0 + WB - 1), W0, WB); assert r['all_writes_inside_window'] and r['any_write']
r = d.decode_s1wc(blk(W0 - 1, W0 + 5), W0, WB); assert not r['all_writes_inside_window']
r = d.decode_s1wc(blk(W0, W0 + WB), W0, WB); assert not r['all_writes_inside_window']
r = d.decode_s1wc(blk(0xffffffffffffffff, 0), W0, WB); assert not r['any_write'] and r['all_writes_inside_window']
try: d.decode_s1wc(b'\0' * 88); raise SystemExit('accepted bad magic')
except d.DecodeError: pass
print('s1wc decoder: 5 cases PASS')
