#!/bin/sh
# Reproducible build of the r3.2 + display round-3 (disp3) resident in the pinned container
# (m5pro-windows/mu-build:f43, clang 21.1.8). Same command as the disp2 parent; only the tag differs.
# The display patch changes no Rust. The pinned librust.a (sha256 4ed52ac9...) is NOT copied into this
# tree: it is referenced from the read-only parent by hash, staged into source/.buildshim for the
# duration of the build (source/.buildshim/cargo materialises it where the Makefile expects it), and
# removed again on exit.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/source"
TAG="${TAG:-v2.0.0-169-g868807f9-dirty-s1r3.2-disp3}"
PIN="$HERE/../s1-resident-r3.2-disp2-20261110/source/.buildshim/librust.a"
PIN_SHA=4ed52ac9ef8456ba088671c64531479d71cc6705d82ec231cc406a7c96efc304
test "$(shasum -a 256 "$PIN" | cut -d' ' -f1)" = "$PIN_SHA" || { echo "pinned librust.a hash mismatch: $PIN"; exit 1; }
cp "$PIN" "$SRC/.buildshim/librust.a"
trap 'rm -f "$SRC/.buildshim/librust.a"' EXIT
container run --rm -v "$SRC:$SRC" -w "$SRC" m5pro-windows/mu-build:f43 \
  sh -c "PATH=$SRC/.buildshim:\$PATH make -j8 CHAINLOADING=0 TOOLCHAIN=/usr/sbin/ M1N1_VERSION_TAG=$TAG"
