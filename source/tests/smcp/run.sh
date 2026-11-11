#!/bin/sh
# smcp core + power sequencer host tests (ASan/UBSan), compiled against this tree's src/smcp.
set -eu
H=$(CDPATH= cd -- "$(dirname "$0")" && pwd); S=$H/../../src/smcp
O=${FB_OUT:-$H/out}; mkdir -p "$O"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$S/core/smcp_core.c" "$S/core/smcp_power.c" "$H/mock_smc.c" "$H/test_smcp.c" -o "$O/test_smcp"
"$O/test_smcp"
