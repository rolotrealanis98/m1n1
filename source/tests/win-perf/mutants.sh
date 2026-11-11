#!/bin/bash
# perf-exits: each mutant changes one line of hv_win_perf.h; test_perf must FAIL on every one.
set -u
cd "$(dirname "$0")"
H=../../src/hv/win/hv_win_perf.h
CC=/opt/homebrew/opt/llvm/bin/clang; [ -x "$CC" ] || CC=clang
mkdir -p mut
killed=0; total=0
mut() {  # name, python replace old -> new (must match exactly once)
  total=$((total+1))
  python3 -I - "$H" "mut/$1.h" "$2" "$3" <<'PY' || { echo "mutant $1: pattern not found (count != 1)"; exit 1; }
import sys
src, dst, old, new = sys.argv[1:]
s = open(src).read()
assert s.count(old) == 1, (old, s.count(old))
open(dst, 'w').write(s.replace(old, new))
PY
  if $CC -std=gnu11 -O1 -w -I../../src -DPERF_HDR="\"mut/$1.h\"" -o "mut/$1" test_perf.c 2>/dev/null && ./"mut/$1" >/dev/null 2>&1; then
    echo "mutant $1: SURVIVED"
  else
    killed=$((killed+1))
  fi
}
mut rw_swapped        '    if (is_read) {
        if (rt != 31)'  '    if (!is_read) {
        if (rt != 31)'
mut xzr_write_regs31  '*shadow = rt == 31 ? 0 : regs[rt];'  '*shadow = regs[rt];'
# (not a mutant: dropping the 'rt != 31' guard on a read only writes regs[31], the frame's padding word,
#  which the vector never restores and the slow path itself zeroes - an equivalent change)
mut mask_no_crm       '#define HV_PERF_ISS_REGMASK  0x3ffc1eUL'  '#define HV_PERF_ISS_REGMASK  0x3ffc00UL'
mut mask_no_op0       '#define HV_PERF_ISS_REGMASK  0x3ffc1eUL'  '#define HV_PERF_ISS_REGMASK  0x0ffc1eUL'
mut no_ec_check       '    if (((esr >> HV_PERF_ESR_EC_SHIFT) & 0x3f) != HV_PERF_EC_MSR)
        return false;'  ''
mut no_il_check       '    if (!(esr & BIT(25))) /* IL: a 32-bit instruction; always set for AArch64 MSR/MRS */
        return false;'  ''
mut rt_shift          '    *rt = (esr >> 5) & 0x1f;'  '    *rt = (esr >> 6) & 0x1f;'
mut wrong_reg         '#define HV_PERF_ISS_MDSCR HV_PERF_ISS_SR(2, 0, 0, 2, 2)'  '#define HV_PERF_ISS_MDSCR HV_PERF_ISS_SR(2, 0, 0, 2, 0)'
mut mdcr_keeps_tda    'return (mdcr & ~(HV_PERF_MDCR_TDE | HV_PERF_MDCR_TDA))'  'return (mdcr & ~(HV_PERF_MDCR_TDE))'
mut mdcr_drops_tdosa  '| HV_PERF_MDCR_TDOSA | HV_PERF_MDCR_TDRA;'  '| HV_PERF_MDCR_TDRA;'
mut hdfg_no_wvr       '#define HV_PERF_HDFG_DBGBW (BIT(0) | BIT(1) | BIT(2) | BIT(3))'  '#define HV_PERF_HDFG_DBGBW (BIT(0) | BIT(1) | BIT(2))'
mut sr_overwrite      '        if (!e->key) {'  '        if (1) {'
mut sr_no_dir         'return (u32)((esr & HV_PERF_ISS_REGMASK) | (esr & 1)) + 1;'  'return (u32)(esr & HV_PERF_ISS_REGMASK) + 1;'
mut rate_overflow     'return (delta / dt) * freq + ((delta % dt) * freq) / dt;'  'return (delta * freq) / dt;'
mut permille_nocap    'u64 p = busy >= dt ? 1000 : (busy * 1000) / dt;'  'u64 p = (busy * 1000) / dt;'
echo "win-perf mutants: $killed/$total killed"
[ "$killed" = "$total" ]
