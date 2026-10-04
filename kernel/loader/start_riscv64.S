/*
 * start_riscv64.S — RISC-V (RV64) loader entry point
 *
 * QEMU loads loader.elf via -kernel and OpenSBI (running in M-mode) hands
 * control to its entry point in **S-mode** with paging off (satp = 0):
 *   a0 = boot hart id
 *   a1 = device-tree blob physical address
 *
 * Unlike the AArch64 arm there is no M-mode/EL2 setup to do here — OpenSBI
 * already did it.  We only need a stack, a zeroed BSS (which holds the Sv39
 * root page table), and a jump into C.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .text.start, "ax"
    .global _start
    .type   _start, @function

_start:
    /* Mask all S-mode interrupts; OpenSBI may leave some enabled. */
    csrw    sie, zero
    csrci   sstatus, 0x2        /* sstatus.SIE = 0 */

    /* Stack: 256 KB above the loader base (see loader_riscv64.ld). */
    li      sp, 0x81040000

    /* Clear BSS, including the bootstrap page table. */
    la      t0, __bss_start
    la      t1, __bss_end
    bgeu    t0, t1, 2f
1:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    bltu    t0, t1, 1b
2:

    /* loader_main(hart_id, dtb_paddr) — a0/a1 already hold them. */
    j       loader_main

    .global _halt
_halt:
    wfi
    j       _halt

    .size _start, .-_start

    .section .bss
    .align 12                   /* 4096-byte alignment for the page table */

    .global boot_lvl1_pt
boot_lvl1_pt:
    .space (512 * 8)            /* Sv39 root table: 512 entries x 8 bytes */
