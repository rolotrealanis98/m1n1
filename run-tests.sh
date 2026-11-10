#!/bin/sh
# smp2g: every host suite, as recorded in test-results.txt. Offline; no hardware.
set -u
cd "$(dirname "$0")/source"
for d in win-wfi win-vgic3 win-psci win-pmu-rewrite; do
  echo "== tests/$d"
  make -s -C tests/$d clean >/dev/null 2>&1
  make -s -C tests/$d check 2>&1 | tail -${TAIL:-3}
done
echo "== tests/win-launcher-smp2/test_args.py"
python3 -I tests/win-launcher-smp2/test_args.py 2>&1 | tail -1
echo "== tests/win-launcher-smp2/test_hv_sysreg.py (m1n1 venv)"
/Users/rolo/Work/m1n1/.venv/bin/python tests/win-launcher-smp2/test_hv_sysreg.py 2>&1 | tail -1
