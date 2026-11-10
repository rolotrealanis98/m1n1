#!/bin/sh
# Two clean builds; compare artifact hashes for bit-identity. Uses build.sh (pinned container +
# cargo shim reusing the pinned librust.a). Run offline with the image present.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/source"
arts="m1n1.bin m1n1-raw.elf m1n1.elf m1n1.macho build_tag.h build_cfg.h"
doburn() {
  ( cd "$SRC" && make clean >/dev/null 2>&1 || true )
  sh "$HERE/build.sh" >"$HERE/$1.log" 2>&1
  ( cd "$SRC/build" && shasum -a 256 $arts ) > "$HERE/$1.txt"
}
doburn build1
doburn build2
awk '{print $1}' "$HERE/build1.txt" > "$HERE/.h1"
awk '{print $1}' "$HERE/build2.txt" > "$HERE/.h2"
if diff -q "$HERE/.h1" "$HERE/.h2" >/dev/null; then echo "BIT-IDENTICAL across two clean builds"; else echo "MISMATCH"; diff "$HERE/build1.txt" "$HERE/build2.txt" || true; fi
rm -f "$HERE/.h1" "$HERE/.h2"
