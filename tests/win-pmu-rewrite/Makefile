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

clean:
	rm -rf test_pmu_rewrite test_pmu_rewrite.dSYM sysreg-space.bin sysreg-space.o sysreg-space.dis

.PHONY: check oracle corpus clean
