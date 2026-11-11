# SPDX-License-Identifier: GPL-2.0-or-later
# r4.9 host unit test for src/hv/hv_sysreg_logic.h (debug-register shadow index, extra sysreg table,
# pre-escalation decode, fail-soft class gate) + a static check that hv_exc.c handles every table row.
#   make -C tests/sysreg-r49 check
HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare -I../../src \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
test_sysreg: test_sysreg.c ../../src/hv/hv_sysreg_logic.h Makefile
	$(CC) $(CFLAGS) -o $@ test_sysreg.c
check: test_sysreg
	./test_sysreg
	python3 -I static_check.py
clean:
	rm -rf test_sysreg test_sysreg.dSYM
.PHONY: check clean
