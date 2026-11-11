# SPDX-License-Identifier: MIT
#
# Host unit test for src/hv/win/hv_win_cppc_core.h (cpuperf-A: the EL2 CPPC page behind _CPC).
#   make -C tests/win-cppc check

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
SRC_WIN := ../../src/hv/win
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare -iquote $(SRC_WIN)
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_cppc: test_cppc.c $(SRC_WIN)/hv_win_cppc_core.h Makefile
	$(CC) $(CFLAGS) -o $@ test_cppc.c

check: test_cppc
	./test_cppc

clean:
	rm -rf test_cppc test_cppc.dSYM

.PHONY: check clean
