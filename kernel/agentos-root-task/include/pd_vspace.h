/*
 * pd_vspace.h — VSpace (virtual address space) creation for protection domains
 *
 * Creates and configures the page-table hierarchy for a protection domain,
 * loads an ELF image into it, and returns the entry point and initial stack
 * pointer required to start the PD's thread.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"
#include <stdint.h>
#include <stddef.h>

/*
 * pd_vspace_result_t — result returned by pd_vspace_create and pd_vspace_load_elf.
 *
 * Fields:
 *   vspace_cap    seL4 VSpace (ASID-bound page directory) capability
 *   entry_point   virtual address of the ELF entry symbol (_start)
 *   stack_top     virtual address of the top of the initial stack region
 *                 (the TCB stack pointer should be initialised to this value)
 *   ipc_buf_va    virtual address of the IPC buffer page mapped in this VSpace
 *   ipc_buf_cap   frame capability for the IPC buffer page
 *   error         0 on success, non-zero seL4_Error on failure
 */
typedef struct {
    seL4_CPtr  vspace_cap;
    seL4_Word  entry_point;
    seL4_Word  stack_top;
    seL4_Word  ipc_buf_va;
    seL4_CPtr  ipc_buf_cap;
    int        error;
} pd_vspace_result_t;

/*
 * pd_vspace_create — allocate a new VSpace for a protection domain.
 *
 * Retypes untyped memory into the paging structures required by the current
 * architecture (PageDirectory + PageTable on AArch64 / RISC-V / x86-64) and
 * binds the VSpace to an ASID pool.
 *
 * Parameters:
 *   pd_cnode    the PD's own CNode (capabilities for internal pages go here)
 *   asid_pool   ASID pool capability used to assign a hardware ASID
 *
 * Returns pd_vspace_result_t.  Only vspace_cap and error are valid after
 * pd_vspace_create; the remaining fields are populated by pd_vspace_load_elf.
 */
pd_vspace_result_t pd_vspace_create(seL4_CPtr pd_cnode,
                                     seL4_CPtr asid_pool);

/*
 * pd_vspace_load_elf — load an ELF image into a VSpace and map a stack.
 *
 * Maps each PT_LOAD segment at its specified virtual address, allocates a
 * stack region, and maps an IPC buffer page.
 *
 * Parameters:
 *   vspace_cap   VSpace capability returned by pd_vspace_create
 *   elf_base     pointer to the start of the ELF image in the root task's
 *                own address space (read-only; the image is not modified)
 *   elf_size     size of the ELF image in bytes
 *   stack_size   desired stack size in bytes (must be a multiple of 4096)
 *
 * Returns pd_vspace_result_t with all fields populated on success.
 * On failure, pd_vspace_result_t.error is non-zero.
 */
pd_vspace_result_t pd_vspace_load_elf(seL4_CPtr    vspace_cap,
                                       const void  *elf_base,
                                       uint32_t     elf_size,
                                       uint32_t     stack_size);

/*
 * pd_vspace_map_region — allocate and map an anonymous RAM region into a VSpace.
 *
 * Maps [va_start, va_start + size) into vspace using 2 MB large pages.
 * Both va_start and size must be 2 MB-aligned (2097152 byte multiples).
 * Frame capabilities are retained in the root task's CNode to maintain the
 * mappings; they are never returned to the caller.
 *
 * This is used by the root task to give PDs like linux_vmm large private RAM
 * regions (e.g. 256 MB of guest RAM at 0x40000000 on AArch64).
 *
 * Parameters:
 *   vspace    VSpace capability for the target PD
 *   va_start  first virtual address of the region (2 MB aligned)
 *   size      size in bytes (2 MB aligned; must be ≥ 2 MB)
 *   writable  1 = R/W mapping, 0 = R/O mapping
 *
 * Returns seL4_NoError on success, or an seL4 error code on failure.
 */
seL4_Error pd_vspace_map_region(seL4_CPtr vspace,
                                 seL4_Word  va_start,
                                 size_t     size,
                                 int        writable);
