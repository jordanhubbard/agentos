/*
 * agentOS FreeBSD VMM — Microkit entry point
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Microkit PD entry point for the FreeBSD VM multiplexer.
 * Handles IPC from controller (create/destroy/switch VMs) and
 * dispatches vCPU faults and IRQ notifications to the correct slot.
 *
 * Boot flow (first VM auto-created at init):
 *   vmm_init() → vmm_mux_init() → vmm_mux_create("freebsd-0")
 *   → EDK2 UEFI → bootaa64.efi → loader.efi → FreeBSD kernel
 *
 * Subsequent VMs are created on demand via controller IPC:
 *   controller PPCs OP_VM_CREATE → second FreeBSD instance boots in slot 1
 *   controller PPCs OP_VM_SWITCH 1 → console switches to slot 1
 *   controller PPCs OP_VM_DESTROY 0 → slot 0 halted and freed
 */

#include <microkit.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/tcb.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/fault.h>

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vmm.h"
#include "vmm_mux.h"

/* ─── Global multiplexer state ───────────────────────────────────────────── */
static vm_mux_t g_mux;

/* ─── Microkit entry points ─────────────────────────────────────────────── */

/**
 * vmm_init — called once at PD startup by Microkit
 *
 * Initialises the VM multiplexer and boots the first FreeBSD instance.
 */
void vmm_init(void)
{
    microkit_dbg_puts("\n");
    microkit_dbg_puts("╔══════════════════════════════════════════╗\n");
    microkit_dbg_puts("║     agentOS FreeBSD VM Multiplexer       ║\n");
    microkit_dbg_puts("║    seL4 (EL2) → EDK2 → FreeBSD (EL1)    ║\n");
    microkit_dbg_puts("╚══════════════════════════════════════════╝\n");
    microkit_dbg_puts("\n");

    /* Initialise the multiplexer (loads UEFI firmware, zeros all slots) */
    vmm_mux_init(&g_mux);

    /*
     * Auto-create the first VM instance.
     * Controller can create additional instances via OP_VM_CREATE.
     */
    uint8_t slot = vmm_mux_create(&g_mux, "freebsd-0");
    if (slot == 0xFF) {
        microkit_dbg_puts("vmm_init: FATAL — could not create initial VM\n");
        return;
    }

    microkit_dbg_puts("vmm_init: FreeBSD VM slot ");
    /* print slot id */
    char c[2] = {'0' + slot, '\0'};
    microkit_dbg_puts(c);
    microkit_dbg_puts(" booting...\n");
}

/**
 * vmm_notified — called when a Microkit channel notification arrives
 *
 * Dispatches hardware IRQs to the active VM's vGIC, and handles
 * controller lifecycle notifications.
 */
void vmm_notified(microkit_channel ch)
{
    vmm_mux_handle_notify(&g_mux, ch);
}

/**
 * vmm_protected — called for controller PPC (protected procedure calls)
 *
 * Handles VM lifecycle operations:
 *   OP_VM_CREATE  (0x10) — allocate new VM slot, boot FreeBSD
 *   OP_VM_DESTROY (0x11) — halt and free a slot
 *   OP_VM_SWITCH  (0x12) — switch active console
 *   OP_VM_STATUS  (0x13) — return status of all slots
 *   OP_VM_LIST    (0x14) — list slot IDs and states
 */
microkit_msginfo vmm_protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;

    uint32_t op = (uint32_t)microkit_msginfo_get_label(msginfo);

    switch (op) {

    case OP_VM_CREATE: {
        /*
         * Allocate a new VM slot and boot a FreeBSD instance.
         * mr[0] is reserved for future use (e.g. boot config flags).
         *
         * Returns mr[0] = slot_id (0..VM_MAX_SLOTS-1), or 0xFF on failure.
         */
        char label[16] = "freebsd-X";
        uint8_t slot = vmm_mux_create(&g_mux, label);
        if (slot != 0xFF) {
            /* Patch the label with the actual slot number */
            label[8] = (char)('0' + slot);
        }
        microkit_mr_set(0, slot);
        return microkit_msginfo_new(0, 1);
    }

    case OP_VM_DESTROY: {
        /*
         * Halt and free a VM slot.
         * mr[0] = slot_id
         *
         * Returns mr[0] = 0 (ok) or 1 (error: invalid/free slot).
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(0);
        int rc = vmm_mux_destroy(&g_mux, slot_id);
        microkit_mr_set(0, rc == 0 ? 0 : 1);
        return microkit_msginfo_new(0, 1);
    }

    case OP_VM_SWITCH: {
        /*
         * Switch active console to a VM slot.
         * mr[0] = slot_id
         *
         * Returns mr[0] = 0 (ok) or 1 (error: slot not runnable).
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(0);
        int rc = vmm_mux_switch(&g_mux, slot_id);
        microkit_mr_set(0, rc == 0 ? 0 : 1);
        return microkit_msginfo_new(0, 1);
    }

    case OP_VM_STATUS: {
        /*
         * Return the state of all VM slots.
         * Returns VM_MAX_SLOTS message registers, each = vm_slot_state_t.
         *
         * State values:
         *   0 = FREE, 1 = BOOTING, 2 = RUNNING,
         *   3 = SUSPENDED, 4 = HALTED, 5 = ERROR
         */
        uint8_t status[VM_MAX_SLOTS];
        vmm_mux_status(&g_mux, status, VM_MAX_SLOTS);
        for (int i = 0; i < VM_MAX_SLOTS; i++) {
            microkit_mr_set(i, status[i]);
        }
        return microkit_msginfo_new(0, VM_MAX_SLOTS);
    }

    case OP_VM_LIST: {
        /*
         * List all non-FREE slots with their IDs and states.
         * Returns:
         *   mr[0] = count of non-FREE slots
         *   mr[1..N] = (slot_id << 8) | state
         *
         * Also includes active_slot in mr[count+1].
         */
        uint32_t count = 0;
        for (int i = 0; i < VM_MAX_SLOTS; i++) {
            if (g_mux.slots[i].state != VM_SLOT_FREE) {
                microkit_mr_set(count + 1,
                    ((uint32_t)i << 8) | (uint32_t)g_mux.slots[i].state);
                count++;
            }
        }
        microkit_mr_set(0, count);
        microkit_mr_set(count + 1, g_mux.active_slot);
        return microkit_msginfo_new(0, count + 2);
    }

    default:
        microkit_dbg_puts("vmm_protected: unknown opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }
}

/**
 * fault — called by Microkit when a vCPU fault arrives
 *
 * Dispatches to the correct VM slot's fault handler via vmm_mux_handle_fault().
 * libvmm handles: MMIO faults, WFI/WFE, SMC, HVC, unknown instructions.
 */
void fault(microkit_child child, microkit_msginfo msginfo,
           microkit_msginfo *reply_msginfo)
{
    vmm_mux_handle_fault(&g_mux, child, msginfo, reply_msginfo);
}
