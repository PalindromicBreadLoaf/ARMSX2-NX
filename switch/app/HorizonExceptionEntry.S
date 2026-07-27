// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonException.h"

#define EXCEPTION_SLOT_MASK ((1 << EXCEPTION_SLOT_COUNT) - 1)

.section .text.__libnx_exception_entry, "ax", %progbits
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
.align 2
__libnx_exception_entry:
    // Reserve a context before replacing the kernel exception stack.
    adrp x6, g_horizon_exception_slot_mask
    add  x6, x6, :lo12:g_horizon_exception_slot_mask
1:
    ldaxr w2, [x6]
    mvn   w3, w2
    and   w3, w3, #EXCEPTION_SLOT_MASK
    cbnz  w3, 2f
    yield
    b     1b
2:
    rbit  w4, w3
    clz   w4, w4
    mov   w5, #1
    lsl   w5, w5, w4
    orr   w3, w2, w5
    stxr  w7, w3, [x6]
    cbnz  w7, 1b

    adrp x2, g_horizon_exception_slots
    add  x2, x2, :lo12:g_horizon_exception_slots
    add  x2, x2, x4, lsl #EXCEPTION_SLOT_SHIFT
    str  x1, [x2, #SLOT_FRAME]
    str  w4, [x2, #SLOT_INDEX]
    str  wzr, [x2, #SLOT_DEFERRED]
    str  wzr, [x2, #SLOT_RESULT]

    str  w0, [x2, #DUMP_ERROR_DESC]

    // The kernel frame contains x0-x8.
    // The remaining registers are still live.
    add  x3, x2, #DUMP_GPRS + 0x48
    stp  x9,  x10, [x3], #16
    stp  x11, x12, [x3], #16
    stp  x13, x14, [x3], #16
    stp  x15, x16, [x3], #16
    stp  x17, x18, [x3], #16
    stp  x19, x20, [x3], #16
    stp  x21, x22, [x3], #16
    stp  x23, x24, [x3], #16
    stp  x25, x26, [x3], #16
    stp  x27, x28, [x3], #16
    str  x29, [x2, #DUMP_FP]

    add  x3, x2, #DUMP_GPRS
    ldp  x4, x5, [x1, #0x00]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #0x10]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #0x20]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #0x30]
    stp  x4, x5, [x3], #16
    ldr  x4, [x1, #0x40]
    str  x4, [x3]

    ldr  x4, [x1, #FRAME_LR]
    str  x4, [x2, #DUMP_LR]
    ldr  x4, [x1, #FRAME_SP]
    str  x4, [x2, #DUMP_SP]
    ldr  x4, [x1, #FRAME_PC]
    str  x4, [x2, #DUMP_PC]
    ldr  w4, [x1, #FRAME_PSTATE]
    str  w4, [x2, #DUMP_PSTATE]
    ldr  w4, [x1, #FRAME_ESR]
    str  w4, [x2, #DUMP_ESR]
    ldr  x4, [x1, #FRAME_FAR]
    str  x4, [x2, #DUMP_FAR]

    add  x3, x2, #DUMP_FPU
    stp  q0,  q1,  [x3], #32
    stp  q2,  q3,  [x3], #32
    stp  q4,  q5,  [x3], #32
    stp  q6,  q7,  [x3], #32
    stp  q8,  q9,  [x3], #32
    stp  q10, q11, [x3], #32
    stp  q12, q13, [x3], #32
    stp  q14, q15, [x3], #32
    stp  q16, q17, [x3], #32
    stp  q18, q19, [x3], #32
    stp  q20, q21, [x3], #32
    stp  q22, q23, [x3], #32
    stp  q24, q25, [x3], #32
    stp  q26, q27, [x3], #32
    stp  q28, q29, [x3], #32
    stp  q30, q31, [x3], #32

    // Nested faults need a stack which the interrupted thread cannot corrupt.
    adrp x4, __nx_exception_stack
    add  x4, x4, :lo12:__nx_exception_stack
    ldr  w5, [x2, #SLOT_INDEX]
    add  x4, x4, x5, lsl #EXCEPTION_STACK_SHIFT
    add  x4, x4, #0x10, lsl #12
    mov  sp, x4
    sub  sp, sp, #16
    str  x2, [sp]

    mov  x0, x2
    bl   __libnx_exception_handler

    ldr  x2, [sp]
    add  sp, sp, #16
    ldr  x1, [x2, #SLOT_FRAME]

    add  x3, x2, #DUMP_GPRS
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #0x00]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #0x10]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #0x20]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #0x30]
    ldr  x4, [x3]
    str  x4, [x1, #0x40]
    ldr  x4, [x2, #DUMP_LR]
    str  x4, [x1, #FRAME_LR]
    ldr  x4, [x2, #DUMP_SP]
    str  x4, [x1, #FRAME_SP]
    ldr  x4, [x2, #DUMP_PC]
    str  x4, [x1, #FRAME_PC]

    add  x3, x2, #DUMP_GPRS + 0x48
    ldp  x9,  x10, [x3], #16
    ldp  x11, x12, [x3], #16
    ldp  x13, x14, [x3], #16
    ldp  x15, x16, [x3], #16
    ldp  x17, x18, [x3], #16
    ldp  x19, x20, [x3], #16
    ldp  x21, x22, [x3], #16
    ldp  x23, x24, [x3], #16
    ldp  x25, x26, [x3], #16
    ldp  x27, x28, [x3], #16
    ldr  x29, [x2, #DUMP_FP]

    add  x3, x2, #DUMP_FPU
    ldp  q0,  q1,  [x3], #32
    ldp  q2,  q3,  [x3], #32
    ldp  q4,  q5,  [x3], #32
    ldp  q6,  q7,  [x3], #32
    ldp  q8,  q9,  [x3], #32
    ldp  q10, q11, [x3], #32
    ldp  q12, q13, [x3], #32
    ldp  q14, q15, [x3], #32
    ldp  q16, q17, [x3], #32
    ldp  q18, q19, [x3], #32
    ldp  q20, q21, [x3], #32
    ldp  q22, q23, [x3], #32
    ldp  q24, q25, [x3], #32
    ldp  q26, q27, [x3], #32
    ldp  q28, q29, [x3], #32
    ldp  q30, q31, [x3], #32

    // Deferred faults keep their slot until the trampoline deliberately faults back.
    ldr  w8, [x2, #SLOT_DEFERRED]
    cbnz w8, 4f

    ldr  w4, [x2, #SLOT_INDEX]
    mov  w5, #1
    lsl  w5, w5, w4
    mvn  w5, w5
    adrp x6, g_horizon_exception_slot_mask
    add  x6, x6, :lo12:g_horizon_exception_slot_mask
3:
    ldaxr w3, [x6]
    and   w3, w3, w5
    stlxr w7, w3, [x6]
    cbnz  w7, 3b

4:
    mov  w0, wzr
    svc  #0x28
    brk  #0
.size __libnx_exception_entry, . - __libnx_exception_entry

.global horizon_fault_trampoline
.type horizon_fault_trampoline, %function
.align 2
horizon_fault_trampoline:
    mov  x19, x0
    bl   horizon_run_deferred_fault
    str  w0, [x19, #SLOT_RESULT]
    mov  x0, xzr

.global horizon_fault_resume_marker
.type horizon_fault_resume_marker, %function
.size horizon_fault_trampoline, . - horizon_fault_trampoline
horizon_fault_resume_marker:
    ldr  x0, [x0]
    b    horizon_fault_resume_marker
.size horizon_fault_resume_marker, . - horizon_fault_resume_marker
