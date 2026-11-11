#!/bin/bash
# [disp3 copy of the disp2 script (itself a copy of runs/20261110-052123-disp2x/disp2x-run.sh: resident tree -> r3.2-disp2, + --fb-stats-period 2; FB_EXTRA appends A/B flags); resident tree -> r3.2-disp3]
# Display test-card hardware run (r3.2-disp3): WinPE renders the m5w test card on the internal panel,
# with the FB converter (--fb-shadow --fb-dirty --fb-bench). No USB passthrough, no s1-nvme, no native
# backend, no write. Waits for the m1n1 proxy (node 1), then runs the disp tree's run_windows.py with a
# prestart shell (-S) so the coordinator starts the KD bridge (node 3) and releases the guest.
#   Env: M1N1DEVICE (optional; auto-detected), RUNDIR (required), FD (the control Mu FD), MEDIA (display .ntasi)
set -u
umask 077
RES=/Users/rolo/Work/agents/win-el2/storage/s1-resident-r3.2-disp3-20261110
M=$RES/source
PY=${PY:-$HOME/Work/m1n1/.venv/bin/python}
FD=${FD:-/Users/rolo/Work/agents/win-el2/storage/s1-mu-windows-bindfix-20261109/artifacts/J716MACBOOKPRO16M5_EFI.fd}
FD_SHA=${FD_SHA:-912a9527de2b47c659f150e2f81ca1c006c22ac29a279ce521456003281f7e88}
MEDIA=${MEDIA:-/Users/rolo/Work/agents/win-el2/display/improve-20261109/winpe/build/display-testcard.ntasi}
RUNDIR=${RUNDIR:?set RUNDIR}
WAIT=${WAIT:-600}

m1n1_dev() {   # /dev/cu.usbmodem<serial>N of the first 1209:316d device
  local ser
  ser=$(ioreg -p IOUSB -l -w0 2>/dev/null | python3 -I -c '
import sys, re
for b in sys.stdin.read().split("+-o "):
    if re.search(r"\"idVendor\" = 4617\b", b) and re.search(r"\"idProduct\" = 12653\b", b):
        m = re.search(r"\"USB Serial Number\" = \"([^\"]+)\"", b)
        if m: print(m.group(1)); break')
  [ -n "$ser" ] && [ -e "/dev/cu.usbmodem${ser}$1" ] && echo "/dev/cu.usbmodem${ser}$1"
}

[ -f "$FD" ] || { echo "STOP: missing FD $FD"; exit 2; }
[ -f "$MEDIA" ] || { echo "STOP: missing media $MEDIA"; exit 2; }
echo "waiting for the m1n1 USB proxy (reboot the Mac now; ~140 s after stage-1 hand-over)..."
t0=$(date +%s)
while :; do
  M1N1DEVICE=$(m1n1_dev 1)
  [ -n "$M1N1DEVICE" ] && break
  [ $(( $(date +%s) - t0 )) -ge $WAIT ] && { echo "timeout: no 1209:316d device"; exit 1; }
  sleep 0.5
done
export M1N1DEVICE
echo "proxy $M1N1DEVICE (vuart $(m1n1_dev 3)) after $(( $(date +%s) - t0 )) s; node 3 left free for KD"
cd "$M/proxyclient" || exit 1
PATH=/usr/bin:/bin:/usr/sbin:/sbin TOOLCHAIN=/opt/homebrew/opt/llvm/bin/ LLDDIR=/opt/homebrew/opt/lld/bin/ \
  PYTHONPATH=. M1N1DEVICE=$M1N1DEVICE HV27_WDT=keep \
  "$PY" tools/run_windows.py --mu-profile m1a --route gic -C 1 -l disp.log --label disp \
    --ramdisk "$MEDIA" --sha256 "$FD_SHA" --pmu-rewrite el2 \
    --fb-shadow --fb-dirty --fb-scale2 --fb-bench --fb-stats-period 2 ${FB_EXTRA:-} -S --run-dir "$RUNDIR" "$FD"
