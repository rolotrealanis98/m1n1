# SPDX-License-Identifier: MIT
CC := clang
OUT ?= build
WIN := ../../src/hv/win
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
 -fno-strict-aliasing -DPMU_REWRITE_HOST_TEST -iquote ../win-pmu-rewrite/shim -iquote $(WIN) \
 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
SOURCES := test_4k.c $(WIN)/hv_win_pmu.c $(WIN)/hv_win_pmu_rewrite.c
$(OUT)/test_4k: $(SOURCES) $(WIN)/hv_win_pmu_4k.h $(WIN)/hv_win.h $(WIN)/hv_win_pmu_rewrite.h replay.h Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SOURCES) -o $@
check: $(OUT)/test_4k
	$(OUT)/test_4k
mutants:
	python3 mutants.py
.PHONY: check mutants
