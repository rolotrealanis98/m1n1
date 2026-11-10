# SPDX-License-Identifier: MIT
#
# Host unit test for src/hv/win/hv_win_psci.c (smp2: PSCI CPU_ON and the calls Windows makes at boot).
#   make -C tests/win-psci check      (add -v to see the firmware's log)

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)

SRC_WIN := ../../src/hv/win
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare \
	-Wno-unused-parameter -fno-strict-aliasing \
	-DPSCI_HOST_TEST -iquote shim -iquote ../win-vgic3/shim -iquote $(SRC_WIN)
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_psci: test_psci.c $(SRC_WIN)/hv_win_psci.c $(SRC_WIN)/hv_win.h $(wildcard shim/*.h) Makefile
	$(CC) $(CFLAGS) -o $@ test_psci.c $(SRC_WIN)/hv_win_psci.c

check: test_psci
	./test_psci

clean:
	rm -rf test_psci test_psci.dSYM

.PHONY: check clean
