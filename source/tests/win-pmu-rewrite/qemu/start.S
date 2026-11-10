/* SPDX-License-Identifier: MIT */
/* tests/win-pmu-rewrite/qemu EL2 entry, vectors, guest entry, semihosting exit */
    .section .text.start, "ax"
    .globl _start
_start:
    ldr     x0, =stack_top
    mov     sp, x0
    ldr     x0, =el2_vectors
    msr     vbar_el2, x0
    isb
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:  bl      el2_main
    b       .

    .globl guest_enter
guest_enter:
    msr     elr_el2, x0
    mov     x1, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x1
    isb
    eret

    .globl bm_exit
bm_exit:
    ldr     x1, =exit_block
    mov     w2, w0
    str     x2, [x1, #8]
    mov     x0, #0x18               /* SYS_EXIT */
    hlt     #0xf000
    b       .

    .macro  bad idx
    .align  7
    mov     x0, #\idx
    b       el2_bad_vector
    .endm

    .align  11
el2_vectors:
    bad 0
    bad 1
    bad 2
    bad 3
    bad 4
    bad 5
    bad 6
    bad 7
    .align  7                       /* 0x400: lower EL, AArch64, synchronous */
    sub     sp, sp, #(36 * 8)
    stp     x0, x1, [sp, #(0 * 8)]
    stp     x2, x3, [sp, #(2 * 8)]
    stp     x4, x5, [sp, #(4 * 8)]
    stp     x6, x7, [sp, #(6 * 8)]
    stp     x8, x9, [sp, #(8 * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30, [sp, #(30 * 8)]
    b       el2_sync_entry
    bad 9
    bad 10
    bad 11
    bad 12
    bad 13
    bad 14
    bad 15

el2_sync_entry:
    mov     x0, sp
    bl      el2_sync
    ldp     x0, x1, [sp, #(0 * 8)]
    ldp     x2, x3, [sp, #(2 * 8)]
    ldp     x4, x5, [sp, #(4 * 8)]
    ldp     x6, x7, [sp, #(6 * 8)]
    ldp     x8, x9, [sp, #(8 * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30, [sp, #(30 * 8)]
    add     sp, sp, #(36 * 8)
    eret

    .data
    .align  3
exit_block:
    .quad   0x20026                 /* ADP_Stopped_ApplicationExit */
    .quad   0
