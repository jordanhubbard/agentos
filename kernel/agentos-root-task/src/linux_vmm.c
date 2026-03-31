/*
 * agentOS Linux VMM — Virtual Machine Monitor
 *
 * Boots a Linux guest inside agentOS using libvmm on seL4/Microkit.
 * The VMM bridges IPC between native agentOS agents (via controller)
 * and the Linux guest, enabling agents to run on a full Linux userland
 * while remaining under seL4 capability isolation.
 *
 * Based on au-ts/libvmm examples/simple, extended with agentOS IPC.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <microkit.h>
#include <libvmm/libvmm.h>

/* ─── Guest Configuration ──────────────────────────────────────────────── */

/* 256MB guest RAM — plenty for Buildroot + basic agent workloads */
#define GUEST_RAM_SIZE          0x10000000

/* Guest DTB and initrd placement addresses (must match DTS) */
#define GUEST_DTB_VADDR         0x4f000000
#define GUEST_INIT_RAM_DISK_VADDR 0x4d000000

/* ─── Channel IDs ──────────────────────────────────────────────────────── */

/* IRQ channel: PL011 UART interrupt (SPI 1 → IRQ 33) */
#define SERIAL_IRQ_CH           1
#define SERIAL_IRQ              33

/* IPC channel: controller <-> linux_vmm (agent-to-linux bridge) */
#define CONTROLLER_CH           2

/* ─── Guest Image Symbols ──────────────────────────────────────────────── */
/* These are linked in by package_guest_images.S */

extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];

/* Microkit sets this to the start of guest_ram MR (0x40000000) */
uintptr_t guest_ram_vaddr;

/* ─── State ────────────────────────────────────────────────────────────── */

static bool guest_started = false;

/* ─── IRQ Handling ─────────────────────────────────────────────────────── */

static void serial_ack(size_t vcpu_id, int irq, void *cookie)
{
    microkit_irq_ack(SERIAL_IRQ_CH);
}

/* ─── Init ─────────────────────────────────────────────────────────────── */

void init(void)
{
    LOG_VMM("agentOS linux_vmm starting \"%s\"\n", microkit_name);
    LOG_VMM("  Guest RAM: 0x%lx (%d MB)\n",
            (unsigned long)guest_ram_vaddr, GUEST_RAM_SIZE / (1024 * 1024));

    /* Place guest images in RAM */
    size_t kernel_size = _guest_kernel_image_end - _guest_kernel_image;
    size_t dtb_size    = _guest_dtb_image_end - _guest_dtb_image;
    size_t initrd_size = _guest_initrd_image_end - _guest_initrd_image;

    LOG_VMM("  Kernel: %zu bytes\n", kernel_size);
    LOG_VMM("  DTB:    %zu bytes\n", dtb_size);
    LOG_VMM("  Initrd: %zu bytes\n", initrd_size);

    uintptr_t kernel_pc = linux_setup_images(
        guest_ram_vaddr,
        (uintptr_t)_guest_kernel_image, kernel_size,
        (uintptr_t)_guest_dtb_image, GUEST_DTB_VADDR, dtb_size,
        (uintptr_t)_guest_initrd_image, GUEST_INIT_RAM_DISK_VADDR, initrd_size
    );

    if (!kernel_pc) {
        LOG_VMM_ERR("Failed to initialise guest images\n");
        return;
    }

    LOG_VMM("  Kernel entry: 0x%lx\n", (unsigned long)kernel_pc);

    /* Initialise the virtual GIC driver */
    bool success = virq_controller_init();
    if (!success) {
        LOG_VMM_ERR("Failed to initialise emulated interrupt controller\n");
        return;
    }

    /* Register UART IRQ passthrough */
    success = virq_register(GUEST_BOOT_VCPU_ID, SERIAL_IRQ, &serial_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register serial IRQ\n");
        return;
    }

    /* Ack any pending IRQ */
    microkit_irq_ack(SERIAL_IRQ_CH);

    /* Start the guest! */
    LOG_VMM("  Starting Linux guest...\n");
    guest_start(kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
    guest_started = true;

    LOG_VMM("  Linux guest started successfully\n");

    /* Notify controller that VMM is ready */
    microkit_notify(CONTROLLER_CH);
}

/* ─── Notification Handler ─────────────────────────────────────────────── */

void notified(microkit_channel ch)
{
    switch (ch) {
    case SERIAL_IRQ_CH: {
        /* UART interrupt from hardware → inject into guest */
        bool success = virq_inject(SERIAL_IRQ);
        if (!success) {
            LOG_VMM_ERR("IRQ %d dropped on inject\n", SERIAL_IRQ);
        }
        break;
    }

    case CONTROLLER_CH: {
        /*
         * Controller sent us a notification. This is the agent-to-linux
         * bridge channel. For now, we just log it. Future: read a command
         * from shared memory and forward to the guest via virtIO console.
         */
        LOG_VMM("Received notification from controller (agent bridge)\n");
        /* TODO: Read command from shared memory region, execute in guest */
        break;
    }

    default:
        LOG_VMM("Unexpected notification on channel 0x%lx\n", (unsigned long)ch);
        break;
    }
}

/* ─── Fault Handler ────────────────────────────────────────────────────── */

/*
 * After init, the VMM's main job is fault handling. When the guest causes
 * an exception (MMIO access to unpassedthrough device, etc.), it arrives here.
 * libvmm's fault_handle() deals with GIC emulation and other traps.
 */
seL4_Bool fault(microkit_child child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo)
{
    bool success = fault_handle(child, msginfo);
    if (success) {
        *reply_msginfo = microkit_msginfo_new(0, 0);
        return seL4_True;
    }

    LOG_VMM_ERR("Unhandled fault from child %d\n", child);
    return seL4_False;
}
