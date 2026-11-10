#!/usr/bin/env python3
"""Mock test of s1dg_live.read_s1dg_live: bounded single read, O_EXCL outputs, cont() always."""
import json, sys, tempfile, types
from pathlib import Path
R = Path(__file__).resolve().parents[2]
g = {}; exec(open(R / 's1dg_live.py').read(), g)
dump = Path(sys.argv[1]).read_bytes()
calls = []
class Iface:
    def readmem(self, a, n): calls.append(('readmem', a, n)); return dump[:n]
p = types.SimpleNamespace(hv_win_storage_stat=lambda i: calls.append(('stat', i)) or 0x314e355300000007)
u = types.SimpleNamespace(base=0x10000000000, iface=Iface())
conts = []
d = Path(tempfile.mkdtemp())
r = g['read_s1dg_live'](p, u, d, lambda: conts.append(1))
assert r['decoded'] and conts == [1] and calls == [('stat', 0), ('readmem', 0x10000000000 + 0x447088, 32232)], calls
assert (d / 's1dg-live.bin').read_bytes() == dump and json.loads((d / 's1dg-live.json').read_text())['decoded']
try: g['read_s1dg_live'](p, u, d, lambda: conts.append(2)); raise SystemExit('O_EXCL not enforced')
except FileExistsError: pass
assert conts == [1, 2]
p2 = types.SimpleNamespace(hv_win_storage_stat=lambda i: 0)
try: g['read_s1dg_live'](p2, u, d, lambda: conts.append(3), tag='x'); raise SystemExit('no identity check')
except RuntimeError: pass
assert conts == [1, 2, 3] and len([c for c in calls if c[0] == 'readmem']) == 1
bad = bytearray(dump); bad[0] ^= 1
u.iface.readmem = lambda a, n: bytes(bad)
try: g['read_s1dg_live'](p, u, d, lambda: conts.append(4), tag='y'); raise SystemExit('no header check')
except RuntimeError: pass
assert conts == [1, 2, 3, 4]
print('s1dg_live mock tests PASS (8 assertions)')
