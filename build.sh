#!/bin/sh
# Reproducible build of the r3.2-disp + smp2g (smp2f + review W1/W2 fixes and diagnostics) resident in the pinned container
# (m5pro-windows/mu-build:f43, clang 21.1.8). smp2h (like smp2g) changes no Rust: the pinned librust.a (sha256
# 4ed52ac9...) is taken from the parent build dir by hash (not copied into this dir, to save space),
# placed next to source/.buildshim/cargo for the build, and removed again. Everything else is the
# parent's (r3.2 BUILD-MANIFEST) command; only the tag differs.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/source"
TAG="v2.0.0-169-g868807f9-dirty-s1r3.2-smp2h"
RUST="$HERE/../s1-resident-r3.2-disp-20261109/source/.buildshim/librust.a"
RUST_SHA=4ed52ac9ef8456ba088671c64531479d71cc6705d82ec231cc406a7c96efc304
[ "$(shasum -a 256 "$RUST" | cut -d' ' -f1)" = "$RUST_SHA" ] || { echo "librust.a hash mismatch"; exit 1; }
cp "$RUST" "$SRC/.buildshim/librust.a"
trap 'rm -f "$SRC/.buildshim/librust.a"' EXIT
container run --rm -v "$SRC:$SRC" -w "$SRC" m5pro-windows/mu-build:f43 \
  sh -c "PATH=$SRC/.buildshim:\$PATH make -j8 CHAINLOADING=0 TOOLCHAIN=/usr/sbin/ M1N1_VERSION_TAG=$TAG"
