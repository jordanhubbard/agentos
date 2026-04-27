/*
 * main.c — agentOS AArch64 ELF loader (runs before seL4)
 *
 * QEMU loads loader.elf as a proper ELF at 0x44000000 (entry = _start in
 * start.S which calls loader_main here).  A second QEMU loader device places
 * the agentos.img blob at IMAGE_DATA_ADDR.
 *
 * Boot sequence:
 *   1. Parse agentos_img_hdr_t from IMAGE_DATA_ADDR
 *   2. Load seL4 kernel ELF LOAD segments → physical addresses
 *   3. Load root_task ELF LOAD segments → physical addresses
 *   4. Set up EL2 page tables (L0 + L1, 1 GB blocks)
 *   5. Enable MMU (TTBR0_EL2)
 *   6. Jump to seL4 kernel virtual entry point
 *
 * Memory map assumed:
 *   0x40000000  DRAM start (QEMU virt, 2 GB)
 *   0x44000000  loader.elf load address (this code)
 *   0x44010000  loader stack top
 *   0x48000000  agentos.img blob (mapped by QEMU loader device, addr=)
 *   0x60000000  seL4 kernel physical load address (from sel4.elf p_paddr)
 *   0x41000000  root_task physical load address (from root_task.elf p_paddr)
 *
 * seL4 AArch64 kernel entry calling convention (from Microkit 2.1.0 loader):
 *   x0 = ui_p_reg_start  (root_task physical region start)
 *   x1 = ui_p_reg_end    (root_task physical region end)
 *   x2 = pv_offset       (phys - virt; 0 if identity-mapped)
 *   x3 = v_entry         (root_task virtual entry point)
 *   x4 = dtb_addr_p      (0 = no DTB)
 *   x5 = dtb_size        (0 = no DTB)
 *   PC = 0x8060000000    (kernel virtual entry)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include "elf.h"
#include "agentos_img.h"

/* ── Memory addresses ─────────────────────────────────────────────────────── */

/* QEMU loads the agentos.img blob here */
#define IMAGE_DATA_ADDR   UINT64_C(0x48000000)

/* ── Minimal PL011 UART (QEMU virt AArch64 at 0x09000000) ───────────────── */
#define UART_BASE UINT64_C(0x09000000)

static inline void uart_putc(char c)
{
    volatile uint32_t *dr = (volatile uint32_t *)UART_BASE;
    *dr = (uint32_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc((char)*s++);
    }
}

static void uart_puthex(uint64_t v)
{
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (v >> i) & 0xf;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

/* seL4 kernel virtual entry point (from sel4.elf e_entry) */
#define KERNEL_VENTRY     UINT64_C(0x8060000000)

/* ── Page table descriptor bit fields (AArch64 LPAE) ────────────────────── */

/*
 * For EL2 TTBR0 (stage-1 only, no stage-2 here):
 *   bits[1:0] = 01  → block descriptor (valid, block not table)
 *   bit[10]   = AF  → access flag (must be 1 to avoid access-flag fault)
 *   bits[8:7] = SH  → shareability: 11 = inner-shareable
 *   bits[4:2] = AttrIndx → index into MAIR_EL2; 0 = normal WB memory
 *
 * Block descriptor for 1 GB L1 blocks:
 *   [1:0] = 01 (block)
 *   [10]  = 1  (AF)
 *   [8:7] = 11 (inner-shareable)
 *   [4:2] = 000 (AttrIndx=0, normal memory from MAIR)
 */
#define BLOCK_VALID  UINT64_C(0x1)   /* valid bit */
#define BLOCK_ENTRY  UINT64_C(0x1)   /* [1]=0 means block descriptor at L1 */
/* For L1 entries (1 GB blocks) bits[1:0] must be 01 (block = valid + !table) */
#define BLOCK_AF     (UINT64_C(1) << 10)  /* access flag */
#define BLOCK_SH_IS  (UINT64_C(3) << 8)  /* inner-shareable */
#define BLOCK_ATTRS  (BLOCK_VALID | BLOCK_AF | BLOCK_SH_IS)

/*
 * Table descriptor for L0 entries pointing to L1 tables:
 *   [1:0] = 11 (table, valid)
 */
#define TABLE_VALID  UINT64_C(0x3)

/* ── Page table globals (defined in start.S .bss) ───────────────────────── */

extern uint64_t page_table_l0[512];
extern uint64_t page_table_l1a[512]; /* covers VA 0..512GB (L0[0])  */
extern uint64_t page_table_l1b[512]; /* covers VA 512GB..1TB (L0[1])*/

/* ── Minimal bare-metal memory operations ────────────────────────────────── */

static void loader_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void loader_memset(void *dst, int val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) {
        *d++ = (uint8_t)val;
    }
}

/* ── ELF loading ─────────────────────────────────────────────────────────── */

/*
 * elf_load — load all PT_LOAD segments from an ELF64 image.
 *
 * @elf_data    pointer to the start of the ELF image in memory
 * @p_start     out: lowest physical address loaded
 * @p_end       out: one byte past the highest physical address loaded
 * @v_entry     out: ELF virtual entry point (e_entry)
 *
 * Returns 0 on success, -1 on error.
 */
static int elf_load(const void *elf_data,
                    uint64_t   *p_start,
                    uint64_t   *p_end,
                    uint64_t   *v_entry)
{
    const uint8_t *base = (const uint8_t *)elf_data;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)base;

    /* Validate ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return -1;
    }

    *v_entry = ehdr->e_entry;
    *p_start = UINT64_MAX;
    *p_end   = 0u;

    const Elf64_Phdr *phdr_table =
        (const Elf64_Phdr *)(base + ehdr->e_phoff);

    for (uint16_t i = 0u; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = &phdr_table[i];

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        uint64_t paddr = phdr->p_paddr;
        uint64_t filesz = phdr->p_filesz;
        uint64_t memsz  = phdr->p_memsz;

        /* Copy file bytes to physical address */
        if (filesz > 0u) {
            loader_memcpy((void *)paddr, base + phdr->p_offset, filesz);
        }

        /* Zero the BSS portion (memsz > filesz) */
        if (memsz > filesz) {
            loader_memset((void *)(paddr + filesz), 0, memsz - filesz);
        }

        /* Track physical extent */
        if (paddr < *p_start) {
            *p_start = paddr;
        }
        uint64_t seg_end = paddr + memsz;
        if (seg_end > *p_end) {
            *p_end = seg_end;
        }
    }

    return 0;
}

/* ── Page table setup ────────────────────────────────────────────────────── */

/*
 * setup_page_tables — populate L0 + L1 tables with 1 GB block mappings.
 *
 * VA layout (with 48-bit VAs, 4 KB pages, 3-level walk starting at L0):
 *   L0 index = VA[47:39]   (9 bits, selects 512 GB region)
 *   L1 index = VA[38:30]   (9 bits, selects 1 GB block within that region)
 *
 * Mappings installed:
 *   L0[0] → L1a  (VA 0..512GB)
 *     L1a[0] = 0x00000000 (identity: VA 0..1GB → PA 0..1GB)
 *     L1a[1] = 0x40000000 (identity: VA 1GB..2GB → PA 0x40000000..0x7FFFFFFF)
 *             covers loader at 0x44000000, image at 0x48000000,
 *             kernel phys at 0x60000000, root_task phys at 0x41000000
 *
 *   L0[1] → L1b  (VA 512GB..1TB)
 *     L1b[1] = 0x40000000 (VA 0x8040000000..0x807FFFFFFF → PA 0x40000000..0x7FFFFFFF)
 *             kernel virtual 0x8060000000 maps to PA 0x60000000 ✓
 *
 * All blocks use normal-memory inner-shareable WB attributes (AttrIndx=0).
 */
static void setup_page_tables(void)
{
    /*
     * L0[0]: table descriptor → L1a
     * L0[1]: table descriptor → L1b
     */
    page_table_l0[0] = ((uint64_t)(uintptr_t)page_table_l1a) | TABLE_VALID;
    page_table_l0[1] = ((uint64_t)(uintptr_t)page_table_l1b) | TABLE_VALID;

    /*
     * L1a — identity map for lower 2 GB of DRAM and below
     *   [0]: PA 0x00000000 — not strictly needed but harmless
     *   [1]: PA 0x40000000 — covers 0x40000000..0x7FFFFFFF
     */
    page_table_l1a[0] = (UINT64_C(0x00000000)) | BLOCK_ATTRS;
    page_table_l1a[1] = (UINT64_C(0x40000000)) | BLOCK_ATTRS;

    /*
     * L1b — high kernel window
     * VA 0x8040000000 is L0[1]/L1b[1] (index = (0x8040000000 >> 30) & 0x1FF = 1).
     * This block maps VA 0x8040000000..0x807FFFFFFF → PA 0x40000000..0x7FFFFFFF.
     * Kernel virtual entry 0x8060000000 falls in PA 0x60000000.  ✓
     */
    page_table_l1b[1] = (UINT64_C(0x40000000)) | BLOCK_ATTRS;
}

/* ── MMU enable (implemented in mmu.S) ──────────────────────────────────── */

extern void setup_mmu(uint64_t l0_table_pa);

/* ── Loader main ─────────────────────────────────────────────────────────── */

void loader_main(void)
{
    uart_puts("\nagentOS loader\n");

    const uint8_t *img = (const uint8_t *)IMAGE_DATA_ADDR;

    /* 1. Validate image header */
    const agentos_img_hdr_t *hdr = (const agentos_img_hdr_t *)img;
    uart_puts("image magic: "); uart_puthex(hdr->magic); uart_puts("\n");
    if (hdr->magic != AGENTOS_IMAGE_MAGIC) {
        uart_puts("FATAL: bad magic\n");
        for (;;) { __asm__ volatile("wfe"); }
    }
    uart_puts("image ok, loading kernel...\n");

    /* 2. Load seL4 kernel ELF */
    uint64_t kernel_p_start, kernel_p_end, kernel_v_entry;
    const void *kernel_elf = (const void *)(img + hdr->kernel_off);
    if (elf_load(kernel_elf, &kernel_p_start, &kernel_p_end, &kernel_v_entry) != 0) {
        uart_puts("FATAL: kernel ELF load failed\n");
        for (;;) { __asm__ volatile("wfe"); }
    }
    uart_puts("kernel loaded: pstart="); uart_puthex(kernel_p_start);
    uart_puts(" pend="); uart_puthex(kernel_p_end);
    uart_puts(" entry="); uart_puthex(kernel_v_entry); uart_puts("\n");

    /* 3. Load root_task ELF */
    uart_puts("loading root_task...\n");
    uint64_t root_p_start, root_p_end, root_v_entry;
    const void *root_elf = (const void *)(img + hdr->root_off);
    if (elf_load(root_elf, &root_p_start, &root_p_end, &root_v_entry) != 0) {
        uart_puts("FATAL: root_task ELF load failed\n");
        for (;;) { __asm__ volatile("wfe"); }
    }
    uart_puts("root_task loaded: pstart="); uart_puthex(root_p_start);
    uart_puts(" pend="); uart_puthex(root_p_end);
    uart_puts(" entry="); uart_puthex(root_v_entry); uart_puts("\n");

    /* 4. Set up EL2 page tables */
    uart_puts("setting up page tables...\n");
    setup_page_tables();

    /* Memory/instruction barrier before enabling MMU */
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* 5. Enable MMU with our L0 table */
    uart_puts("enabling MMU...\n");
    setup_mmu((uint64_t)(uintptr_t)page_table_l0);
    uart_puts("MMU enabled, jumping to seL4...\n");

    /*
     * 6. Jump to seL4 kernel virtual entry.
     *
     * Calling convention (seL4 AArch64 boot protocol):
     *   x0 = ui_p_reg_start  (root_task phys start)
     *   x1 = ui_p_reg_end    (root_task phys end)
     *   x2 = pv_offset       (phys - virt = 0 for identity-mapped root_task)
     *   x3 = v_entry         (root_task virtual entry point)
     *   x4 = 0               (no DTB)
     *   x5 = 0               (DTB size = 0)
     *
     * The kernel entry lives at a high virtual address (0x8060000000) which
     * C cannot call directly as a function pointer on all toolchains, so we
     * use inline assembly with an explicit register load for the branch target.
     */
    register uint64_t r0 __asm__("x0") = root_p_start;
    register uint64_t r1 __asm__("x1") = root_p_end;
    register int64_t  r2 __asm__("x2") = (int64_t)0;     /* pv_offset = 0 */
    register uint64_t r3 __asm__("x3") = root_v_entry;
    register uint64_t r4 __asm__("x4") = (uint64_t)0;    /* no DTB */
    register uint64_t r5 __asm__("x5") = (uint64_t)0;    /* DTB size = 0 */

    uint64_t kentry = KERNEL_VENTRY;

    __asm__ volatile(
        "br %[kentry]"
        : /* no outputs */
        : [kentry] "r" (kentry),
          "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r5)
        : /* no clobbers — br is noreturn */
    );

    /* Should never reach here */
    __builtin_unreachable();
}
