/*
 * boot_info.h — seL4 BootInfo structure and supplementary types
 *
 * Extends sel4_boot.h with the seL4_BootInfo structure (received by the
 * root task at startup), seL4 object type constants not already defined
 * in sel4_boot.h, and the seL4_Yield() scheduler primitive.
 *
 * Production builds: sel4_boot.h already includes <sel4/sel4.h>, which
 * provides seL4_Yield, seL4_BootInfo, seL4_SlotRegion, seL4_UntypedDesc,
 * and seL4_BootInfoHeader.  This header only adds the supplementary
 * constants and agentOS-specific extensions in that case.
 *
 * Test-host builds (AGENTOS_TEST_HOST): no seL4 SDK available; this header
 * provides all required type stubs.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"
#include <stdint.h>

/* ── Supplementary constants — split by build path ───────────────────────── */

#ifdef AGENTOS_TEST_HOST

/* Test-host stubs: provide numeric constants matching seL4 non-MCS values.
 * seL4 non-MCS object type enum: Untyped=0, TCB=1, EP=2, Notif=3, CapTable=4
 * seL4 RISC-V arch types: Giga=5, 4K_Page=6, MegaPage=7, PageTable=8        */
#define seL4_CapTableObject           4u
#define seL4_ARM_VSpaceObject         8u   /* maps to PageTableObject for test */
#define seL4_ARM_SmallPageObject      6u   /* maps to 4K_Page for test         */
#define seL4_ARM_LargePageObject      7u   /* maps to MegaPage for test        */
#define seL4_ARM_Default_VMAttributes 0u
#define seL4_ReadWrite                3u
#define seL4_NumInitialCaps           16u  /* matches seL4_RootCNodeCapSlots   */

/* Architecture-neutral large page (2 MB) alias — test host */
#define seL4_ARCH_LargePageObject  seL4_ARM_LargePageObject
#define seL4_ARCH_LargePageBits    21u   /* log2(2 MB) */

/* seL4_Yield — host-side no-op */
static inline void seL4_Yield(void) { /* no-op */ }

/* seL4 BootInfo types for test builds */

typedef seL4_Word seL4_SlotPos;

typedef struct {
    seL4_SlotPos start; /* first CNode slot of region (inclusive) */
    seL4_SlotPos end;   /* first CNode slot after region (exclusive) */
} seL4_SlotRegion;

typedef struct {
    seL4_Word  paddr;    /* physical base address of the untyped region */
    uint8_t    sizeBits; /* log2(size in bytes) of the untyped cap */
    uint8_t    isDevice; /* non-zero if this is device memory */
    uint8_t    padding[sizeof(seL4_Word) - 2 * sizeof(uint8_t)];
} seL4_UntypedDesc;

#define SEL4_BOOTINFO_MAX_UNTYPED  230u

typedef struct {
    seL4_Word          extraLen;
    seL4_Word          nodeID;
    seL4_Word          numNodes;
    seL4_Word          numIOPTLevels;
    void              *ipcBuffer;
    seL4_SlotRegion    empty;
    seL4_SlotRegion    sharedFrames;
    seL4_SlotRegion    userImageFrames;
    seL4_SlotRegion    userImagePaging;
    seL4_SlotRegion    ioSpaceCaps;
    seL4_SlotRegion    extraBIPages;
    seL4_Word          initThreadCNodeSizeBits;
    seL4_Word          initThreadDomain;
    seL4_SlotRegion    untyped;
    seL4_UntypedDesc   untypedList[SEL4_BOOTINFO_MAX_UNTYPED];
} seL4_BootInfo;

typedef struct {
    seL4_Word id;  /* chunk type identifier */
    seL4_Word len; /* total length of this chunk, including this header */
} seL4_BootInfoHeader;

#else /* Production build ─────────────────────────────────────────────────── */

/*
 * Architecture-neutral aliases — object types and VSpace mapping API.
 *
 * seL4_ARM_* names are used throughout the codebase.  On RISC-V they do not
 * exist in the SDK; we provide aliases pointing to the correct RISC-V types.
 * On AArch64 the SDK already defines seL4_ARM_VSpaceObject/SmallPageObject.
 *
 * seL4_ARCH_* names abstract the actual seL4 invocations that differ between
 * architectures (Page_Map, PageTable_Map, ASIDPool_Assign, etc.) so
 * pd_vspace.c can compile on both RISC-V and AArch64 without #ifdefs.
 */
#  if defined(__riscv)
#    define seL4_ARM_VSpaceObject         seL4_RISCV_PageTableObject
#    define seL4_ARM_SmallPageObject      seL4_RISCV_4K_Page
#    define seL4_ARM_LargePageObject      seL4_RISCV_Mega_Page
#    define seL4_ARM_Default_VMAttributes seL4_RISCV_Default_VMAttributes
/* Intermediate page table object type (same as VSpace root on RISC-V) */
#    define seL4_ARCH_IntermediatePTObject seL4_RISCV_PageTableObject
/* Large page (2 MB / 1 GiB level) */
#    define seL4_ARCH_LargePageObject     seL4_RISCV_Mega_Page
#    define seL4_ARCH_LargePageBits       21u   /* log2(2 MB) */
/* VSpace API wrappers */
#    define seL4_ARCH_VMAttributes          seL4_RISCV_VMAttributes
#    define seL4_ARCH_Page_Map(f,v,a,r,at)  seL4_RISCV_Page_Map((f),(v),(a),(r),(at))
#    define seL4_ARCH_Page_Unmap(f)         seL4_RISCV_Page_Unmap(f)
#    define seL4_ARCH_PageTable_Map(p,v,a,at) seL4_RISCV_PageTable_Map((p),(v),(a),(at))
#    define seL4_ARCH_ASIDPool_Assign(pool,vs) seL4_RISCV_ASIDPool_Assign((pool),(vs))
#    define AGENTOS_MEMORY_FENCE() __asm__ volatile ("fence rw,rw" ::: "memory")
#  elif defined(__aarch64__)
/* Intermediate page table object type (separate from VSpace root on AArch64) */
#    define seL4_ARCH_IntermediatePTObject seL4_ARM_PageTableObject
/* Large page (2 MB block entry at L2) */
#    define seL4_ARCH_LargePageObject     seL4_ARM_LargePageObject
#    define seL4_ARCH_LargePageBits       21u   /* log2(2 MB) */
/* VSpace API wrappers */
#    define seL4_ARCH_VMAttributes          seL4_ARM_VMAttributes
#    define seL4_ARCH_Page_Map(f,v,a,r,at)  seL4_ARM_Page_Map((f),(v),(a),(r),(at))
#    define seL4_ARCH_Page_Unmap(f)         seL4_ARM_Page_Unmap(f)
#    define seL4_ARCH_PageTable_Map(p,v,a,at) seL4_ARM_PageTable_Map((p),(v),(a),(at))
#    define seL4_ARCH_ASIDPool_Assign(pool,vs) seL4_ARM_ASIDPool_Assign((pool),(vs))
#    define AGENTOS_MEMORY_FENCE() __asm__ volatile ("dsb sy" ::: "memory")
#  endif

/* seL4_ReadWrite — capability rights alias.  The seL4 SDK provides
 * seL4_AllRights which grants R+W+G+GR.  seL4_ReadWrite is used in some
 * legacy paths; alias to seL4_AllRights for simplicity. */
#  ifndef seL4_ReadWrite
#    define seL4_ReadWrite seL4_AllRights
#  endif

#endif /* AGENTOS_TEST_HOST */

/* ── seL4 extra boot-info chunk header IDs ────────────────────────────────── */

#define SEL4_BOOTINFO_HEADER_PADDING  0u
#define SEL4_BOOTINFO_HEADER_FDT      6u
#define AGENTOS_BOOTINFO_HEADER_ELF   0x4147u  /* 'AG' */

/* ── ELF region descriptor (agentOS extension) ────────────────────────────── */

/*
 * agentos_elf_region_t — describes one embedded PD ELF in the extra BootInfo.
 *
 * The loader (xtask gen-image) appends one of these immediately before each
 * embedded ELF blob.  boot_find_elf() in main.c walks the extra region list
 * to locate ELFs by name.
 */
typedef struct {
    char      pd_name[32];
    seL4_Word elf_offset;  /* byte offset of ELF data from start of extra BI */
    seL4_Word elf_size;    /* byte length of ELF image */
} agentos_elf_region_t;
