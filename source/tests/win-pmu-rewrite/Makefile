# SPDX-License-Identifier: MIT
#
# Host test for the Windows EL2 runtime's stage-2 PMUv3 rewrite (src/hv/win/hv_win_pmu_rewrite.c and
# src/hv/win/hv_win_pmu.c, design in m5pro-windows docs/PMU-EL2-REWRITE.md).
#   make -C tests/win-pmu-rewrite check     synthetic tests (add V=1 for the firmware's console lines)
#   make -C tests/win-pmu-rewrite oracle    every op0=3 MRS/MSR encoding against llvm-objdump's names
#   make -C tests/win-pmu-rewrite corpus CORPUS=<dir>
#                                           <dir>/*.dis (llvm-objdump -d listings of Windows images,
#                                           never committed) per instruction, and <dir>/<image> per PE
#                                           section for PMU-looking words in data
#   make -C tests/win-pmu-rewrite qemu      hv_vm.c's execute tracking + hv_win_pmu.c at QEMU's EL2
#                                           with an EL1 guest (qemu/run.sh; qemu-system-aarch64)
# Builds with Homebrew LLVM when present, else the system clang. SAN=0 drops the sanitizers.

LLVM := /opt/homebrew/opt/llvm/bin
CC := $(if $(wildcard $(LLVM)/clang),$(LLVM)/clang,clang)
OBJDUMP := $(if $(wildcard $(LLVM)/llvm-objdump),$(LLVM)/llvm-objdump,llvm-objdump)
OBJCOPY := $(if $(wildcard $(LLVM)/llvm-objcopy),$(LLVM)/llvm-objcopy,llvm-objcopy)

SRC_WIN := ../../src/hv/win
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare \
	-Wno-unused-parameter -fno-strict-aliasing \
	-DPMU_REWRITE_HOST_TEST -iquote shim -iquote $(SRC_WIN)
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

SRCS := test_pmu_rewrite.c $(SRC_WIN)/hv_win_pmu_rewrite.c $(SRC_WIN)/hv_win_pmu.c

test_pmu_rewrite: $(SRCS) $(SRC_WIN)/hv_win.h $(SRC_WIN)/hv_win_pmu_rewrite.h $(wildcard shim/*.h) Makefile
	$(CC) $(CFLAGS) -o $@ $(SRCS)

check: test_pmu_rewrite
	./test_pmu_rewrite $(if $(V),-v)

oracle: test_pmu_rewrite
	./test_pmu_rewrite --emit-sysreg-space sysreg-space.bin
	$(OBJCOPY) -I binary -O elf64-littleaarch64 --rename-section=.data=.text,alloc,load,readonly,code \
		sysreg-space.bin sysreg-space.o
	$(OBJDUMP) -d --mattr=+all sysreg-space.o > sysreg-space.dis
	./test_pmu_rewrite --dis sysreg-space.dis

corpus: test_pmu_rewrite
	@test -n "$(CORPUS)" || { echo "CORPUS=<dir with *.dis and the images>"; exit 2; }
	./test_pmu_rewrite --dis $(wildcard $(CORPUS)/*.dis)
	./test_pmu_rewrite --pe $(filter-out %.dis %.inf,$(wildcard $(CORPUS)/*))

qemu:
	./qemu/run.sh
	XT_DEFS=-DGRANULE_16K ./qemu/run.sh
	XT_DEFS=-DNO_ARM ./qemu/run.sh > /dev/null 2>&1; test $$? -ne 0 && echo "negative control (not armed): failed as it must"
	XT_DEFS=-DNO_TICK ./qemu/run.sh > /dev/null 2>&1; test $$? -ne 0 && grep -q "unrewritten PMU access" $${QEMU_OUT:-qemu}/out/xt.log && echo "residual control (4 KiB, no re-protection tick): the unscanned sibling trapped, as documented"

clean:
	rm -rf qemu/build qemu/out test_pmu_rewrite test_pmu_rewrite.dSYM sysreg-space.bin sysreg-space.o sysreg-space.dis

.PHONY: check oracle corpus qemu clean
