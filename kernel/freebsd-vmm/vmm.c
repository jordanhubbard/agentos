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
 *   vmm_init() → vmm_register() → vmm_mux_init() → vmm_mux_create("freebsd-0")
 *   → EDK2 UEFI → bootaa64.efi → loader.efi → FreeBSD kernel
 *
 * Subsequent VMs are created on demand via controller IPC:
 *   controller PPCs OP_VM_CREATE → second FreeBSD instance boots in slot 1
 *   controller PPCs OP_VM_SWITCH 1 → console switches to slot 1
 *   controller PPCs OP_VM_DESTROY 0 → slot 0 halted and freed
 *
 * Ring-1 consumer model (Phase 3c):
 *   All device I/O is mediated through ring-0 service PDs.
 *   freebsd_vmm holds no direct hardware capabilities after init.
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

/* Phase 3 — guest OS binding contract (guest_contract.h §3.1 compliance) */
#include "../../kernel/agentos-root-task/include/contracts/freebsd_vmm_contract.h"

/* ─── Shared memory symbols (set by Microkit linker) ────────────────────── */
uintptr_t vmm_serial_shmem_vaddr;
uintptr_t vmm_block_shmem_vaddr;
uintptr_t vmm_net_shmem_vaddr;

/* ─── Global multiplexer state ───────────────────────────────────────────── */
static vm_mux_t g_mux;

/* ─── VMM registration ───────────────────────────────────────────────────── */

/*
 * vmm_register — register this PD as a VMM with the root-task.
 *
 * Sends MSG_VMM_REGISTER on CH_VMM_KERNEL_LOCAL and stores the returned
 * vmm_token in g_mux.vmm_token.  All subsequent MSG_VMM_* calls must
 * include this token.
 *
 * Returns true on success.
 */
static bool vmm_register(void)
{
    struct vmm_register_req req = {
        .os_type    = VMM_OS_TYPE_FREEBSD,
        .flags      = VMM_FLAG_SMP,
        .max_guests = VM_MAX_SLOTS,
        .name       = "freebsd_vmm",
    };
    (void)req;  /* req would be passed via shmem in a full implementation */

    microkit_mr_set(0, VMM_OS_TYPE_FREEBSD);
    microkit_mr_set(1, VMM_FLAG_SMP);
    microkit_mr_set(2, VM_MAX_SLOTS);

    microkit_msginfo reply = microkit_ppcall(
        CH_VMM_KERNEL_LOCAL,
        microkit_msginfo_new(MSG_VMM_REGISTER, 3));

    uint32_t ok = (uint32_t)microkit_mr_get(0);
    if (!ok) {
        microkit_dbg_puts("vmm_register: root-task rejected VMM registration\n");
        return false;
    }

    g_mux.vmm_token = (uint32_t)microkit_mr_get(1);
    microkit_dbg_puts("vmm_register: registered, token acquired\n");
    (void)reply;
    return true;
}

/* ─── Guest Binding Protocol (guest_contract.h §3.1) ────────────────────── */

/*
 * freebsd_vmm_binding_init — announce this VMM to the system before boot.
 *
 * Mirrors linux_vmm_binding_init().  Steps 2-4 are deferred until the
 * relevant channels (CH_SERIAL_PD, CH_EVENTBUS_VMM, CH_QUOTA_CTRL) are
 * wired in the freebsd .system manifest.  vmm_mux_create() opens the
 * device handles per-slot when those channels are available.
 *
 * Step 1: Register with root-task (MSG_VMM_REGISTER) — done by vmm_register().
 * Step 2: Open serial/net/block handles — done in vmm_mux_create() per slot.
 * Step 3: Bind devices to guest_id — done in vmm_mux_create() per slot.
 * Step 4: Register resource quota — TODO: wire CH_QUOTA_CTRL in manifest.
 * Step 5: Publish EVENT_GUEST_READY — TODO: wire CH_EVENTBUS_VMM in manifest.
 */
static void freebsd_vmm_binding_init(void)
{
    /* Step 1 already complete — vmm_register() was called before this. */
    microkit_dbg_puts("freebsd_vmm: binding protocol — "
                      "device opens handled per-slot in vmm_mux_create()\n");

    /* Step 4: MSG_QUOTA_REGISTER — deferred (CH_QUOTA_CTRL not yet wired) */
    /* struct vmm_quota_req qr = {                                          */
    /*     .pd_id      = FREEBSD_VMM_OS_TYPE,                               */
    /*     .cpu_budget = 0,   \/\* unlimited — MCS scheduler enforces \*\/  */
    /*     .mem_kb     = 0,   \/\* unlimited — seL4 UTs manage this \*\/    */
    /* };                                                                    */

    /* Step 5: MSG_EVENTBUS_PUBLISH_BATCH EVENT_GUEST_READY — deferred */
    microkit_dbg_puts("freebsd_vmm: EVENT_GUEST_READY deferred "
                      "(CH_EVENTBUS_VMM not yet wired)\n");
}

/* ─── Microkit entry points ─────────────────────────────────────────────── */

/**
 * vmm_init — called once at PD startup by Microkit
 *
 * Registers the VMM with the root-task, initialises the VM multiplexer,
 * and boots the first FreeBSD instance.
 */
void vmm_init(void)
{
    microkit_dbg_puts("\n");
    microkit_dbg_puts("╔══════════════════════════════════════════╗\n");
    microkit_dbg_puts("║     agentOS FreeBSD VM Multiplexer       ║\n");
    microkit_dbg_puts("║    seL4 (EL2) → EDK2 → FreeBSD (EL1)    ║\n");
    microkit_dbg_puts("╚══════════════════════════════════════════╝\n");
    microkit_dbg_puts("\n");

    /*
     * Step 1: Register with the root-task as a VMM.
     * This grants us a vmm_token required for all subsequent MSG_VMM_*
     * calls and validates that we hold no direct hardware capabilities.
     */
    if (!vmm_register()) {
        microkit_dbg_puts("vmm_init: FATAL — VMM registration failed\n");
        return;
    }

    /* Phase 3 — complete guest binding protocol announcement */
    freebsd_vmm_binding_init();

    /* Step 2: Initialise the multiplexer (loads UEFI firmware, zeros slots) */
    vmm_mux_init(&g_mux);

    /*
     * Step 3: Auto-create the first VM instance.
     * Controller can create additional instances via OP_VM_CREATE.
     * vmm_mux_create() performs the full guest binding protocol:
     *   MSG_SERIAL_OPEN → MSG_NET_OPEN → MSG_BLOCK_OPEN
     *   → MSG_GUEST_BIND_DEVICE (×3) → MSG_GUEST_SET_MEMORY
     *   → register MMIO fault handlers → vm_run()
     */
    uint8_t slot = vmm_mux_create(&g_mux, "freebsd-0");
    if (slot == 0xFF) {
        microkit_dbg_puts("vmm_init: FATAL — could not create initial VM\n");
        return;
    }

    microkit_dbg_puts("vmm_init: FreeBSD VM slot ");
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
