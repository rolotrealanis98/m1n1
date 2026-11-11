# r4.12 host test: the emulated NVMe controller's INTx condition across a firmware session, a guest
# reset and Windows' admin bring-up (incl. a held AER). make -C tests/nvme-intx-r412 check
HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)
SRC ?= ../../src/hv/win/s1
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -iquote shim -I shim -iquote $(SRC)
test_intx: test_intx.c $(SRC)/hv_nvme_queue.c $(SRC)/hv_nvme_queue.h Makefile
	$(CC) $(CFLAGS) -o $@ test_intx.c $(SRC)/hv_nvme_queue.c
check: test_intx
	./test_intx
clean:
	rm -rf test_intx test_intx.dSYM
.PHONY: check clean
