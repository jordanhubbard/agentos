/*
 * start_aarch64.S — agentOS root task AArch64 entry stub
 *
 * seL4 starts the root task (EL0) with SP = 0.  Any C function prologue
 * will immediately fault on the stack.  This stub initializes SP to the
 * statically-allocated __stack_top (defined in root_task.ld) and then
 * branches to _rt_start (the C entry point in main.c).
 *
 * seL4 AArch64 boot protocol:
 *   x0 = seL4_BootInfo * (capRegister; bi_frame_vptr set by seL4 boot.c)
 *   Other registers: unspecified
 *
 * _rt_start(seL4_BootInfo *bi) receives bi via x0 per AArch64 C ABI.
 * We only touch x9 (scratch) and sp, preserving x0 throughout.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .text.start, "ax"
    .global _start
    .type   _start, %function

_start:
    /* Initialize stack pointer before entering C */
    adrp x9, __stack_top
    add  x9, x9, :lo12:__stack_top
    mov  sp, x9

    /* Branch to C entry — x0 (BootInfo *) preserved */
    b    _rt_start

    .size _start, .-_start
