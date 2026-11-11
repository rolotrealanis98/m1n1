# SPDX-License-Identifier: MIT
# perf-exits host test for src/hv/win/hv_win_perf.h (MDSCR fast-path decode/emulation against the slow
# path's semantics, MDCR value, sysreg statistics table, rate helpers) + static checks of the wiring
# (hv_asm.S, hv_exc.c, hv_win.c, proxy) + mutants of the header that the test must kill.
#   make -C tests/win-perf check
HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
HDR ?= ../../src/hv/win/hv_win_perf.h
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare -I../../src \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
test_perf: test_perf.c $(HDR) Makefile
	$(CC) $(CFLAGS) -DPERF_HDR='"$(HDR)"' -o $@ test_perf.c
check: test_perf
	./test_perf
	python3 -I static_check.py
	bash mutants.sh
clean:
	rm -rf test_perf test_perf.dSYM mut
.PHONY: check clean
