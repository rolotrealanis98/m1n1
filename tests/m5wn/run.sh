#!/bin/bash
# SPDX-License-Identifier: MIT
# M5WN suite: (1) the in-tree service/ABI are byte-identical to the host-tested delivery
# (wireless/n1-20261111), (2) that delivery's ASan/UBSan host tests + freestanding check,
# (3) the launcher contract builder.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=/Users/rolo/Work/agents/win-el2/wireless/n1-20261111
for f in abi/m5wn_abi.h service/m5wn_service.c service/m5wn_service.h service/m5wn_fake.c service/m5wn_fake.h \
         n1/n1_hal.h n1/n1_probe.c n1/n1_probe.h n1/n1_core.c n1/n1_core.h; do
  cmp -s "source/src/m5wn/$f" "$D/$f" || { echo "m5wn: source/src/m5wn/$f differs from $D/$f"; exit 1; }
done
"$D/tools/test-host.sh" > "${FB_OUT:-/tmp}/m5wn-host.log" 2>&1 || { tail -5 "${FB_OUT:-/tmp}/m5wn-host.log"; exit 1; }
grep "m5wn host tests" "${FB_OUT:-/tmp}/m5wn-host.log"
"$D/tools/test-n1.sh" > "${FB_OUT:-/tmp}/n1-host.log" 2>&1 || { tail -5 "${FB_OUT:-/tmp}/n1-host.log"; exit 1; }
grep "n1 host tests" "${FB_OUT:-/tmp}/n1-host.log"
python3 -I tests/m5wn/test_m5wn_contract.py
python3 -I tests/m5wn/test_launcher_args.py | tail -1
echo "m5wn: all PASS"
