/* SPDX-License-Identifier: MIT */
/*
 * tests/win-pmu-rewrite/qemu EL1 guest. Every PMU instruction here traps to EL2 (MDCR_EL2.TPM|TPMCR), which fails the
 * run, unless EL2 rewrote it before it ran; an EL1 exception ends the run through M5W_HVC_FAULT_REPORT.
 * Checks go to EL2 with HVC #0x4d40 (x0 test, x1 pass, x2 value). No literal pools: constants are
 * built with movz/movk so no data word in an executed page looks like an instruction by accident;
 * fp_word below does so on purpose.
 */

    .macro  movl reg, imm
    movz    \reg, #((\imm) & 0xffff)
    movk    \reg, #(((\imm) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\imm) >> 32) & 0xffff), lsl #32
    .endm

    .macro  report test
    mov     x0, #\test
    hvc     #0x4d40
    .endm

    .macro  putc ch
    mov     x0, #\ch
    hvc     #0x4d35
    .endm

#define STACK_TOP  0x44804000
#define TABLES     0x44600000
#define DATA       0x44700000
#define MAGIC      0x5a5a1234

    .section .guest_main, "ax"
    .globl  guest_start
guest_start:
    adr     x0, guest_vectors
    msr     vbar_el1, x0
    isb
    movl    x0, STACK_TOP
    mov     sp, x0
    putc    'G'
    putc    '\n'

    /* T1: PMU accesses rewritten at the first fetch of page A (MMU off) */
    bl      fa
    cmp     x0, #0
    cset    x9, ne                  /* PMCCNTR -> CNTVCT: not zero */
    cmp     x1, x0
    cset    x10, hs                 /* ... and monotonic */
    and     x9, x9, x10
    cmp     x3, #0
    cset    x10, eq                 /* PMCR_EL0 -> 0 */
    and     x9, x9, x10
    cmp     x4, #0
    cset    x10, eq                 /* PMEVCNTR5_EL0 -> 0 */
    and     x9, x9, x10
    mov     x2, x1
    mov     x1, x9
    report  1

    /* T2: page A again: CODE now, no fault, same results */
    bl      fa
    cmp     x0, #0
    cset    x1, ne
    mov     x2, x0
    report  2

    /* T8 (nightly-5): frame 1 of page A is data that looks like a PMU instruction and defeats the
     * data guard (next word 0xf0200100). Only frame 0 of page A ever ran. 4 KiB granule: never
     * touched. 16 KiB granule: rewritten at page A's first fetch (the bugcheck-0x1A hazard). */
    movl    x1, 0x44101000
    ldr     w2, [x1]
#ifdef GRANULE_16K
    movz    w3, #0x0003
    movk    w3, #0xd280, lsl #16    /* movz x3, #0 */
#else
    movz    w3, #0xedc3
    movk    w3, #0xd53b, lsl #16    /* mrs x3, PMEVTYPER<n>_EL0, unchanged */
#endif
    cmp     w2, w3
    cset    x1, eq
    report  8

    /* T9 (nightly-5): page F, code in frames 0 and 1. ff0 opens the page (frame 0 scanned); ten
     * EL2 ticks re-protect it (4 KiB: frame 1 unscanned); ff1's fetch then faults and frame 1 is
     * proven and scanned before its PMU read runs (an unrewritten read would trap: run fails). */
    bl      ff0
#ifndef NO_TICK /* residual control: without the re-protection ff1 runs unscanned in the open page */
    mov     x19, #10
5:  hvc     #0x4d41                 /* EL2: hv_win_pmu_tick() */
    subs    x19, x19, #1
    b.ne    5b
#endif
    bl      ff1
    cmp     x0, #0
    cset    x1, ne
    mov     x2, x0
    report  9

    /* T3: page B ran (CODE); the guest writes a PMU read into it, does its I-cache maintenance and
     * calls it: the write makes the page writable + XN again, the fetch rescans it */
    bl      fb
    cmp     x0, #7
    cset    x9, eq
    movl    x1, 0x44200000
    movz    w2, #0x9d00
    movk    w2, #0xd53b, lsl #16    /* mrs x0, PMCCNTR_EL0 */
    str     w2, [x1]
    movz    w2, #0x03c0
    movk    w2, #0xd65f, lsl #16    /* ret */
    str     w2, [x1, #4]
    dc      cvau, x1
    dsb     ish
    ic      ivau, x1
    dsb     ish
    isb
    bl      fb
    cmp     x0, #7
    cset    x10, ne
    and     x9, x9, x10
    cmp     x0, #0
    cset    x10, ne
    and     x9, x9, x10
    mov     x2, x0
    mov     x1, x9
    report  3

    /* T4: page C mixes code and hot data: four write-after-execute flips pin it RWX */
    mov     x19, #6
1:  bl      fc
    movl    x1, 0x44302000
    str     x19, [x1]
    subs    x19, x19, #1
    b.ne    1b
    mov     x1, #1
    mov     x2, #0
    report  4

    /* T7: a data word in an executed page that looks like mrs PMCCNTR_EL0 is rewritten too
     * (the documented false-positive risk) */
    adr     x1, fp_word
    ldr     w2, [x1]
    movz    w3, #0xe040
    movk    w3, #0xd53b, lsl #16
    cmp     w2, w3
    cset    x1, eq
    report  7

    /* MMU on: 4 KiB stage 1, 39-bit VA, identity 1 GiB at 0x40000000 (2 MiB blocks), an alias
     * VA 0x1_0000_0000 -> page E, and VA 0x2_0000_0000 -> DATA through an L3 table that lives in
     * page D with AF = 0 (TCR_EL1.HA: the walker sets it) */
    movl    x20, TABLES
    mov     x0, x20
    movl    x1, (TABLES + 0x5000)
2:  stp     xzr, xzr, [x0], #16     /* zero L1, L2 ident, L2 alias, L3 alias, L2 d */
    cmp     x0, x1
    b.lo    2b
    movl    x0, 0x44401000          /* L3 d, inside page D (DIRTY: never executed yet) */
    add     x1, x0, #0x1000
3:  stp     xzr, xzr, [x0], #16
    cmp     x0, x1
    b.lo    3b

    /* L1[1] -> L2 ident */
    movl    x1, (TABLES + 0x1000 + 3)
    str     x1, [x20, #(1 * 8)]
    /* L2 ident: 512 x 2 MiB blocks from 0x40000000, AF | SH inner | AttrIdx 0 | block */
    add     x0, x20, #0x1000
    movl    x1, (0x40000000 | 0x400 | 0x300 | 1)
    mov     x2, #512
4:  str     x1, [x0], #8
    add     x1, x1, #0x200000
    subs    x2, x2, #1
    b.ne    4b
    /* L1[4] -> L2 alias -> L3 alias -> page E */
    movl    x1, (TABLES + 0x2000 + 3)
    str     x1, [x20, #(4 * 8)]
    movl    x1, (TABLES + 0x3000 + 3)
    add     x0, x20, #0x2000
    str     x1, [x0]
    movl    x1, (0x44500000 | 0x400 | 0x300 | 3)
    add     x0, x20, #0x3000
    str     x1, [x0]
    /* L1[8] -> L2 d -> L3 d (in page D) -> DATA, AF = 0 */
    movl    x1, (TABLES + 0x4000 + 3)
    str     x1, [x20, #(8 * 8)]
    movl    x1, (0x44401000 + 3)
    add     x0, x20, #0x4000
    str     x1, [x0]
    movl    x1, (DATA | 0x300 | 3)
    movl    x0, 0x44401000
    str     x1, [x0]
    movl    x0, DATA
    movl    x1, MAGIC
    str     w1, [x0]

    mov     x0, #0xff
    msr     mair_el1, x0
    movl    x0, ((1 << 39) | (1 << 32) | (1 << 23) | (3 << 12) | (1 << 10) | (1 << 8) | 25)
    msr     tcr_el1, x0
    msr     ttbr0_el1, x20
    dsb     ish
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    orr     x0, x0, #1
    msr     sctlr_el1, x0
    isb
    putc    'M'
    putc    '\n'

    /* T5: page E through the alias VA: FAR is not the IPA, EL2 translates it with AT S1E1R */
    movl    x1, 0x100000000
    blr     x1
    cmp     x0, #0
    cset    x1, ne
    mov     x2, x0
    report  5

    /* T6: run page D (CODE, read-only), then a load whose walk must set AF in page D's L3 table:
     * a stage-2 permission fault on the walk (S1PTW), EL2 makes page D writable again */
    bl      fd
    movl    x1, 0x200000000
    ldr     w2, [x1]
    movl    x3, MAGIC
    cmp     w2, w3
    cset    x1, eq
    report  6

    movl    x0, 0x84000008          /* PSCI SYSTEM_OFF */
    hvc     #0
    b       .

    .align  2
fp_word:
    .word   0xd53b9d00              /* mrs x0, PMCCNTR_EL0 as data */

    .align  11
guest_vectors:
    .rept   16
    .align  7
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mov     x0, #1
    hvc     #0x4d36
    b       .
    .endr

    .section .guest_a, "ax"
fa:
    mrs     x0, pmccntr_el0
    mrs     x1, pmccntr_el0
    msr     pmcr_el0, x2
    mrs     x3, pmcr_el0
    mrs     x4, pmevcntr5_el0
    msr     pmccfiltr_el0, x5
    ret

    .balign 4096
fa_data:                            /* frame 1 of page A: data, never executed */
    .word   0xd53bedc3
    .word   0xf0200100

    .section .guest_f, "ax"
ff0:
    mov     x0, #1
    ret
    .balign 4096
ff1:                                /* frame 1 of page F */
    mrs     x0, pmccntr_el0
    ret

    .section .guest_b, "ax"
fb:
    mov     x0, #7
    ret

    .section .guest_c, "ax"
fc:
    ret

    .section .guest_d, "ax"
fd:
    ret

    .section .guest_e, "ax"
fe:
    mrs     x0, pmccntr_el0
    ret
