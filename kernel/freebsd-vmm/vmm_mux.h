/*
 * agentOS VM Multiplexer
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Manages a pool of VM slots, each capable of running an independent
 * FreeBSD (or other AArch64) guest. The multiplexer supports:
 *
 *   - Creating VM instances (allocate slot, assign RAM region, boot)
 *   - Destroying VM instances (halt guest, free slot)
 *   - Switching the active console between instances
 *   - Querying status of all slots
 *
 * IPC operations (via controller PPC on CH_CONTROLLER):
 *
 *   OP_VM_CREATE  (0x10) — allocate a new VM slot and boot it
 *     mr[0] = 0           → allocate from pool
 *     returns mr[0] = slot_id (0..VM_MAX_SLOTS-1), or 0xFF on failure
 *
 *   OP_VM_DESTROY (0x11) — halt and free a VM slot
 *     mr[0] = slot_id
 *     returns mr[0] = 0 (ok) or 1 (error)
 *
 *   OP_VM_SWITCH  (0x12) — switch active console to a VM slot
 *     mr[0] = slot_id
 *     returns mr[0] = 0 (ok) or 1 (error)
 *
 *   OP_VM_STATUS  (0x13) — query status of all slots
 *     returns mr[0..VM_MAX_SLOTS-1]: each byte = vm_slot_state_t
 *
 *   OP_VM_LIST    (0x14) — list all slot IDs and states
 *     returns mr[0] = count, mr[1..N] = slot_id<<8 | state
 *
 * Architecture:
 *
 *   VM_MAX_SLOTS static RAM regions are pre-allocated in the .system file.
 *   Each slot gets its own 512MB RAM region (enough for a light FreeBSD).
 *   The UEFI flash region is shared read-only (EDK2 firmware is identical).
 *
 *   Console multiplexing:
 *     All VM UARTs are passed through to the same physical UART (PL011).
 *     Only the "active" slot has its UART interrupt connected; others are
 *     paused on UART input. Output from all slots goes to the physical UART
 *     (multiplexed with a per-slot prefix banner: [VM0], [VM1], etc.)
 *     In a future revision, VirtIO console allows per-slot PTY.
 *
 *   vCPU management:
 *     Each slot has its own seL4 vCPU object.
 *     Microkit pre-allocates one <virtual_machine> per slot in the system
 *     description. Only the active slot is scheduled; idle slots are
 *     suspended (seL4_TCB_Suspend on the vCPU thread).
 *
 *   Memory:
 *     Slot 0: guest phys 0x40000000 (512MB)
 *     Slot 1: guest phys 0x60000000 (512MB)
 *     Slot 2: guest phys 0x80000000 (512MB)
 *     Slot 3: guest phys 0xa0000000 (512MB)
 *     Shared UEFI flash: guest phys 0x00000000 (64MB, read-only)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <microkit.h>
#include <libvmm/guest.h>

/* Maximum number of concurrent VM instances */
#define VM_MAX_SLOTS        4

/* Per-slot RAM size: 512MB (enough for a minimal FreeBSD) */
#define VM_SLOT_RAM_SIZE    0x20000000UL

/* Guest physical base addresses for each slot's RAM */
#define VM_SLOT_RAM_BASE(n) (0x40000000UL + ((n) * VM_SLOT_RAM_SIZE))

/* Shared UEFI flash (all slots share the same read-only firmware) */
#define VM_FLASH_PADDR      0x00000000UL
#define VM_FLASH_SIZE       0x04000000UL   /* 64MB */

/* VMM IPC opcodes */
#define OP_VM_CREATE        0x10
#define OP_VM_DESTROY       0x11
#define OP_VM_SWITCH        0x12
#define OP_VM_STATUS        0x13
#define OP_VM_LIST          0x14

/* Slot states */
typedef enum {
    VM_SLOT_FREE      = 0,   /* not allocated */
    VM_SLOT_BOOTING   = 1,   /* allocated, guest is starting up */
    VM_SLOT_RUNNING   = 2,   /* guest is running normally */
    VM_SLOT_SUSPENDED = 3,   /* guest is suspended (not active slot) */
    VM_SLOT_HALTED    = 4,   /* guest has halted (ACPI shutdown etc.) */
    VM_SLOT_ERROR     = 5,   /* guest faulted unrecoverably */
} vm_slot_state_t;

/* Per-slot VM context */
typedef struct {
    uint8_t          id;          /* slot index (0..VM_MAX_SLOTS-1) */
    vm_slot_state_t  state;       /* current state */
    vm_t             vm;          /* libvmm guest context */
    uintptr_t        ram_vaddr;   /* VMM's virtual address for this slot's RAM */
    size_t           ram_size;    /* size of RAM region */
    uintptr_t        ram_paddr;   /* guest physical base address */
    uint32_t         vcpu_id;     /* seL4 vCPU ID within Microkit */
    char             label[16];   /* human-readable label e.g. "freebsd-0" */
} vm_slot_t;

/* Multiplexer state */
typedef struct {
    vm_slot_t   slots[VM_MAX_SLOTS];
    uint8_t     active_slot;      /* which slot has console focus */
    uint8_t     slot_count;       /* number of allocated (non-FREE) slots */
} vm_mux_t;

/* Microkit symbols set by linker for each slot's RAM region */
extern uintptr_t guest_ram_vaddr_0;
extern uintptr_t guest_ram_vaddr_1;
extern uintptr_t guest_ram_vaddr_2;
extern uintptr_t guest_ram_vaddr_3;
extern uintptr_t guest_flash_vaddr;

/* ─── Multiplexer API ─────────────────────────────────────────────────── */

/**
 * vmm_mux_init — initialise the multiplexer (no VMs running yet)
 * Called once from vmm_init(). Loads UEFI firmware into the shared flash.
 */
void vmm_mux_init(vm_mux_t *mux);

/**
 * vmm_mux_create — allocate a VM slot and boot a guest
 *
 * @param mux       multiplexer state
 * @param label     human-readable name (e.g. "freebsd-0")
 * @returns slot_id on success, 0xFF if no free slots
 */
uint8_t vmm_mux_create(vm_mux_t *mux, const char *label);

/**
 * vmm_mux_destroy — halt and free a VM slot
 *
 * Suspends the vCPU, zeroes the RAM region, and marks the slot FREE.
 * If the destroyed slot was active, console focus moves to the lowest
 * remaining running slot.
 *
 * @param mux       multiplexer state
 * @param slot_id   slot to destroy
 * @returns 0 on success, -1 if slot_id invalid or FREE
 */
int vmm_mux_destroy(vm_mux_t *mux, uint8_t slot_id);

/**
 * vmm_mux_switch — switch active console to a different slot
 *
 * Suspends the current active slot (if different), resumes the target.
 * UART interrupt routing is updated.
 *
 * @param mux       multiplexer state
 * @param slot_id   target slot (must be RUNNING or BOOTING)
 * @returns 0 on success, -1 if slot not in a runnable state
 */
int vmm_mux_switch(vm_mux_t *mux, uint8_t slot_id);

/**
 * vmm_mux_handle_fault — dispatch a vCPU fault to the correct slot
 *
 * Called from the Microkit fault() handler. Identifies which VM the
 * fault belongs to by matching the child capability.
 */
void vmm_mux_handle_fault(vm_mux_t *mux, microkit_child child,
                           microkit_msginfo msginfo,
                           microkit_msginfo *reply_msginfo);

/**
 * vmm_mux_handle_notify — dispatch a channel notification to the correct slot
 */
void vmm_mux_handle_notify(vm_mux_t *mux, microkit_channel ch);

/**
 * vmm_mux_status — fill a status array (VM_MAX_SLOTS entries of vm_slot_state_t)
 */
void vmm_mux_status(const vm_mux_t *mux, uint8_t *out, size_t out_len);
