#!/bin/bash
# r4.7 clean build (offline). Deletes every object/output except the prebuilt librust.a and
# build_cfg.h (Rust is not rebuilt, as for r3.1-r4.3), builds in the pinned container with this
# tree mounted at its own absolute path, and copies the outputs to $1 (default artifacts/).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$HERE/source; OUT=${1:-$HERE/artifacts}
STAGE=${S1N_STAGE:?set S1N_STAGE to 0, 1, 2 or 3}
case "$STAGE" in 0|1|2|3) ;; *) exit 2 ;; esac
case "$OUT" in "$HERE"/*) ;; *) echo 'Output must be inside this directory' >&2; exit 2 ;; esac
TAG=v2.0.0-169-g868807f9-dirty-s1r4.7-stage$STAGE
find "$SRC/build" -name '*.o' -delete
rm -rf "$SRC/build/.deps" "$SRC/build/build_tag.h" "$SRC"/build/m1n1*
echo "librust.a $(shasum -a 256 "$SRC/build/librust.a" | cut -d' ' -f1)"
container run --rm -v "$SRC:$SRC" -w "$SRC" m5pro-windows/mu-build:f43 \
  make -j8 S1N_STAGE="$STAGE" CHAINLOADING=0 TOOLCHAIN=/usr/sbin/ M1N1_VERSION_TAG=$TAG
mkdir -p "$OUT"
cp "$SRC/build/m1n1.bin" "$OUT/m1n1-win-s1-r4.7-stage$STAGE.bin"
cp "$SRC/build/m1n1-raw.elf" "$SRC/build/m1n1.elf" "$SRC/build/build_tag.h" "$SRC/build/build_cfg.h" "$OUT/"
(cd "$OUT" && shasum -a 256 m1n1-win-s1-r4.7-stage$STAGE.bin m1n1-raw.elf m1n1.elf build_tag.h build_cfg.h)
