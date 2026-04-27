/*
 * pd_vspace.c — VSpace creation and ELF loading for protection domains
 *
 * Creates an seL4 VSpace (RISC-V Sv39 or AArch64 4-level page-table hierarchy)
 * for a protection domain, loads an ELF64 image into it, and maps a stack and
 * IPC buffer.  Architecture-specific operations use seL4_ARCH_* aliases from
 * boot_info.h so this file compiles on RISC-V and AArch64 without #ifdefs.
 *
 * Three-phase flow:
 *   pd_vspace_create  — allocate root page table and assign ASID
 *   pd_vspace_load_elf — parse ELF64, map PT_LOAD segments, stack, IPC buffer
 *
 * ELF loading uses a scratch-VA window in the root task's own VSpace.
 * Each PD frame is temporarily mapped at SCRATCH_VA so ELF data can be
 * written before the frame is remapped into the PD VSpace at its final VA.
 *
 * AGENTOS_TEST_HOST: stubs return seL4_IllegalOperation so the host-side
 * test suite does not attempt real seL4 invocations.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "pd_vspace.h"
#include "ut_alloc.h"
#include "boot_info.h"
#include <stdint.h>

#ifndef AGENTOS_TEST_HOST

/* ── RISC-V Sv39 page size ───────────────────────────────────────────────── */

#define PAGE_SIZE         4096u
#define PAGE_MASK         ((seL4_Word)(PAGE_SIZE - 1u))
#define PAGE_ALIGN_DOWN(a) ((a) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(a)   (((a) + PAGE_MASK) & ~PAGE_MASK)

/* ── Virtual address layout for PDs ─────────────────────────────────────── */

/*
 * Sv39 user VA space: 0x0000_0000_0000_0000 to 0x0000_003F_FFFF_FFFF (256 GB).
 * We place the stack just below the top and the IPC buffer at a fixed low VA.
 */
#define PD_STACK_TOP_VA   0x0000003FFFFE0000UL  /* grows down from this address */
#define PD_IPC_BUF_VA_INT 0x0000000010000000UL  /* fixed IPC buffer VA in every PD */

/* ── Scratch VA for ELF loading in root task VSpace ─────────────────────── */

/*
 * We use a single 4 KB scratch slot at SCRATCH_VA to temporarily map each PD
 * frame so we can write ELF data into it before placing it in the PD VSpace.
 * The scratch VA is in the root task's own VSpace (seL4_CapInitThreadVSpace).
 *
 * Sv39 mapping for SCRATCH_VA = 0x0000_0010_0000_0000 (64 GB):
 *   VPN[2] = bits[38:30] = 4   → needs one L2 page table
 *   VPN[1] = bits[29:21] = 0   → needs one L1 page table
 *   VPN[0] = bits[20:12] = 0   → leaf page slot
 */
#define SCRATCH_VA        0x0000001000000000UL

static seL4_CPtr g_scratch_l2 = seL4_CapNull;
static seL4_CPtr g_scratch_l1 = seL4_CapNull;

/* ── ELF64 types ─────────────────────────────────────────────────────────── */

#define ELF_MAGIC0  0x7Fu
#define ELF_MAGIC1  0x45u  /* 'E' */
#define ELF_MAGIC2  0x4Cu  /* 'L' */
#define ELF_MAGIC3  0x46u  /* 'F' */
#define ELFCLASS64  2u
#define ET_EXEC     2u
#define PT_LOAD     1u

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_hdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

/* ── Helper: build an error result ──────────────────────────────────────── */

static pd_vspace_result_t error_result(int err)
{
    return (pd_vspace_result_t){
        .vspace_cap  = seL4_CapNull,
        .entry_point = 0u,
        .stack_top   = 0u,
        .ipc_buf_va  = 0u,
        .ipc_buf_cap = seL4_CapNull,
        .error       = err,
    };
}

/* ── Scratch VA initialisation ───────────────────────────────────────────── */

/*
 * scratch_init — install L2 and L1 page tables for SCRATCH_VA in the root
 * task's own VSpace so we can temporarily map 4 KB frames there.
 *
 * Idempotent: returns seL4_NoError if already initialised.
 */
static seL4_Error scratch_init(void)
{
    if (g_scratch_l2 != seL4_CapNull) {
        return seL4_NoError;  /* already done */
    }

    seL4_Error err;

    /* Allocate and install first intermediate page table for SCRATCH_VA */
    err = ut_alloc_cap(seL4_ARCH_IntermediatePTObject, 0u, &g_scratch_l2);
    if (err != seL4_NoError) {
        return err;
    }
    err = seL4_ARCH_PageTable_Map(g_scratch_l2,
                                   seL4_CapInitThreadVSpace,
                                   SCRATCH_VA,
                                   seL4_ARM_Default_VMAttributes);
    if (err != seL4_NoError) {
        return err;
    }

    /* Allocate and install second intermediate page table for SCRATCH_VA */
    err = ut_alloc_cap(seL4_ARCH_IntermediatePTObject, 0u, &g_scratch_l1);
    if (err != seL4_NoError) {
        return err;
    }
    err = seL4_ARCH_PageTable_Map(g_scratch_l1,
                                   seL4_CapInitThreadVSpace,
                                   SCRATCH_VA,
                                   seL4_ARM_Default_VMAttributes);
    return err;
}

/* ── Map a page with intermediate PT installation ────────────────────────── */

/*
 * map_page — map frame_cap at vaddr in vspace with up to 3 PT-install retries.
 *
 * On seL4_FailedLookup, allocates a new PageTable and retries.  For Sv39 at
 * most 2 intermediate tables are needed (L2 then L1), so 3 attempts suffice.
 */
static seL4_Error map_page(seL4_CPtr frame_cap,
                            seL4_CPtr vspace,
                            seL4_Word vaddr,
                            seL4_CapRights_t rights,
                            seL4_ARCH_VMAttributes attr)
{
    seL4_Error err;

    /* Up to 4 retries: RISC-V Sv39 needs 2 intermediate tables; AArch64 needs 3. */
    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        err = seL4_ARCH_Page_Map(frame_cap, vspace, vaddr, rights, attr);
        if (err == seL4_NoError) {
            return seL4_NoError;
        }
        if (err != seL4_FailedLookup) {
            return err;
        }

        /* Install the missing intermediate page table and retry */
        seL4_CPtr pt;
        err = ut_alloc_cap(seL4_ARCH_IntermediatePTObject, 0u, &pt);
        if (err != seL4_NoError) {
            return err;
        }
        err = seL4_ARCH_PageTable_Map(pt, vspace, vaddr,
                                       seL4_ARM_Default_VMAttributes);
        if (err != seL4_NoError) {
            return err;
        }
    }

    return seL4_NotEnoughMemory; /* exhausted retries */
}

/* ── Map a large anonymous RAM region into a PD VSpace ───────────────────── */

/*
 * pd_vspace_map_region — allocate and map a large RAM region using 2 MB pages.
 *
 * Both va_start and size must be 2 MB-aligned.  Frame caps are retained in
 * the root task's CNode (via ut_alloc_cap) to keep the mappings alive.
 * No zeroing is performed — the caller (or the PD itself) is responsible.
 */
seL4_Error pd_vspace_map_region(seL4_CPtr vspace,
                                 seL4_Word  va_start,
                                 size_t     size,
                                 int        writable)
{
    const seL4_Word LARGE_PAGE = (1UL << seL4_ARCH_LargePageBits);  /* 2 MB */
    seL4_CapRights_t rights = writable ? seL4_AllRights : seL4_CanRead;

    for (seL4_Word va = va_start; va < va_start + (seL4_Word)size; va += LARGE_PAGE) {
        seL4_CPtr frame;
        seL4_Error err = ut_alloc_cap((uint32_t)seL4_ARCH_LargePageObject, 0u, &frame);
        if (err != seL4_NoError) {
            return err;
        }
        /* map_page handles FailedLookup by installing missing intermediate PTs.
         * For 2 MB pages: at most 2 PT installs needed (L1 then L2 on AArch64). */
        err = map_page(frame, vspace, va, rights, seL4_ARM_Default_VMAttributes);
        if (err != seL4_NoError) {
            return err;
        }
    }
    return seL4_NoError;
}

/* ── Zero + write one page via scratch VA, then map into PD VSpace ────────── */

/*
 * load_page — allocate a 4 KB frame, write ELF data into it via the scratch
 * VA, then map the frame into the PD VSpace at pd_vaddr.
 *
 * Parameters:
 *   vspace      PD VSpace capability
 *   pd_vaddr    target virtual address in the PD (must be page-aligned)
 *   src         pointer to ELF file data to copy into this page (may be NULL)
 *   src_off     byte offset within this page where src data begins (0 typically)
 *   src_bytes   number of bytes to copy from src (rest of page is zeroed)
 */
static seL4_Error load_page(seL4_CPtr    vspace,
                              seL4_Word    pd_vaddr,
                              const uint8_t *src,
                              seL4_Word    src_off,
                              seL4_Word    src_bytes)
{
    /* Allocate a fresh 4 KB frame */
    seL4_CPtr frame;
    seL4_Error err = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &frame);
    if (err != seL4_NoError) {
        return err;
    }

    /* Map at scratch VA in the root task's VSpace for writing */
    err = seL4_ARCH_Page_Map(frame,
                              seL4_CapInitThreadVSpace,
                              SCRATCH_VA,
                              seL4_AllRights,
                              seL4_ARM_Default_VMAttributes);
    if (err != seL4_NoError) {
        return err;
    }

    /* Zero the page then copy ELF data */
    volatile uint8_t *dst = (volatile uint8_t *)SCRATCH_VA;
    for (seL4_Word i = 0u; i < PAGE_SIZE; i++) {
        dst[i] = 0u;
    }
    if (src != (const uint8_t *)0 && src_bytes > 0u) {
        seL4_Word copy_end = src_off + src_bytes;
        if (copy_end > PAGE_SIZE) {
            copy_end = PAGE_SIZE;
        }
        for (seL4_Word i = src_off; i < copy_end; i++) {
            dst[i] = src[i - src_off];
        }
    }

    /* Ensure writes are visible before unmapping */
    AGENTOS_MEMORY_FENCE();

    /* Unmap from root task VSpace */
    seL4_ARCH_Page_Unmap(frame);

    /* Map into PD VSpace at the final virtual address */
    err = map_page(frame, vspace, pd_vaddr,
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    return err;
}

/* ── ELF64 validation ─────────────────────────────────────────────────────── */

static int elf64_valid(const elf64_hdr_t *h)
{
    return h->e_ident[0] == ELF_MAGIC0 &&
           h->e_ident[1] == ELF_MAGIC1 &&
           h->e_ident[2] == ELF_MAGIC2 &&
           h->e_ident[3] == ELF_MAGIC3 &&
           h->e_ident[4] == ELFCLASS64 &&
           h->e_type      == ET_EXEC;
}

/* ── ELF segment loading ─────────────────────────────────────────────────── */

/*
 * load_elf_segments — parse PT_LOAD segments and map them into vspace.
 *
 * Processes all PT_LOAD segments page-by-page across their combined VA range.
 * Each unique page is allocated once; all segments that contribute bytes to
 * that page are merged into it.  This correctly handles adjacent or overlapping
 * PT_LOAD segments that share a page boundary (e.g. GOT at 0x428200 and data
 * at 0x428210 both requiring page 0x428000).
 *
 * Sets *entry_out to the ELF entry point virtual address.
 */
static seL4_Error load_elf_segments(seL4_CPtr vspace,
                                     const uint8_t *elf_base,
                                     seL4_Word *entry_out)
{
    const elf64_hdr_t *ehdr = (const elf64_hdr_t *)elf_base;

    if (!elf64_valid(ehdr)) {
        return seL4_InvalidArgument;
    }

    *entry_out = (seL4_Word)ehdr->e_entry;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(elf_base + ehdr->e_phoff);

    /* Find the combined page-aligned VA range of all PT_LOAD segments. */
    seL4_Word va_min = ~(seL4_Word)0u;
    seL4_Word va_max = 0u;
    for (uint16_t i = 0u; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0u) {
            continue;
        }
        seL4_Word s = PAGE_ALIGN_DOWN((seL4_Word)phdrs[i].p_vaddr);
        seL4_Word e = PAGE_ALIGN_UP((seL4_Word)phdrs[i].p_vaddr + (seL4_Word)phdrs[i].p_memsz);
        if (s < va_min) { va_min = s; }
        if (e > va_max) { va_max = e; }
    }

    if (va_min == ~(seL4_Word)0u) {
        return seL4_NoError;  /* no PT_LOAD segments — nothing to do */
    }

    /*
     * For each page in [va_min, va_max), allocate one frame, zero it, copy
     * in all file bytes from every PT_LOAD segment that covers this page,
     * then map the frame into the PD VSpace.  Processing the full VA range in
     * a single pass ensures overlapping segments share one frame per page.
     */
    for (seL4_Word va = va_min; va < va_max; va += PAGE_SIZE) {
        seL4_CPtr frame;
        seL4_Error err = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &frame);
        if (err != seL4_NoError) {
            return err;
        }

        err = seL4_ARCH_Page_Map(frame,
                                  seL4_CapInitThreadVSpace,
                                  SCRATCH_VA,
                                  seL4_AllRights,
                                  seL4_ARM_Default_VMAttributes);
        if (err != seL4_NoError) {
            return err;
        }

        volatile uint8_t *dst = (volatile uint8_t *)SCRATCH_VA;
        for (seL4_Word b = 0u; b < PAGE_SIZE; b++) {
            dst[b] = 0u;
        }

        /* Merge file bytes from every segment that overlaps this page. */
        for (uint16_t si = 0u; si < ehdr->e_phnum; si++) {
            if (phdrs[si].p_type != PT_LOAD || phdrs[si].p_memsz == 0u) {
                continue;
            }
            seL4_Word seg_va     = (seL4_Word)phdrs[si].p_vaddr;
            seL4_Word seg_filesz = (seL4_Word)phdrs[si].p_filesz;
            seL4_Word seg_off    = (seL4_Word)phdrs[si].p_offset;
            seL4_Word seg_end    = seg_va + (seL4_Word)phdrs[si].p_memsz;
            seL4_Word page_end   = va + PAGE_SIZE;

            if (seg_va >= page_end || seg_end <= va) {
                continue;  /* segment doesn't touch this page */
            }

            /* File data range within this page. */
            seL4_Word file_va_start = (seg_va > va) ? seg_va : va;
            seL4_Word file_va_end   = seg_va + seg_filesz;
            if (file_va_end > page_end) { file_va_end = page_end; }

            for (seL4_Word bva = file_va_start; bva < file_va_end; bva++) {
                dst[bva - va] = elf_base[seg_off + (bva - seg_va)];
            }
        }

        AGENTOS_MEMORY_FENCE();
        seL4_ARCH_Page_Unmap(frame);

        err = map_page(frame, vspace, va,
                       seL4_AllRights,
                       seL4_ARM_Default_VMAttributes);
        if (err != seL4_NoError) {
            return err;
        }
    }

    return seL4_NoError;
}

/* ── Map a zero-filled page (stack / IPC buffer) ─────────────────────────── */

static seL4_Error map_zero_page(seL4_CPtr vspace, seL4_Word vaddr)
{
    return load_page(vspace, vaddr, (const uint8_t *)0, 0u, 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

pd_vspace_result_t pd_vspace_create(seL4_CPtr pd_cnode,
                                     seL4_CPtr asid_pool)
{
    (void)pd_cnode;  /* VSpace cap lands in root task's CNode via ut_alloc_cap */

    /* Ensure scratch page tables are installed in the root task's VSpace */
    seL4_Error err = scratch_init();
    if (err != seL4_NoError) {
        return error_result((int)err);
    }

    /* Allocate root page table for this PD's VSpace */
    seL4_CPtr vspace;
    err = ut_alloc_cap(seL4_ARM_VSpaceObject, 0u, &vspace);
    if (err != seL4_NoError) {
        return error_result((int)err);
    }

    /* Assign an ASID so the kernel can flush TLB entries for this VSpace */
    err = seL4_ARCH_ASIDPool_Assign(asid_pool, vspace);
    if (err != seL4_NoError) {
        return error_result((int)err);
    }

    return (pd_vspace_result_t){
        .vspace_cap  = vspace,
        .entry_point = 0u,
        .stack_top   = 0u,
        .ipc_buf_va  = 0u,
        .ipc_buf_cap = seL4_CapNull,
        .error       = (int)seL4_NoError,
    };
}

pd_vspace_result_t pd_vspace_load_elf(seL4_CPtr    vspace_cap,
                                       const void  *elf_base,
                                       uint32_t     elf_size,
                                       uint32_t     stack_size)
{
    if (!elf_base || elf_size < sizeof(elf64_hdr_t)) {
        return error_result((int)seL4_InvalidArgument);
    }

    /* ── Load ELF PT_LOAD segments ──────────────────────────────────────── */
    seL4_Word entry_point;
    seL4_Error err = load_elf_segments(vspace_cap,
                                        (const uint8_t *)elf_base,
                                        &entry_point);
    if (err != seL4_NoError) {
        return error_result((int)err);
    }

    /* ── Map stack (grows down from PD_STACK_TOP_VA) ────────────────────── */
    seL4_Word stack_top  = PD_STACK_TOP_VA;
    seL4_Word stack_base = stack_top - (seL4_Word)stack_size;
    for (seL4_Word va = stack_base; va < stack_top; va += PAGE_SIZE) {
        err = map_zero_page(vspace_cap, va);
        if (err != seL4_NoError) {
            return error_result((int)err);
        }
    }

    /* ── Map IPC buffer page ─────────────────────────────────────────────── */
    seL4_Word ipc_va = (seL4_Word)PD_IPC_BUF_VA_INT;
    seL4_CPtr ipc_frame;
    err = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &ipc_frame);
    if (err != seL4_NoError) {
        return error_result((int)err);
    }
    err = map_page(ipc_frame, vspace_cap, ipc_va,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err != seL4_NoError) {
        return error_result((int)err);
    }

    return (pd_vspace_result_t){
        .vspace_cap  = vspace_cap,
        .entry_point = entry_point,
        .stack_top   = stack_top,
        .ipc_buf_va  = ipc_va,
        .ipc_buf_cap = ipc_frame,
        .error       = (int)seL4_NoError,
    };
}

seL4_Error pd_vspace_map_device_frame(seL4_CPtr vspace,
                                       seL4_CPtr frame_cap,
                                       seL4_Word vaddr)
{
    return map_page(frame_cap, vspace, vaddr,
                    seL4_AllRights,
                    seL4_ARM_Default_VMAttributes);
}

#else /* AGENTOS_TEST_HOST ───────────────────────────────────────────────── */

pd_vspace_result_t pd_vspace_create(seL4_CPtr pd_cnode, seL4_CPtr asid_pool)
{
    (void)pd_cnode;
    (void)asid_pool;
    return (pd_vspace_result_t){
        .vspace_cap  = seL4_CapNull,
        .entry_point = 0u,
        .stack_top   = 0u,
        .ipc_buf_va  = 0u,
        .ipc_buf_cap = seL4_CapNull,
        .error       = (int)seL4_IllegalOperation,
    };
}

pd_vspace_result_t pd_vspace_load_elf(seL4_CPtr    vspace_cap,
                                       const void  *elf_base,
                                       uint32_t     elf_size,
                                       uint32_t     stack_size)
{
    (void)vspace_cap;
    (void)elf_base;
    (void)elf_size;
    (void)stack_size;
    return (pd_vspace_result_t){
        .vspace_cap  = seL4_CapNull,
        .entry_point = 0u,
        .stack_top   = 0u,
        .ipc_buf_va  = 0u,
        .ipc_buf_cap = seL4_CapNull,
        .error       = (int)seL4_IllegalOperation,
    };
}

seL4_Error pd_vspace_map_device_frame(seL4_CPtr vspace,
                                       seL4_CPtr frame_cap,
                                       seL4_Word vaddr)
{
    (void)vspace;
    (void)frame_cap;
    (void)vaddr;
    return seL4_IllegalOperation;
}

#endif /* AGENTOS_TEST_HOST */
