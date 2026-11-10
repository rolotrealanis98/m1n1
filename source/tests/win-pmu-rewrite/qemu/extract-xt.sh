#!/bin/bash
# SPDX-License-Identifier: MIT
# Pull the stage-2 execute-tracking code out of m1n1's hv_vm.c verbatim: the descriptor macros, hv_Ltop,
# hv_pt_get_l2/l3 (block split) and the hv_xt_* block, so the QEMU harness runs the committed code.
set -euo pipefail
SRC=${1:?hv_vm.c}
awk '
/^#define PAGE_SIZE /                 {on=1}
on                                    {print}
/^uint64_t vaddr_bits;/               {on=0; print ""}
/^static u64 \*hv_Ltop;/              {print; print ""}
/^static u64 \*hv_pt_get_l2\(u64 from\)/ {fn=1}
/^static u64 \*hv_pt_get_l3\(u64 from\)/ {fn=1}
fn                                    {print}
fn && /^}/                            {fn=0; print ""}
/^#define PTE_S2AP_MASK/               {xt=1}
/^int hv_pt_set_writable\(/           {xt=0}
xt                                    {print}
' "$SRC"
