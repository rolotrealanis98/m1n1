#!/usr/bin/env python3
"""r3.1 R3 static gate: no host P_NVME_* path in proxy.c and no nvme_flush in the built ELF."""
import re, subprocess, sys
from pathlib import Path
R = Path(__file__).resolve().parents[1]
proxy = (R / 'source/src/proxy.c').read_text()
bad = [f for f in ('nvme_init(', 'nvme_read(', 'nvme_flush(', 'nvme_shutdown(', 'case P_NVME_') if f in proxy]
syms = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-nm', str(R / 'artifacts/m1n1-raw.elf')],
                      capture_output=True, text=True, check=True).stdout
flush = bool(re.search(r' nvme_flush$', syms, re.M))
quiesce = bool(re.search(r' hv_win_storage_native_quiesce$', syms, re.M))
ok = not bad and not flush and quiesce
print('static r3.1:', 'PASS' if ok else 'FAIL', {'proxy_nvme_refs': bad, 'nvme_flush_in_elf': flush, 'quiesce_op_in_elf': quiesce})
sys.exit(0 if ok else 1)
