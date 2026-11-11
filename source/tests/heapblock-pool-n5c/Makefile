# SPDX-License-Identifier: MIT
# n5c: src/heapblock.c extension pool on the host. make check
# heapblock.{c,h} are copied verbatim into $(OUT) so their "types.h" resolves to the host shim, not src/.
CC := clang
OUT ?= build
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -iquote shim \
 -iquote ../win-pmu-rewrite/shim -fsanitize=address,undefined -fno-sanitize-recover=all
$(OUT)/test_pool: test_pool.c ../../src/heapblock.c ../../src/heapblock.h $(wildcard shim/*.h) Makefile
	mkdir -p $(OUT)
	cp ../../src/heapblock.c ../../src/heapblock.h $(OUT)/
	$(CC) $(CFLAGS) -iquote $(OUT) test_pool.c $(OUT)/heapblock.c -o $@
check: $(OUT)/test_pool
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/test_pool
.PHONY: check
