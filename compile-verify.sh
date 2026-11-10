#!/bin/sh
# Offline compile-verification of the smp2h patch's changed C translation units for aarch64
# (homebrew LLVM). This proves the patched sources compile clean with m1n1's real flags/headers;
# it is NOT the shippable binary (that must be linked in the pinned build container, see build.sh).
set -eu
cd "$(dirname "$0")/source"
CC=/opt/homebrew/opt/llvm/bin/clang
OUT=../build-verify
mkdir -p "$OUT"
INC="-isystem $($CC -print-file-name=include) -isystem sysinc -Isrc -Isrc/hv"
set -- --target=aarch64-none-elf -O2 -Wall -Wundef -Werror=strict-prototypes -fno-common -fno-PIE \
  -Werror=implicit-function-declaration -Werror=implicit-int -Wsign-compare -Wunused-parameter \
  -Wno-multichar -ffreestanding -fpic -nostdinc $INC -fno-stack-protector -mstrict-align \
  -march=armv8.2-a -mgeneral-regs-only -Wstack-usage=2048
log="$OUT/compile.log"; : > "$log"
for f in src/mcc.c src/exception.c src/memory.c; do
  echo "== $f ==" >> "$log"
  $CC "$@" -c "$f" -o "$OUT/$(basename "$f").o" >> "$log" 2>&1 && echo "  ok $(basename "$f").o" | tee -a "$log"
done
echo "warnings/errors:"; grep -iE "warning|error" "$log" || echo "  none"
ls -la "$OUT"/*.o
