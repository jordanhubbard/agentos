/*
 * mmu.S — AArch64 EL2 MMU enable stub
 *
 * setup_mmu(uint64_t l0_table_pa):
 *   Programs MAIR_EL2, TCR_EL2, TTBR0_EL2, then enables the MMU via
 *   SCTLR_EL2.  Must be called with:
 *     - x0 = physical address of the 4 KB L0 page table
 *     - MMU currently disabled
 *     - running at EL2
 *
 * After this function returns the MMU is enabled with:
 *   - TTBR0_EL2 pointing at the supplied L0 table
 *   - 48-bit VA, 4 KB granule, inner-shareable normal-WB caches
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .text
    .global setup_mmu
    .type   setup_mmu, %function

setup_mmu:
    /*
     * MAIR_EL2:
     *   Attr0 = 0xFF  → Normal memory, inner/outer write-back, read-allocate,
     *                   write-allocate (fully cacheable)
     *   All other attributes left as 0 (device memory, unused).
     */
    mov x1, #0xff
    msr mair_el2, x1

    /*
     * TCR_EL2 — Translation Control Register for EL2 (uses single-TTBR layout)
     *
     * EL2 uses a distinct register layout compared to EL1 (single range only).
     * Bits of interest:
     *   [5:0]  T0SZ = 16   → VA range = 2^(64-16) = 2^48 (48-bit VAs)
     *   [9:8]  IRGN0 = 1   → inner write-back, read-allocate, write-allocate
     *   [11:10] ORGN0 = 1  → outer write-back, read-allocate, write-allocate
     *   [13:12] SH0 = 3    → inner shareable
     *   [15:14] TG0 = 0    → 4 KB granule
     *   [18:16] PS = 2     → 40-bit physical address (covers 0x8060000000)
     *   [23]   HD = 1      → hardware dirty state tracking (required by seL4)
     *   [31]              → set per seL4 compile-time assertion (0x80823510)
     *
     * IMPORTANT: seL4 init_kernel() reads TCR_EL2 and halts if != 0x80823510.
     * This value must match exactly what the seL4 SDK was compiled with.
     *
     * Encoding:
     *   T0SZ=16  → 0x10
     *   IRGN0=1  → 1 << 8  = 0x100
     *   ORGN0=1  → 1 << 10 = 0x400
     *   SH0=3    → 3 << 12 = 0x3000
     *   TG0=0    → 0 << 14 = 0
     *   PS=2     → 2 << 16 = 0x20000
     *   HD=1     → 1 << 23 = 0x800000  (hardware dirty state tracking; seL4 asserts this)
     *   bit 31   → 1 << 31 = 0x80000000 (seL4 checks TCR_EL2 == 0x80823510 exactly)
     *
     * Total = 0x80823510  — must match seL4's compile-time assertion in init_kernel().
     */
    ldr x1, =0x0000000080823510
    msr tcr_el2, x1

    /* TTBR0_EL2 = physical address of L0 page table (passed in x0) */
    msr ttbr0_el2, x0

    /* Memory barrier before enabling MMU */
    dsb sy
    isb

    /*
     * Enable MMU via SCTLR_EL2:
     *   bit 0 (M)  = 1  → enable MMU
     *   bit 2 (C)  = 1  → enable D-cache
     *   bit 12 (I) = 1  → enable I-cache
     *
     * Read-modify-write to preserve reserved bits.
     */
    mrs x1, sctlr_el2
    ldr x2, =0x1005
    orr x1, x1, x2
    msr sctlr_el2, x1

    /* Instruction barrier — pipeline must be flushed after enabling MMU */
    isb

    ret
    .size setup_mmu, .-setup_mmu
