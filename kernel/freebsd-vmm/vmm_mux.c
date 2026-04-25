/*
 * agentOS VM Multiplexer
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Manages a pool of VM slots: create, destroy, switch active console.
 * See vmm_mux.h for the full design rationale.
 *
 * Phase 3c: ring-1 consumer model.
 *   All device I/O is mediated through ring-0 service PDs via IPC.
 *   MMIO faults from guest VMs are intercepted by vmm_uart_fault(),
 *   vmm_blk_fault(), and vmm_net_fault() and forwarded to serial_pd,
 *   block_pd, and net_pd respectively.  No direct hardware capabilities
 *   are held after vmm_mux_init() completes.
 */

#include <sel4/sel4.h>
#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/tcb.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/virtio/mmio.h>

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vmm.h"
#include "vmm_mux.h"
#include "contracts/guest_contract.h"

/* ─── External firmware blobs ────────────────────────────────────────────── */
extern char _binary_edk2_aarch64_code_fd_start[];
extern char _binary_edk2_aarch64_code_fd_end[];
extern char _binary_freebsd_dtb_start[];
extern char _binary_freebsd_dtb_end[];

/* ─── Slot RAM vaddr symbols (set by root task / linker) ─────────────────── */
uintptr_t guest_ram_vaddr_0;
uintptr_t guest_ram_vaddr_1;
uintptr_t guest_ram_vaddr_2;
uintptr_t guest_ram_vaddr_3;
uintptr_t guest_flash_vaddr;

/* Map slot index → vaddr symbol */
static uintptr_t *slot_ram_vaddrs[VM_MAX_SLOTS] = {
    &guest_ram_vaddr_0,
    &guest_ram_vaddr_1,
    &guest_ram_vaddr_2,
    &guest_ram_vaddr_3,
};

/* ─── Channel → endpoint cap table ──────────────────────────────────────── */

#define VMM_MUX_MAX_CHANNELS  8u

static seL4_CPtr g_ch_ep[VMM_MUX_MAX_CHANNELS];

void vmm_mux_set_channel_ep(uint32_t ch, seL4_CPtr ep)
{
    if (ch < VMM_MUX_MAX_CHANNELS)
        g_ch_ep[ch] = ep;
}

/* ─── Forward declarations for MMIO handlers ──────────────────────────────── */

static bool vmm_uart_fault(size_t vcpu_id, size_t offset, size_t fsr,
                            seL4_UserContext *regs, void *data);
static bool vmm_blk_fault(size_t vcpu_id, size_t offset, size_t fsr,
                           seL4_UserContext *regs, void *data);
static bool vmm_net_fault(size_t vcpu_id, size_t offset, size_t fsr,
                           seL4_UserContext *regs, void *data);

/* ─── Reference to global mux (needed by MMIO handlers) ──────────────────── */
extern vm_mux_t g_mux;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void dbg_uint8(uint8_t v)
{
    char buf[4] = {'0' + (v / 100 % 10), '0' + (v / 10 % 10), '0' + (v % 10), '\0'};
    vmm_dbg_puts(buf);
}

static void load_uefi_firmware(void)
{
    size_t fw_size = (size_t)(_binary_edk2_aarch64_code_fd_end
                              - _binary_edk2_aarch64_code_fd_start);
    if (fw_size == 0 || fw_size > VM_FLASH_SIZE) {
        vmm_dbg_puts("vmm_mux: EDK2 firmware missing or too large\n");
        return;
    }
    memcpy((void *)guest_flash_vaddr,
           _binary_edk2_aarch64_code_fd_start,
           fw_size);
    vmm_dbg_puts("vmm_mux: EDK2 UEFI firmware loaded into shared flash\n");
}

static void load_dtb(uintptr_t flash_va)
{
    size_t dtb_size = (size_t)(_binary_freebsd_dtb_end - _binary_freebsd_dtb_start);
    if (dtb_size == 0) return;

    uintptr_t dtb_va = flash_va + GUEST_DTB_PADDR;
    if (GUEST_DTB_PADDR + dtb_size > VM_FLASH_SIZE) {
        vmm_dbg_puts("vmm_mux: DTB too large for flash region\n");
        return;
    }
    memcpy((void *)dtb_va, _binary_freebsd_dtb_start, dtb_size);
}

/* ─── vmm_mux_init ───────────────────────────────────────────────────────── */

void vmm_mux_init(vm_mux_t *mux)
{
    memset(mux, 0, sizeof(*mux));
    mux->active_slot = 0xFF;   /* no active slot yet */

    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        mux->slots[i].id    = (uint8_t)i;
        mux->slots[i].state = VM_SLOT_FREE;
    }

    load_uefi_firmware();
    load_dtb(guest_flash_vaddr);

    vmm_dbg_puts("vmm_mux: initialised, ");
    dbg_uint8(VM_MAX_SLOTS);
    vmm_dbg_puts(" slots available\n");
}

/* ─── vmm_mux_create ─────────────────────────────────────────────────────── */

uint8_t vmm_mux_create(vm_mux_t *mux, const char *label)
{
    int slot_id = -1;
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (mux->slots[i].state == VM_SLOT_FREE) {
            slot_id = i;
            break;
        }
    }
    if (slot_id < 0) {
        vmm_dbg_puts("vmm_mux: no free VM slots\n");
        return 0xFF;
    }

    vm_slot_t *slot = &mux->slots[slot_id];

    slot->ram_vaddr = *slot_ram_vaddrs[slot_id];
    slot->ram_size  = VM_SLOT_RAM_SIZE;
    slot->ram_paddr = VM_SLOT_RAM_BASE(slot_id);
    slot->vcpu_id   = (uint32_t)slot_id;

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

    vmm_dbg_puts("vmm_mux: creating VM slot ");
    dbg_uint8((uint8_t)slot_id);
    vmm_dbg_puts(" (");
    vmm_dbg_puts(slot->label);
    vmm_dbg_puts(")\n");

    vm_init(&slot->vm,
            slot->vcpu_id,
            slot->ram_paddr,
            slot->ram_size,
            slot->ram_vaddr);

    vcpu_regs_t boot_regs = {
        .pc   = UEFI_RESET_VECTOR,
        .sp   = slot->ram_paddr + slot->ram_size - 0x1000,
        .x0   = 0,
        .spsr = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0),
    };
    vm_set_boot_regs(&slot->vm, &boot_regs);

    /* ── Phase 3c: guest binding protocol ─────────────────────────────────
     *
     * Register this slot with ring-0 service PDs before starting the guest.
     */

    /* Step A: Open serial port with serial_pd */
    seL4_SetMR(0, MSG_SERIAL_OPEN);
    seL4_SetMR(1, 0);
    seL4_Call(g_ch_ep[CH_VMM_SERIAL], seL4_MessageInfo_new(MSG_SERIAL_OPEN, 0, 0, 2));
    uint32_t serial_ok = (uint32_t)seL4_GetMR(0);
    slot->serial_slot = serial_ok ? (uint32_t)seL4_GetMR(1) : UINT32_MAX;
    if (!serial_ok) {
        vmm_dbg_puts("vmm_mux: WARNING — serial_pd open failed for slot ");
        dbg_uint8((uint8_t)slot_id);
        vmm_dbg_puts("\n");
    }

    /* Step B: Open net interface with net_pd */
    seL4_SetMR(0, MSG_NET_OPEN);
    seL4_SetMR(1, (uint32_t)slot_id);
    seL4_Call(g_ch_ep[CH_VMM_NET], seL4_MessageInfo_new(MSG_NET_OPEN, 0, 0, 2));
    uint32_t net_ok = (uint32_t)seL4_GetMR(0);
    slot->net_handle = net_ok ? (uint32_t)seL4_GetMR(1) : UINT32_MAX;
    if (!net_ok) {
        vmm_dbg_puts("vmm_mux: WARNING — net_pd open failed for slot ");
        dbg_uint8((uint8_t)slot_id);
        vmm_dbg_puts("\n");
    }

    /* Step C: Open block device with block_pd */
    seL4_SetMR(0, MSG_BLOCK_OPEN);
    seL4_SetMR(1, (uint32_t)slot_id);
    seL4_SetMR(2, 0);
    seL4_Call(g_ch_ep[CH_VMM_BLOCK], seL4_MessageInfo_new(MSG_BLOCK_OPEN, 0, 0, 3));
    uint32_t blk_ok = (uint32_t)seL4_GetMR(0);
    slot->block_handle = blk_ok ? (uint32_t)seL4_GetMR(1) : UINT32_MAX;
    if (!blk_ok) {
        vmm_dbg_puts("vmm_mux: WARNING — block_pd open failed for slot ");
        dbg_uint8((uint8_t)slot_id);
        vmm_dbg_puts("\n");
    }

    /* Step D: Bind devices to guest slot via root-task (MSG_GUEST_BIND_DEVICE) */
    if (serial_ok) {
        seL4_SetMR(0, MSG_GUEST_BIND_DEVICE);
        seL4_SetMR(1, slot->guest_id);
        seL4_SetMR(2, GUEST_DEV_SERIAL);
        seL4_SetMR(3, slot->serial_slot);
        seL4_Call(g_ch_ep[CH_VMM_KERNEL_LOCAL],
                  seL4_MessageInfo_new(MSG_GUEST_BIND_DEVICE, 0, 0, 4));
    }
    if (net_ok) {
        seL4_SetMR(0, MSG_GUEST_BIND_DEVICE);
        seL4_SetMR(1, slot->guest_id);
        seL4_SetMR(2, GUEST_DEV_NET);
        seL4_SetMR(3, slot->net_handle);
        seL4_Call(g_ch_ep[CH_VMM_KERNEL_LOCAL],
                  seL4_MessageInfo_new(MSG_GUEST_BIND_DEVICE, 0, 0, 4));
    }
    if (blk_ok) {
        seL4_SetMR(0, MSG_GUEST_BIND_DEVICE);
        seL4_SetMR(1, slot->guest_id);
        seL4_SetMR(2, GUEST_DEV_BLOCK);
        seL4_SetMR(3, slot->block_handle);
        seL4_Call(g_ch_ep[CH_VMM_KERNEL_LOCAL],
                  seL4_MessageInfo_new(MSG_GUEST_BIND_DEVICE, 0, 0, 4));
    }

    /* Step E: Declare guest RAM to root-task (MSG_GUEST_SET_MEMORY) */
    seL4_SetMR(0, MSG_GUEST_SET_MEMORY);
    seL4_SetMR(1, slot->guest_id);
    seL4_SetMR(2, (uint32_t)(slot->ram_paddr & 0xFFFFFFFFu));
    seL4_SetMR(3, (uint32_t)(slot->ram_paddr >> 32));
    seL4_SetMR(4, (uint32_t)(slot->ram_size / (1024u * 1024u)));
    seL4_SetMR(5, GUEST_MEM_FLAG_CACHED);
    seL4_Call(g_ch_ep[CH_VMM_KERNEL_LOCAL],
              seL4_MessageInfo_new(MSG_GUEST_SET_MEMORY, 0, 0, 6));

    /* ── Register MMIO fault handlers ────────────────────────────────────── */

    fault_register_vm_exception_handler(
        GUEST_UART_PADDR, GUEST_UART_SIZE,
        vmm_uart_fault,
        mux);

    fault_register_vm_exception_handler(
        GUEST_VIRTIO_BLK_PADDR((size_t)slot_id), GUEST_VIRTIO_SIZE,
        vmm_blk_fault,
        slot);

    fault_register_vm_exception_handler(
        GUEST_VIRTIO_NET_PADDR((size_t)slot_id), GUEST_VIRTIO_SIZE,
        vmm_net_fault,
        slot);

    /* ── Initialise per-slot VirtIO emulation state ─────────────────────── */
    slot->vblk.DeviceID        = VIRTIO_DEVICE_ID_BLOCK;
    slot->vblk.QueueNumMax     = QUEUE_SIZE;
    slot->vblk.ConfigGeneration = 1;

    slot->vnet.DeviceID        = VIRTIO_DEVICE_ID_NET;
    slot->vnet.QueueNumMax     = QUEUE_SIZE;
    slot->vnet.ConfigGeneration = 1;

    vgic_init();

    slot->state = VM_SLOT_BOOTING;
    mux->slot_count++;

    if (mux->active_slot == 0xFF) {
        mux->active_slot = (uint8_t)slot_id;
        vmm_dbg_puts("vmm_mux: auto-activating slot ");
        dbg_uint8((uint8_t)slot_id);
        vmm_dbg_puts(" (first VM)\n");
        vm_run(&slot->vm);
        slot->state = VM_SLOT_RUNNING;
    } else {
        vm_run(&slot->vm);
        vm_suspend(&slot->vm);
        slot->state = VM_SLOT_SUSPENDED;
        vmm_dbg_puts("vmm_mux: slot ");
        dbg_uint8((uint8_t)slot_id);
        vmm_dbg_puts(" created (suspended, not active)\n");
    }

    return (uint8_t)slot_id;
}

/* ─── vmm_mux_destroy ────────────────────────────────────────────────────── */

int vmm_mux_destroy(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    vm_slot_t *slot = &mux->slots[slot_id];
    if (slot->state == VM_SLOT_FREE) return -1;

    vmm_dbg_puts("vmm_mux: destroying slot ");
    dbg_uint8(slot_id);
    vmm_dbg_puts(" (");
    vmm_dbg_puts(slot->label);
    vmm_dbg_puts(")\n");

    if (slot->state == VM_SLOT_RUNNING || slot->state == VM_SLOT_BOOTING)
        vm_suspend(&slot->vm);

    memset((void *)slot->ram_vaddr, 0, slot->ram_size);

    slot->state = VM_SLOT_FREE;
    mux->slot_count--;

    if (mux->active_slot == slot_id) {
        mux->active_slot = 0xFF;
        for (int i = 0; i < VM_MAX_SLOTS; i++) {
            if (mux->slots[i].state == VM_SLOT_RUNNING ||
                mux->slots[i].state == VM_SLOT_BOOTING) {
                mux->active_slot = (uint8_t)i;
                vm_resume(&mux->slots[i].vm);
                vmm_dbg_puts("vmm_mux: console focus moved to slot ");
                dbg_uint8((uint8_t)i);
                vmm_dbg_puts("\n");
                break;
            }
        }
        if (mux->active_slot == 0xFF)
            vmm_dbg_puts("vmm_mux: no remaining active VMs\n");
    }

    vmm_dbg_puts("vmm_mux: slot ");
    dbg_uint8(slot_id);
    vmm_dbg_puts(" destroyed\n");
    return 0;
}

/* ─── vmm_mux_pause ─────────────────────────────────────────────────────── */

int vmm_mux_pause(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    vm_slot_t *slot = &mux->slots[slot_id];

    if (slot->state == VM_SLOT_FREE || slot->state == VM_SLOT_ERROR) return -1;
    if (slot->state == VM_SLOT_SUSPENDED || slot->state == VM_SLOT_HALTED) return -1;

    vmm_dbg_puts("vmm_mux: pausing slot ");
    dbg_uint8(slot_id);
    vmm_dbg_puts("\n");

    vm_suspend(&slot->vm);
    slot->state = VM_SLOT_SUSPENDED;
    return 0;
}

/* ─── vmm_mux_resume ─────────────────────────────────────────────────────── */

int vmm_mux_resume(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    vm_slot_t *slot = &mux->slots[slot_id];

    if (slot->state == VM_SLOT_FREE || slot->state == VM_SLOT_ERROR) return -1;
    if (slot->state == VM_SLOT_RUNNING || slot->state == VM_SLOT_BOOTING) return -1;

    vmm_dbg_puts("vmm_mux: resuming slot ");
    dbg_uint8(slot_id);
    vmm_dbg_puts("\n");

    vm_resume(&slot->vm);
    slot->state = VM_SLOT_RUNNING;
    return 0;
}

/* ─── vmm_mux_switch ─────────────────────────────────────────────────────── */

int vmm_mux_switch(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;

    vm_slot_t *target = &mux->slots[slot_id];
    if (target->state == VM_SLOT_FREE || target->state == VM_SLOT_ERROR) {
        vmm_dbg_puts("vmm_mux: switch to slot ");
        dbg_uint8(slot_id);
        vmm_dbg_puts(" failed: slot not runnable\n");
        return -1;
    }

    if (mux->active_slot == slot_id) return 0;

    if (mux->active_slot != 0xFF) {
        vm_slot_t *current = &mux->slots[mux->active_slot];
        if (current->state == VM_SLOT_RUNNING) {
            vm_suspend(&current->vm);
            current->state = VM_SLOT_SUSPENDED;
        }
    }

    vmm_dbg_puts("\n[vmm_mux] Switching to VM ");
    dbg_uint8(slot_id);
    vmm_dbg_puts(" (");
    vmm_dbg_puts(target->label);
    vmm_dbg_puts(")\n");
    vmm_dbg_puts("──────────────────────────────\n");

    vm_resume(&target->vm);
    if (target->state == VM_SLOT_SUSPENDED || target->state == VM_SLOT_BOOTING)
        target->state = VM_SLOT_RUNNING;

    mux->active_slot = slot_id;
    return 0;
}

/* ─── vmm_mux_handle_fault ───────────────────────────────────────────────── */

void vmm_mux_handle_fault(vm_mux_t *mux, seL4_Word vcpu_id,
                           seL4_MessageInfo_t msginfo,
                           seL4_MessageInfo_t *reply_msginfo)
{
    uint8_t slot_id = (uint8_t)vcpu_id;
    if (slot_id >= VM_MAX_SLOTS || mux->slots[slot_id].state == VM_SLOT_FREE) {
        vmm_dbg_puts("vmm_mux: fault from unknown vcpu\n");
        return;
    }

    vm_slot_t *slot = &mux->slots[slot_id];
    bool handled = vm_handle_fault(&slot->vm, (uint32_t)vcpu_id,
                                   msginfo, reply_msginfo);
    if (!handled) {
        vmm_dbg_puts("vmm_mux: unhandled fault in slot ");
        dbg_uint8(slot_id);
        vmm_dbg_puts("\n");
        slot->state = VM_SLOT_ERROR;
        vmm_notify(g_ch_ep[CH_VMM_CONTROLLER_EVT]);
    }
}

/* ─── vmm_mux_handle_notify ──────────────────────────────────────────────── */

void vmm_mux_handle_notify(vm_mux_t *mux, seL4_Word badge)
{
    /*
     * Badge values match channel IDs: the root task mints notification caps
     * with badge = CH_VMM_* when it wires up each service PD endpoint.
     *
     * CH_VMM_SERIAL / NET / BLOCK: async I/O completion from a service PD.
     * Other badge values: hardware IRQ forwarded to the active slot's vGIC.
     */
    if (badge == CH_VMM_CONTROLLER_PPC || badge == CH_VMM_CONTROLLER_EVT)
        return;

    if (badge == CH_VMM_SERIAL || badge == CH_VMM_NET || badge == CH_VMM_BLOCK) {
        if (mux->active_slot != 0xFF) {
            /* TODO: inject per-device VirtIO IRQ into the correct slot */
        }
        return;
    }

    /* Hardware IRQ — deliver to active slot's vGIC.
     * badge carries the IRQ handler cap (root task passes cap as badge
     * for hardware IRQ notifications). */
    if (mux->active_slot != 0xFF) {
        bool handled = virq_handle_passthrough((seL4_CPtr)badge);
        (void)handled;
    }
}

/* ─── vmm_mux_status ─────────────────────────────────────────────────────── */

void vmm_mux_status(const vm_mux_t *mux, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < VM_MAX_SLOTS && i < out_len; i++)
        out[i] = (uint8_t)mux->slots[i].state;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MMIO FAULT HANDLERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uintptr_t gpa_to_hva(const vm_slot_t *slot, uint64_t gpa)
{
    if (gpa < slot->ram_paddr || gpa >= slot->ram_paddr + slot->ram_size)
        return 0;
    return slot->ram_vaddr + (uintptr_t)(gpa - slot->ram_paddr);
}

/* ─── vmm_uart_fault — PL011 UART emulation → serial_pd ─────────────────── */

static bool vmm_uart_fault(size_t vcpu_id, size_t offset, size_t fsr,
                            seL4_UserContext *regs, void *data)
{
    vm_mux_t *mux = (vm_mux_t *)data;
    if (vcpu_id >= VM_MAX_SLOTS) return false;
    vm_slot_t *slot = &mux->slots[vcpu_id];
    if (slot->state == VM_SLOT_FREE || slot->serial_slot == UINT32_MAX)
        return false;

    if (fault_is_write(fsr)) {
        switch (offset) {
        case UART_DR_OFF: {
            uint64_t val = fault_get_data(regs, fsr);
            volatile uint8_t *shmem = (volatile uint8_t *)vmm_serial_shmem_vaddr;
            shmem[0] = (uint8_t)(val & 0xFFu);
            seL4_SetMR(0, MSG_SERIAL_WRITE);
            seL4_SetMR(1, slot->serial_slot);
            seL4_SetMR(2, 1);
            seL4_Call(g_ch_ep[CH_VMM_SERIAL],
                      seL4_MessageInfo_new(MSG_SERIAL_WRITE, 0, 0, 3));
            break;
        }
        default:
            break;
        }
        fault_advance_vcpu(vcpu_id, regs);
        return true;
    }

    uint64_t reg_val = 0;
    switch (offset) {
    case UART_DR_OFF: {
        seL4_SetMR(0, MSG_SERIAL_READ);
        seL4_SetMR(1, slot->serial_slot);
        seL4_SetMR(2, 1);
        seL4_Call(g_ch_ep[CH_VMM_SERIAL],
                  seL4_MessageInfo_new(MSG_SERIAL_READ, 0, 0, 3));
        uint32_t count = (uint32_t)seL4_GetMR(0);
        if (count > 0) {
            volatile uint8_t *shmem = (volatile uint8_t *)vmm_serial_shmem_vaddr;
            reg_val = shmem[0];
        } else {
            reg_val = 0xFFFFFFFFu;
        }
        break;
    }
    case UART_FR_OFF:
        reg_val = UART_FR_RXFE;
        break;
    case UART_MIS_OFF:
        reg_val = 0;
        break;
    default:
        reg_val = 0;
        break;
    }

    fault_advance(vcpu_id, regs,
                  GUEST_UART_PADDR + offset, fsr, reg_val);
    return true;
}

/* ─── VirtIO MMIO register model ─────────────────────────────────────────── */

static uint32_t vmm_virtio_reg_read(vmm_virtio_state_t *dev, size_t offset)
{
    switch (offset) {
    case REG_VIRTIO_MMIO_MAGIC_VALUE:         return VIRTIO_MMIO_DEV_MAGIC;
    case REG_VIRTIO_MMIO_VERSION:             return VIRTIO_MMIO_DEV_VERSION;
    case REG_VIRTIO_MMIO_DEVICE_ID:           return dev->DeviceID;
    case REG_VIRTIO_MMIO_VENDOR_ID:           return VIRTIO_MMIO_DEV_VENDOR_ID;
    case REG_VIRTIO_MMIO_DEVICE_FEATURES:     return 0;
    case REG_VIRTIO_MMIO_QUEUE_NUM_MAX:       return dev->QueueNumMax;
    case REG_VIRTIO_MMIO_QUEUE_READY:         return dev->QueueReady ? 1u : 0u;
    case REG_VIRTIO_MMIO_INTERRUPT_STATUS:    return dev->InterruptStatus;
    case REG_VIRTIO_MMIO_STATUS:              return dev->Status;
    case REG_VIRTIO_MMIO_CONFIG_GENERATION:   return dev->ConfigGeneration;
    default:                                  return 0;
    }
}

static void vmm_virtio_reg_write(vmm_virtio_state_t *dev, size_t offset,
                                  uint32_t val)
{
    switch (offset) {
    case REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL: dev->DeviceFeaturesSel  = val; break;
    case REG_VIRTIO_MMIO_DRIVER_FEATURES:     dev->DriverFeatures      = val; break;
    case REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL: dev->DriverFeaturesSel   = val; break;
    case REG_VIRTIO_MMIO_QUEUE_SEL:           dev->QueueSel             = val; break;
    case REG_VIRTIO_MMIO_QUEUE_NUM:           dev->QueueNum             = val; break;
    case REG_VIRTIO_MMIO_QUEUE_READY:         dev->QueueReady           = (val == 1); break;
    case REG_VIRTIO_MMIO_INTERRUPT_ACK:
        dev->InterruptStatus &= ~val;
        break;
    case REG_VIRTIO_MMIO_STATUS:
        dev->Status = val;
        if (val == 0) {
            dev->QueueReady      = false;
            dev->QueueNum        = 0;
            dev->InterruptStatus = 0;
            dev->last_avail_idx  = 0;
            dev->QueueDescAddr   = 0;
            dev->QueueAvailAddr  = 0;
            dev->QueueUsedAddr   = 0;
        }
        break;
    case REG_VIRTIO_MMIO_QUEUE_DESC_LOW:
        dev->QueueDescAddr  = (dev->QueueDescAddr & 0xFFFFFFFF00000000ULL) | (uint64_t)val;
        break;
    case REG_VIRTIO_MMIO_QUEUE_DESC_HIGH:
        dev->QueueDescAddr  = (dev->QueueDescAddr & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    case REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        dev->QueueAvailAddr = (dev->QueueAvailAddr & 0xFFFFFFFF00000000ULL) | (uint64_t)val;
        break;
    case REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        dev->QueueAvailAddr = (dev->QueueAvailAddr & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    case REG_VIRTIO_MMIO_QUEUE_USED_LOW:
        dev->QueueUsedAddr  = (dev->QueueUsedAddr & 0xFFFFFFFF00000000ULL) | (uint64_t)val;
        break;
    case REG_VIRTIO_MMIO_QUEUE_USED_HIGH:
        dev->QueueUsedAddr  = (dev->QueueUsedAddr & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    default:
        break;
    }
}

/* ─── vmm_blk_fault — VirtIO block emulation → block_pd ─────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} vmm_blk_req_hdr_t;

#define VIRTIO_BLK_T_IN    0u
#define VIRTIO_BLK_T_OUT   1u
#define VIRTIO_BLK_T_FLUSH 4u

#define VIRTIO_BLK_S_OK    0u
#define VIRTIO_BLK_S_IOERR 1u

static bool vmm_blk_fault(size_t vcpu_id, size_t offset, size_t fsr,
                           seL4_UserContext *regs, void *data)
{
    vm_slot_t *slot = (vm_slot_t *)data;
    vmm_virtio_state_t *dev = &slot->vblk;

    if (fault_is_write(fsr)) {
        uint32_t val = (uint32_t)fault_get_data(regs, fsr);
        if (offset == REG_VIRTIO_MMIO_QUEUE_NOTIFY && dev->QueueReady
            && slot->block_handle != UINT32_MAX) {

            uintptr_t avail_hva = gpa_to_hva(slot, dev->QueueAvailAddr);
            uintptr_t desc_hva  = gpa_to_hva(slot, dev->QueueDescAddr);
            uintptr_t used_hva  = gpa_to_hva(slot, dev->QueueUsedAddr);

            if (!avail_hva || !desc_hva || !used_hva) {
                fault_advance_vcpu(vcpu_id, regs);
                return true;
            }

            volatile uint16_t *avail_ring = (volatile uint16_t *)avail_hva;
            volatile uint16_t  avail_idx  = avail_ring[1];
            volatile uint16_t *used_ring  = (volatile uint16_t *)used_hva;
            volatile uint32_t *used_elems = (volatile uint32_t *)(used_hva + 4);

            typedef struct __attribute__((packed)) {
                uint64_t addr;
                uint32_t len;
                uint16_t flags;
                uint16_t next;
            } vq_desc_t;
            volatile vq_desc_t *descs = (volatile vq_desc_t *)desc_hva;

            while (dev->last_avail_idx != avail_idx) {
                uint16_t ring_idx = dev->last_avail_idx % dev->QueueNum;
                uint16_t desc_idx = avail_ring[2 + ring_idx];

                uintptr_t hdr_hva = gpa_to_hva(slot, descs[desc_idx].addr);
                if (!hdr_hva) { dev->last_avail_idx++; continue; }

                volatile vmm_blk_req_hdr_t *hdr =
                    (volatile vmm_blk_req_hdr_t *)hdr_hva;
                uint32_t req_type = hdr->type;
                uint64_t sector   = hdr->sector;

                uint16_t data_desc = descs[desc_idx].next;
                uintptr_t data_hva = gpa_to_hva(slot, descs[data_desc].addr);
                uint32_t  data_len = descs[data_desc].len;

                uint16_t stat_desc = descs[data_desc].next;
                uintptr_t stat_hva = gpa_to_hva(slot, descs[stat_desc].addr);

                uint8_t status_byte = VIRTIO_BLK_S_IOERR;

                if (data_hva && stat_hva) {
                    if (req_type == VIRTIO_BLK_T_IN) {
                        seL4_SetMR(0, MSG_BLOCK_READ);
                        seL4_SetMR(1, slot->block_handle);
                        seL4_SetMR(2, (uint32_t)(sector & 0xFFFFFFFFu));
                        seL4_SetMR(3, (uint32_t)(data_len / 512u));
                        seL4_Call(g_ch_ep[CH_VMM_BLOCK],
                                  seL4_MessageInfo_new(MSG_BLOCK_READ, 0, 0, 4));
                        if (seL4_GetMR(0)) {
                            volatile uint8_t *shmem =
                                (volatile uint8_t *)vmm_block_shmem_vaddr;
                            memcpy((void *)data_hva, (void *)shmem, data_len);
                            status_byte = VIRTIO_BLK_S_OK;
                        }
                    } else if (req_type == VIRTIO_BLK_T_OUT) {
                        volatile uint8_t *shmem =
                            (volatile uint8_t *)vmm_block_shmem_vaddr;
                        memcpy((void *)shmem, (void *)data_hva, data_len);
                        seL4_SetMR(0, MSG_BLOCK_WRITE);
                        seL4_SetMR(1, slot->block_handle);
                        seL4_SetMR(2, (uint32_t)(sector & 0xFFFFFFFFu));
                        seL4_SetMR(3, (uint32_t)(data_len / 512u));
                        seL4_Call(g_ch_ep[CH_VMM_BLOCK],
                                  seL4_MessageInfo_new(MSG_BLOCK_WRITE, 0, 0, 4));
                        status_byte = seL4_GetMR(0)
                                      ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
                    } else {
                        status_byte = VIRTIO_BLK_S_OK;
                    }
                    *(volatile uint8_t *)stat_hva = status_byte;
                }

                uint16_t used_idx = used_ring[1];
                used_elems[2 * (used_idx % dev->QueueNum)]     = desc_idx;
                used_elems[2 * (used_idx % dev->QueueNum) + 1] = data_len;
                used_ring[1] = used_idx + 1;

                dev->last_avail_idx++;
            }

            dev->InterruptStatus |= 1u;
            virq_inject(slot->vcpu_id);

        } else {
            vmm_virtio_reg_write(dev, offset, val);
        }
        fault_advance_vcpu(vcpu_id, regs);
        return true;
    }

    uint32_t reg_val = vmm_virtio_reg_read(dev, offset);
    fault_advance(vcpu_id, regs,
                  GUEST_VIRTIO_BLK_PADDR(slot->id) + offset, fsr,
                  (uint64_t)reg_val);
    return true;
}

/* ─── vmm_net_fault — VirtIO net emulation → net_pd ─────────────────────── */

static bool vmm_net_fault(size_t vcpu_id, size_t offset, size_t fsr,
                           seL4_UserContext *regs, void *data)
{
    vm_slot_t *slot = (vm_slot_t *)data;
    vmm_virtio_state_t *dev = &slot->vnet;

    if (fault_is_write(fsr)) {
        uint32_t val = (uint32_t)fault_get_data(regs, fsr);
        if (offset == REG_VIRTIO_MMIO_QUEUE_NOTIFY && dev->QueueReady
            && slot->net_handle != UINT32_MAX) {

            uintptr_t avail_hva = gpa_to_hva(slot, dev->QueueAvailAddr);
            uintptr_t desc_hva  = gpa_to_hva(slot, dev->QueueDescAddr);
            uintptr_t used_hva  = gpa_to_hva(slot, dev->QueueUsedAddr);

            if (!avail_hva || !desc_hva || !used_hva) {
                fault_advance_vcpu(vcpu_id, regs);
                return true;
            }

            volatile uint16_t *avail_ring = (volatile uint16_t *)avail_hva;
            volatile uint16_t  avail_idx  = avail_ring[1];
            volatile uint16_t *used_ring  = (volatile uint16_t *)used_hva;
            volatile uint32_t *used_elems = (volatile uint32_t *)(used_hva + 4);

            typedef struct __attribute__((packed)) {
                uint64_t addr;
                uint32_t len;
                uint16_t flags;
                uint16_t next;
            } vq_desc_t;
            volatile vq_desc_t *descs = (volatile vq_desc_t *)desc_hva;

            while (dev->last_avail_idx != avail_idx) {
                uint16_t ring_idx = dev->last_avail_idx % dev->QueueNum;
                uint16_t desc_idx = avail_ring[2 + ring_idx];

                uintptr_t frame_hva = gpa_to_hva(slot, descs[desc_idx].addr);
                uint32_t  frame_len = descs[desc_idx].len;

                if (frame_hva && frame_len > 0) {
                    volatile uint8_t *shmem =
                        (volatile uint8_t *)vmm_net_shmem_vaddr;
                    memcpy((void *)shmem, (void *)frame_hva, frame_len);
                    seL4_SetMR(0, MSG_NET_SEND);
                    seL4_SetMR(1, slot->net_handle);
                    seL4_SetMR(2, frame_len);
                    seL4_Call(g_ch_ep[CH_VMM_NET],
                              seL4_MessageInfo_new(MSG_NET_SEND, 0, 0, 3));
                }

                uint16_t used_idx = used_ring[1];
                used_elems[2 * (used_idx % dev->QueueNum)]     = desc_idx;
                used_elems[2 * (used_idx % dev->QueueNum) + 1] = frame_len;
                used_ring[1] = used_idx + 1;

                dev->last_avail_idx++;
            }

            dev->InterruptStatus |= 1u;
            virq_inject(slot->vcpu_id);

        } else {
            vmm_virtio_reg_write(dev, offset, val);
        }
        fault_advance_vcpu(vcpu_id, regs);
        return true;
    }

    uint32_t reg_val = vmm_virtio_reg_read(dev, offset);
    fault_advance(vcpu_id, regs,
                  GUEST_VIRTIO_NET_PADDR(slot->id) + offset, fsr,
                  (uint64_t)reg_val);
    return true;
}
