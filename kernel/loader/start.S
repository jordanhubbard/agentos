/*
 * start.S — AArch64 loader entry point
 *
 * QEMU loads loader.elf as a proper ELF (via the loader device) and sets PC
 * to _start.  We are executing at EL2 with the MMU off.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .text.start
    .global _start
    .type   _start, %function

_start:
    /* Disable all interrupts at EL2 */
    msr daifset, #0xf

    /* Set up stack: 64 KB above loader base = 0x44010000 */
    ldr x0, =0x44010000
    mov sp, x0

    /* Clear BSS (page_table_l0, page_table_l1a, page_table_l1b) */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
    cmp x0, x1
    b.ge 2f
1:
    str xzr, [x0], #8
    cmp x0, x1
    b.lt 1b
2:

    /* Jump to C loader */
    b loader_main

    /* Should never return — halt if it does */
    .global _halt
_halt:
    wfe
    b _halt


    .section .bss
    .align 12           /* 4096-byte alignment for page tables */

    .global page_table_l0
page_table_l0:
    .space (512 * 8)    /* L0 table: 4 KB, 512 entries × 8 bytes */

    .global page_table_l1a
page_table_l1a:
    .space (512 * 8)    /* L1 table for VA[0..512GB): identity map */

    .global page_table_l1b
page_table_l1b:
    .space (512 * 8)    /* L1 table for VA[512GB..1TB): high kernel map */
