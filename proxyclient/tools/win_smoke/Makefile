# SPDX-License-Identifier: MIT
# win_smoke: EL1 smoke guest for the Windows EL2 runtime. Bare-metal clang + lld, no libc.
#   make                                   (clang/ld.lld/llvm-objcopy in PATH, e.g. inside the m1n1 build container)
#   make TOOLCHAIN=/opt/homebrew/opt/llvm/bin/ LLDDIR=/opt/homebrew/opt/lld/bin/   (macOS, Homebrew LLVM)
# Output: smoke.bin (flat, entry at offset 0, position independent, < 64 KiB) and smoke.bin.sha256.

TOOLCHAIN ?=
LLDDIR ?=
CC := $(TOOLCHAIN)clang
LD := $(LLDDIR)ld.lld
OBJCOPY := $(TOOLCHAIN)llvm-objcopy
READELF := $(TOOLCHAIN)llvm-readelf

TARGET := --target=aarch64-none-elf -march=armv8.2-a
CFLAGS := $(TARGET) -O2 -g0 -Wall -Wextra -Werror -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
	-fno-pic -fno-pie -fno-jump-tables -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-common \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections
ASFLAGS := $(TARGET) -g0

all: smoke.bin

start.o: start.S
	$(CC) $(ASFLAGS) -c -o $@ $<

smoke.o: smoke.c
	$(CC) $(CFLAGS) -c -o $@ $<

smoke.elf: start.o smoke.o smoke.ld
	@if $(READELF) -r start.o smoke.o | grep -qE 'R_AARCH64_(ABS16|ABS32|ABS64|ADR_GOT|LD64_GOT)'; then \
		echo "absolute or GOT relocation: the image would not be position independent"; exit 1; fi
	$(LD) -T smoke.ld --gc-sections -static -nostdlib --no-dynamic-linker -o $@ start.o smoke.o

smoke.bin: smoke.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(wc -c < $@); if [ $$size -ge 65536 ]; then echo "smoke.bin is $$size bytes (limit 64 KiB)"; exit 1; fi; \
		echo "smoke.bin: $$size bytes"
	@shasum -a 256 $@ 2>/dev/null > $@.sha256 || sha256sum $@ > $@.sha256; cat $@.sha256

clean:
	rm -f *.o smoke.elf smoke.bin smoke.bin.sha256

.PHONY: all clean
