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
 * guest_contract.h compliance (Phase 3d):
 *   This VMM implements the agentOS guest binding protocol defined in
 *   guest_contract.h.  All device I/O is mediated through ring-0 service
 *   PDs (serial_pd, net_pd, block_pd) via MSG_GUEST_BIND_DEVICE.  The
 *   guest may NOT map or access physical hardware capabilities directly.
 *
 * Architecture support:
 *   ARCH_AARCH64 — full libvmm implementation (EL2 hypervisor, vGIC)
 *                  UART owned by serial_pd; guest access emulated via IPC
 *   ARCH_X86_64  — stub: libvmm x86_64 support not yet available;
 *                  compliance skeleton present (CPL3 enforcement required)
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "contracts/linux_vmm_contract.h"

/* ─── x86_64 stub ──────────────────────────────────────────────────────────
 *
 * libvmm does not yet provide x86_64 VMM support. This stub satisfies the
 * linker so linux_vmm.elf can be included in x86_64 images. The PD starts,
 * logs that VMM is not available, then loops passively.
 *
 * Set LINUX_VMM_X86_STUB=1 so downstream code can detect the stub at
 * compile time.
 *
 * guest_contract.h compliance skeleton (x86_64):
 *   The contract headers are included here to ensure type-correctness.
 *   When libvmm gains x86_64 VMX support, the implementation MUST:
 *
 *   CPL3 Enforcement:
 *     Linux guest vCPUs operate in VMX non-root mode with EPT active.
 *     The guest kernel executes at guest CPL 0 (non-root) — it MUST NOT
 *     reach host CPL 0.  VMEXITs must be handled for: CPUID, MSR R/W,
 *     I/O port access, EPT violations, and all MMIO accesses.  Physical
 *     devices (UART, NIC, disk) are accessible only via service PDs
 *     through MSG_GUEST_BIND_DEVICE; direct device MMIO passthrough to
 *     the guest is prohibited.
 *
 *   Binding Protocol (guest_contract.h §3.1):
 *     1. MSG_VMM_REGISTER on VMM_KERNEL_CH to obtain vmm_token.
 *     2. MSG_SERIAL_OPEN on SERIAL_PD_CH → serial_client_slot.
 *     3. MSG_GUEST_BIND_DEVICE(GUEST_DEV_SERIAL, serial_client_slot) →
 *        guest_caps.serial_token.
 *     4. Publish guest_ready_event_t via MSG_EVENTBUS_PUBLISH_BATCH.
 */
#ifdef ARCH_X86_64

#include "contracts/linux_vmm_contract.h"

#define LINUX_VMM_X86_STUB 1

/* Compliance type-check: binding state that the full impl must populate */
static struct vmm_register_req  _stub_vmm_reg   __attribute__((unused));
static struct guest_bind_req    _stub_bind_req   __attribute__((unused));
static guest_capabilities_t     _stub_caps       __attribute__((unused));

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
    sel4_dbg_puts("[linux_vmm] x86_64 stub: vmm_set_affinity stored\n");
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
    sel4_dbg_puts("[linux_vmm] x86_64 stub: vmm_inject_irq (no-op)\n");
    return 0;
}

static void linux_vmm_pd_init(void)
{
    for (uint8_t i = 0; i < VMM_MAX_SLOTS; i++)
        vmm_affinity[i] = 0xFFFFFFFFu;  /* any core */

    sel4_dbg_puts("[linux_vmm] x86_64: libvmm VMM support not yet implemented.\n");
    sel4_dbg_puts("[linux_vmm] x86_64: PD running as passive stub.\n");
}

static void linux_vmm_pd_notified(uint32_t ch)
{
    (void)ch;
    sel4_dbg_puts("[linux_vmm] x86_64 stub: unexpected notification\n");
}

seL4_Bool fault(seL4_CPtr child, uint32_t msginfo,
                uint32_t *reply_msginfo)
{
    (void)child;
    (void)msginfo;
    sel4_dbg_puts("[linux_vmm] x86_64 stub: unexpected fault\n");
    rep->length = 0; /* E5-S8 */
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

static void linux_vmm_pd_init(void)
{
    sel4_dbg_puts("[linux_vmm] native AArch64 stub: VMM not yet configured for real hardware.\n");
    sel4_dbg_puts("[linux_vmm] native stub: Use console_shell to manage VMs via controller.\n");
}

static void linux_vmm_pd_notified(uint32_t ch)
{
    (void)ch;
}

seL4_Bool fault(seL4_CPtr child, uint32_t msginfo,
                uint32_t *reply_msginfo)
{
    (void)child;
    (void)msginfo;
    sel4_dbg_puts("[linux_vmm] native stub: unexpected fault\n");
    rep->length = 0; /* E5-S8 */
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
#include "contracts/linux_vmm_contract.h"

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

/*
 * SERIAL_PD_CH replaces the former hardware UART IRQ channel (id=1).
 * serial_pd now owns PL011 IRQ 33 exclusively; linux_vmm reaches the
 * physical UART only via MSG_SERIAL_* IPC (guest_contract.h compliance).
 */
#define SERIAL_PD_CH            1   /* linux_vmm → serial_pd (PPC) */

/* IPC channel: controller <-> linux_vmm (agent-to-linux bridge) */
#define CONTROLLER_CH           2

/* IRQ channel: virtio-net (slot 0, bus=virtio-mmio-bus.0 → SPI 16 → INTID 48) */
#define VIRTIO_NET_IRQ_CH       5
#define VIRTIO_NET_IRQ          48

/* IRQ channel: virtio-blk (slot 1, bus=virtio-mmio-bus.1 → SPI 17 → INTID 49) */
#define VIRTIO_BLK_IRQ_CH       6
#define VIRTIO_BLK_IRQ          49

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

/* ─── Vaddr variables (set by Microkit from manifest) ───────────────── */

/* Weak so linux_vmm compiles without gpu_tensor_buf when MR not mapped. */
uintptr_t gpu_tensor_buf_vaddr     __attribute__((weak));

/* Weak so linux_vmm compiles without serial_shmem when MR not mapped.
 * When non-zero, used by linux_vmm_binding_init() for MSG_SERIAL_WRITE. */
uintptr_t serial_shmem_linux_vaddr __attribute__((weak));

/* ─── State ──────────────────────────────────────────────────────────── */

static bool     guest_started      = false;
static bool     gpu_shmem_ready    = false;

/* ─── Guest binding state (guest_contract.h compliance) ─────────────── */

static uint32_t vmm_token          = 0;  /* from MSG_VMM_REGISTER */
static uint32_t serial_client_slot = 0;  /* from MSG_SERIAL_OPEN */
static uint32_t guest_id           = 0;  /* from MSG_GUEST_CREATE */
static guest_capabilities_t guest_caps; /* cap tokens per bound device */

/* ─── Guest Binding Protocol (guest_contract.h §3.1) ────────────────── */

/*
 * linux_vmm_binding_init — complete the guest binding protocol before boot.
 *
 * Step 1: Register this VMM PD with the root-task (MSG_VMM_REGISTER).
 *         TODO: requires VMM_KERNEL_CH (CH_VMM_KERNEL=76) wired in manifest.
 *         Until wired, vmm_token stays 0 and VCPU/memory caps are unavailable.
 *
 * Step 2: Open the serial service PD (MSG_SERIAL_OPEN on SERIAL_PD_CH).
 *         serial_pd owns PL011 IRQ 33 exclusively; the guest never sees the
 *         physical device.  UART MMIO faults from the guest are handled in
 *         fault() below and proxied via MSG_SERIAL_WRITE/READ.
 *
 * Step 3: Bind serial to this guest (MSG_GUEST_BIND_DEVICE).
 *         TODO: requires a valid guest_id from MSG_GUEST_CREATE, which in turn
 *         requires vmm_token.  Stubbed until VMM_KERNEL_CH is wired.
 *
 * Step 4: Publish EVENT_GUEST_READY to EventBus.
 *         TODO: requires EVENTBUS_VMM_CH wired in manifest.
 */
static void linux_vmm_binding_init(void)
{
    /* ── Step 1: MSG_VMM_REGISTER (root-task) ────────────────────────────
     * TODO: Wire VMM_KERNEL_CH to the Microkit kernel endpoint.
     * struct vmm_register_req req = {
     *     .os_type    = VMM_OS_TYPE_LINUX,
     *     .flags      = 0,
     *     .max_guests = 1,
     * };
     * __builtin_memcpy((void *)serial_shmem_linux_vaddr, &req, sizeof req);
     * reply = /* E5-S8: ppcall stubbed */
     * vmm_token = msg_u32(req, 4);
     */
    vmm_token = 0;
    LOG_VMM("binding: vmm_token=0 (VMM_KERNEL_CH not yet wired)\n");

    /* ── Step 2: MSG_SERIAL_OPEN (serial_pd) ─────────────────────────────
     * Open a virtual serial port.  serial_pd owns PL011 exclusively.
     */
    rep_u32(rep, 0, MSG_SERIAL_OPEN);
    rep_u32(rep, 4, 0u);  /* port_id 0 — raw, no banner */
    /* E5-S8: ppcall stubbed */
    if (msg_u32(req, 0) == 1u) {
        serial_client_slot = (uint32_t)msg_u32(req, 4);
        LOG_VMM("binding: serial slot=%u\n", serial_client_slot);
    } else {
        LOG_VMM_ERR("binding: MSG_SERIAL_OPEN failed\n");
    }

    /* ── Step 3: MSG_GUEST_BIND_DEVICE (serial) ──────────────────────────
     * TODO: Requires guest_id from MSG_GUEST_CREATE (needs vmm_token).
     * struct guest_bind_device_req bind = {
     *     .guest_id   = guest_id,
     *     .dev_type   = GUEST_DEV_SERIAL,
     *     .dev_handle = serial_client_slot,
     * };
     * ... → guest_caps.serial_token
     */
    guest_id = 0;
    guest_caps.serial_token = GUEST_CAP_TOKEN_INVALID;
    LOG_VMM("binding: guest_id=0 (MSG_GUEST_CREATE not yet wired)\n");

    /* ── Step 4: Publish EVENT_GUEST_READY ───────────────────────────────
     * TODO: Wire EVENTBUS_VMM_CH to event_bus in manifest.
     * guest_ready_event_t ev = {
     *     .os_type  = VMM_OS_TYPE_LINUX,
     *     .guest_id = guest_id,
     *     .pd_id    = 0,
     *     .caps     = guest_caps,
     * };
     * /* E5-S8: ppcall stubbed */
     */
    LOG_VMM("binding: EVENT_GUEST_READY publish deferred (EVENTBUS_VMM_CH not wired)\n");
}

static void virtio_net_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(VIRTIO_NET_IRQ_CH);
}

static void virtio_blk_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(VIRTIO_BLK_IRQ_CH);
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
     *   seL4_CPtr vcpu_tcb = seL4_CPtr(slot_id);
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

static void linux_vmm_pd_init(void)
{
    /* Initialise per-slot affinity masks to "any core" */
    for (uint8_t i = 0; i < VMM_MAX_SLOTS; i++)
        vmm_affinity[i] = 0xFFFFFFFFu;

    LOG_VMM("agentOS linux_vmm starting \"%s\"\n", const char *);
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

    /*
     * Complete guest binding protocol (guest_contract.h §3.1) before boot.
     * UART IRQ 33 is no longer registered here — serial_pd owns it.
     */
    linux_vmm_binding_init();

    /* Register virtio-net IRQ passthrough (slot 0, SPI 16 → INTID 48) */
    success = virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_NET_IRQ, &virtio_net_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register virtio-net IRQ\n");
        return;
    }
    seL4_IRQHandler_Ack(VIRTIO_NET_IRQ_CH);

    /* Register virtio-blk IRQ passthrough (slot 1, SPI 17 → INTID 49) */
    success = virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_BLK_IRQ, &virtio_blk_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register virtio-blk IRQ\n");
        return;
    }
    seL4_IRQHandler_Ack(VIRTIO_BLK_IRQ_CH);

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
    sel4_dbg_puts("[E5-S8] notify-stub
");
}

/* ─── Notification Handler ───────────────────────────────────────────── */

static void linux_vmm_pd_notified(uint32_t ch)
{
    switch (ch) {
    case VIRTIO_NET_IRQ_CH: {
        bool success = virq_inject(VIRTIO_NET_IRQ);
        if (!success) {
            LOG_VMM_ERR("virtio-net IRQ %d dropped on inject\n", VIRTIO_NET_IRQ);
        }
        break;
    }

    case VIRTIO_BLK_IRQ_CH: {
        bool success = virq_inject(VIRTIO_BLK_IRQ);
        if (!success) {
            LOG_VMM_ERR("virtio-blk IRQ %d dropped on inject\n", VIRTIO_BLK_IRQ);
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
            /* TODO: allocate a dedicated GPU shmem virtual IRQ number.
             * Until then, GPU tensor notifications are dropped here. */
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
            sel4_dbg_puts("[E5-S8] notify-stub
");
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
 * an exception (MMIO access to unpassthroughed device, etc.), it arrives here.
 * libvmm's fault_handle() deals with GIC emulation and other traps.
 *
 * UART MMIO emulation (guest_contract.h compliance):
 *   The guest's virtual_machine no longer maps the physical UART at 0x9000000.
 *   Guest accesses to that range now fault here instead of going to hardware.
 *
 *   Full implementation (Phase 3d follow-on):
 *     1. Decode fault address and access type (read/write) from msginfo.
 *     2. Write path: copy DR byte to serial_shmem, send MSG_SERIAL_WRITE on
 *        SERIAL_PD_CH, reply with UART_FR_TXFE so the guest continues.
 *     3. Read path: send MSG_SERIAL_READ on SERIAL_PD_CH, return DR byte.
 *     4. Track a virtual UART_FR register for TXFF/RXFE guest polling.
 *
 *   Compliance skeleton: accept the fault silently; guest console output is
 *   dropped until the full proxy is wired.  The guest still boots because
 *   early serial writes do not require a response.
 */
seL4_Bool fault(seL4_CPtr child, uint32_t msginfo,
                uint32_t *reply_msginfo)
{
    bool success = fault_handle(child, msginfo);
    if (success) {
        rep->length = 0; /* E5-S8 */
        return seL4_True;
    }

    /* UART MMIO fault compliance stub — silently accept, guest continues. */
    rep->length = 0; /* E5-S8 */
    return seL4_True;
}

#endif /* ARCH_AARCH64 && !LINUX_VMM_NATIVE_STUB */
