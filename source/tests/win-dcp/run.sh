#!/bin/sh
# dcp-pt: host tests of the DCP passthrough guards (hv_win_dcp_logic.h), the converter pause (hv_win_fb.c) and static
# checks of the wiring (hv_win_dcp.c, hv.c, hv_win.c, hv_win_psci.c).
set -e
unset CDPATH; D=$(cd "$(dirname "$0")" >/dev/null && pwd); S=$D/../..; L=$S/..; OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"
cc -O1 -Wall -Werror -o "$OUT/t" "$D/test.c"
"$OUT/t"
# converter pause/resume on the disp3 harness model (the real hv_win_fb.c)
B=$OUT/fbp; rm -rf "$B"; mkdir -p "$B/hv/win"; cp -R "$L/harness/stub/hv/." "$B/hv/"
cp "$S/src/hv/win/hv_win_fb.c" "$S/src/hv/win/hv_win_fb_conv.h" "$B/hv/win/"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-include-next-absolute-path -Wno-unused-function \
   -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DNEW -DFB_HARNESS_COST -I"$B/hv/win" \
   -o "$OUT/fbp_t" "$D/fbpause.c" "$B/hv/win/hv_win_fb.c" -lm
"$OUT/fbp_t" | tail -3
# static wiring checks
G=$S/src/hv/win/hv_win_dcp.c
grep -q 'dcp_dart_write(d, off, width, \*val)' "$G" || { echo "win-dcp FAIL: DART write path does not use the logic"; exit 1; }
grep -q 'dcp_root_write_ok(streams\[i\].zero, off, width, \*val)' "$G" || { echo "win-dcp FAIL: root write path does not use the logic"; exit 1; }
grep -q 'write32(addr, w.val);' "$G" || { echo "win-dcp FAIL: DART pass is not the filtered value"; exit 1; }
grep -q 'dcp_asc_write_ok(off, width, \*val)' "$G" || { echo "win-dcp FAIL: ASC path does not use the logic"; exit 1; }
grep -q 'hv_map_hook(asc_base, hv_win_dcp_asc_hook, DCP_ASC_PAGE)' "$G" || { echo "win-dcp FAIL: ASC page not hooked"; exit 1; }
grep -q 'hv_win_dcp_start()' "$S/src/hv/hv.c" || { echo "win-dcp FAIL: hv_start does not arm the guards"; exit 1; }
grep -q 'HV_WIN_F_DCP_PT | HV_WIN_F_GPU_PS_GUARD))' "$S/src/hv/win/hv_win.c" || { echo "win-dcp FAIL: CONFIGURE does not accept the flag"; exit 1; }
grep -q 'hv_win_dcp_report();' "$S/src/hv/win/hv_win_psci.c" || { echo "win-dcp FAIL: guest exit does not resume the converter"; exit 1; }
grep -q 'hv_win_dcp.o' "$S/Makefile" || { echo "win-dcp FAIL: hv_win_dcp.o not in the Makefile"; exit 1; }
grep -q 'hv_win_storage_guest_ram_contains(dcp_staged.pa' "$G" || { echo "win-dcp FAIL: scanout commit does not check guest RAM"; exit 1; }
grep -q 'hv_win_storage_guest_ram_contains(dcp_latched.pa' "$G" || { echo "win-dcp FAIL: scanout lookup does not re-check guest RAM"; exit 1; }
grep -q 'hv_win_dcp_scanout(&sc)' "$S/src/hv/win/hv_win_shot.c" || { echo "win-dcp FAIL: screenshot does not follow the scanout"; exit 1; }
# the DART mappings must be hooked before the root pages and the owner page; the hooks must come after the checks
a=$(grep -n 'if (!(read32(asc_base + DCP_ASC_CPU_CONTROL)' "$G" | cut -d: -f1); b=$(grep -n 'hv_map_hook(dart_base' "$G" | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] || { echo "win-dcp FAIL: ADT/running checks must precede the hooks"; exit 1; }
# the tick pauses and nothing else in the converter changed its behaviour when not paused
grep -q 'win_fb.state != 2 || (win_fb.flags & FB_F_NO_COPY) || win_fb.paused' "$S/src/hv/win/hv_win_fb.c" || { echo "win-dcp FAIL: tick does not honour pause"; exit 1; }
echo "win-dcp static PASS"
