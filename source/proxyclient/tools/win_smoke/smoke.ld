/* SPDX-License-Identifier: MIT */
/* win_smoke: flat image linked at 0, run from wherever the launcher puts it (PC-relative code only) */
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }
    .data : ALIGN(16) {
        *(.data .data.*)
    }
    _image_end = .;
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
    }
}
