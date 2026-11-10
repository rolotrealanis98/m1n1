#!/bin/bash
# Confirmation-review tests on the unmodified r2.1 model C under ASan+UBSan.
#   run.sh [prp-fuzz-iterations-per-seed]   (default 20000; 8 seeds)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
M=$here/../source/src/hv/win/s1
out=${OUT:-$(mktemp -d -t m5w-r21-confirm)}
clang -std=c11 -DVNVME_HOST_TEST -Wall -Wextra -Wno-unused-function -O0 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$M" "$M/hv_pci.c" "$M/hv_nvme.c" "$M/hv_nvme_queue.c" "$M/ram_disk.c" \
  "$here/test_confirm.c" -o "$out/confirm"
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$out/confirm" "${1:-20000}"
