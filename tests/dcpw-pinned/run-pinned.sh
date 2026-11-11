#!/bin/bash
# nightly-2: the dcpw unit tests, pinned. tests/dcpw-pinned/ = display/dcp-20261111/{tests,kmdod/test,kmdod/edid.h} as of
# the merged dcpw-live-1 280f964 (copied when the author's service/ and abi/ were byte-identical to this tree's), run
# against THIS tree's source/src/dcpw (not the author's moving directory). Usage: OUT=dir run-pinned.sh
set -eu
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." >/dev/null && pwd)
T=$(mktemp -d "${OUT:-/tmp}/dcpw-pinned.XXXX")
mkdir -p "$T/service" "$T/abi" "$T/tests" "$T/kmdod"
cp "$R"/source/src/dcpw/service/*.[ch] "$T/service/"
cp "$R"/source/src/dcpw/abi/dcpw_abi.h "$T/abi/"
cp "$R"/tests/dcpw-pinned/*.c "$R"/tests/dcpw-pinned/*.h "$R"/tests/dcpw-pinned/run-tests.sh "$R"/tests/dcpw-pinned/mutants.sh "$T/tests/"
cp -R "$R"/tests/dcpw-pinned/kmdod/. "$T/kmdod/"
# identity: this tree's dcpw sources must equal the merged dcpw-live-1 commit (git blobs), so the pin means something
for f in "$R"/source/src/dcpw/service/*.[ch] "$R"/source/src/dcpw/abi/dcpw_abi.h; do
  rel=${f#$R/}
  [ "$(git -C "$R" rev-parse "280f9640004452d51505a9cc8bfb076a4c578b76:$rel" 2>/dev/null)" = "$(git -C "$R" hash-object "$f")" ] \
    || { echo "dcpw-pinned: $rel differs from dcpw-live-1 280f964"; exit 1; }
done
cd "$T"
if [ "${1:-}" = mutants ]; then
  OUT="$T/mut" bash tests/mutants.sh | tee "$T/mutants.txt"
  last=$(tail -1 "$T/mutants.txt"); k=$(echo "$last" | sed -n 's|^mutants: \([0-9]*\)/\([0-9]*\) killed$|\1 \2|p')
  set -- $k; [ -n "${1:-}" ] && [ "$1" = "$2" ] && [ "$1" -gt 0 ] || { echo "dcpw mutants: not all killed ($last)"; exit 1; }
else
  OUT="$T/out" bash tests/run-tests.sh
fi
