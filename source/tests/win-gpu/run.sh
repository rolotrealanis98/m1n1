#!/bin/sh
# gpu-pt: host test of the GPU PS guard logic + static checks of hv_win_gpu.c wiring.
set -e
unset CDPATH; D=$(cd "$(dirname "$0")" >/dev/null && pwd); S=$D/../..; OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"
cc -O1 -Wall -Werror -o "$OUT/t" "$D/test.c"
"$OUT/t"
G=$S/src/hv/win/hv_win_gpu.c
grep -q 'u32 put = gpu_ps_filter(want);' "$G" || { echo "win-gpu FAIL: write path does not filter"; exit 1; }
grep -q 'write32(addr, put);' "$G" || { echo "win-gpu FAIL: 32-bit write not the filtered value"; exit 1; }
grep -q 'hv_win_gpu_start()' "$S/src/hv/hv.c" || { echo "win-gpu FAIL: hv_start does not arm the guard"; exit 1; }
grep -q 'HV_WIN_F_GPU_PS_GUARD))' "$S/src/hv/win/hv_win.c" || { echo "win-gpu FAIL: CONFIGURE does not accept the flag"; exit 1; }
grep -q "hv_win_t6050_route_allowed(aic_line, gsiv)" "$S/src/hv/win/hv_win.c" || { echo "win-gpu FAIL: MAP_IRQ does not use the T6050 route table"; exit 1; }
# gpu-pt2: sgx forced Device-nGnRnE at stage 2 (MemAttr 0000), before the PS hooks replace their page
grep -q 'return hv_map(from, to | (PTE_ATTRIBUTES & ~PTE_MEMATTR_UNCHANGED) | PTE_VALID, size, 1);' "$S/src/hv/hv_vm.c" \
  || { echo "win-gpu FAIL: hv_map_hw_dev is not MemAttr 0000"; exit 1; }
a=$(grep -n 'hv_map_hw_dev(sgx_base' "$G" | cut -d: -f1); b=$(grep -n 'hv_map_hook(gpu_ps_addr' "$G" | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] || { echo "win-gpu FAIL: Device-nGnRnE map must precede the PS hooks"; exit 1; }
echo "win-gpu static PASS"
