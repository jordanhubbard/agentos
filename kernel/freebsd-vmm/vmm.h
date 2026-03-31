/*
 * agentOS FreeBSD VMM
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM Protection Domain for running FreeBSD AArch64 as a seL4 Microkit
 * virtual machine guest. Uses libvmm (au-ts/libvmm) for vCPU management,
 * GICv3 virtualisation, and VirtIO device emulation.
 *
 * Boot sequence:
 *   seL4 (EL2) → freebsd_vmm PD → EDK2 UEFI reset vector (0x00000000)
 *   → EDK2 scans VirtIO block → bootaa64.efi → loader.efi → FreeBSD kernel
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Guest memory layout (guest physical addresses) */
#define GUEST_RAM_VADDR         0x40000000UL   /* 2GB guest RAM base */
#define GUEST_RAM_SIZE          0x80000000UL   /* 2GB */
#define GUEST_FLASH_PADDR       0x00000000UL   /* EDK2 UEFI firmware (64MB) */
#define GUEST_FLASH_SIZE        0x04000000UL   /* 64MB */

/* Platform MMIO (QEMU virt AArch64) */
#define GUEST_UART_PADDR        0x09000000UL
#define GUEST_UART_SIZE         0x00001000UL
#define GUEST_GIC_DIST_PADDR    0x08000000UL
#define GUEST_GIC_DIST_SIZE     0x00010000UL
#define GUEST_GIC_VCPU_PADDR    0x08040000UL   /* vCPU interface (mapped as CPU i/f) */
#define GUEST_GIC_CPU_PADDR     0x08010000UL   /* where guest expects CPU interface */
#define GUEST_GIC_VCPU_SIZE     0x00001000UL

/* VirtIO MMIO (QEMU virt AArch64 default) */
#define GUEST_VIRTIO_BLK_PADDR  0x0a003000UL  /* VirtIO block device base */
#define GUEST_VIRTIO_CON_PADDR  0x0a004000UL  /* VirtIO console */
#define GUEST_VIRTIO_SIZE       0x00001000UL

/* EDK2 reset vector — UEFI firmware entry point */
#define UEFI_RESET_VECTOR       0x00000000UL

/* DTB passed to EDK2/FreeBSD (placed at end of flash region) */
#define GUEST_DTB_PADDR         0x03f00000UL

/* agentOS event_bus channel for VMM status events */
#define CH_CONTROLLER           70

/* VMM state */
typedef struct {
    uintptr_t guest_ram_vaddr;   /* VMM's VA for guest RAM (set by Microkit) */
    uintptr_t guest_flash_vaddr; /* VMM's VA for UEFI flash */
    bool      guest_running;
    uint32_t  vcpu_id;
} freebsd_vmm_t;

/* External symbols set by Microkit linker */
extern uintptr_t guest_ram_vaddr;
extern uintptr_t guest_flash_vaddr;

/* Embedded binary blobs (linked via objcopy) */
extern char _binary_edk2_aarch64_code_fd_start[];
extern char _binary_edk2_aarch64_code_fd_end[];
extern char _binary_freebsd_dtb_start[];
extern char _binary_freebsd_dtb_end[];

/* VMM entry points */
void vmm_init(void);
void vmm_notified(microkit_channel ch);
microkit_msginfo vmm_protected(microkit_channel ch, microkit_msginfo msginfo);
