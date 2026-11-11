# SPDX-License-Identifier: MIT
CC := clang
OUT ?= build
WIN := ../../src/hv/win
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
 -fno-strict-aliasing -DPMU_REWRITE_HOST_TEST -DPMU_MAX_OPEN=4 -iquote ../win-pmu-rewrite/shim -iquote $(WIN) \
 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(EXTRA)
SOURCES := test_4k.c $(WIN)/hv_win_pmu.c $(WIN)/hv_win_pmu_rewrite.c
$(OUT)/test_4k: $(SOURCES) $(WIN)/hv_win_pmu_4k.h $(WIN)/hv_win.h $(WIN)/hv_win_pmu_rewrite.h replay.h Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SOURCES) -o $@
$(OUT)/test_sizing: test_sizing.c $(WIN)/hv_win_pmu.c $(WIN)/hv_win_pmu_rewrite.c $(WIN)/hv_win.h Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) test_sizing.c $(WIN)/hv_win_pmu.c $(WIN)/hv_win_pmu_rewrite.c -o $@
check: $(OUT)/test_4k $(OUT)/test_sizing
	$(OUT)/test_sizing
	$(OUT)/test_4k
mutants:
	python3 mutants.py
# nightly-6d: this test file against another tree's hv_win_pmu.c (default: nightly-6c aa98ccf, no UNDEF
# catcher). It must FAIL in catcher_sibling (the unscanned-sibling UNDEF reaches the guest).
BASE_WIN ?= ../../../../nightly6c/source/src/hv/win
demo-baseline:
	$(MAKE) --no-print-directory OUT=$(OUT)/baseline WIN=$(BASE_WIN) EXTRA=-DPMU_4K_BASELINE_DEMO \
	 $(OUT)/baseline/test_4k
	! $(OUT)/baseline/test_4k
.PHONY: check mutants demo-baseline
