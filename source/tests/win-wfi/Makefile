# SPDX-License-Identifier: GPL-2.0-or-later
# smp2d host unit test for src/hv/hv_wfi_logic.h (WFI emulation + debug-register shadow logic).
#   make -C tests/win-wfi check

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_wfi: test_wfi.c ../../src/hv/hv_wfi_logic.h Makefile
	$(CC) $(CFLAGS) -o $@ test_wfi.c

check: test_wfi
	./test_wfi

clean:
	rm -rf test_wfi test_wfi.dSYM

.PHONY: check clean
