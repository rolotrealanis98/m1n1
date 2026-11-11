# SPDX-License-Identifier: MIT
# nightly-5: storage admission (hv_win_storage_mem.c hv_storage_guest_span) against the real PMU
# rewrite (hv_win_pmu.c) over one shared mock stage 2. make check | make mutants
CC := clang
OUT ?= build
SRC := ../../src/hv/win
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing \
 -DPMU_REWRITE_HOST_TEST -iquote shim -iquote ../win-pmu-rewrite/shim -iquote $(SRC) \
 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
SOURCES := test_admit.c $(SRC)/hv_win_storage_mem.c $(SRC)/hv_win_pmu.c $(SRC)/hv_win_pmu_rewrite.c
$(OUT)/test_admit: $(SOURCES) $(SRC)/hv_win_storage_mem.h $(SRC)/hv_win.h $(SRC)/hv_win_pmu_4k.h $(wildcard shim/*.h) Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SOURCES) -o $@
check: $(OUT)/test_admit
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/test_admit
mutants:
	python3 mutants.py
.PHONY: check mutants
