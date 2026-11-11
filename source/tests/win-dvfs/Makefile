# SPDX-License-Identifier: MIT
#
# Host unit test for src/hv/win/hv_win_dvfs_core.h (cpuperf-B: Desired -> cluster p-state, mock DVFS block).
#   make -C tests/win-dvfs check

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
SRC_WIN := ../../src/hv/win
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare -iquote $(SRC_WIN)
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_dvfs: test_dvfs.c $(SRC_WIN)/hv_win_dvfs_core.h Makefile
	$(CC) $(CFLAGS) -o $@ test_dvfs.c

check: test_dvfs
	./test_dvfs

clean:
	rm -rf test_dvfs test_dvfs.dSYM cross_cfg cross_cfg.dSYM cfg.bin cfg.bin.gov

.PHONY: check clean

# cross test: the launcher's real config bytes through dvfs_check_cfg/dvfs_probe
cross: cross_cfg.c $(SRC_WIN)/hv_win_dvfs_core.h
	$(CC) $(CFLAGS) -o cross_cfg cross_cfg.c
	python3 -I ../win-launcher-smp2/test_dvfs_plan.py --write cfg.bin >/dev/null
	./cross_cfg cfg.bin
