#!/bin/sh
# Host harness, disp3. Builds the ACTUAL converter of several trees against the same stubs:
#   orig   = r3.2/r4.1 legacy sweep (c5f57839), prev = r3.2-disp (bin 053623c1),
#   parent = disp2 (bin ed252158, read-only parent tree), new = this tree (disp3),
#   and three mutants of new, each with exactly one disp3 fix reverted (sed, verified to apply once):
#     mut_nobarrier  (M1: no dsb ishst before the small-pass ipas2e1is)
#     mut_noalign    (L1: fault-path ipas2e1 operand not aligned to the 16 KiB page)
#     mut_noprint    (L2: print time not added to busy)
# Host-only; no device access.
# Usage: ./run.sh [test|strict|print|work|card|all]
#   "strict" and "print" require new to PASS and parent + the matching mutant to FAIL (the new tests
#   catch each reverted fix); the script exits 1 otherwise.
set -eu
H=$(cd "$(dirname "$0")" && pwd)
D=$(dirname "$H")
B="$H/build"
PREV_TREE=${PREV_TREE:-"$D/../s1-resident-r3.2-disp-20261109/source"}
PARENT_TREE=${PARENT_TREE:-"$D/../s1-resident-r3.2-disp2-20261110/source"}
ORIG_FB=${ORIG_FB:-"$D/../../display/improve-20261109/orig/src/hv/win/hv_win_fb.c"}
V="orig prev parent new mut_nobarrier mut_noalign mut_noprint mut_norefresh mut_nofixcount"
rm -rf "$B"
for v in $V; do mkdir -p "$B/$v/hv/win"; cp -R "$H/stub/hv/." "$B/$v/hv/"; done
cp "$ORIG_FB" "$B/orig/hv/win/hv_win_fb.c"
cp "$PREV_TREE/src/hv/win/hv_win_fb.c" "$PREV_TREE/src/hv/win/hv_win_fb_conv.h" "$B/prev/hv/win/"
cp "$PARENT_TREE/src/hv/win/hv_win_fb.c" "$PARENT_TREE/src/hv/win/hv_win_fb_conv.h" "$B/parent/hv/win/"
for v in new mut_nobarrier mut_noalign mut_noprint mut_norefresh mut_nofixcount; do
  cp "$D/source/src/hv/win/hv_win_fb.c" "$D/source/src/hv/win/hv_win_fb_conv.h" "$B/$v/hv/win/"
done
chk() { test "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$2" || { echo "hash mismatch: $1"; exit 1; }; }
chk "$B/orig/hv/win/hv_win_fb.c" c5f5783928c1922aca747e0771a3ce85f6a9f90b020544ef50cdf554f2d3262a
chk "$B/prev/hv/win/hv_win_fb.c" 8598bd0195ff2f220b4cace30c5296e9237ee20c6d6bf046ae107006354dc1ac
chk "$B/prev/hv/win/hv_win_fb_conv.h" d0afb111a6a8f141515318e312c0e1485e19cb120e991a478ccf5c8ab6850b46
chk "$B/parent/hv/win/hv_win_fb.c" c43fc76d3de70508a156cf563df00394f2ecfa76d28ac92c9f3f233efc68af2b
chk "$B/parent/hv/win/hv_win_fb_conv.h" 911e5bc7d631ffdbc5fae6f456090ea957e626adfe6afbbdf946aa05470203e3
# mutants: each sed must change exactly one line of this tree's converter
mut() { # variant, sed expression
  f="$B/$1/hv/win/hv_win_fb.c"
  sed "$2" "$D/source/src/hv/win/hv_win_fb.c" > "$f"
  n=$(diff "$D/source/src/hv/win/hv_win_fb.c" "$f" | grep -c '^[<>]' || true)
  test "$n" -ge 1 -a "$n" -le 2 || { echo "mutant $1 did not apply cleanly ($n diff lines)"; exit 1; }
}
mut mut_nobarrier '/^            sysop("dsb ishst");$/d'
mut mut_noalign 's/((ipa & ~(FB_PAGE - 1)) >> 12)/(ipa >> 12)/'
mut mut_noprint '/^        win_fb.busy += after - now;$/d'
# nightly-6-perf: the refresh round never starts / missed updates not counted
mut mut_norefresh 's/        if (now - win_fb.last_refresh < win_fb.refresh_ticks)/        if (1)/'
mut mut_nofixcount 's/            win_fb.refresh_fixes += win_fb.chunks_wr - wr0;/            (void)wr0;/'
echo "new converter: $(shasum -a 256 "$B/new/hv/win/hv_win_fb.c" | cut -c1-16)  conv: $(shasum -a 256 "$B/new/hv/win/hv_win_fb_conv.h" | cut -c1-16)"
CC=${CC:-cc}
W="-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-include-next-absolute-path"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
FAST="-O2"
bld() { # name define tree flags
  $CC $W $4 $2 -DFB_HARNESS_COST -I"$B/$3/hv/win" -o "$B/$1" "$H/sim.c" "$B/$3/hv/win/hv_win_fb.c" -lm
}
bld sim_orig_san "-DORIG -Wno-unused" orig "$SAN"; bld sim_orig "-DORIG -Wno-unused" orig "$FAST"
bld sim_prev_san -DPREV prev "$SAN"; bld sim_prev -DPREV prev "$FAST"
bld sim_parent_san -DPARENT parent "$SAN"
bld sim_new_san "" new "$SAN";       bld sim_new "" new "$FAST"
for m in mut_nobarrier mut_noalign mut_noprint mut_norefresh mut_nofixcount; do bld "sim_${m}_san" "" "$m" "$SAN"; done
$CC $W -O2 -mgeneral-regs-only -I"$B/new/hv/win" -o "$B/bench" "$H/bench.c" 2>/dev/null || \
  $CC $W -O2 -I"$B/new/hv/win" -o "$B/bench" "$H/bench.c"
bad=0
expect() { # pass|fail, label, command...
  want=$1; label=$2; shift 2
  if "$@"; then got=pass; else got=fail; fi
  if [ "$got" = "$want" ]; then echo "== $label: $got (expected)"; else echo "== $label: $got (UNEXPECTED, wanted $want)"; bad=1; fi
}
what=${1:-all}
if [ "$what" = test ] || [ "$what" = all ]; then
  "$B/sim_orig_san" test
  "$B/sim_prev_san" test
  "$B/sim_new_san" test
  "$B/sim_new_san" reject
  "$B/sim_new_san" reject-tuning
  "$B/sim_new_san" reject-tuning2
  # nightly-6-perf: bits 47:44 are now the refresh period (DIRTY only); bit 44 without DIRTY must be refused,
  # and the remaining reserved bits 63:56 still are (bit 56 below, bit 63 added)
  "$B/sim_new_san" reject-refresh-sweep
  expect pass "nightly-6 refresh"            "$B/sim_new_san" refresh
  expect fail "mutant no-refresh refresh"    "$B/sim_mut_norefresh_san" refresh
  expect fail "mutant no-fix-count refresh"  "$B/sim_mut_nofixcount_san" refresh
  "$B/sim_new_san" reject-tuning2 0x8000000000000000 # bit 63 reserved
  "$B/sim_new_san" reject-tuning2 0x100000000000000 # bit 56 reserved
fi
if [ "$what" = strict ] || [ "$what" = all ]; then
  expect pass "disp3 strict"                "$B/sim_new_san" strict
  expect fail "disp2 parent strict"         "$B/sim_parent_san" strict
  expect fail "mutant no-barrier strict"    "$B/sim_mut_nobarrier_san" strict
  expect fail "mutant no-align strict"      "$B/sim_mut_noalign_san" strict
  expect pass "mutant no-print strict"      "$B/sim_mut_noprint_san" strict
fi
if [ "$what" = print ] || [ "$what" = all ]; then
  expect pass "disp3 print"                 "$B/sim_new_san" print
  expect fail "disp2 parent print"          "$B/sim_parent_san" print
  expect fail "mutant no-print print"       "$B/sim_mut_noprint_san" print
  expect pass "mutant no-barrier print"     "$B/sim_mut_nobarrier_san" print
fi
if [ "$what" = work ] || [ "$what" = all ]; then
  "$B/bench"
  "$B/sim_prev" work
  "$B/sim_new" work
fi
if [ "$what" = card ] || [ "$what" = all ]; then
  "$B/sim_prev" card
  "$B/sim_new" card
fi
exit $bad
