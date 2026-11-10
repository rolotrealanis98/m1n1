#!/bin/bash
# Independent review tests: actual r2.1 resident model C (unmodified) under ASan+UBSan.
#   run.sh [fuzz-ops-per-seed]   (default 200000; 8 seeds)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
M=$here/../source/src/hv/win/s1
out=${OUT:-$(mktemp -d -t m5w-r2-review)}
clang -std=c11 -DVNVME_HOST_TEST -Wall -Wextra -Werror -O0 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$M" "$M/hv_pci.c" "$M/hv_nvme.c" "$M/hv_nvme_queue.c" "$M/ram_disk.c" \
  "$here/test_review.c" -o "$out/review"
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$out/review" "${1:-200000}"
