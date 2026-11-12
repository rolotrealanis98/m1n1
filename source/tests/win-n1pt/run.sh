#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/win-n1pt: host tests of the N1 PCIe passthrough (n1pt), ASan + UBSan.
#  test_logic: hv_win_n1pt_logic.h (ECAM emulation, service page, IRQ latch, config-window filter) against stub ops.
#  test_plat:  byte-identical copies of src/pcie_t6050.c, src/n1pt_plat.c, src/n1pt_dart.c and src/hv/win/hv_win_n1pt.c
#              against the register model (model.c: apcie0, PHY, port, lane, intr2axi, PERST# pad, DART, storage
#              registers, SMC gP13, ADT) and the resident stand-ins in glue/.
#  N1PT_MUTANTS=1: also builds each mutant in mutants/*.sed applied to its source and requires the suite to FAIL.
# Usage: tests/win-n1pt/run.sh   (FB_OUT = output dir)
set -eu
H=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
S=$H/../../src
O=${FB_OUT:-$H/out}
B=$O/build
CC=${CC:-cc}
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-constant-conversion -g -O1 -fno-omit-frame-pointer
 -fsanitize=address,undefined -fno-sanitize-recover=all"

stage() {   # stage DIR: byte-identical copies of the sources next to the host stand-ins
  d=$1
  mkdir -p "$d/hv/win" "$d/m5wn/n1" "$d/glue"
  cp "$H"/shim/*.h "$H/model.h" "$d/"
  cp "$H"/glue/*.h "$d/glue/"
  cp "$H/glue/hv.h" "$d/hv.h"
  for f in pcie_t6050.c pcie_t6050.h pcie_t6050_n1.h n1pt_plat.c n1pt_plat.h n1pt_dart.c n1pt_dart.h; do cp "$S/$f" "$d/$f"; done
  cp "$S/hv/win/hv_win_n1pt_logic.h" "$d/hv/win/"
  cp "$S/hv/win/hv_win_n1pt_logic.h" "$S/hv/win/hv_win_n1pt.h" "$S/hv/win/hv_win_n1pt.c" "$d/glue/"
  cp "$S/m5wn/n1/n1_hal.h" "$d/m5wn/n1/"
}

build() {   # build DIR OUTPREFIX
  d=$1; p=$2
  # shellcheck disable=SC2086
  $CC $CFLAGS -iquote "$d" -iquote "$d/glue" -I"$H" "$H/test_logic.c" -o "$p-logic"
  # shellcheck disable=SC2086
  $CC $CFLAGS -iquote "$d" -iquote "$d/glue" "$d/pcie_t6050.c" "$d/n1pt_plat.c" "$d/n1pt_dart.c" "$d/glue/hv_win_n1pt.c" \
    "$H/model.c" "$H/glue/glue.c" "$H/test_plat.c" -o "$p-plat"
}

rm -rf "$B"; mkdir -p "$B"
stage "$B"
for f in pcie_t6050.c n1pt_plat.c n1pt_dart.c hv/win/hv_win_n1pt.c hv/win/hv_win_n1pt_logic.h; do
  case $f in hv/win/hv_win_n1pt.c) c=$B/glue/hv_win_n1pt.c ;; *) c=$B/$f ;; esac
  cmp -s "$S/$f" "$c" || { echo "FAIL: copy of $f differs"; exit 1; }
done
python3 -I "$H/linux/poll_audit.py" check "$H/linux/linux-polls.tsv" "$S/pcie_t6050.c" > "$O/poll-audit.txt" || { cat "$O/poll-audit.txt"; echo "FAIL: poll audit"; exit 1; }
tail -1 "$O/poll-audit.txt"
build "$B" "$O/n1pt"
"$O/n1pt-logic"
N1PT_LINUX_EXPECTED="$H/linux/linux-cycle.expected" N1PT_LINUX_COLD="$H/linux/linux-cold-port.expected" "$O/n1pt-plat"

if [ "${N1PT_MUTANTS:-0}" = 1 ]; then
  killed=0; total=0
  for m in "$H"/mutants/${N1PT_MUTANT_GLOB:-*}.sed; do
    total=$((total + 1)); name=$(basename "$m" .sed)
    file=$(sed -n 's/^# file: //p' "$m")
    md=$O/mut/$name; rm -rf "$md"; mkdir -p "$md"; stage "$md"
    case $file in hv/win/hv_win_n1pt.c) t=$md/glue/hv_win_n1pt.c ;; hv/win/hv_win_n1pt_logic.h) t="$md/hv/win/hv_win_n1pt_logic.h $md/glue/hv_win_n1pt_logic.h" ;; *) t=$md/$file ;; esac
    for tf in $t; do cp "$tf" "$tf.orig"; sed -f "$m" "$tf.orig" > "$tf"
      cmp -s "$tf" "$tf.orig" && { echo "MUTANT $name: sed changed nothing in $file"; exit 1; }; rm "$tf.orig"; done
    if ! build "$md" "$md/t" 2>"$md/build.err"; then echo "MUTANT $name does not build: $md/build.err"; exit 1; fi
    if "$md/t-logic" >"$md/out.txt" 2>&1 && N1PT_LINUX_EXPECTED="$H/linux/linux-cycle.expected" N1PT_LINUX_COLD="$H/linux/linux-cold-port.expected" "$md/t-plat" >>"$md/out.txt" 2>&1; then
      echo "MUTANT $name SURVIVED"
    else
      killed=$((killed + 1)); echo "mutant $name killed"
    fi
  done
  echo "win-n1pt mutants: $killed/$total killed"
  [ "$killed" = "$total" ] || exit 1
fi
echo "PASS: win-n1pt"
