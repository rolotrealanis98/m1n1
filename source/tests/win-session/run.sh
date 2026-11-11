#!/bin/sh
# nightly-7: host test of src/hv/win/hv_win_session.c (the REAL file, #include lines stripped, after prelude.h).
set -e
H=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
{ cat "$H/prelude.h"; grep -v '^#include' "$H/../../src/hv/win/hv_win_session.c"; cat "$H/body.c"; } > "$OUT/test.c"
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -o "$OUT/test" "$OUT/test.c"
"$OUT/test"
echo
