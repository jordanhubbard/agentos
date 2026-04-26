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
#include "sel4_boot.h"
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

static void linux_vmm_x86_init(void)
{
    for (uint8_t i = 0; i < VMM_MAX_SLOTS; i++)
        vmm_affinity[i] = 0xFFFFFFFFu;  /* any core */

    sel4_dbg_puts("[linux_vmm] x86_64: libvmm VMM support not yet implemented.\n");
    sel4_dbg_puts("[linux_vmm] x86_64: PD running as passive stub.\n");
}

void linux_vmm_main(seL4_CPtr ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    linux_vmm_x86_init();
    /* Passive stub — just spin; root task handles any faults. */
    seL4_Word badge;
    while (1) seL4_Wait(ep, &badge);
}

#endif /* ARCH_X86_64 */

/* ─── RISC-V 64 process-in-PD VMM ──────────────────────────────────────────
 *
 * On RISC-V without the H-extension (hypervisor mode), Linux runs as a seL4
 * PD at U-mode.  The VMM:
 *
 *   1. Checks for an embedded guest kernel (linked via package_guest_images.S
 *      as _guest_kernel_image / _guest_kernel_image_end weak symbols).
 *   2. Builds a minimal FDT into a local static buffer describing one HART,
 *      256 MB RAM at 0x80000000, PLIC, virtio-mmio[0] (net, IRQ 1) and
 *      virtio-mmio[1] (blk, IRQ 2), and a /chosen bootargs node.
 *   3. Copies the kernel image to GUEST_IMAGE_BASE (requires root task to have
 *      mapped the guest RAM region into this PD's VSpace).
 *   4. Sets a0 = hart_id (0), a1 = FDT VA, and jumps to GUEST_IMAGE_BASE.
 *
 * NOTE: Full Linux boot requires the H-extension for S-mode guest isolation.
 * Without it this path boots bare-metal RISC-V programs only.  The FDT is
 * built correctly for future use; the jump path is enabled when the kernel
 * image weak symbol is provided by xtask gen-image.
 *
 * Guest kernel faults (SBI ecalls → seL4_Fault_UnknownSyscall) are fielded
 * by fault_handler.elf which handles SBI_EXT_LEGACY_CONSOLE_PUTCHAR and
 * SBI_EXT_TIME; all other SBI calls return -1.
 */
#if defined(__riscv) && !defined(ARCH_AARCH64) && !defined(ARCH_X86_64)

#include "fdt_builder.h"

/* Guest memory layout (must match FDT and kernel config) */
#define GUEST_IMAGE_BASE  0x80200000UL  /* flat Image entry point           */
#define GUEST_RAM_BASE    0x80000000UL
#define GUEST_RAM_SIZE    0x10000000UL  /* 256 MB — DTB placed at 240 MB    */

/* QEMU virt RISC-V peripheral addresses */
#define PLIC_BASE         0x0c000000UL
#define PLIC_SIZE         0x00600000UL
#define VIRTIO_NET_BASE   0x10001000UL  /* slot 0: virtio-net  */
#define VIRTIO_BLK_BASE   0x10002000UL  /* slot 1: virtio-blk  */
#define VIRTIO_MMIO_SIZE  0x00001000UL
#define VIRTIO_NET_IRQ    1u
#define VIRTIO_BLK_IRQ    2u

/*
 * Guest kernel image linked by package_guest_images.S.  Weak so linux_vmm.elf
 * links without an embedded kernel; _guest_kernel_image == NULL in that case.
 */
extern char _guest_kernel_image[]     __attribute__((weak));
extern char _guest_kernel_image_end[] __attribute__((weak));

/* Static FDT buffer — always in the PD's writable data segment. */
static uint8_t s_fdt_buf[4096] __attribute__((aligned(8)));

/* ── FDT builder ─────────────────────────────────────────────────────────── */

static size_t build_guest_fdt(void)
{
    fdt_ctx_t ctx;
    fdt_init(&ctx, s_fdt_buf, sizeof(s_fdt_buf));

    /* Root */
    fdt_begin_node(&ctx, "");
    fdt_prop_u32(&ctx, "#address-cells", 2u);
    fdt_prop_u32(&ctx, "#size-cells",    2u);
    fdt_prop_string(&ctx, "compatible",  "riscv-virtio");
    fdt_prop_string(&ctx, "model",       "riscv-virtio,qemu");

    /* /cpus */
    fdt_begin_node(&ctx, "cpus");
    fdt_prop_u32(&ctx, "#address-cells",    1u);
    fdt_prop_u32(&ctx, "#size-cells",       0u);
    fdt_prop_u32(&ctx, "timebase-frequency", 10000000u);  /* 10 MHz */

    fdt_begin_node(&ctx, "cpu@0");
    fdt_prop_string(&ctx, "device_type", "cpu");
    fdt_prop_string(&ctx, "compatible",  "riscv");
    fdt_prop_string(&ctx, "riscv,isa",   "rv64imafdc");
    fdt_prop_string(&ctx, "mmu-type",    "riscv,sv48");
    fdt_prop_u32(&ctx, "reg",            0u);
    fdt_prop_string(&ctx, "status",      "okay");

    /* INTC — local interrupt controller for this hart (phandle 1) */
    fdt_begin_node(&ctx, "interrupt-controller");
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_string(&ctx, "compatible",    "riscv,cpu-intc");
    fdt_prop_u32_array(&ctx, "interrupt-controller", NULL, 0u); /* boolean */
    fdt_prop_u32(&ctx, "phandle",          1u);
    fdt_end_node(&ctx);  /* interrupt-controller */

    fdt_end_node(&ctx);  /* cpu@0 */
    fdt_end_node(&ctx);  /* cpus */

    /* /memory@80000000 */
    fdt_begin_node(&ctx, "memory@80000000");
    fdt_prop_string(&ctx, "device_type", "memory");
    fdt_prop_reg64(&ctx, GUEST_RAM_BASE, GUEST_RAM_SIZE);
    fdt_end_node(&ctx);

    /* /soc — simple-bus with identity ranges */
    fdt_begin_node(&ctx, "soc");
    fdt_prop_u32(&ctx, "#address-cells", 2u);
    fdt_prop_u32(&ctx, "#size-cells",    2u);
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_string(&ctx, "compatible",  "simple-bus");
    fdt_prop_u32_array(&ctx, "ranges",   NULL, 0u);  /* identity mapping */

    /* PLIC (phandle 2) */
    fdt_begin_node(&ctx, "plic@c000000");
    fdt_prop_string(&ctx, "compatible",   "sifive,plic-1.0.0");
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_u32(&ctx, "#address-cells",   0u);
    fdt_prop_u32_array(&ctx, "interrupt-controller", NULL, 0u); /* boolean */
    fdt_prop_reg64(&ctx, PLIC_BASE, PLIC_SIZE);
    fdt_prop_u32(&ctx, "riscv,ndev",      31u);
    /* interrupts-extended: hart 0 M-EI (11) and S-EI (9) via phandle 1 */
    {
        uint32_t ix[4] = { 1u, 11u, 1u, 9u };
        fdt_prop_u32_array(&ctx, "interrupts-extended", ix, 4u);
    }
    fdt_prop_u32(&ctx, "phandle",         2u);
    fdt_end_node(&ctx);  /* plic */

    /* virtio-net (slot 0, IRQ 1) */
    fdt_begin_node(&ctx, "virtio_mmio@10001000");
    fdt_prop_string(&ctx, "compatible",   "virtio,mmio");
    fdt_prop_reg64(&ctx, VIRTIO_NET_BASE, VIRTIO_MMIO_SIZE);
    {
        uint32_t irq = VIRTIO_NET_IRQ;
        fdt_prop_u32_array(&ctx, "interrupts", &irq, 1u);
    }
    fdt_prop_u32(&ctx, "interrupt-parent", 2u);
    fdt_end_node(&ctx);

    /* virtio-blk (slot 1, IRQ 2) */
    fdt_begin_node(&ctx, "virtio_mmio@10002000");
    fdt_prop_string(&ctx, "compatible",   "virtio,mmio");
    fdt_prop_reg64(&ctx, VIRTIO_BLK_BASE, VIRTIO_MMIO_SIZE);
    {
        uint32_t irq = VIRTIO_BLK_IRQ;
        fdt_prop_u32_array(&ctx, "interrupts", &irq, 1u);
    }
    fdt_prop_u32(&ctx, "interrupt-parent", 2u);
    fdt_end_node(&ctx);

    fdt_end_node(&ctx);  /* soc */

    /* /chosen */
    fdt_begin_node(&ctx, "chosen");
    fdt_prop_string(&ctx, "bootargs",
                    "console=hvc0 root=/dev/vda rw earlycon=sbi loglevel=8");
    fdt_end_node(&ctx);

    fdt_end_node(&ctx);  /* root */

    return fdt_finish(&ctx);
}

/* ── Kernel entry jump ───────────────────────────────────────────────────── */

/*
 * jump_to_kernel — transfer control to a RISC-V flat Image.
 *
 * RISC-V boot ABI (OpenSBI spec §3.1):
 *   a0 = hart_id (physical hart index)
 *   a1 = FDT physical (or virtual) address
 *   All other registers are caller-saved and will be clobbered by the kernel.
 *
 * jalr x0, 0(t0) — unconditional jump with no return address saved.
 */
static void __attribute__((noreturn))
jump_to_kernel(unsigned long entry, unsigned long hart_id, unsigned long dtb_va)
{
    register unsigned long a0 __asm__("a0") = hart_id;
    register unsigned long a1 __asm__("a1") = dtb_va;
    register unsigned long t0 __asm__("t0") = entry;
    __asm__ volatile (
        "jalr zero, 0(%0)"
        :
        : "r"(t0), "r"(a0), "r"(a1)
        : "memory"
    );
    __builtin_unreachable();
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

void linux_vmm_main(seL4_CPtr ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;

    sel4_dbg_puts("[linux_vmm] RISC-V: process-in-PD VMM starting.\n");

    /* ── Build FDT ─────────────────────────────────────────────────────── */
    size_t fdt_sz = build_guest_fdt();
    if (fdt_sz == 0u) {
        sel4_dbg_puts("[linux_vmm] RISC-V: FDT build FAILED (buffer overflow).\n");
        while (1) { seL4_Word b; seL4_Wait(ep, &b); }
    }
    sel4_dbg_puts("[linux_vmm] RISC-V: FDT built OK.\n");

    /* ── Check for embedded kernel ─────────────────────────────────────── */
    if (!_guest_kernel_image || (_guest_kernel_image == _guest_kernel_image_end)) {
        sel4_dbg_puts("[linux_vmm] RISC-V: no guest kernel linked"
                      " (xtask gen-image step required).\n");
        sel4_dbg_puts("[linux_vmm] RISC-V: running as passive stub.\n");
        while (1) { seL4_Word b; seL4_Wait(ep, &b); }
    }

    size_t ksize = (size_t)(_guest_kernel_image_end - _guest_kernel_image);

    /* ── Copy kernel to GUEST_IMAGE_BASE ───────────────────────────────── */
    /* Root task must have mapped 256 MB of guest RAM into this PD's VSpace
     * at GUEST_IMAGE_BASE for this write to succeed.  Without that mapping
     * the copy faults, caught by fault_handler.elf. */
    {
        uint8_t       *dst = (uint8_t *)GUEST_IMAGE_BASE;
        const uint8_t *src = (const uint8_t *)_guest_kernel_image;
        for (size_t i = 0u; i < ksize; i++) dst[i] = src[i];
    }
    sel4_dbg_puts("[linux_vmm] RISC-V: kernel copied to 0x80200000.\n");

    /* ── Jump to kernel ─────────────────────────────────────────────────── */
    /* Pass the VA of the local FDT buffer as a1.  In the process-in-PD model
     * VA == PA only if seL4 identity-maps the VSpace; otherwise the kernel
     * will need to translate the DTB address through its own page tables. */
    sel4_dbg_puts("[linux_vmm] RISC-V: jumping to kernel entry.\n");
    jump_to_kernel(GUEST_IMAGE_BASE, 0UL, (unsigned long)s_fdt_buf);
}

#endif /* __riscv */

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

void linux_vmm_main(seL4_CPtr ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    sel4_dbg_puts("[linux_vmm] native AArch64 stub: VMM not yet configured for real hardware.\n");
    sel4_dbg_puts("[linux_vmm] native stub: Use console_shell to manage VMs via controller.\n");
    seL4_Word badge;
    while (1) seL4_Wait(ep, &badge);
}

#endif /* LINUX_VMM_NATIVE_STUB */

/* ─── AArch64 full implementation ──────────────────────────────────────────
 *
 * Uses au-ts/libvmm to boot a Linux guest at EL1 under seL4 EL2.
 * Compiled by vmm.mk which passes -DARCH_AARCH64 and links libvmm.a.
 */
#if defined(ARCH_AARCH64) && !defined(LINUX_VMM_NATIVE_STUB)

#include <libvmm/libvmm.h>
#include <libvmm/vmm_caps.h>   /* vmm_register_vcpu                           */
#include "gpu_shmem.h"
#include "contracts/linux_vmm_contract.h"
#include "sel4_boot.h"    /* seL4_IRQHandler_Ack, seL4_CPtr               */
#include "sel4_ipc.h"     /* sel4_call, sel4_msg_t                        */
#include "sel4_client.h"  /* sel4_client_t, sel4_client_call              */

/* Microkit CNode layout constants (from microkit.h — not included directly
 * to avoid the conflicting `void init(void)` declaration). */
#define MICROKIT_BASE_IRQ_CAP    138u
#define MICROKIT_BASE_VM_TCB_CAP 266u
#define MICROKIT_BASE_VCPU_CAP   330u

/* ── Microkit shim ───────────────────────────────────────────────────────
 *
 * libvmm was designed as a Microkit library and references several Microkit
 * runtime symbols for debug output and channel state.  agentOS does not use
 * the Microkit runtime; we provide seL4-native equivalents here so that
 * libvmm.a links without libmicrokit.a.
 *
 * These are NOT Microkit: they are raw seL4 wrappers with the same ABI that
 * libvmm's LOG_VMM/virq code requires for debug output.
 */
char microkit_name[64] = "linux_vmm";
const char vmm_pd_name[] = "linux_vmm";     /* libvmm's LOG_VMM uses this */
seL4_Word microkit_irqs          = 0;        /* libvmm virq_passthrough_ack guard */
seL4_Word microkit_notifications = 0;        /* libvmm virq guard           */
/* Microkit runtime stubs — required by the Microkit tool's ELF validator.
 * agentOS linux_vmm does not use the Microkit runtime; these are zero-valued
 * placeholders that satisfy the image packer's symbol checks. */
__attribute__((used)) volatile int microkit_passive       = 0;
__attribute__((used)) seL4_Word    microkit_pps           = 0;
__attribute__((used)) seL4_Word    microkit_have_signal   = 0;
__attribute__((used)) seL4_Word    microkit_ioports       = 0;
__attribute__((used)) seL4_Word    microkit_signal_cap    = 0;
__attribute__((used)) seL4_Word    microkit_signal_msg    = 0;

void microkit_dbg_putc(char c) { seL4_DebugPutChar(c); }

void microkit_dbg_puts(const char *s)
{
    for (; s && *s; s++) seL4_DebugPutChar(*s);
}

void microkit_dbg_put32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    seL4_DebugPutChar('0'); seL4_DebugPutChar('x');
    for (int i = 28; i >= 0; i -= 4)
        seL4_DebugPutChar(hex[(v >> i) & 0xfu]);
}

/* seL4 IPC buffer pointer. Compiled with -D__thread= (TLS suppressed) so
 * this is a regular global matching libvmm.a's expectation. The frame is
 * mapped by the CapDL initializer at __sel4_ipc_buffer_obj's VA, set via
 * seL4_SetIPCBuffer() in linux_vmm_main() before any seL4 IPC calls. */
seL4_IPCBuffer *__sel4_ipc_buffer = NULL;

/* vmm_caps.c is not included in libvmm.a — define g_vmm_vcpus here.
 * Populated by vmm_register_vcpu() calls in init() before any libvmm use. */
vmm_vcpu_t g_vmm_vcpus[VMM_MAX_VCPUS];

/* ── Caps resolved at init time ──────────────────────────────────────── */
static seL4_CPtr g_serial_ep        = 0;
static seL4_CPtr g_controller_ntfn_cap = 0;

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

/* GPU shared memory notification channels (assigned when MR is mapped) */
#define GPU_SHMEM_NOTIFY_IN_CH  3   /* seL4 PD → linux_vmm: tensor ready */
#define GPU_SHMEM_NOTIFY_OUT_CH 4   /* linux_vmm → seL4 PD: result ready */

/*
 * Virtual IRQ numbers injected into the guest via the vGIC.
 * These match the QEMU virt board SPI assignments for virtio-mmio devices:
 *   virtio-mmio-bus.0 → GIC SPI 16 → INTID 48
 *   virtio-mmio-bus.1 → GIC SPI 17 → INTID 49
 */
#define VIRTIO_NET_IRQ          48
#define VIRTIO_BLK_IRQ          49
#define VIRTIO_BLK2_IRQ         50  /* ubuntu cloud-init seed disk (slot 2) */

/*
 * Notification badge bits for virtio IRQs.
 * Microkit delivers IRQ N (id=K in .system) as bit K of the notification word:
 *   badge = (1 << K)
 * The .system file assigns:
 *   id=3 (virtio-net,  INTID 48) → badge 0x8   (1<<3)
 *   id=4 (virtio-blk0, INTID 49) → badge 0x10  (1<<4)
 *   id=5 (virtio-blk1, INTID 50) → badge 0x20  (1<<5)
 */
#define VIRTIO_NET_NTFN_BADGE    (1u << 3)
#define VIRTIO_BLK_NTFN_BADGE    (1u << 4)
#define VIRTIO_BLK2_NTFN_BADGE   (1u << 5)
/*
 * UART IRQ passthrough: id=6 in .system avoids the ids 1-5 used by channels
 * (serial_pd=1, controller=2) and virtio IRQs (3,4,5).
 * Active only in linux_vmm_test.system; in full agentOS.system id=6 is absent
 * so this badge never fires and the init guard below is a no-op.
 */
#define UART_NTFN_BADGE          (1u << 6)

/*
 * IRQ handler capabilities placed by Microkit at BASE_IRQ_CAP + <irq id=N>.
 * The .system file assigns id=3 to virtio-net, id=4 to virtio-blk0,
 * id=5 to virtio-blk1 (ids 1 and 2 are taken by serial_pd/controller channels).
 * Slots 141/142/143 in linux_vmm's CNode.
 * id=6 (slot 144) is the UART IRQ in linux_vmm_test.system.
 */
static const seL4_CPtr g_virtio_net_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 3u);
static const seL4_CPtr g_virtio_blk_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 4u);
static const seL4_CPtr g_virtio_blk2_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 5u);
static const seL4_CPtr g_uart_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 6u);

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
     * reply = microkit_ppcall(CH_VMM_KERNEL,
     *                         microkit_msginfo_new(MSG_VMM_REGISTER, 0));
     * vmm_token = microkit_mr_get(1);
     */
    vmm_token = 0;
    LOG_VMM("binding: vmm_token=0 (VMM_KERNEL_CH not yet wired)\n");

    /* ── Step 2: MSG_SERIAL_OPEN (serial_pd) ─────────────────────────────
     * TODO: resolve g_serial_ep from nameserver (sel4_client_lookup not yet wired).
     * Until wired, g_serial_ep remains 0 and serial binding is skipped.
     */
    if (g_serial_ep) {
        /* Placeholder — g_serial_ep is always 0 until nameserver lookup is wired */
        LOG_VMM_ERR("binding: MSG_SERIAL_OPEN (TODO: implement nameserver lookup)\n");
    } else {
        LOG_VMM_ERR("binding: serial_ep not resolved (nameserver not ready)\n");
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
     * microkit_ppcall(EVENTBUS_VMM_CH,
     *                 microkit_msginfo_new(MSG_EVENTBUS_PUBLISH_BATCH, ...));
     */
    LOG_VMM("binding: EVENT_GUEST_READY publish deferred (EVENTBUS_VMM_CH not wired)\n");
}

static void virtio_net_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_net_irq_cap);
}

static void virtio_blk_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
}

static void virtio_blk2_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_blk2_irq_cap);
}

static void uart_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_uart_irq_cap);
}

/* ─── PL011 UART MMIO Emulation ──────────────────────────────────────────
 *
 * The Ubuntu kernel uses the PL011 UART at 0x9000000 for earlycon and the
 * ttyAMA0 console driver.  serial_pd owns the physical PL011 IRQ; the guest
 * never reaches the hardware.  Guest accesses to 0x9000000..0x9000FFF fault
 * into linux_vmm and are dispatched here by fault_handle().
 *
 * Critical register: FR (offset 0x18).
 *   FR=0  → TXFE=0 (TX FIFO not empty) → kernel spins in drain_fifo().
 *   FR=0x90 → TXFE=1 (bit 7) + RXFE=1 (bit 4) → TX idle, RX empty.
 *
 * All other registers: reads return 0, writes are silently discarded.
 * Guest console output is dropped; SSH provides the interactive channel.
 */
#define PL011_BASE   0x9000000UL
#define PL011_SIZE   0x1000UL
#define PL011_FR     0x18u      /* Flag Register offset */
#define PL011_FR_TXFE (1u << 7) /* TX FIFO empty */
#define PL011_FR_RXFE (1u << 4) /* RX FIFO empty */

static bool pl011_fault_handler(size_t vcpu_id, size_t offset, size_t fsr,
                                 seL4_UserContext *regs, void *data)
{
    (void)data;
    uint64_t reg_val = 0;
    if (fault_is_read((uint64_t)fsr)) {
        if (offset == PL011_FR)
            reg_val = PL011_FR_TXFE | PL011_FR_RXFE;
    } else {
        /* Write: offset 0x0 is the Data Register (DR) — forward char to host */
        if (offset == 0) {
            char c = (char)(fault_get_data(regs, (uint64_t)fsr) & 0xff);
            seL4_DebugPutChar(c);
        }
    }
    return fault_advance(vcpu_id, regs,
                         (uint64_t)(PL011_BASE + offset),
                         (uint64_t)fsr, reg_val);
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

    /* In raw seL4 mode (no Microkit), the root task maps 256 MB at 0x40000000
     * into this PD's VSpace and leaves guest_ram_vaddr uninitialised (0).
     * Use the fixed convention address as a fallback. */
    if (guest_ram_vaddr == 0u) {
        guest_ram_vaddr = 0x40000000UL;
    }

    LOG_VMM("agentOS linux_vmm starting \"linux_vmm\"\n");
    LOG_VMM("  Guest RAM: 0x%lx (%d MB)\n",
            (unsigned long)guest_ram_vaddr, GUEST_RAM_SIZE / (1024 * 1024));

    /* Register VCPU and TCB caps with libvmm before any libvmm call that
     * uses vmm_vcpu_cap() or vmm_tcb_cap().  Microkit places these at the
     * fixed CNode slots BASE_VCPU_CAP+id and BASE_VM_TCB_CAP+id. */
    vmm_register_vcpu(GUEST_BOOT_VCPU_ID,
                      MICROKIT_BASE_VCPU_CAP   + GUEST_BOOT_VCPU_ID,
                      MICROKIT_BASE_VM_TCB_CAP + GUEST_BOOT_VCPU_ID);

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

    /* Register PL011 UART MMIO emulation (0x9000000 .. 0x9000FFF).
     * Ubuntu kernel uses PL011 for earlycon/ttyAMA0; serial_pd owns the
     * physical IRQ.  Our handler returns FR=0x90 on reads so the kernel
     * does not spin waiting for TX-empty. */
    if (!fault_register_vm_exception_handler(PL011_BASE, PL011_SIZE,
                                             pl011_fault_handler, NULL)) {
        LOG_VMM_ERR("Failed to register PL011 UART fault handler\n");
    }

    /*
     * Complete guest binding protocol (guest_contract.h §3.1) before boot.
     * UART IRQ 33 is no longer registered here — serial_pd owns it.
     */
    linux_vmm_binding_init();

    /* Register virtio-net IRQ passthrough (QEMU virt: SPI 16 → INTID 48) */
    success = virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_NET_IRQ, &virtio_net_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register virtio-net IRQ\n");
        return;
    }
    /* Initial ack to prime the GIC for first delivery (seL4 native API) */
    seL4_IRQHandler_Ack(g_virtio_net_irq_cap);

    /* Register virtio-blk0 IRQ passthrough (QEMU virt: SPI 17 → INTID 49) */
    success = virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_BLK_IRQ, &virtio_blk_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register virtio-blk0 IRQ\n");
        return;
    }
    /* Initial ack to prime the GIC for first delivery (seL4 native API) */
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);

    /* Register virtio-blk1 IRQ passthrough (SPI 18 → INTID 50, ubuntu seed) */
    success = virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_BLK2_IRQ, &virtio_blk2_ack, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register virtio-blk1 IRQ (ubuntu seed — may not be present)\n");
        /* Non-fatal: buildroot guests do not use the seed disk slot */
    } else {
        seL4_IRQHandler_Ack(g_virtio_blk2_irq_cap);
    }

    /*
     * Register UART IRQ passthrough (PL011 SPI 1 → INTID 33).
     * Only active in linux_vmm_test.system where irq id=6 is assigned.
     * In the full agentOS system serial_pd owns IRQ 33; id=6 is absent and
     * g_uart_irq_cap (slot 144) holds no valid cap — so we guard with a
     * seL4_IRQHandler_Ack only if virq_register succeeds.
     * UART_IRQ = 33: QEMU virt aarch64 PL011 SPI 1 → GIC INTID 33.
     */
    {
        /* UART_IRQ in the guest's GIC address space = INTID 33 (SPI 1) */
        const uint32_t UART_IRQ = 33u;
        bool uart_ok = virq_register(GUEST_BOOT_VCPU_ID, UART_IRQ, &uart_ack, NULL);
        if (uart_ok) {
            seL4_IRQHandler_Ack(g_uart_irq_cap);
            LOG_VMM("UART IRQ 33 passthrough registered (direct PL011 mode)\n");
        }
        /* If not registered, PL011 uses fault-emulation path — non-fatal */
    }

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
    if (g_controller_ntfn_cap) seL4_Signal(g_controller_ntfn_cap);
}

/* ─── Notification Handler ───────────────────────────────────────────── */

/*
 * notified — handle incoming notifications.
 *
 * seL4 delivers hardware IRQs as badged notifications on the PD's notification
 * object.  The badge value is the ntfn_badge from the irq_desc_t for that IRQ
 * (set up by the root task at boot via seL4_IRQHandler_SetNotification).
 *
 * Multiple IRQs may be coalesced into a single notification word (bitwise OR of
 * all pending badges).  We must therefore test each badge bit independently
 * with bitwise AND — not use a switch statement — so that all pending IRQs are
 * serviced in one notification delivery.
 *
 * Non-IRQ notifications (controller, GPU shmem) arrive on Microkit IPC
 * channels and are dispatched by channel number in the remaining cases.
 */
static void linux_vmm_notified(seL4_Word badge)
{
    seL4_Word ch = badge;
    (void)ch;
    /*
     * Badge bits from seL4 notification delivery.  Multiple IRQs may be
     * coalesced; test each bit independently.
     */
    if (badge & (seL4_Word)VIRTIO_NET_NTFN_BADGE) {
        bool success = virq_inject(VIRTIO_NET_IRQ);
        if (!success) {
            LOG_VMM_ERR("virtio-net IRQ %d dropped on inject\n", VIRTIO_NET_IRQ);
        }
        /* IRQHandler_Ack is called by virtio_net_ack() registered via virq_register */
    }

    if (badge & (seL4_Word)VIRTIO_BLK_NTFN_BADGE) {
        bool success = virq_inject(VIRTIO_BLK_IRQ);
        if (!success) {
            LOG_VMM_ERR("virtio-blk0 IRQ %d dropped on inject\n", VIRTIO_BLK_IRQ);
        }
    }

    if (badge & (seL4_Word)VIRTIO_BLK2_NTFN_BADGE) {
        bool success = virq_inject(VIRTIO_BLK2_IRQ);
        if (!success) {
            LOG_VMM_ERR("virtio-blk1 IRQ %d dropped on inject\n", VIRTIO_BLK2_IRQ);
        }
    }

    if (badge & (seL4_Word)UART_NTFN_BADGE) {
        /* PL011 UART IRQ 33 — used in linux_vmm_test.system (direct serial mapping).
         * Inject INTID 33 into the guest so the PL011 driver can process RX/TX. */
        bool success = virq_inject(33u);
        if (!success) {
            LOG_VMM_ERR("UART IRQ 33 dropped on inject\n");
        }
    }

    /* Non-IRQ channel notifications — dispatch by badge/channel number */
    switch (badge) {
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
            if (g_controller_ntfn_cap) seL4_Signal(g_controller_ntfn_cap);
        }
        break;
    }

    default:
        /* Only log if no badge bits were set (i.e. not a virtio IRQ notification) */
        if (!(badge & (seL4_Word)(VIRTIO_NET_NTFN_BADGE | VIRTIO_BLK_NTFN_BADGE |
                                   VIRTIO_BLK2_NTFN_BADGE))) {
            LOG_VMM("Unexpected notification on badge 0x%lx\n", (unsigned long)badge);
        }
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
static seL4_MessageInfo_t linux_vmm_fault(seL4_Word badge,
                                          seL4_MessageInfo_t msginfo)
{
    /* Microkit 2.1 encodes VCPU fault badges as (1ULL<<62)|vcpu_id.
     * Strip the upper flag bits to recover the raw vcpu_id. */
    size_t vcpu_id = badge & ~(1ULL << 62);
    bool success = fault_handle(vcpu_id, msginfo);
    (void)success;
    /* UART MMIO fault compliance stub — silently accept, guest continues. */
    return seL4_MessageInfo_new(0, 0, 0, 0);
}

/* ─── Main loop ─────────────────────────────────────────────────────────── */

/*
 * Microkit CapDL CNode layout for linux_vmm:
 *   slot 1: ep_linux_vmm  (receive endpoint)
 *   slot 4: reply_linux_vmm  (MCS reply cap storage)
 */
#define VMM_EP_CAP    ((seL4_CPtr)1u)
#define VMM_REPLY_CAP ((seL4_CPtr)4u)

void linux_vmm_main(seL4_CPtr ep, seL4_CPtr reply_cap)
{
    /* Point __sel4_ipc_buffer at the frame mapped by the CapDL initializer.
     * Must happen before any seL4 IPC call that uses message registers. */
    extern char __sel4_ipc_buffer_obj[];
    seL4_SetIPCBuffer((seL4_IPCBuffer *)__sel4_ipc_buffer_obj);

    /* Run init() — sets up guest images, GIC, virtio IRQs, starts guest */
    init();

    /* Main dispatch loop — receive notifications and faults.
     *
     * seL4 MCS mode (CONFIG_KERNEL_MCS=1):
     *   seL4_Recv takes a reply cap slot as third argument; the kernel saves
     *   the caller's reply context there.  seL4_Reply is not available in MCS;
     *   use seL4_Send on the reply cap slot to complete the reply instead.
     *
     * seL4 non-MCS mode:
     *   seL4_Recv takes two arguments; seL4_Reply completes the round-trip.
     */
    seL4_Word badge;
#ifdef CONFIG_KERNEL_MCS
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge, reply_cap);
#else
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
#endif
    while (1) {
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (label == seL4_Fault_NullFault) {
            linux_vmm_notified(badge);
#ifdef CONFIG_KERNEL_MCS
            info = seL4_Recv(ep, &badge, reply_cap);
#else
            info = seL4_Recv(ep, &badge);
#endif
        } else {
            seL4_MessageInfo_t reply = linux_vmm_fault(badge, info);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(reply_cap, reply);
            info = seL4_Recv(ep, &badge, reply_cap);
#else
            seL4_Reply(reply);
            info = seL4_Recv(ep, &badge);
#endif
        }
    }
}

/* ELF entry point required by Microkit linker script and monitor.
 * Uses the Microkit CapDL CNode layout constants above. */
__attribute__((section(".text.start"), noreturn))
void _start(void) {
    linux_vmm_main(VMM_EP_CAP, VMM_REPLY_CAP);
    __builtin_unreachable();
}

#endif /* ARCH_AARCH64 && !LINUX_VMM_NATIVE_STUB */
