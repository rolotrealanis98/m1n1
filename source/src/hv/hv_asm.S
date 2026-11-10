/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "exception.h"
#include "hv_sprr.h"

#define gexit .long 0x00201400

.align 11
.globl _hv_vectors_start
_hv_vectors_start:

    /* EL2 with SP_EL0 */
    mov x9, '0'
    b cpu_reset
    .align 7
    mov x9, '1'
    b exc_unk
    .align 7
    mov x9, '2'
    b exc_unk
    .align 7
    mov x9, '3'
    b exc_unk
    .align 7

    /* EL2 with SP_EL2 */
    b _v_sp0_sync
    .align 7
    b _v_sp0_irq
    .align 7
    b _v_sp0_fiq
    .align 7
    b _v_sp0_serr
    .align 7

    /* EL1/0 64-bit */
    b _v_hv_sync
    .align 7
    b _v_hv_irq
    .align 7
    b _v_hv_fiq
    .align 7
    b _v_hv_serr
    .align 7

    /* EL1/0 32-bit */
    mov x9, 'p'
    b exc_unk
    .align 7
    mov x9, 'q'
    b exc_unk
    .align 7
    mov x9, 'r'
    b exc_unk
    .align 7
    mov x9, 's'
    b exc_unk
    .align 7

.globl _hv_entry
.type _hv_entry, @function
_hv_entry:
    stp x28, x29, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x0, x1, [sp, #-16]!

    dsb sy
    isb

    mov x0, sp
    ret

.globl _hv_return
.type _hv_return, @function
_hv_return:
    ldp x0, x1, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x28, x29, [sp], #16
    ldr x30, [sp], #16

    add sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)

    eret

.globl _hv_gexit
.type _hv_gexit, @function
_hv_gexit:
    ldp x0, x1, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x28, x29, [sp], #16
    ldr x30, [sp], #16

    add sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)

    gexit

.globl _v_hv_sync
.type _v_hv_sync, @function
_v_hv_sync:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _hv_entry
    bl hv_exc_sync
    cbz x0, 1f
    b _hv_gexit
1:
    b _hv_return

.globl _v_hv_irq
.type _v_hv_irq, @function
_v_hv_irq:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _hv_entry
    bl hv_exc_irq

    b _hv_return

.globl _v_hv_fiq
.type _v_hv_fiq, @function
_v_hv_fiq:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _hv_entry
    bl hv_exc_fiq

    b _hv_return

.globl _v_hv_serr
.type _v_hv_serr, @function
_v_hv_serr:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _hv_entry
    bl hv_exc_serr

    b _hv_return

.extern hv_saved_sp

.globl hv_enter_guest
.type hv_enter_guest, @function
hv_enter_guest:
    stp x29, x30, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    str x18, [sp, #-16]!

    mrs x7, tpidr_el2
    ldr x6, =hv_saved_sp
    mov x5, sp
    str x5, [x6, x7, LSL #3]

    mrs x5, daif
    mov x6, #5
    orr x5, x5, x6 // EL1h
    msr spsr_el2, x5

    msr elr_el2, x4
    mov x5, #0
    msr sp_el0, x5
    msr sp_el1, x5

    eret

.globl hv_exit_guest
.type hv_exit_guest, @function
hv_exit_guest:
    mrs x7, tpidr_el2
    ldr x6, =hv_saved_sp
    ldr x5, [x6, x7, LSL #3]
    mov sp, x5

    ldr x18, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16

    ret

/*
 * smp2d: save-around-WFI for EL2's own idle in the emulated guest-WFI wait (hv_win_wfi()).
 *
 * T6050 (Apple M4+) cores can resume from WFI with their general-purpose registers cleared; SP and
 * stack memory survive. This mirrors, at EL2, exactly what the owner's Linux port does at EL1 for
 * its own idle: arch/arm64/kernel/idle-apple.S apple_cpu_wfi (save x18-x30 + callee-saved around
 * `dsb sy; wfi`, with isb on resume), enabled on M4+ by apple_wfi_quirk_init() and validated on the
 * J716s (contrib/validated/integration/RESULTS.md WFI-T1: 18 cores pass an FP check over a 76 min
 * soak). m1n1's own secondary spin-table idle uses the same save set (src/utils_asm.S
 * _deep_wfi_helper). We save x18-x30 (x18 may hold a shadow-call-stack pointer in some builds; it is
 * free here but kept for parity) so the C caller's callee-saved locals survive one physical WFI; the
 * caller re-reads all wait state from memory each iteration, so clobbered x0-x17 do not matter.
 *
 * It does NOT touch CYC_OVRD (UNDEF on T6050, features_m4) or any DVFS/cpm hazard register, and does
 * not configure an Apple deep-sleep state: it is a plain WFI made register-safe in software, the only
 * method proven on this machine.
 */
.globl hv_wfi_idle
.type hv_wfi_idle, @function
hv_wfi_idle:
    stp x29, x30, [sp, #-112]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    str x18, [sp, #96]
    dsb sy
    wfi
    isb
    ldr x18, [sp, #96]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #112
    ret

.section .text.hv_sprr_vectors, "ax", %progbits
.align 14
.global hv_sprr_vectors
.type hv_sprr_vectors, %object
hv_sprr_vectors:

    /* EL1 with SP_EL0 */
    hvc #(HV_HVC_VECTOR_BASE | 0)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 1)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 2)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 3)
    .align 7

    /* EL1 with SP_EL1 */
    hvc #(HV_HVC_VECTOR_BASE | 4)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 5)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 6)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 7)
    .align 7

    /* EL0 64-bit */
    hvc #(HV_HVC_VECTOR_BASE | 8)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 9)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 10)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 11)
    .align 7

    /* EL0 32-bit */
    hvc #(HV_HVC_VECTOR_BASE | 12)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 13)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 14)
    .align 7
    hvc #(HV_HVC_VECTOR_BASE | 15)
    .align 7

    .align 14
.size hv_sprr_vectors, . - hv_sprr_vectors
