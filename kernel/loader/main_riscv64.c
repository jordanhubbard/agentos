/*
 * main_riscv64.c — agentOS RISC-V (RV64) ELF loader (runs before seL4)
 *
 * This is the riscv64 arm of kernel/loader/.  It is a direct translation of
 * main.c (AArch64); the image container format, the ELF loading and the seL4
 * entry convention are identical.  What differs, and why:
 *
 *   - OpenSBI has already run in M-mode and hands us S-mode with paging off.
 *     There is no EL2/M-mode configuration step: a0 = boot hart id and
 *     a1 = device-tree pointer are the only inbound state, and seL4's
 *     RISC-V _start wants neither (it takes the same six boot arguments
 *     AArch64 does; see the SDK's own loader `arch_jump_to_kernel`).
 *   - Paging is Sv39: a three-level, 39-bit-VA scheme selected by writing
 *     mode 8 into satp, not by TTBR0/SCTLR.  The root table alone is enough
 *     here because every mapping we need is a 1 GiB "gigapage" leaf.
 *   - Serial is an NS16550A at 0x10000000 (byte-wide registers), not a PL011.
 *
 * Boot sequence:
 *   1. Parse agentos_img_hdr_t from IMAGE_DATA_ADDR
 *   2. Load seL4 kernel ELF PT_LOAD segments -> physical addresses
 *   3. Load root_task ELF PT_LOAD segments   -> physical addresses
 *   4. Build an Sv39 table: identity map of the low half, plus the kernel's
 *      own high virtual window derived from its PT_LOAD headers
 *   5. Enable paging (satp)
 *   6. Jump to the seL4 kernel's virtual entry point
 *
 * Memory map assumed (QEMU virt, riscv64, OpenSBI as -bios):
 *   0x10000000  NS16550A UART0
 *   0x80000000  DRAM start; 0x80000000..0x80200000 reserved for OpenSBI
 *   0x80200000  seL4 kernel physical load address (from sel4.elf p_paddr)
 *   0x81000000  loader.elf load address (this code)
 *   0x81040000  loader stack top
 *   0x88000000  agentos.img blob (QEMU -device loader,addr=)
 *   0x90000000  root_task physical load address (root_task_riscv64.ld)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdbool.h>
#include <stdint.h>
#include "elf.h"
#include "agentos_img.h"

/* QEMU loads the agentos.img blob here (see xtask/src/cmd_test.rs). */
#define IMAGE_DATA_ADDR   UINT64_C(0x88000000)

/* ── Minimal NS16550A UART (QEMU virt riscv64 at 0x10000000) ─────────────── */
#define UART_BASE   UINT64_C(0x10000000)
#define UART_THR    0x00u   /* transmit holding register      */
#define UART_LSR    0x05u   /* line status register           */
#define UART_LSR_THRE 0x20u /* transmit holding reg. empty    */

static void uart_putc(char c)
{
    volatile uint8_t *base = (volatile uint8_t *)UART_BASE;
    while (!(base[UART_LSR] & UART_LSR_THRE)) {
    }
    base[UART_THR] = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

static void uart_puthex(uint64_t v)
{
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (uint8_t)((v >> i) & 0xf);
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}

__attribute__((noreturn)) static void fatal(const char *msg)
{
    uart_puts("FATAL: ");
    uart_puts(msg);
    uart_puts("\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ── Sv39 page table encoding ────────────────────────────────────────────── */
/*
 * Sv39 splits a 39-bit VA into VPN[2]=VA[38:30], VPN[1]=VA[29:21],
 * VPN[0]=VA[20:12].  A valid PTE whose R/W/X bits are all zero is a pointer
 * to the next level; any other valid PTE is a leaf.  A leaf at the root
 * level therefore maps 1 GiB and requires a 1 GiB-aligned PPN.
 *
 * PTE layout: [63:54] reserved, [53:10] PPN, [9:8] RSW, then
 *   D(7) A(6) G(5) U(4) X(3) W(2) R(1) V(0)
 */
#define PTE_V  UINT64_C(0x001)
#define PTE_R  UINT64_C(0x002)
#define PTE_W  UINT64_C(0x004)
#define PTE_X  UINT64_C(0x008)
#define PTE_G  UINT64_C(0x020)
#define PTE_A  UINT64_C(0x040)
#define PTE_D  UINT64_C(0x080)

/* Supervisor-mode RWX leaf, global, pre-set access/dirty so no hardware or
 * software A/D update is ever needed while this table is live. */
#define PTE_LEAF_FLAGS (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)

#define GIGAPAGE  (UINT64_C(1) << 30)

/* satp: MODE[63:60] = 8 (Sv39), ASID[59:44] = 0, PPN[43:0] = table >> 12 */
#define SATP_MODE_SV39 (UINT64_C(8) << 60)

/*
 * Only the root table is needed: every mapping we install is a gigapage.
 * Defined in start_riscv64.S's .bss so it is 4 KiB-aligned and zeroed before
 * C runs.
 */
extern uint64_t boot_lvl1_pt[512];

static inline uint64_t sv39_leaf(uint64_t pa)
{
    return ((pa >> 12) << 10) | PTE_LEAF_FLAGS;
}

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

/* ── ELF loading (identical in behaviour to the AArch64 arm) ─────────────── */

static int elf_load(const void *elf_data,
                    uint64_t   *p_start,
                    uint64_t   *p_end,
                    uint64_t   *v_start,
                    uint64_t   *v_entry)
{
    const uint8_t *base = (const uint8_t *)elf_data;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)base;

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
    *v_start = UINT64_MAX;

    const Elf64_Phdr *phdr_table = (const Elf64_Phdr *)(base + ehdr->e_phoff);

    for (uint16_t i = 0u; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = &phdr_table[i];
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0u) {
            continue;
        }

        uint64_t paddr  = phdr->p_paddr;
        uint64_t filesz = phdr->p_filesz;
        uint64_t memsz  = phdr->p_memsz;

        if (filesz > 0u) {
            loader_memcpy((void *)paddr, base + phdr->p_offset, filesz);
        }
        if (memsz > filesz) {
            loader_memset((void *)(paddr + filesz), 0, memsz - filesz);
        }

        if (paddr < *p_start) {
            *p_start = paddr;
        }
        if (paddr + memsz > *p_end) {
            *p_end = paddr + memsz;
        }
        if (phdr->p_vaddr < *v_start) {
            *v_start = phdr->p_vaddr;
        }
    }

    return (*p_end == 0u) ? -1 : 0;
}

/* ── Sv39 bootstrap mapping ──────────────────────────────────────────────── */

/*
 * setup_page_tables — identity map the low half, then map the kernel window.
 *
 * Root-table index = VA[38:30].  Sv39 requires VA[63:39] to be a sign
 * extension of VA[38], so indices 0..255 are reachable only from the low
 * half (VA == PA identity: loader, image blob, kernel and root_task physical
 * destinations, and the UART) and indices 256..511 only from the high half,
 * where the seL4 kernel's own virtual window lives.  The two never collide,
 * which is why 256 identity gigapages and the ELF-derived kernel window can
 * share one table.
 *
 * Returns false if the kernel ELF cannot be expressed this way (its
 * virtual-to-physical delta must be a multiple of 1 GiB) or if its entry
 * point is not inside a mapped segment.
 */
static bool setup_page_tables(const void *kernel_elf)
{
    /* Identity map PA 0 .. 256 GiB using gigapages. */
    for (uint64_t i = 0; i < 256u; i++) {
        boot_lvl1_pt[i] = sv39_leaf(i * GIGAPAGE);
    }

    const Elf64_Ehdr *header = (const Elf64_Ehdr *)kernel_elf;
    const Elf64_Phdr *segments =
        (const Elf64_Phdr *)((const uint8_t *)kernel_elf + header->e_phoff);

    bool entry_mapped = false;
    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const Elf64_Phdr *segment = &segments[i];
        if (segment->p_type != PT_LOAD || segment->p_memsz == 0u) {
            continue;
        }

        uint64_t delta = segment->p_vaddr - segment->p_paddr;
        if (delta & (GIGAPAGE - 1u)) {
            return false;
        }

        uint64_t first = segment->p_vaddr & ~(GIGAPAGE - 1u);
        uint64_t last  = (segment->p_vaddr + segment->p_memsz - 1u) &
                         ~(GIGAPAGE - 1u);
        for (uint64_t va = first; va <= last; va += GIGAPAGE) {
            unsigned index = (unsigned)((va >> 30) & 511u);
            uint64_t entry = sv39_leaf(va - delta);
            if (boot_lvl1_pt[index] && boot_lvl1_pt[index] != entry) {
                return false;
            }
            boot_lvl1_pt[index] = entry;
            if (va == last) {
                break;  /* guard against wrap at the top of the address space */
            }
        }

        if (header->e_entry >= segment->p_vaddr &&
            header->e_entry - segment->p_vaddr < segment->p_memsz) {
            entry_mapped = true;
        }
    }

    return entry_mapped;
}

/* ── Loader main ─────────────────────────────────────────────────────────── */

__attribute__((noreturn)) void loader_main(uint64_t hart_id, uint64_t dtb_paddr);

__attribute__((noreturn)) void loader_main(uint64_t hart_id, uint64_t dtb_paddr)
{
    (void)dtb_paddr;

    uart_puts("\nagentOS loader (riscv64, S-mode)\n");
    uart_puts("boot hart="); uart_puthex(hart_id);
    uart_puts(" dtb="); uart_puthex(dtb_paddr); uart_puts("\n");

    const uint8_t *img = (const uint8_t *)IMAGE_DATA_ADDR;

    /* 1. Validate image header */
    const agentos_img_hdr_t *hdr = (const agentos_img_hdr_t *)img;
    uart_puts("image magic: "); uart_puthex(hdr->magic); uart_puts("\n");
    if (hdr->magic != AGENTOS_IMAGE_MAGIC) {
        fatal("bad magic");
    }
    uart_puts("image ok, loading kernel...\n");

    /* 2. Load seL4 kernel ELF */
    uint64_t kernel_p_start, kernel_p_end, kernel_v_start, kernel_v_entry;
    const void *kernel_elf = (const void *)(img + hdr->kernel_off);
    if (elf_load(kernel_elf, &kernel_p_start, &kernel_p_end,
                 &kernel_v_start, &kernel_v_entry) != 0) {
        fatal("kernel ELF load failed");
    }
    uart_puts("kernel loaded: pstart="); uart_puthex(kernel_p_start);
    uart_puts(" pend="); uart_puthex(kernel_p_end);
    uart_puts(" entry="); uart_puthex(kernel_v_entry); uart_puts("\n");

    /* 3. Load root_task ELF */
    uart_puts("loading root_task...\n");
    uint64_t root_p_start, root_p_end, root_v_start, root_v_entry;
    const void *root_elf = (const void *)(img + hdr->root_off);
    if (elf_load(root_elf, &root_p_start, &root_p_end,
                 &root_v_start, &root_v_entry) != 0) {
        fatal("root_task ELF load failed");
    }
    int64_t pv_offset = (int64_t)(root_p_start - root_v_start);
    uart_puts("root_task loaded: pstart="); uart_puthex(root_p_start);
    uart_puts(" pend="); uart_puthex(root_p_end);
    uart_puts(" entry="); uart_puthex(root_v_entry);
    uart_puts(" pv_offset="); uart_puthex((uint64_t)pv_offset);
    uart_puts("\n");

    /* 4. Build the Sv39 bootstrap table */
    uart_puts("setting up page tables...\n");
    if (!setup_page_tables(kernel_elf)) {
        fatal("unsupported kernel ELF mapping");
    }

    /* 5. Enable Sv39 paging.  The loader keeps executing from its identity
     *    mapping, so no trampoline is needed across the satp write. */
    uart_puts("enabling Sv39 paging...\n");
    uint64_t satp = SATP_MODE_SV39 | ((uint64_t)(uintptr_t)boot_lvl1_pt >> 12);
    __asm__ volatile(
        "sfence.vma zero, zero\n"
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        "fence.i\n"
        :
        : "r"(satp)
        : "memory");
    uart_puts("paging enabled, jumping to seL4...\n");

    /*
     * 6. Jump to the seL4 kernel's virtual entry point.
     *
     * seL4's RISC-V _start sets gp/sp/sscratch and tail-calls init_kernel
     * with its arguments untouched, so the boot protocol is the same six
     * values AArch64 uses (confirmed against the SDK loader's
     * arch_jump_to_kernel and sel4.elf's disassembly):
     *   a0 = ui_p_reg_start, a1 = ui_p_reg_end, a2 = pv_offset,
     *   a3 = v_entry,        a4 = dtb_addr_p,   a5 = dtb_size
     *
     * We pass no DTB: this kernel build describes the platform statically
     * (CONFIG_PLAT_QEMU_RISCV_VIRT), and the SDK's own loader passes zeroes
     * here too.
     */
    register uint64_t a0 __asm__("a0") = root_p_start;
    register uint64_t a1 __asm__("a1") = root_p_end;
    register int64_t  a2 __asm__("a2") = pv_offset;
    register uint64_t a3 __asm__("a3") = root_v_entry;
    register uint64_t a4 __asm__("a4") = 0u;  /* no DTB      */
    register uint64_t a5 __asm__("a5") = 0u;  /* DTB size 0  */

    __asm__ volatile(
        "jr %[kentry]"
        :
        : [kentry] "r"(kernel_v_entry),
          "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "memory");

    __builtin_unreachable();
}
