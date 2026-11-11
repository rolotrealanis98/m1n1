#!/bin/bash
# Host tests for display/dcp-20261111: ASan+UBSan build of the service, codec, session and the
# fake DCP; then a freestanding build of the resident-side objects with an undefined-symbol check.
set -euo pipefail
cd "$(dirname "$0")/.." >/dev/null
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/dcpw-test.XXXXXX")}; mkdir -p "$OUT"
CC=${CC:-clang}
SRC="service/dcpw_core.c service/dcpw_static.c service/iomfb_codec.c service/iomfb_session.c service/dart4.c service/iomfb_live.c"
W="-std=c11 -Wall -Wextra -Werror -Wshadow"

echo "-- sanitizer build"
$CC $W -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    tests/test_dcpw.c tests/fake_dcp.c $SRC -o "$OUT/test_dcpw"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test_dcpw"
$CC $W -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_dart4.c service/dart4.c -o "$OUT/test_dart4"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test_dart4" | tail -1
$CC $W -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_live.c tests/fake_live.c tests/fake_dcp.c $SRC -o "$OUT/test_live"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test_live" | grep -v "fake: \|  live: " | tail -2
$CC $W -Wno-unused-function -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Iabi -Ikmdod kmdod/test/test_edid.c service/dcpw_static.c service/dcpw_core.c -o "$OUT/test_edid"
"$OUT/test_edid"

echo "-- freestanding build (aarch64, as the resident compiles C)"
for f in $SRC; do
  $CC $W -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib --target=aarch64-none-elf \
      -mgeneral-regs-only -c "$f" -o "$OUT/$(basename "$f" .c).o"
done
ld.lld -r "$OUT"/dcpw_core.o "$OUT"/dcpw_static.o "$OUT"/iomfb_codec.o "$OUT"/iomfb_session.o "$OUT"/dart4.o "$OUT"/iomfb_live.o -o "$OUT/dcpw_all.o"
UND=$(llvm-nm -u "$OUT/dcpw_all.o" | grep -v ':$' | awk '{print $2}' | sort -u | grep -v -E '^$' || true)
EXTRA=$(echo "$UND" | grep -v -x -E 'memcpy|memset' || true)
echo "undefined symbols: ${UND//$'\n'/ }"
if [ -n "$EXTRA" ]; then echo "FAIL: unexpected undefined symbols: $EXTRA"; exit 1; fi
echo "freestanding: OK (memcpy/memset come from the resident's string.c)"
echo "ALL PASS"
