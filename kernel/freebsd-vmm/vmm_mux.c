/*
 * agentOS VM Multiplexer
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Manages a pool of VM slots: create, destroy, switch active console.
 * See vmm_mux.h for the full design rationale.
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

/* ─── External firmware blobs ────────────────────────────────────────────── */
extern char _binary_edk2_aarch64_code_fd_start[];
extern char _binary_edk2_aarch64_code_fd_end[];
extern char _binary_freebsd_dtb_start[];
extern char _binary_freebsd_dtb_end[];

/* ─── Microkit slot RAM vaddr symbols ────────────────────────────────────── */
uintptr_t guest_ram_vaddr_0;
uintptr_t guest_ram_vaddr_1;
uintptr_t guest_ram_vaddr_2;
uintptr_t guest_ram_vaddr_3;
uintptr_t guest_flash_vaddr;

/* Map slot index → Microkit-provided RAM vaddr */
static uintptr_t *slot_ram_vaddrs[VM_MAX_SLOTS] = {
    &guest_ram_vaddr_0,
    &guest_ram_vaddr_1,
    &guest_ram_vaddr_2,
    &guest_ram_vaddr_3,
};

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void dbg_uint8(uint8_t v)
{
    char buf[4] = {'0' + (v / 100 % 10), '0' + (v / 10 % 10), '0' + (v % 10), '\0'};
    microkit_dbg_puts(buf);
}

static void load_uefi_firmware(void)
{
    size_t fw_size = (size_t)(_binary_edk2_aarch64_code_fd_end
                              - _binary_edk2_aarch64_code_fd_start);
    if (fw_size == 0 || fw_size > VM_FLASH_SIZE) {
        microkit_dbg_puts("vmm_mux: EDK2 firmware missing or too large\n");
        return;
    }
    memcpy((void *)guest_flash_vaddr,
           _binary_edk2_aarch64_code_fd_start,
           fw_size);
    microkit_dbg_puts("vmm_mux: EDK2 UEFI firmware loaded into shared flash\n");
}

static void load_dtb(uintptr_t flash_va)
{
    size_t dtb_size = (size_t)(_binary_freebsd_dtb_end - _binary_freebsd_dtb_start);
    if (dtb_size == 0) return;

    uintptr_t dtb_va = flash_va + GUEST_DTB_PADDR;
    if (GUEST_DTB_PADDR + dtb_size > VM_FLASH_SIZE) {
        microkit_dbg_puts("vmm_mux: DTB too large for flash region\n");
        return;
    }
    memcpy((void *)dtb_va, _binary_freebsd_dtb_start, dtb_size);
}

/* ─── vmm_mux_init ───────────────────────────────────────────────────────── */

void vmm_mux_init(vm_mux_t *mux)
{
    memset(mux, 0, sizeof(*mux));
    mux->active_slot = 0xFF;   /* no active slot yet */

    /* Mark all slots as FREE */
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        mux->slots[i].id    = (uint8_t)i;
        mux->slots[i].state = VM_SLOT_FREE;
    }

    /* Load UEFI firmware once — shared across all slots (read-only in guest) */
    load_uefi_firmware();

    /* Load DTB into the flash region (used by all slots) */
    load_dtb(guest_flash_vaddr);

    microkit_dbg_puts("vmm_mux: initialised, ");
    dbg_uint8(VM_MAX_SLOTS);
    microkit_dbg_puts(" slots available\n");
}

/* ─── vmm_mux_create ─────────────────────────────────────────────────────── */

uint8_t vmm_mux_create(vm_mux_t *mux, const char *label)
{
    /* Find a free slot */
    int slot_id = -1;
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (mux->slots[i].state == VM_SLOT_FREE) {
            slot_id = i;
            break;
        }
    }
    if (slot_id < 0) {
        microkit_dbg_puts("vmm_mux: no free VM slots\n");
        return 0xFF;
    }

    vm_slot_t *slot = &mux->slots[slot_id];

    /* Populate slot metadata */
    slot->ram_vaddr = *slot_ram_vaddrs[slot_id];
    slot->ram_size  = VM_SLOT_RAM_SIZE;
    slot->ram_paddr = VM_SLOT_RAM_BASE(slot_id);
    slot->vcpu_id   = (uint32_t)slot_id;   /* vCPU IDs match slot indices */

    /* Copy label */
    if (label) {
        int i = 0;
        while (i < (int)sizeof(slot->label) - 1 && label[i]) {
            slot->label[i] = label[i];
            i++;
        }
        slot->label[i] = '\0';
    } else {
        slot->label[0] = 'v';
        slot->label[1] = 'm';
        slot->label[2] = '0' + slot_id;
        slot->label[3] = '\0';
    }

    microkit_dbg_puts("vmm_mux: creating VM slot ");
    dbg_uint8((uint8_t)slot_id);
    microkit_dbg_puts(" (");
    microkit_dbg_puts(slot->label);
    microkit_dbg_puts(")\n");

    /* Initialise libvmm guest context for this slot */
    vm_init(&slot->vm,
            /* vcpu_id  */ slot->vcpu_id,
            /* ram_base */ slot->ram_paddr,
            /* ram_size */ slot->ram_size,
            /* ram_vaddr */ slot->ram_vaddr);

    /*
     * Configure vCPU boot state.
     *
     * All slots share the same UEFI flash (guest PA 0x00000000).
     * Each slot's EDK2 instance will independently discover its own
     * VirtIO block device (different VirtIO MMIO slot per instance).
     */
    vcpu_regs_t boot_regs = {
        .pc   = UEFI_RESET_VECTOR,
        .sp   = slot->ram_paddr + slot->ram_size - 0x1000,
        .x0   = 0,
        .spsr = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0), /* SPSR_EL1h */
    };
    vm_set_boot_regs(&slot->vm, &boot_regs);

    /* Register MMIO fault handler */
    vm_register_mmio_fault_handler(&slot->vm, NULL, NULL);

    /* Initialise vGIC for this slot */
    vgic_init();

    slot->state = VM_SLOT_BOOTING;
    mux->slot_count++;

    /*
     * If this is the first VM, make it the active slot automatically
     * and start it running. Otherwise, leave it suspended until
     * vmm_mux_switch() is called.
     */
    if (mux->active_slot == 0xFF) {
        mux->active_slot = (uint8_t)slot_id;
        microkit_dbg_puts("vmm_mux: auto-activating slot ");
        dbg_uint8((uint8_t)slot_id);
        microkit_dbg_puts(" (first VM)\n");
        vm_run(&slot->vm);
        slot->state = VM_SLOT_RUNNING;
    } else {
        /*
         * Start the vCPU but immediately suspend it. The guest will
         * begin executing from its reset vector when vmm_mux_switch()
         * is called.
         */
        vm_run(&slot->vm);
        vm_suspend(&slot->vm);
        slot->state = VM_SLOT_SUSPENDED;
        microkit_dbg_puts("vmm_mux: slot ");
        dbg_uint8((uint8_t)slot_id);
        microkit_dbg_puts(" created (suspended, not active)\n");
    }

    return (uint8_t)slot_id;
}

/* ─── vmm_mux_destroy ────────────────────────────────────────────────────── */

int vmm_mux_destroy(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    vm_slot_t *slot = &mux->slots[slot_id];
    if (slot->state == VM_SLOT_FREE) return -1;

    microkit_dbg_puts("vmm_mux: destroying slot ");
    dbg_uint8(slot_id);
    microkit_dbg_puts(" (");
    microkit_dbg_puts(slot->label);
    microkit_dbg_puts(")\n");

    /* Suspend the vCPU first (safe to call even if already suspended/halted) */
    if (slot->state == VM_SLOT_RUNNING || slot->state == VM_SLOT_BOOTING) {
        vm_suspend(&slot->vm);
    }

    /*
     * Zero the RAM region so a future slot can start fresh.
     * This also prevents a newly created VM from inheriting stale state.
     */
    memset((void *)slot->ram_vaddr, 0, slot->ram_size);

    /* Free the slot */
    slot->state = VM_SLOT_FREE;
    mux->slot_count--;

    /*
     * If this was the active slot, reassign console focus to the lowest
     * running slot. If no slots are running, set active_slot = 0xFF.
     */
    if (mux->active_slot == slot_id) {
        mux->active_slot = 0xFF;
        for (int i = 0; i < VM_MAX_SLOTS; i++) {
            if (mux->slots[i].state == VM_SLOT_RUNNING ||
                mux->slots[i].state == VM_SLOT_BOOTING) {
                mux->active_slot = (uint8_t)i;
                vm_resume(&mux->slots[i].vm);
                microkit_dbg_puts("vmm_mux: console focus moved to slot ");
                dbg_uint8((uint8_t)i);
                microkit_dbg_puts("\n");
                break;
            }
        }
        if (mux->active_slot == 0xFF) {
            microkit_dbg_puts("vmm_mux: no remaining active VMs\n");
        }
    }

    microkit_dbg_puts("vmm_mux: slot ");
    dbg_uint8(slot_id);
    microkit_dbg_puts(" destroyed\n");
    return 0;
}

/* ─── vmm_mux_switch ─────────────────────────────────────────────────────── */

int vmm_mux_switch(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;

    vm_slot_t *target = &mux->slots[slot_id];
    if (target->state == VM_SLOT_FREE || target->state == VM_SLOT_ERROR) {
        microkit_dbg_puts("vmm_mux: switch to slot ");
        dbg_uint8(slot_id);
        microkit_dbg_puts(" failed: slot not runnable\n");
        return -1;
    }

    /* No-op if already active */
    if (mux->active_slot == slot_id) return 0;

    /* Suspend the current active slot */
    if (mux->active_slot != 0xFF) {
        vm_slot_t *current = &mux->slots[mux->active_slot];
        if (current->state == VM_SLOT_RUNNING) {
            vm_suspend(&current->vm);
            current->state = VM_SLOT_SUSPENDED;
        }
    }

    /*
     * Print a console banner so the user knows which VM now has focus.
     * In a VirtIO console revision, each slot would have its own PTY;
     * for now we multiplex on the single physical UART.
     */
    microkit_dbg_puts("\n[vmm_mux] Switching to VM ");
    dbg_uint8(slot_id);
    microkit_dbg_puts(" (");
    microkit_dbg_puts(target->label);
    microkit_dbg_puts(")\n");
    microkit_dbg_puts("──────────────────────────────\n");

    /* Resume the target slot */
    vm_resume(&target->vm);
    if (target->state == VM_SLOT_SUSPENDED || target->state == VM_SLOT_BOOTING) {
        target->state = VM_SLOT_RUNNING;
    }

    mux->active_slot = slot_id;
    return 0;
}

/* ─── vmm_mux_handle_fault ───────────────────────────────────────────────── */

void vmm_mux_handle_fault(vm_mux_t *mux, microkit_child child,
                           microkit_msginfo msginfo,
                           microkit_msginfo *reply_msginfo)
{
    /*
     * Identify which slot this fault belongs to.
     * Microkit assigns child IDs sequentially; we assume:
     *   child 0 → slot 0's vCPU, child 1 → slot 1's vCPU, etc.
     */
    uint8_t slot_id = (uint8_t)child;
    if (slot_id >= VM_MAX_SLOTS || mux->slots[slot_id].state == VM_SLOT_FREE) {
        microkit_dbg_puts("vmm_mux: fault from unknown child\n");
        return;
    }

    vm_slot_t *slot = &mux->slots[slot_id];
    bool handled = vm_handle_fault(&slot->vm, child, msginfo, reply_msginfo);
    if (!handled) {
        microkit_dbg_puts("vmm_mux: unhandled fault in slot ");
        dbg_uint8(slot_id);
        microkit_dbg_puts("\n");
        slot->state = VM_SLOT_ERROR;
        /* Notify controller that slot N has faulted */
        microkit_notify(CH_CONTROLLER);
    }
}

/* ─── vmm_mux_handle_notify ──────────────────────────────────────────────── */

void vmm_mux_handle_notify(vm_mux_t *mux, microkit_channel ch)
{
    /*
     * Dispatch IRQ notifications to the appropriate slot's vGIC.
     * Channel layout (from the .system file):
     *   CH_CONTROLLER (70): lifecycle commands from controller
     *   CH_IRQ_BASE+N:      hardware IRQ N forwarded to active slot
     */
    if (ch == CH_CONTROLLER) {
        /* Controller is sending a lifecycle command; handled by vmm_protected() */
        return;
    }

    /* Try to deliver as a pass-through IRQ to the active slot */
    if (mux->active_slot != 0xFF) {
        bool handled = virq_handle_passthrough(ch);
        (void)handled;
    }
}

/* ─── vmm_mux_status ─────────────────────────────────────────────────────── */

void vmm_mux_status(const vm_mux_t *mux, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < VM_MAX_SLOTS && i < out_len; i++) {
        out[i] = (uint8_t)mux->slots[i].state;
    }
}
