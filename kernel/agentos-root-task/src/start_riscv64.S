/*
 * start_riscv64.S — agentOS root task RISC-V (RV64) entry stub
 *
 * seL4 starts the root task in U-mode with sp = 0.  Any C function prologue
 * would immediately fault on the stack.  This stub initialises sp to the
 * statically-allocated __stack_top (defined in tools/ld/root_task_riscv64.ld)
 * and then jumps to _rt_start (the C entry point in main.c).
 *
 * seL4 RISC-V boot protocol:
 *   a0 = seL4_BootInfo * (capRegister; bi_frame_vptr set by seL4 boot.c)
 *   All other registers: unspecified
 *
 * _rt_start(seL4_BootInfo *bi) receives bi via a0 per the RISC-V LP64 C ABI.
 * We only touch t0 (scratch) and sp, preserving a0 throughout.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .text.start, "ax"
    .global _start
    .type   _start, @function

_start:
    /* Initialise the stack pointer before entering C. */
    la   t0, __stack_top
    mv   sp, t0

    /* Tail-jump to the C entry — a0 (BootInfo *) preserved. */
    j    _rt_start

    .size _start, .-_start
