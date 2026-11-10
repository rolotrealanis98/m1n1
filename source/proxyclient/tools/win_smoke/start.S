/* SPDX-License-Identifier: MIT */
/*
 * win_smoke entry and exception vectors. Loaded by run_windows.py --raw-guest at a 2 MiB aligned guest PA,
 * entered at offset 0 in EL1h with the MMU and caches off. Position independent: only PC-relative
 * addressing (adr/adrp), no absolute pointers in data.
 *
 * Entry: x0 = boot_args, x1 = uart0 (vuart) base, x2 = PL011 base or 0, x3 = 0x4d355753 ("M5WS");
 * x4..x7 are whatever m1n1's hv_enter_guest left (x4 = entry address, x5 = 0, x6 = SPSR, x7 = CPU index).
 */

#define FRAME_SIZE 272 /* x0..x30, ELR, SPSR, padded to 16 */

    .section .text.start, "ax"
    .globl _start
_start:
    adr     x9, entry_regs
    stp     x0, x1, [x9, #0]
    stp     x2, x3, [x9, #16]
    stp     x4, x5, [x9, #32]
    stp     x6, x7, [x9, #48]
    mrs     x10, sctlr_el1
    mrs     x11, CurrentEL
    stp     x10, x11, [x9, #64]
    mrs     x10, daif
    mov     x11, sp
    stp     x10, x11, [x9, #80]
    msr     daifset, #0xf

    /* .bss (stack included) is not part of the flat image: zero it */
    adr     x10, __bss_start
    adr     x11, __bss_end
1:  cmp     x10, x11
    b.hs    2f
    str     xzr, [x10], #8
    b       1b
2:
    adr     x10, stack_top
    mov     sp, x10
    adr     x10, vectors
    msr     vbar_el1, x10
    isb
    bl      smoke_main
3:  wfe
    b       3b

/*
 * void wfi_probe(u64 out[32], u64 seed): xN = seed + N for N = 1..28, WFI, store x1..x28 to out[1..28];
 * out[0] = SP before, out[29] = CNTVCT before, out[30] = CNTVCT after, out[31] = SP after.
 */
    .text
    .globl wfi_probe
wfi_probe:
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    mov     x29, x0
    mov     x30, sp
    str     x30, [x29, #0]
    add     x2, x1, #2
    add     x3, x1, #3
    add     x4, x1, #4
    add     x5, x1, #5
    add     x6, x1, #6
    add     x7, x1, #7
    add     x8, x1, #8
    add     x9, x1, #9
    add     x10, x1, #10
    add     x11, x1, #11
    add     x12, x1, #12
    add     x13, x1, #13
    add     x14, x1, #14
    add     x15, x1, #15
    add     x16, x1, #16
    add     x17, x1, #17
    add     x18, x1, #18
    add     x19, x1, #19
    add     x20, x1, #20
    add     x21, x1, #21
    add     x22, x1, #22
    add     x23, x1, #23
    add     x24, x1, #24
    add     x25, x1, #25
    add     x26, x1, #26
    add     x27, x1, #27
    add     x28, x1, #28
    add     x1, x1, #1
    isb
    mrs     x30, cntvct_el0
    str     x30, [x29, #29 * 8]
    dsb     sy
    wfi
    isb
    mrs     x30, cntvct_el0
    str     x30, [x29, #30 * 8]
    mov     x30, sp
    str     x30, [x29, #31 * 8]
    stp     x1, x2, [x29, #1 * 8]
    stp     x3, x4, [x29, #3 * 8]
    stp     x5, x6, [x29, #5 * 8]
    stp     x7, x8, [x29, #7 * 8]
    stp     x9, x10, [x29, #9 * 8]
    stp     x11, x12, [x29, #11 * 8]
    stp     x13, x14, [x29, #13 * 8]
    stp     x15, x16, [x29, #15 * 8]
    stp     x17, x18, [x29, #17 * 8]
    stp     x19, x20, [x29, #19 * 8]
    stp     x21, x22, [x29, #21 * 8]
    stp     x23, x24, [x29, #23 * 8]
    stp     x25, x26, [x29, #25 * 8]
    stp     x27, x28, [x29, #27 * 8]
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret

/* Vectors: every entry saves the full frame and calls exc_handler(frame, vector index). */
.macro ventry idx
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x1, #\idx
    b       exc_common
.endm

    .section .text.vectors, "ax"
    .balign 0x800
    .globl vectors
vectors:
    ventry 0
    ventry 1
    ventry 2
    ventry 3
    ventry 4
    ventry 5
    ventry 6
    ventry 7
    ventry 8
    ventry 9
    ventry 10
    ventry 11
    ventry 12
    ventry 13
    ventry 14
    ventry 15

exc_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, elr_el1
    stp     x30, x2, [sp, #240]
    mrs     x2, spsr_el1
    str     x2, [sp, #256]
    mov     x0, sp
    bl      exc_handler
    ldr     x2, [sp, #256]
    msr     spsr_el1, x2
    ldp     x30, x2, [sp, #240]
    msr     elr_el1, x2
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #FRAME_SIZE
    eret

    .data
    .balign 16
    .globl entry_regs
entry_regs:
    .skip 12 * 8

    .bss
    .balign 16
stack:
    .skip 16384
    .globl stack_top
stack_top:
