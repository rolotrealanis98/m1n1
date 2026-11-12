#!/bin/sh
# dcp-pt: mutation check of hv_win_dcp_logic.h. Each mutant changes one line of the logic; test.c must fail on it.
set -e
unset CDPATH; D=$(cd "$(dirname "$0")" >/dev/null && pwd); S=$D/../..; OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"
H=$S/src/hv/win/hv_win_dcp_logic.h
n=0; killed=0
mut() { # name sed-expression
  n=$((n+1)); M=$OUT/m$n; rm -rf "$M"; mkdir -p "$M/tests/win-dcp" "$M/src/hv/win"
  sed "$2" "$H" > "$M/src/hv/win/hv_win_dcp_logic.h"
  c=$(diff "$H" "$M/src/hv/win/hv_win_dcp_logic.h" | grep -c '^>' || true)
  [ "$c" -ge 1 ] || { echo "mutant $1 did not apply"; exit 1; }
  cp "$D/test.c" "$M/tests/win-dcp/test.c"
  cc -O1 -w -o "$M/t" "$M/tests/win-dcp/test.c"
  if "$M/t" >/dev/null 2>&1; then echo "SURVIVED: $1"; else killed=$((killed+1)); fi
}
mut flush-all-allowed      's/ || ((v >> DCP_TLB_OP_SHIFT) \& DCP_TLB_OP_MASK) != DCP_TLB_OP_FLUSH_SID//'
mut any-sid-flush          's/ ||$/ ||/; s/            !dcp_sid_allowed(dart, v \& DCP_TLB_STREAM_MASK))/            0)/'
mut busy-bit-allowed       's/#define DCP_TLB_ALLOWED     (DCP_TLB_STREAM_MASK/#define DCP_TLB_ALLOWED     (0x80000000u | DCP_TLB_STREAM_MASK/'
mut width-not-checked      's/if (width != 2 || (off \& 3) || (dart/if ((off \& 3) || (dart/'
mut root-unaligned         's/if (width != 3 || (off \& 7) || off >= DCP_ROOT_PAGE)/if (width != 3 || off >= DCP_ROOT_PAGE)/'
mut ttbr-open              's/    return w; \/\* TTBR, TCR/    w.act = DCP_PASS; w.val = v; return w; \/* TTBR, TCR/'
mut streams-unmasked       's/        v \&= dcp_sid_mask(dart);/        v \&= 0xffffffffu;/'
mut errsid-any-sid         's/        if (dcp_sid_allowed(dart, sid)) {/        if (sid < 32) {/'
mut disp-allows-23         's/return sid == DCP_SID_DISP || sid == DCP_SID_PIODMA;/return sid == DCP_SID_DISP || sid == DCP_SID_PIODMA || sid == 23;/'
mut dcp-allows-24          's/return sid == DCP_SID_DCP;/return sid == DCP_SID_DCP || sid == 24;/'
mut root-window-low        's/#define DCP_ROOT_WIN_FIRST   16u/#define DCP_ROOT_WIN_FIRST   15u/'
mut root-window-wide       's/#define DCP_ROOT_WIN_COUNT   16u/#define DCP_ROOT_WIN_COUNT   17u/'
mut root-nonzero-ok        's/    if (!dcp_root_zero_at_handoff(zero_bitmap, idx))/    if (0)/'
mut root-width-any         's/if (width != 3 || (off \& 7) || off >= DCP_ROOT_PAGE)/if ((off \& 7) || off >= DCP_ROOT_PAGE)/'
mut root-shape-unchecked   's/    return dcp_root_value_ok(val);/    return 1;/'
mut owner-any-value        's/val > 1)/val > 3)/'
mut owner-width            's/^    if (width != 2)$/    if (0)/'
mut owner-any-offset       's/    if (off == DCPO_OWNER) {/    if (off == DCPO_OWNER || off == 0x24) {/'
mut asc-clear-run-ok       's/        return (val \& DCP_ASC_RUN) != 0;/        return 1;/'
mut asc-64bit-unchecked    's/        return ((val >> 32) \& DCP_ASC_RUN) != 0;/        return 1;/'
mut asc-narrow-ok          's/    return 0; \/\* byte, halfword/    return 1; \/* byte, halfword/'
mut asc-page-too-big       's/#define DCP_ASC_PAGE         0x4000ul/#define DCP_ASC_PAGE         0x0ul/'
mut scan-fmt-any           's/ || d->fmt > 1)/ || d->fmt > 9)/'
mut scan-stride-check      's/(unsigned long long)d->stride < 4ull \* d->w || //'
mut scan-size-cap          's/size <= DCP_SCAN_BYTES_MAX \&\& //'
mut scan-pa-zero           's/ || !d->pa || (d->pa \& 3))/ || (d->pa \& 3))/'
mut scan-commit-clear      's/return val ? DCPO_SCAN_COMMIT_OP : DCPO_SCAN_CLEAR_OP;/return DCPO_SCAN_COMMIT_OP;/'
mut scan-stage-range       's/off <= DCPO_SCAN_FORMAT \&\& !(off \& 3)/off <= DCPO_SCAN_COMMIT \&\& !(off \& 3)/'
mut state-bits             's/((s->fb_paused ? 1u : 0) << 1)/((s->fb_paused ? 1u : 0) << 0)/'
echo "win-dcp mutants: $killed/$n killed"
[ "$killed" = "$n" ]
