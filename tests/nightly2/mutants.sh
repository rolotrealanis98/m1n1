#!/bin/bash
# nightly-2 mutants for the review fixes (F1 iBoot-tables page check, F2 resident refusals, F3 non-blocking SMCP
# send). Each mutant is applied to a scratch copy of the tree; the guarding tests (test_policy.py, tests/u1,
# tests/smcp, tests/kbd-hidm test_mtp_dma) must FAIL on it. Exit 0 only if every mutant is killed.
set -u
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." >/dev/null && pwd)
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/n2mut.XXXX")}; mkdir -p "$OUT"
total=0; killed=0
copy() {
  rm -rf "$OUT/t"; mkdir -p "$OUT/t/source/tests" "$OUT/t/tests" "$OUT/t/source/proxyclient"
  cp -R "$R/source/src" "$OUT/t/source/"; cp "$R/source/config.h" "$OUT/t/source/"
  cp -R "$R/source/tests/smcp" "$R/source/tests/kbd-hidm" "$OUT/t/source/tests/"
  cp -R "$R/source/proxyclient/m1n1" "$R/source/proxyclient/tools" "$OUT/t/source/proxyclient/"
  cp -R "$R/tests/u1" "$R/tests/nightly2" "$OUT/t/tests/"
  rm -rf "$OUT/t/source/tests/smcp/out" "$OUT/t/source/tests/kbd-hidm/out"
}
guards() { # 0 = all guards pass (mutant survived)
  local T=$OUT/t
  python3 -I "$T/tests/nightly2/test_policy.py" "$T" >/dev/null 2>&1 || return 1
  python3 -I "$T/tests/u1/test_u1.py" >/dev/null 2>&1 || return 1
  FB_OUT=$OUT/smcp sh "$T/source/tests/smcp/run.sh" >/dev/null 2>&1 || return 1
  (cd "$T/source/tests/kbd-hidm" && FB_OUT=$OUT/kh sh run.sh) >/dev/null 2>&1 || return 1
  return 0
}
mut() { # file perl-expr name
  total=$((total+1)); copy
  perl -0pi -e "$2" "$OUT/t/$1" || { echo "MUTANT $3: substitution failed"; return; }
  if cmp -s "$R/$1" "$OUT/t/$1"; then echo "MUTANT $3: no change (bad mutant)"; return; fi
  if guards; then echo "MUTANT $3: SURVIVED"; else echo "MUTANT $3: killed"; killed=$((killed+1)); fi
}
copy; guards || { echo "baseline guards fail: not running mutants"; exit 1; }
# F1
mut source/src/mtp_dma.c 's/return mtp_dma_resident\(pa, SZ_16K\) \|\| !mtp_dma_os_ram\(pa, SZ_16K\);/return true;/' f1-page-ok-true
mut source/src/mtp_dma.c 's/if \(!iboot_tables_ok\(root_pa\)\) \{/if (0) {/' f1-no-table-check
mut source/src/mtp_dma.c 's/            if \(!page_ok\(leaf\)\) \{/            if (0) {/' f1-no-leaf-check
mut source/src/mtp_dma.c 's/if \(!mtp_dma_dram\(t, SZ_16K\) \|\| !page_ok\(t\)\) \{/if (!mtp_dma_dram(t, SZ_16K)) {/' f1-no-l2-check
mut source/src/mtp_dma.c 's/    if \(!mtp_dma_resident\(TP_PA\(buf\), size\)\) \{ free\(buf\); return false; \}/    ;/' f1-buffer-any
# F2
mut source/src/hv_win_feature_policy.h 's/return !disk_used;\n\}\n\nstatic inline bool fp_m5wn/return true;\n}\n\nstatic inline bool fp_m5wn/' f2-power-always
mut source/src/hv_win_feature_policy.h 's/static inline bool fp_m5wn_configure_ok\(bool disk_used\)\n\{\n    return !disk_used;/static inline bool fp_m5wn_configure_ok(bool disk_used)\n{\n    return true;/' f2-m5wn-always
mut source/src/hv_win_feature_policy.h 's/return !\(\(flags & FP_SMCP_F_POWER_ACTIONS\) && disk_used\);/return !disk_used;/' f2-cfg-any-flags
mut source/src/hv_win_smcp.c 's/    if \(!fp_smcp_power_ok\(hv_win_disk_path_used\(\)\)\) \{/    if (0) {/' f2-power-call-dropped
mut source/src/hv_win_m5wn.c 's/    if \(!fp_m5wn_configure_ok\(hv_win_disk_path_used\(\)\)\) \{/    if (0) {/' f2-m5wn-call-dropped
mut source/src/hv/win/hv_win.c 's/    if \(hv_win_smcp_actions_armed\(\)\) \{/    if (0) {/' f2-gate-smcp-dropped
mut source/src/hv/win/hv_win.c 's/    if \(hv_win_m5wn_attempted\(\)\) \{/    if (0) {/' f2-gate-m5wn-dropped
mut source/src/hv/win/hv_win.c 's/return u1_storage_touched \|\| \(hv_win_storage_native_stat\(0\) & 6\) \|\| attempted;/return u1_storage_touched || attempted;/' f2-used-ignores-native
mut source/src/hv/win/hv_win.c 's/return u1_storage_touched \|\| \(hv_win_storage_native_stat\(0\) & 6\) \|\| attempted;/return u1_storage_touched || (hv_win_storage_native_stat(0) & 6);/' f2-used-ignores-prep
# F3
mut source/src/hv_win_smcp.c 's/return asc_try_send\(smc_asc, &m\) \? 0 : SMCP_SEND_BUSY;/return asc_send(smc_asc, \&m) ? 0 : -1;/' f3-blocking-send
mut source/src/smcp/core/smcp_core.c 's/ +s->msgid = \(s->msgid - 1\) & 0xf;\n//' f3-id-consumed
mut source/src/smcp/core/smcp_core.c 's/        \} else if \(t - s->busy_since >= SMCP_MAX_BUSY_US\) \{/        } else if (0) {/' f3-busy-forever
mut source/src/smcp/core/smcp_core.c 's/    if \(r == SMCP_SEND_BUSY\) \{\n        \/\* nightly-2/    if (0) {\n        \/* nightly-2/' f3-busy-is-error
echo "nightly2 mutants: $killed/$total killed"
[ "$killed" = "$total" ]
