# nightly-4 host test: PRP planner overlap shapes on the real model (hv_nvme_queue.c), byte-for-byte against an
# in-order reference model, plus a randomised differential test. make -C tests/prp-overlap-n4 check
HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
SRC ?= ../../src/hv/win/s1
ITER ?= 200000
SEED ?= 20261111
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -iquote shim -I shim -iquote $(SRC)
test_prp_overlap: test_prp_overlap.c $(SRC)/hv_nvme_queue.c $(SRC)/hv_nvme_queue.h Makefile
	$(CC) $(CFLAGS) -o $@ test_prp_overlap.c $(SRC)/hv_nvme_queue.c
check: test_prp_overlap
	./test_prp_overlap $(ITER) $(SEED)
clean:
	rm -rf test_prp_overlap test_prp_overlap.dSYM
.PHONY: check clean
