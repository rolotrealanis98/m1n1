/* SPDX-License-Identifier: MIT */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end = .; }
    . = ALIGN(16);
    . += 0x10000;
    stack_top = .;
    /* the guest: one 16 KiB stage-2 page per role (IPA = PA) */
    .guest_main 0x44000000 : { *(.guest_main) }
    .guest_a    0x44100000 : { *(.guest_a) }
    .guest_b    0x44200000 : { *(.guest_b) }
    .guest_c    0x44300000 : { *(.guest_c) }
    .guest_d    0x44400000 : { *(.guest_d) }
    .guest_e    0x44500000 : { *(.guest_e) }
    .guest_f    0x44a00000 : { *(.guest_f) } /* nightly-5: code in frames 0 and 1 */
    /DISCARD/ : { *(.comment) *(.note*) }
}
