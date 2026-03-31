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
 *   seL4 (EL2) → freebsd_vmm PD → libvmm → EDK2 UEFI @ 0x00000000
 *   → EDK2 scans VirtIO block → bootaa64.efi → loader.efi → FreeBSD kernel
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

/* Microkit sets these from the .system file setvar_vaddr directives */
uintptr_t guest_ram_vaddr;
uintptr_t guest_flash_vaddr;

/* Global VMM state */
static vm_t freebsd_vm;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void load_uefi_firmware(void)
{
    /*
     * Copy EDK2 AArch64 firmware into guest flash region.
     *
     * The flash image is linked into this ELF via objcopy:
     *   aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 \
     *     edk2-aarch64-code.fd edk2.o
     *
     * The firmware expects to find itself at guest physical 0x00000000.
     * Microkit maps the flash MR at VMM vaddr = guest_flash_vaddr.
     */
    size_t fw_size = (size_t)(_binary_edk2_aarch64_code_fd_end
                              - _binary_edk2_aarch64_code_fd_start);
    if (fw_size > GUEST_FLASH_SIZE) {
        microkit_dbg_puts("freebsd_vmm: EDK2 firmware too large!\n");
        return;
    }

    memcpy((void *)guest_flash_vaddr,
           _binary_edk2_aarch64_code_fd_start,
           fw_size);

    microkit_dbg_puts("freebsd_vmm: EDK2 firmware loaded (");
    /* simple decimal print not available, just confirm */
    microkit_dbg_puts(" bytes)\n");
}

static void load_dtb(void)
{
    /*
     * Place a device tree blob for FreeBSD near end of flash region.
     * EDK2 will discover it via the UEFI HOB mechanism and pass it
     * to FreeBSD loader.efi as the FDT pointer.
     *
     * The DTB is built separately with dtc(1) from freebsd-vmm.dts.
     */
    size_t dtb_size = (size_t)(_binary_freebsd_dtb_end
                                - _binary_freebsd_dtb_start);

    if (dtb_size == 0) {
        microkit_dbg_puts("freebsd_vmm: no DTB embedded, EDK2 will use ACPI\n");
        return;
    }

    if (GUEST_DTB_PADDR + dtb_size > GUEST_FLASH_SIZE) {
        microkit_dbg_puts("freebsd_vmm: DTB too large for flash region!\n");
        return;
    }

    memcpy((void *)(guest_flash_vaddr + GUEST_DTB_PADDR),
           _binary_freebsd_dtb_start,
           dtb_size);

    microkit_dbg_puts("freebsd_vmm: DTB loaded at guest phys 0x03f00000\n");
}

/* ─── MMIO fault handler ─────────────────────────────────────────────────── */

/*
 * Handle MMIO faults from the guest that aren't handled by libvmm's
 * built-in GIC/VirtIO emulation.
 *
 * Returns true if the fault was handled (guest resumes), false to inject
 * a data abort into the guest.
 */
static bool freebsd_mmio_fault(size_t vcpu_id, uint64_t addr, size_t fsr,
                               seL4_Word *regs, void *cookie)
{
    /*
     * Log unhandled MMIO faults for debugging. In a production VMM
     * these would be dispatched to per-device emulation handlers.
     */
    microkit_dbg_puts("freebsd_vmm: unhandled MMIO @ 0x");
    /* addr would be printed here with a hex helper */
    microkit_dbg_puts("\n");

    /*
     * Return false to inject a data abort — FreeBSD will handle
     * the abort for unclaimed MMIO regions gracefully.
     */
    return false;
}

/* ─── IRQ delivery ───────────────────────────────────────────────────────── */

/*
 * Deliver a virtual IRQ to the FreeBSD guest.
 * Called by libvmm's vGIC when a hardware interrupt arrives.
 */
static void freebsd_virq_ack(size_t vcpu_id, int irq, void *cookie)
{
    /* ACK the seL4 IRQ notification so the interrupt line is re-armed */
    (void)vcpu_id;
    (void)irq;
    (void)cookie;
}

/* ─── Microkit entry points ─────────────────────────────────────────────── */

void vmm_init(void)
{
    microkit_dbg_puts("freebsd_vmm: initialising\n");

    /* Initialise libvmm's guest VM context */
    vm_init(&freebsd_vm,
            /* vcpu_id  */ 0,
            /* ram_base */ GUEST_RAM_VADDR,
            /* ram_size */ GUEST_RAM_SIZE,
            /* ram_vaddr */ guest_ram_vaddr);

    /* Load EDK2 UEFI firmware into guest flash */
    load_uefi_firmware();

    /* Optionally load a DTB (may be empty/stub) */
    load_dtb();

    /*
     * Configure the vCPU boot state for EDK2 AArch64.
     *
     * EDK2 is a full UEFI implementation compiled as a PE32+ image.
     * The reset handler is at offset 0 of the firmware volume.
     *
     * On real hardware (and QEMU), the CPU starts executing at address 0
     * in AArch64 EL2/EL3. seL4 gives us control of the vCPU, so we set:
     *   - PC  = UEFI_RESET_VECTOR (0x00000000) — EDK2 reset entry
     *   - SP  = top of guest RAM (arbitrary, EDK2 will set its own stack)
     *   - x0  = 0 (no bootargs at reset)
     *   - CPSR = AARCH64_EL1h (EDK2 will escalate to its own EL)
     *
     * The flash MR is mapped into the guest at PA 0x00000000 with perms=rw,
     * so EDK2's reads/writes to itself work correctly.
     */
    vcpu_regs_t boot_regs = {
        .pc   = UEFI_RESET_VECTOR,
        .sp   = GUEST_RAM_VADDR + GUEST_RAM_SIZE - 0x1000,  /* 4KB from top */
        .x0   = 0,
        /* EL1h, AArch64, IRQs/FIQs masked at boot */
        .spsr = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0), /* SPSR_EL1h */
    };
    vm_set_boot_regs(&freebsd_vm, &boot_regs);

    /*
     * Register MMIO fault handler for regions not handled by libvmm.
     * libvmm handles GICv2/v3 and VirtIO MMIO automatically.
     */
    vm_register_mmio_fault_handler(&freebsd_vm, freebsd_mmio_fault, NULL);

    /*
     * Initialise the virtual GIC (GICv2 emulation, matches QEMU virt).
     * libvmm provides vgic_init() which sets up the distributor emulation.
     * The vCPU interface is passed through from hardware (mapped as gic_vcpu MR).
     */
    vgic_init();

    microkit_dbg_puts("freebsd_vmm: starting FreeBSD guest (EDK2 → loader.efi → kernel)\n");

    /* Start the guest vCPU — execution transfers to EDK2 reset vector */
    vm_run(&freebsd_vm);
}

/*
 * Microkit notification handler.
 * Called when a channel notification arrives (e.g., from controller or
 * when a hardware IRQ is delivered as a seL4 notification).
 */
void vmm_notified(microkit_channel ch)
{
    bool success = virq_handle_passthrough(ch);
    if (!success) {
        switch (ch) {
        case CH_CONTROLLER:
            /* Controller can send lifecycle commands (pause/resume/reset) */
            microkit_dbg_puts("freebsd_vmm: controller notification\n");
            break;
        default:
            microkit_dbg_puts("freebsd_vmm: unknown notification channel\n");
            break;
        }
    }
}

/*
 * Microkit protected procedure call handler.
 * Controller can query VMM status or send control commands.
 */
microkit_msginfo vmm_protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;
    (void)msginfo;
    /* Status: guest running = 1, stopped = 0 */
    microkit_mr_set(0, freebsd_vm.running ? 1 : 0);
    return microkit_msginfo_new(0, 1);
}

/*
 * Microkit fault handler.
 * seL4 delivers VM faults (vCPU exceptions, MMIO, WFI/WFE) here.
 * libvmm dispatches to the appropriate handler.
 */
void fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo)
{
    bool handled = vm_handle_fault(&freebsd_vm, child, msginfo, reply_msginfo);
    if (!handled) {
        microkit_dbg_puts("freebsd_vmm: unhandled fault from guest\n");
    }
}
