/*
 * agentOS FreeBSD VMM — seL4-native protection domain
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * seL4 raw-IPC entry point for the FreeBSD VM multiplexer.
 * Handles IPC from controller (create/destroy/switch VMs) and
 * dispatches vCPU faults and IRQ notifications to the correct slot.
 *
 * Boot flow (first VM auto-created at init):
 *   freebsd_vmm_main() → vmm_init() → vmm_register() →
 *   vmm_mux_init() → vmm_mux_create("freebsd-0") →
 *   EDK2 UEFI → bootaa64.efi → loader.efi → FreeBSD kernel
 *
 * Subsequent VMs are created on demand via controller IPC:
 *   controller seL4_Call OP_VM_CREATE → second FreeBSD instance boots in slot 1
 *   controller seL4_Call OP_VM_SWITCH 1 → console switches to slot 1
 *   controller seL4_Call OP_VM_DESTROY 0 → slot 0 halted and freed
 *
 * Ring-1 consumer model:
 *   All device I/O is mediated through ring-0 service PDs.
 *   freebsd_vmm holds no direct hardware capabilities after init.
 *   Microkit has been deliberately removed; all IPC is raw seL4.
 */

#include <sel4/sel4.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/tcb.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/vmm_caps.h>

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vmm.h"
#include "vmm_mux.h"

/* Phase 3 — guest OS binding contract (guest_contract.h §3.1 compliance) */
#include "../../kernel/agentos-root-task/include/contracts/freebsd_vmm_contract.h"

/* PD name required by vmm_caps.h */
const char vmm_pd_name[] = "freebsd_vmm";

/* Badge bit the root task sets on fault IPC messages from VCPU threads */
#define VMM_FAULT_BADGE_BIT  ((seL4_Word)1u << 62)

/* Shared memory region vaddrs (populated by root task / linker) */
uintptr_t vmm_serial_shmem_vaddr;
uintptr_t vmm_block_shmem_vaddr;
uintptr_t vmm_net_shmem_vaddr;

/* Global multiplexer state */
static vm_mux_t g_mux;

/* Endpoint cap for calling the root task (MSG_VMM_*) */
static seL4_CPtr g_kernel_ep;

/* ─── VMM registration ───────────────────────────────────────────────────── */

static bool vmm_register(void)
{
    struct vmm_register_req req = {
        .os_type    = VMM_OS_TYPE_FREEBSD,
        .flags      = VMM_FLAG_SMP,
        .max_guests = VM_MAX_SLOTS,
        .name       = "freebsd_vmm",
    };
    (void)req;

    seL4_SetMR(0, VMM_OS_TYPE_FREEBSD);
    seL4_SetMR(1, VMM_FLAG_SMP);
    seL4_SetMR(2, VM_MAX_SLOTS);

    seL4_MessageInfo_t reply = seL4_Call(
        g_kernel_ep,
        seL4_MessageInfo_new(MSG_VMM_REGISTER, 0, 0, 3));
    (void)reply;

    uint32_t ok = (uint32_t)seL4_GetMR(0);
    if (!ok) {
        vmm_dbg_puts("vmm_register: root-task rejected VMM registration\n");
        return false;
    }

    g_mux.vmm_token = (uint32_t)seL4_GetMR(1);
    vmm_dbg_puts("vmm_register: registered, token acquired\n");
    return true;
}

/* ─── Guest binding protocol (guest_contract.h §3.1) ────────────────────── */

static void freebsd_vmm_binding_init(void)
{
    vmm_dbg_puts("freebsd_vmm: binding protocol — "
                 "device opens handled per-slot in vmm_mux_create()\n");
    vmm_dbg_puts("freebsd_vmm: EVENT_GUEST_READY deferred "
                 "(eventbus ep not yet wired)\n");
}

/* ─── Main entry point ───────────────────────────────────────────────────── */

void vmm_init(seL4_CPtr kernel_ep)
{
    g_kernel_ep = kernel_ep;

    vmm_dbg_puts("\n");
    vmm_dbg_puts("agentOS FreeBSD VM Multiplexer\n");
    vmm_dbg_puts("seL4 (EL2) -> EDK2 -> FreeBSD (EL1)\n");
    vmm_dbg_puts("\n");

    if (!vmm_register()) {
        vmm_dbg_puts("vmm_init: FATAL — VMM registration failed\n");
        return;
    }

    freebsd_vmm_binding_init();
    vmm_mux_init(&g_mux);

    uint8_t slot = vmm_mux_create(&g_mux, "freebsd-0");
    if (slot == 0xFF) {
        vmm_dbg_puts("vmm_init: FATAL — could not create initial VM\n");
        return;
    }

    vmm_dbg_puts("vmm_init: FreeBSD VM slot ");
    char c[2] = {'0' + slot, '\0'};
    vmm_dbg_puts(c);
    vmm_dbg_puts(" booting...\n");
}

/* ─── Notification dispatch ──────────────────────────────────────────────── */

void vmm_notified(seL4_Word badge)
{
    vmm_mux_handle_notify(&g_mux, badge);
}

/* ─── Protected procedure call dispatch ──────────────────────────────────── */

seL4_MessageInfo_t vmm_protected(seL4_Word badge, seL4_MessageInfo_t msginfo)
{
    (void)badge;

    uint32_t op = (uint32_t)seL4_MessageInfo_get_label(msginfo);

    switch (op) {

    case OP_VM_CREATE: {
        char label[16] = "freebsd-X";
        uint8_t slot = vmm_mux_create(&g_mux, label);
        if (slot != 0xFF)
            label[8] = (char)('0' + slot);
        seL4_SetMR(0, slot);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }

    case OP_VM_DESTROY: {
        uint8_t slot_id = (uint8_t)seL4_GetMR(0);
        int rc = vmm_mux_destroy(&g_mux, slot_id);
        seL4_SetMR(0, rc == 0 ? 0 : 1);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }

    case OP_VM_SWITCH: {
        uint8_t slot_id = (uint8_t)seL4_GetMR(0);
        int rc = vmm_mux_switch(&g_mux, slot_id);
        seL4_SetMR(0, rc == 0 ? 0 : 1);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }

    case OP_VM_STATUS: {
        uint8_t status[VM_MAX_SLOTS];
        vmm_mux_status(&g_mux, status, VM_MAX_SLOTS);
        for (int i = 0; i < VM_MAX_SLOTS; i++)
            seL4_SetMR(i, status[i]);
        return seL4_MessageInfo_new(0, 0, 0, VM_MAX_SLOTS);
    }

    case OP_VM_LIST: {
        uint32_t count = 0;
        for (int i = 0; i < VM_MAX_SLOTS; i++) {
            if (g_mux.slots[i].state != VM_SLOT_FREE) {
                seL4_SetMR(count + 1,
                    ((uint32_t)i << 8) | (uint32_t)g_mux.slots[i].state);
                count++;
            }
        }
        seL4_SetMR(0, count);
        seL4_SetMR(count + 1, g_mux.active_slot);
        return seL4_MessageInfo_new(0, 0, 0, count + 2);
    }

    default:
        vmm_dbg_puts("vmm_protected: unknown opcode\n");
        seL4_SetMR(0, 0xFF);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }
}

/* ─── Fault dispatch ─────────────────────────────────────────────────────── */

static seL4_MessageInfo_t vmm_fault(seL4_Word vcpu_id,
                                    seL4_MessageInfo_t msginfo)
{
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    vmm_mux_handle_fault(&g_mux, vcpu_id, msginfo, &reply);
    return reply;
}

/* ─── seL4 server loop ───────────────────────────────────────────────────── */

void freebsd_vmm_main(seL4_CPtr ep, seL4_CPtr kernel_ep,
                      seL4_CPtr serial_ep, seL4_CPtr net_ep,
                      seL4_CPtr block_ep)
{
    /* Register channel→endpoint mappings before init */
    vmm_mux_set_channel_ep(CH_VMM_KERNEL_LOCAL, kernel_ep);
    vmm_mux_set_channel_ep(CH_VMM_SERIAL,       serial_ep);
    vmm_mux_set_channel_ep(CH_VMM_NET,          net_ep);
    vmm_mux_set_channel_ep(CH_VMM_BLOCK,        block_ep);

    vmm_init(kernel_ep);

    seL4_Word badge;
    while (1) {
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(info);

        if (badge & VMM_FAULT_BADGE_BIT) {
            /* VCPU fault — low bits of badge encode the vcpu_id */
            seL4_MessageInfo_t reply =
                vmm_fault(badge & ~VMM_FAULT_BADGE_BIT, info);
            seL4_Reply(reply);
        } else if (label == seL4_Fault_NullFault) {
            /* Notification: IRQ, virtio ring, or controller notify */
            vmm_notified(badge);
        } else {
            /* IPC call from controller: label = opcode */
            seL4_MessageInfo_t reply = vmm_protected(badge, info);
            seL4_Reply(reply);
        }
    }
}
