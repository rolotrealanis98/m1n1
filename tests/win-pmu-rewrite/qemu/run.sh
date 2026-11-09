#!/bin/bash
# SPDX-License-Identifier: MIT
# The stage-2 PMU rewrite (hv_vm.c's execute tracking, extracted verbatim, + src/hv/win/hv_win_pmu.c)
# under QEMU's EL2 with an EL1 guest; needs qemu-system-aarch64 and Homebrew LLVM/LLD.
#   run.sh                     -> out/xt.log; exit 0 when every XT-CHECK passes
#   XT_DEFS=-DNO_ARM run.sh    negative control: the rewrite is not armed, the first PMU access fails it
set -euo pipefail
cd "$(dirname "$0")"
TREE=${1:-../../..}
LLVM=/opt/homebrew/opt/llvm/bin
mkdir -p build out
./extract-xt.sh "$TREE/src/hv/hv_vm.c" > build/xt.c
CFLAGS="--target=aarch64-none-elf -march=armv8.4-a -mgeneral-regs-only -ffreestanding -fno-builtin -nostdlib
  -O2 -mstrict-align -fno-pic -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
  -DPMU_REWRITE_HOST_TEST ${XT_DEFS:-} -iquote shim -iquote $TREE/src/hv/win -iquote build"
for c in el2.c "$TREE/src/hv/win/hv_win_pmu.c" "$TREE/src/hv/win/hv_win_pmu_rewrite.c"; do
    $LLVM/clang $CFLAGS -c -o build/$(basename ${c%.c}).o "$c"
done
$LLVM/clang --target=aarch64-none-elf -march=armv8.4-a -c -o build/start.o start.S
$LLVM/clang --target=aarch64-none-elf -march=armv8.4-a -c -o build/guest.o guest.S
/opt/homebrew/opt/lld/bin/ld.lld -T xt.ld -o build/xt.elf build/start.o build/el2.o build/hv_win_pmu.o \
    build/hv_win_pmu_rewrite.o build/guest.o
set +e
timeout 60 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,secure=off -cpu max,pmu=off -m 512 \
    -nographic -semihosting-config enable=on,target=native -kernel build/xt.elf 2>&1 | tr -d '\r' | tee out/xt.log
status=${PIPESTATUS[0]}
echo "qemu exit status $status"
exit $status
