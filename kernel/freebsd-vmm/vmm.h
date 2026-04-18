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
 *
 * Ring-1 consumer model (Phase 3c):
 *   freebsd_vmm holds NO direct hardware capabilities.  All device I/O is
 *   mediated through ring-0 service PDs (serial_pd, net_pd, block_pd) via
 *   the guest_contract.h and vmm_contract.h IPC protocols.
 *   MMIO faults from guest VMs are intercepted and forwarded to service PDs
 *   rather than being escalated to EL2.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "contracts/guest_contract.h"
#include "contracts/vmm_contract.h"

/* ─── Guest physical memory layout ──────────────────────────────────────── */

#define GUEST_RAM_VADDR         0x40000000UL   /* 2GB guest RAM base */
#define GUEST_RAM_SIZE          0x80000000UL   /* 2GB */
#define GUEST_FLASH_PADDR       0x00000000UL   /* EDK2 UEFI firmware (64MB) */
#define GUEST_FLASH_SIZE        0x04000000UL   /* 64MB */

/* EDK2 reset vector — UEFI firmware entry point */
#define UEFI_RESET_VECTOR       0x00000000UL

/* DTB passed to EDK2/FreeBSD (placed at end of flash region) */
#define GUEST_DTB_PADDR         0x03f00000UL

/* ─── Guest-physical MMIO addresses (emulated — NOT direct hardware) ──────
 *
 * These addresses appear in the guest's IPA space.  The guest accesses them
 * via normal load/store instructions; the VMM intercepts the resulting MMIO
 * faults and forwards I/O to the appropriate ring-0 service PD.
 *
 * NO memory_region in the .system file maps these to physical hardware.
 */

/* PL011 UART — guest console, emulated via serial_pd IPC */
#define GUEST_UART_PADDR        0x09000000UL
#define GUEST_UART_SIZE         0x00001000UL

/* PL011 register offsets used by the UART emulation handler */
#define UART_DR_OFF    0x000u   /* Data Register: TX/RX byte */
#define UART_RSR_OFF   0x004u   /* Receive Status / Error Clear */
#define UART_FR_OFF    0x018u   /* Flag Register (RX/TX full/empty/busy) */
#define UART_IBRD_OFF  0x024u   /* Integer Baud Rate Divisor */
#define UART_FBRD_OFF  0x028u   /* Fractional Baud Rate Divisor */
#define UART_LCR_H_OFF 0x02Cu   /* Line Control Register */
#define UART_CR_OFF    0x030u   /* Control Register */
#define UART_IMSC_OFF  0x038u   /* Interrupt Mask Set/Clear */
#define UART_MIS_OFF   0x040u   /* Masked Interrupt Status */
#define UART_ICR_OFF   0x044u   /* Interrupt Clear */

/* FR register bits — returned by UART emulation reads */
#define UART_FR_TXFF  (1u << 5)  /* TX FIFO full */
#define UART_FR_RXFE  (1u << 4)  /* RX FIFO empty */
#define UART_FR_BUSY  (1u << 3)  /* UART busy */

/* GIC vCPU interface (mapped directly in each guest; no emulation needed) */
#define GUEST_GIC_DIST_PADDR    0x08000000UL
#define GUEST_GIC_DIST_SIZE     0x00010000UL
#define GUEST_GIC_VCPU_PADDR    0x08040000UL   /* vCPU interface (host PA) */
#define GUEST_GIC_CPU_PADDR     0x08010000UL   /* where guest expects CPU i/f */
#define GUEST_GIC_VCPU_SIZE     0x00001000UL

/* VirtIO MMIO — emulated via block_pd / net_pd IPC.
 *
 * Layout: each VM slot N gets three consecutive 4KB MMIO windows:
 *   Block: GUEST_VIRTIO_BLK_BASE + N * GUEST_VIRTIO_STRIDE
 *   Net:   GUEST_VIRTIO_NET_BASE + N * GUEST_VIRTIO_STRIDE
 *
 * These addresses must match the guest DTB.
 */
#define GUEST_VIRTIO_SIZE           0x00001000UL    /* 4KB per device */
#define GUEST_VIRTIO_BLK_BASE       0x0a000000UL    /* block slot 0 */
#define GUEST_VIRTIO_NET_BASE       0x0a004000UL    /* net slot 0 */
#define GUEST_VIRTIO_BLK_PADDR(n)  (GUEST_VIRTIO_BLK_BASE + (n) * GUEST_VIRTIO_SIZE)
#define GUEST_VIRTIO_NET_PADDR(n)  (GUEST_VIRTIO_NET_BASE  + (n) * GUEST_VIRTIO_SIZE)

/* ─── freebsd_vmm local Microkit channel IDs ─────────────────────────────
 *
 * These are the channel IDs as seen from freebsd_vmm's perspective.
 * They must match the <end pd="freebsd_vmm" id="N" /> entries in the
 * agentos-freebsd.system manifest.
 */
#define CH_VMM_CONTROLLER_PPC   0u   /* controller → freebsd_vmm (pp=true) */
#define CH_VMM_CONTROLLER_EVT   1u   /* freebsd_vmm → controller (notify) */
#define CH_VMM_SERIAL           2u   /* freebsd_vmm ↔ serial_pd (pp=true) */
#define CH_VMM_NET              3u   /* freebsd_vmm ↔ net_pd (pp=true) */
#define CH_VMM_BLOCK            4u   /* freebsd_vmm ↔ block_pd (pp=true) */
#define CH_VMM_KERNEL_LOCAL     5u   /* freebsd_vmm → root-task (MSG_VMM_*) */

/* ─── Shared memory vaddrs (set by Microkit linker) ─────────────────────── */

extern uintptr_t vmm_serial_shmem_vaddr;  /* serial_pd ↔ freebsd_vmm data buffer */
extern uintptr_t vmm_block_shmem_vaddr;   /* block_pd  ↔ freebsd_vmm data buffer */
extern uintptr_t vmm_net_shmem_vaddr;     /* net_pd    ↔ freebsd_vmm data buffer */

/* ─── Firmware flash vaddrs (set by Microkit linker) ────────────────────── */

extern uintptr_t guest_flash_vaddr;

extern uintptr_t guest_ram_vaddr_0;
extern uintptr_t guest_ram_vaddr_1;
extern uintptr_t guest_ram_vaddr_2;
extern uintptr_t guest_ram_vaddr_3;

/* ─── Embedded firmware blobs (linked via objcopy) ───────────────────────── */

extern char _binary_edk2_aarch64_code_fd_start[];
extern char _binary_edk2_aarch64_code_fd_end[];
extern char _binary_freebsd_dtb_start[];
extern char _binary_freebsd_dtb_end[];

/* ─── VMM entry points ──────────────────────────────────────────────────── */

void vmm_init(void);
void vmm_notified(microkit_channel ch);
microkit_msginfo vmm_protected(microkit_channel ch, microkit_msginfo msginfo);
