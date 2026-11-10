# SPDX-License-Identifier: MIT
#
# Host unit test for src/hv/win/hv_vgic3.c (the Windows EL2 runtime's GICv3 model).
#   make -C tests/win-vgic3 && tests/win-vgic3/test_vgic3      (add -v to see the firmware's log)
# Builds with Homebrew clang when present, else the system clang. SAN=0 drops the sanitizers.

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)

SRC_WIN := ../../src/hv/win
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare \
	-Wno-unused-parameter -fno-strict-aliasing \
	-DVGIC3_HOST_TEST -iquote shim -iquote $(SRC_WIN)
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_vgic3: test_vgic3.c $(SRC_WIN)/hv_vgic3.c $(SRC_WIN)/hv_win.h $(SRC_WIN)/hv_win_regs.h \
		$(wildcard shim/*.h) Makefile
	$(CC) $(CFLAGS) -o $@ test_vgic3.c $(SRC_WIN)/hv_vgic3.c

check: test_vgic3
	./test_vgic3

clean:
	rm -rf test_vgic3 test_vgic3.dSYM

.PHONY: check clean
