# SPDX-License-Identifier: MIT
#
# Host unit test for src/dart_identity.c (EL2's identity tables for a guest-owned USB DART).
#   make -C tests/usb-host check
# Builds with Homebrew clang when present, else the system clang. SAN=0 drops the sanitizers.

HOMEBREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
CC := $(if $(wildcard $(HOMEBREW_CLANG)),$(HOMEBREW_CLANG),clang)

SRC := ../../src
SAN ?= 1

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -Wsign-compare \
	-Wno-unused-parameter -fno-strict-aliasing \
	-DDART_IDENTITY_HOST_TEST -iquote shim
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif

test_dart_identity: test_dart_identity.c $(SRC)/dart_identity.c $(SRC)/dart_identity.h \
		$(wildcard shim/*.h) Makefile
	$(CC) $(CFLAGS) -o $@ test_dart_identity.c $(SRC)/dart_identity.c

check: test_dart_identity
	./test_dart_identity

clean:
	rm -rf test_dart_identity test_dart_identity.dSYM

.PHONY: check clean
