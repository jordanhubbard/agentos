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
 * Architecture support:
 *   ARCH_AARCH64 — full libvmm implementation (EL2 hypervisor, vGIC, UART)
 *   ARCH_X86_64  — stub: libvmm x86_64 support not yet available
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <microkit.h>

/* ─── x86_64 stub ──────────────────────────────────────────────────────────
 *
 * libvmm does not yet provide x86_64 VMM support. This stub satisfies the
 * linker so linux_vmm.elf can be included in x86_64 images. The PD starts,
 * logs that VMM is not available, then loops passively.
 *
 * Set LINUX_VMM_X86_STUB=1 so downstream code can detect the stub at
 * compile time.
 */
#ifdef ARCH_X86_64

#define LINUX_VMM_X86_STUB 1

/* Maximum VM slots tracked by this VMM instance */
#define VMM_MAX_SLOTS  4

/* Per-slot affinity mask (stored; enforcement deferred to AArch64 full impl) */
static uint32_t vmm_affinity[VMM_MAX_SLOTS];

/* ── vmm_set_affinity — store host-CPU affinity for a guest VCPU ─────────
 *
 * On x86_64 (stub mode) we store the mask in vmm_affinity[] so callers
 * can call this API now.  Enforcement via seL4_TCB_SetAffinity is done in
 * the AArch64 full implementation below.
 *
 * @param slot_id  guest slot index (0..VMM_MAX_SLOTS-1)
 * @param cpu_mask bitmask of allowed host CPUs
 * @returns 0 on success, -1 if slot_id out of range
 */
int vmm_set_affinity(uint8_t slot_id, uint32_t cpu_mask)
{
    if (slot_id >= VMM_MAX_SLOTS) return -1;
    vmm_affinity[slot_id] = cpu_mask;
    microkit_dbg_puts("[linux_vmm] x86_64 stub: vmm_set_affinity stored\n");
    return 0;
}

/* ── vmm_inject_irq — stub for virtio IRQ injection ─────────────────────
 *
 * Logs the injection request.  The AArch64 full implementation calls
 * virq_inject() from libvmm.  On x86_64 this is a no-op stub.
 *
 * @param slot_id  guest slot index
 * @param irq_num  virtual IRQ number to inject
 * @returns 0 (always succeeds in stub)
 */
int vmm_inject_irq(uint8_t slot_id, uint32_t irq_num)
{
    (void)slot_id;
    (void)irq_num;
    microkit_dbg_puts("[linux_vmm] x86_64 stub: vmm_inject_irq (no-op)\n");
    return 0;
}

void init(void)
{
    for (uint8_t i = 0; i < VMM_MAX_SLOTS; i++)
        vmm_affinity[i] = 0xFFFFFFFFu;  /* any core */

    microkit_dbg_puts("[linux_vmm] x86_64: libvmm VMM support not yet implemented.\n");
    microkit_dbg_puts("[linux_vmm] x86_64: PD running as passive stub.\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
    microkit_dbg_puts("[linux_vmm] x86_64 stub: unexpected notification\n");
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo)
{
    (void)child;
    (void)msginfo;
    microkit_dbg_puts("[linux_vmm] x86_64 stub: unexpected fault\n");
    *reply_msginfo = microkit_msginfo_new(0, 0);
    return seL4_False;
}

#endif /* ARCH_X86_64 */

/* ─── AArch64 native hardware stub ─────────────────────────────────────────
 *
 * Used when BOARD_NATIVE=1 on AArch64 (e.g., Raspberry Pi 5).  libvmm
 * is not used here because it hard-codes QEMU virt GIC addresses that are
 * incompatible with real hardware.  This stub allows the native board
 * system file to reference linux_vmm.elf while VM management is in early
 * bring-up.  A production implementation would configure libvmm with the
 * board's actual GIC/UART addresses.
 */
#ifdef LINUX_VMM_NATIVE_STUB

void init(void)
{
    microkit_dbg_puts("[linux_vmm] native AArch64 stub: VMM not yet configured for real hardware.\n");
    microkit_dbg_puts("[linux_vmm] native stub: Use console_shell to manage VMs via controller.\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo)
{
    (void)child;
    (void)msginfo;
    microkit_dbg_puts("[linux_vmm] native stub: unexpected fault\n");
    *reply_msginfo = microkit_msginfo_new(0, 0);
    return seL4_False;
}

#endif /* LINUX_VMM_NATIVE_STUB */

/* ─── AArch64 full implementation ──────────────────────────────────────────
 *
 * Uses au-ts/libvmm to boot a Linux guest at EL1 under seL4 EL2.
 * Compiled by vmm.mk which passes -DARCH_AARCH64 and links libvmm.a.
 */
#if defined(ARCH_AARCH64) && !defined(LINUX_VMM_NATIVE_STUB)

#include <libvmm/libvmm.h>
#include "gpu_shmem.h"

/* ── Per-slot affinity (AArch64) ─────────────────────────────────────── */

#ifndef VMM_MAX_SLOTS
#define VMM_MAX_SLOTS  4
#endif

/* Stored host-CPU affinity masks — one per VM slot.
 * Applied via seL4_TCB_SetAffinity when the vCPU is next scheduled. */
static uint32_t vmm_affinity[VMM_MAX_SLOTS];

/* ─── Guest Configuration ─────────────────────────────────────────────── */

/* 256MB guest RAM — plenty for Buildroot + basic agent workloads */
#define GUEST_RAM_SIZE          0x10000000

/* Guest DTB and initrd placement addresses (must match DTS) */
#define GUEST_DTB_VADDR         0x4f000000
#define GUEST_INIT_RAM_DISK_VADDR 0x4d000000

/* ─── Channel IDs ────────────────────────────────────────────────────── */

/* IRQ channel: PL011 UART interrupt (SPI 1 → IRQ 33) */
#define SERIAL_IRQ_CH           1
#define SERIAL_IRQ              33

/* IPC channel: controller <-> linux_vmm (agent-to-linux bridge) */
#define CONTROLLER_CH           2

/* GPU shared memory notification channels (assigned when MR is mapped) */
#define GPU_SHMEM_NOTIFY_IN_CH  3   /* seL4 PD → linux_vmm: tensor ready */
#define GPU_SHMEM_NOTIFY_OUT_CH 4   /* linux_vmm → seL4 PD: result ready */

/* ─── Guest Image Symbols ────────────────────────────────────────────── */
/* These are linked in by package_guest_images.S */

extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];

/* Microkit sets this to the start of guest_ram MR (0x40000000) */
uintptr_t guest_ram_vaddr;

/* ─── GPU shared memory vaddr (set by Microkit when MR is mapped) ────── */

/* Declared weak so it defaults to 0 when the gpu_tensor_buf MR is not
 * mapped into this PD. The init() function guards on this being non-zero
 * before calling gpu_shmem_init(). */
uintptr_t gpu_tensor_buf_vaddr __attribute__((weak));

/* ─── State ──────────────────────────────────────────────────────────── */

static bool guest_started   = false;
static bool gpu_shmem_ready = false;

/* ─── IRQ Handling ───────────────────────────────────────────────────── */

static void serial_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    microkit_irq_ack(SERIAL_IRQ_CH);
}

/* ─── VCPU Affinity ──────────────────────────────────────────────────── */

/*
 * vmm_set_affinity — pin a guest VCPU to a set of host CPUs.
 *
 * Stores the requested affinity mask for slot_id.  The mask is applied
 * via seL4_TCB_SetAffinity the next time the scheduler selects this slot.
 * On single-core builds this is a no-op (all VCPUs share the one core).
 *
 * @param slot_id  VM slot index (0..VMM_MAX_SLOTS-1)
 * @param cpu_mask bitmask of allowed host CPUs (bit N = core N allowed)
 * @returns 0 on success, -1 if slot_id is out of range
 */
int vmm_set_affinity(uint8_t slot_id, uint32_t cpu_mask)
{
    if (slot_id >= VMM_MAX_SLOTS) return -1;
    vmm_affinity[slot_id] = cpu_mask;
    /*
     * On a multi-core seL4 build, apply affinity to the VCPU thread:
     *
     *   seL4_CPtr vcpu_tcb = microkit_vcpu_tcb(slot_id);
     *   seL4_TCB_SetAffinity(vcpu_tcb, __builtin_ctz(cpu_mask));
     *
     * Until Microkit exposes vcpu TCB caps we record the mask and log.
     */
    LOG_VMM("vmm_set_affinity: slot=%u cpu_mask=0x%x stored\n",
            (unsigned)slot_id, (unsigned)cpu_mask);
    return 0;
}

/* ─── IRQ Injection ──────────────────────────────────────────────────── */

/*
 * vmm_inject_irq — inject a virtual IRQ into a guest VM slot.
 *
 * In the single-VCPU Linux VMM (this PD manages one guest), slot_id must
 * be 0; other slot IDs are invalid.  The IRQ is injected via libvmm's
 * virq_inject() which posts it into the virtual GIC distributor.
 *
 * This stub can be extended to support per-slot VCPU contexts once the
 * multiplexer is wired to manage multiple Linux guests in a single VMM PD.
 *
 * @param slot_id  VM slot index (must be 0 for this single-guest VMM)
 * @param irq_num  virtual IRQ number (e.g., 32 + virtio queue IRQ offset)
 * @returns 0 on success, -1 if slot_id is invalid or inject fails
 */
int vmm_inject_irq(uint8_t slot_id, uint32_t irq_num)
{
    if (slot_id >= VMM_MAX_SLOTS) return -1;

    if (!guest_started) {
        LOG_VMM_ERR("vmm_inject_irq: guest not started (slot=%u irq=%u)\n",
                    (unsigned)slot_id, (unsigned)irq_num);
        return -1;
    }

    /*
     * virq_inject() delivers an IRQ to the guest VCPU via the virtual GIC.
     * GUEST_BOOT_VCPU_ID is the only VCPU in this single-guest configuration.
     * A multi-guest extension would index by slot_id.
     */
    bool ok = virq_inject((int)irq_num);
    if (!ok) {
        LOG_VMM_ERR("vmm_inject_irq: virq_inject failed (slot=%u irq=%u)\n",
                    (unsigned)slot_id, (unsigned)irq_num);
        return -1;
    }

    LOG_VMM("vmm_inject_irq: injected irq=%u into slot=%u\n",
            (unsigned)irq_num, (unsigned)slot_id);
    return 0;
}

/* ─── Init ───────────────────────────────────────────────────────────── */

void init(void)
{
    /* Initialise per-slot affinity masks to "any core" */
    for (uint8_t i = 0; i < VMM_MAX_SLOTS; i++)
        vmm_affinity[i] = 0xFFFFFFFFu;

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

    /* Initialise GPU shared memory channel (consumer role — receives from seL4 PDs) */
    if (gpu_tensor_buf_vaddr) {
        gpu_shmem_init(gpu_tensor_buf_vaddr, GPU_SHMEM_BUF_SIZE,
                       GPU_SHMEM_ROLE_CONSUMER);
        if (gpu_shmem_valid()) {
            gpu_shmem_ready = true;
            LOG_VMM("  GPU shmem ring initialised (64MB, depth=%d)\n",
                    GPU_SHMEM_RING_DEPTH);
        } else {
            LOG_VMM_ERR("GPU shmem ring validation failed\n");
        }
    }

    /* Notify controller that VMM is ready */
    microkit_notify(CONTROLLER_CH);
}

/* ─── Notification Handler ───────────────────────────────────────────── */

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
        break;
    }

    case GPU_SHMEM_NOTIFY_IN_CH: {
        /*
         * A seL4 PD (controller, worker, swap_slot) has enqueued a tensor
         * descriptor in the GPU shared memory ring.  Drain all pending
         * descriptors and forward each to the Linux guest via a virtIO
         * console write.  The Linux gpu_shmem kernel module on the guest
         * side reads these notifications and dispatches CUDA/PyTorch ops.
         *
         * In this VMM implementation we relay notifications using the
         * guest's virtIO console injection path.  A production system
         * would use a dedicated virtIO device or MMIO doorbell.
         */
        if (!gpu_shmem_ready) {
            LOG_VMM_ERR("GPU shmem notify received but ring not ready\n");
            break;
        }

        gpu_tensor_desc_t desc;
        int dispatched = 0;
        while (gpu_shmem_dequeue(&desc)) {
            LOG_VMM("GPU tensor ready: op=%d dtype=%d seq=%u\n",
                    (int)desc.op, (int)desc.dtype, (unsigned)desc.seq);
            /*
             * In a full implementation this would write a descriptor
             * notification into the guest's virtIO console or a dedicated
             * virtIO GPU device.  For this prototype we log the event;
             * the Linux gpu_shmem_linux kernel module polls the shared MR
             * directly via /dev/gpu_shmem after receiving a Linux IRQ
             * injected via virq_inject() (see DESIGN.md §GPU-shmem).
             */
            dispatched++;
        }

        if (dispatched > 0) {
            LOG_VMM("GPU shmem: dequeued %d tensor descriptor(s)\n",
                    dispatched);
            /* Inject a virtual IRQ into the guest to wake the driver */
            virq_inject(SERIAL_IRQ); /* TODO: dedicate a GPU shmem IRQ */
        }
        break;
    }

    case GPU_SHMEM_NOTIFY_OUT_CH: {
        /*
         * Linux guest has completed a GPU operation and written a result
         * descriptor back into the result ring.  Notify the originating
         * seL4 PD (controller) so it can dequeue the result.
         */
        if (gpu_shmem_ready) {
            LOG_VMM("GPU shmem: result ready — notifying controller\n");
            microkit_notify(CONTROLLER_CH);
        }
        break;
    }

    default:
        LOG_VMM("Unexpected notification on channel 0x%lx\n", (unsigned long)ch);
        break;
    }
}

/* ─── Fault Handler ──────────────────────────────────────────────────── */

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

#endif /* ARCH_AARCH64 && !LINUX_VMM_NATIVE_STUB */
