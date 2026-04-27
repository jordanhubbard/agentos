/*
 * elf.h — minimal ELF64 types for the agentOS AArch64 loader
 *
 * Only the fields needed to load PT_LOAD segments are defined here.
 * No libc dependency.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ELF identification indices */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define ELFMAG0    0x7fu
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u

/* e_type values */
#define ET_EXEC 2u

/* e_machine values */
#define EM_AARCH64 0xB7u

/* p_type values */
#define PT_LOAD 1u

/* p_flags values */
#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint32_t Elf64_Word;
typedef uint16_t Elf64_Half;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16u

typedef struct {
    unsigned char  e_ident[EI_NIDENT]; /* ELF identification bytes */
    Elf64_Half     e_type;             /* Object file type */
    Elf64_Half     e_machine;          /* Architecture */
    Elf64_Word     e_version;          /* Object file version */
    Elf64_Addr     e_entry;            /* Entry point virtual address */
    Elf64_Off      e_phoff;            /* Program header table file offset */
    Elf64_Off      e_shoff;            /* Section header table file offset */
    Elf64_Word     e_flags;            /* Processor-specific flags */
    Elf64_Half     e_ehsize;           /* ELF header size in bytes */
    Elf64_Half     e_phentsize;        /* Program header table entry size */
    Elf64_Half     e_phnum;            /* Program header table entry count */
    Elf64_Half     e_shentsize;        /* Section header table entry size */
    Elf64_Half     e_shnum;            /* Section header table entry count */
    Elf64_Half     e_shstrndx;         /* Section name string table index */
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;    /* Segment type */
    Elf64_Word  p_flags;   /* Segment flags */
    Elf64_Off   p_offset;  /* Segment file offset */
    Elf64_Addr  p_vaddr;   /* Segment virtual address */
    Elf64_Addr  p_paddr;   /* Segment physical address */
    Elf64_Xword p_filesz;  /* Segment size in file */
    Elf64_Xword p_memsz;   /* Segment size in memory */
    Elf64_Xword p_align;   /* Segment alignment */
} __attribute__((packed)) Elf64_Phdr;
