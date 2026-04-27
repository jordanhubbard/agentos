/*
 * agentOS FreeBSD VMM — Virtual Machine Monitor
 *
 * Boots FreeBSD 14 AArch64 via EDK2 UEFI firmware inside agentOS using
 * libvmm on seL4/Microkit.
 *
 * Boot sequence:
 *   1. VMM copies EDK2 code flash (_guest_kernel_image) to uefi_code_vaddr.
 *   2. VMM zeroes uefi_data_vaddr (fresh UEFI variable store).
 *   3. VMM copies freebsd-edk2.dtb (_guest_dtb_image) to guest phys 0x40000000.
 *      EDK2's ArmVirtQemu port checks for FDT magic (0xD00DFEED) at the DRAM
 *      base.  Without a valid FDT it enters a multi-core spin loop; the DTB
 *      provides the memory map and unblocks initialisation.
 *   4. guest_start(0x0, 0, 0): vCPU PC = 0x0 (EDK2 entry), EL1h.
 *   5. EDK2 initialises, discovers VirtIO-blk at 0xa003e00 (QEMU virt bus 31).
 *   6. EDK2 loads loader.efi from the FreeBSD EFI partition on VirtIO-blk.
 *   7. FreeBSD boots from VirtIO-blk root.
 *
 * Memory layout (host phys -> guest phys):
 *   uefi_code (64MB)  host:0x40000000 -> guest:0x00000000  (EDK2 code flash)
 *   uefi_data (64MB)  host:0x44000000 -> guest:0x04000000  (UEFI var store)
 *   guest_ram (384MB) host:0x48000000 -> guest:0x40000000  (main RAM)
 *     [freebsd-edk2.dtb placed at guest phys 0x40000000 = start of guest_ram]
 *
 * IRQ passthrough (freebsd_vmm_test.system assigns):
 *   id=3 -> badge 0x08 -> INTID 79 (SPI 47, virtio-blk bus 31 = slot 31)
 *   id=6 -> badge 0x40 -> INTID 33 (SPI 1,  PL011 UART console)
 *
 * QEMU bus assignment: QEMU assigns -device virtio-blk-device to the highest
 * available virtio-mmio bus (bus 31), at 0xa003e00, SPI 47 = INTID 79.
 *
 * PSCI: EDK2 uses SMC-based PSCI (psci { method = "smc"; }).
 *   seL4 intercepts HVC from VCPU EL1 as a seL4 UnknownSyscall, so we use SMC.
 *   libvmm fault.c routes HSR_SMC_64_EXCEPTION to smc_handle() / handle_psci().
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sel4_boot.h"

#if defined(ARCH_AARCH64)

#include <libvmm/libvmm.h>
#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>

/* ── Microkit CNode layout constants ─────────────────────────────────────── */
#define MICROKIT_BASE_IRQ_CAP    138u
#define MICROKIT_BASE_VM_TCB_CAP 266u
#define MICROKIT_BASE_VCPU_CAP   330u

/* ── Microkit shim (same pattern as linux_vmm.c) ─────────────────────────── */
char microkit_name[64] = "freebsd_vmm";
const char vmm_pd_name[] = "freebsd_vmm";
seL4_Word microkit_irqs          = 0;
seL4_Word microkit_notifications = 0;
__attribute__((used)) volatile int microkit_passive       = 0;
__attribute__((used)) seL4_Word    microkit_pps           = 0;
__attribute__((used)) seL4_Word    microkit_have_signal   = 0;
__attribute__((used)) seL4_Word    microkit_ioports       = 0;
__attribute__((used)) seL4_Word    microkit_signal_cap    = 0;
__attribute__((used)) seL4_Word    microkit_signal_msg    = 0;

/* PL011 UART on QEMU virt — direct write, no seL4_DebugPutChar needed */
#define FREEBSD_VMM_UART_VA 0x10001000UL
static inline void _uart_putc(char c) {
    volatile uint32_t *dr = (volatile uint32_t *)FREEBSD_VMM_UART_VA;
    *dr = (uint32_t)(unsigned char)c;
}

void microkit_dbg_putc(char c) { _uart_putc(c); }

void microkit_dbg_puts(const char *s)
{
    for (; s && *s; s++) _uart_putc(*s);
}

void microkit_dbg_put32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    _uart_putc('0'); _uart_putc('x');
    for (int i = 28; i >= 0; i -= 4)
        _uart_putc(hex[(v >> i) & 0xfu]);
}

seL4_IPCBuffer *__sel4_ipc_buffer = NULL;

vmm_vcpu_t g_vmm_vcpus[VMM_MAX_VCPUS];

/* ── IRQ capabilities ────────────────────────────────────────────────────── */
static const seL4_CPtr g_virtio_blk_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 3u);   /* id=3, INTID 79, SPI 47 */
static const seL4_CPtr g_uart_irq_cap =
    (seL4_CPtr)(MICROKIT_BASE_IRQ_CAP + 6u);   /* id=6, INTID 33, SPI 1  */

/* ── Notification badge bits (badge = 1 << id from .system) ─────────────── */
#define VIRTIO_BLK_NTFN_BADGE  (1u << 3)
#define UART_NTFN_BADGE        (1u << 6)

/* ── GIC INTID values ────────────────────────────────────────────────────── */
#define VIRTIO_BLK_IRQ  79u    /* QEMU virt bus 31 = 0xa003e00, SPI 47 */
#define UART_IRQ        33u    /* QEMU virt SPI 1  */

/* ── Guest image symbols (EDK2 + FDT from package_guest_images.S) ────────── */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];

/* ── Microkit setvar_vaddr symbols (set from .system MR map entries) ──────── */
uintptr_t uefi_code_vaddr;   /* VMM virtual address of uefi_code MR */
uintptr_t uefi_data_vaddr;   /* VMM virtual address of uefi_data MR */
uintptr_t guest_ram_vaddr;   /* VMM virtual address of guest_ram MR */

static bool guest_started = false;

/* ── IRQ ack callbacks ───────────────────────────────────────────────────── */

static void virtio_blk_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
}

static void uart_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_uart_irq_cap);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void init(void)
{
    LOG_VMM("agentOS freebsd_vmm starting\n");

    vmm_register_vcpu(GUEST_BOOT_VCPU_ID,
                      MICROKIT_BASE_VCPU_CAP   + GUEST_BOOT_VCPU_ID,
                      MICROKIT_BASE_VM_TCB_CAP + GUEST_BOOT_VCPU_ID);

    size_t edk2_size = (size_t)(_guest_kernel_image_end - _guest_kernel_image);
    if (edk2_size == 0) {
        LOG_VMM_ERR("EDK2 image not linked (build with GUEST_OS=freebsd)\n");
        return;
    }
    LOG_VMM("  EDK2: %zu bytes (%.1f MB)\n",
            edk2_size, (double)edk2_size / (1024.0 * 1024.0));

    if (!uefi_code_vaddr) {
        LOG_VMM_ERR("uefi_code_vaddr not set (check system file MR map)\n");
        return;
    }

    /* Copy EDK2 code flash to uefi_code region (guest phys 0x0) */
    __builtin_memcpy((void *)uefi_code_vaddr, _guest_kernel_image, edk2_size);
    LOG_VMM("  EDK2 copied to uefi_code (guest phys 0x0)\n");

    /* Zero UEFI variable store so EDK2 starts with clean NVRAM */
    if (uefi_data_vaddr)
        __builtin_memset((void *)uefi_data_vaddr, 0, 0x4000000u);
    LOG_VMM("  UEFI variable store zeroed (guest phys 0x4000000)\n");

    /*
     * Copy the minimal FDT to guest phys 0x40000000 (= start of guest_ram).
     * EDK2's ArmVirtQemu checks for FDT magic (0xD00DFEED) at the DRAM base;
     * without it, the BSP enters a multi-core spin loop and never outputs UART.
     */
    size_t dtb_size = (size_t)(_guest_dtb_image_end - _guest_dtb_image);
    if (dtb_size && guest_ram_vaddr) {
        __builtin_memcpy((void *)guest_ram_vaddr, _guest_dtb_image, dtb_size);
        LOG_VMM("  FDT (%zu bytes) copied to guest phys 0x40000000\n", dtb_size);
    } else {
        LOG_VMM_ERR("FDT not embedded or guest_ram_vaddr not set — EDK2 may spin\n");
    }

    if (!virq_controller_init()) {
        LOG_VMM_ERR("Failed to initialise vGIC\n");
        return;
    }

    /* VirtIO-blk: QEMU assigns to highest bus = bus 31 = 0xa003e00, SPI 47 = INTID 79 */
    if (!virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_BLK_IRQ, &virtio_blk_ack, NULL)) {
        LOG_VMM_ERR("Failed to register virtio-blk IRQ %u\n", VIRTIO_BLK_IRQ);
        return;
    }
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
    LOG_VMM("  VirtIO-blk IRQ %u registered\n", VIRTIO_BLK_IRQ);

    /* PL011 UART: direct passthrough, SPI 1 = INTID 33 */
    if (virq_register(GUEST_BOOT_VCPU_ID, UART_IRQ, &uart_ack, NULL)) {
        seL4_IRQHandler_Ack(g_uart_irq_cap);
        LOG_VMM("  UART IRQ %u registered\n", UART_IRQ);
    }

    LOG_VMM("  Starting EDK2 at guest phys 0x0 (EL1h)...\n");
    /* x0 = FDT guest phys address: ArmVirtQemu PrePi reads x0 for the DTB
     * pointer first; without it, PrePi falls back to scanning DRAM but may
     * use a slightly different code path. */
    guest_start(0x0UL, 0x40000000UL, 0UL);
    guest_started = true;
    LOG_VMM("  FreeBSD VMM: EDK2 running\n");
}

/* ── Notification handler ─────────────────────────────────────────────────── */

static void freebsd_vmm_notified(seL4_Word badge)
{
    static uint64_t ntfn_count = 0;
    ntfn_count++;
    /* Log every notification so we can see VirtIO activity */
    if (ntfn_count <= 10 || ntfn_count % 100 == 0)
        LOG_VMM("notified #%llu badge=0x%lx\n",
                (unsigned long long)ntfn_count, (unsigned long)badge);

    if (badge & (seL4_Word)VIRTIO_BLK_NTFN_BADGE) {
        LOG_VMM("virtio-blk IRQ %u: injecting\n", VIRTIO_BLK_IRQ);
        if (!virq_inject(VIRTIO_BLK_IRQ))
            LOG_VMM_ERR("virtio-blk IRQ %u dropped\n", VIRTIO_BLK_IRQ);
    }

    if (badge & (seL4_Word)UART_NTFN_BADGE) {
        if (!virq_inject(UART_IRQ))
            LOG_VMM_ERR("UART IRQ %u dropped\n", UART_IRQ);
    }
}

/* ── Fault handler ────────────────────────────────────────────────────────── */

static seL4_MessageInfo_t freebsd_vmm_fault(seL4_Word badge,
                                            seL4_MessageInfo_t msginfo)
{
    /* Microkit 2.1 encodes VCPU fault badges as (1ULL<<62)|vcpu_id */
    size_t vcpu_id = badge & ~(1ULL << 62);

    /* Diagnostic: print fault types to detect WFI storm or silent hang */
    static uint64_t fault_count = 0;
    static uint64_t wfi_count = 0;
    static uint64_t other_vcpu_count = 0;
    size_t label = seL4_MessageInfo_get_label(msginfo);
    if (label == seL4_Fault_VCPUFault) {
        uint64_t hsr = seL4_GetMR(seL4_VCPUFault_HSR);
        uint64_t ec = (hsr >> 26) & 0x3f;
        if (ec == 0x01) { /* HSR_WFx = 0x01 */
            wfi_count++;
            if (wfi_count <= 3 || wfi_count % 10000 == 0)
                LOG_VMM("WFI fault #%llu\n", (unsigned long long)wfi_count);
        } else {
            other_vcpu_count++;
            if (other_vcpu_count <= 5)
                LOG_VMM("VCPUFault ec=0x%lx hsr=0x%lx\n",
                        (unsigned long)ec,
                        (unsigned long)seL4_GetMR(seL4_VCPUFault_HSR));
        }
    } else if (label == seL4_Fault_VMFault) {
        /* Log VMFaults at GIC distributor region to diagnose vGIC hangs */
        static uint64_t vmfault_count = 0;
        vmfault_count++;
        uint64_t fault_addr = seL4_GetMR(seL4_VMFault_IP);
        uint64_t fault_data_addr = seL4_GetMR(seL4_VMFault_Addr);
        if (fault_data_addr >= 0x8000000 && fault_data_addr < 0x8020000) {
            /* GIC distributor/CPU interface region */
            static uint64_t gic_vmfault_count = 0;
            gic_vmfault_count++;
            if (gic_vmfault_count <= 20 || gic_vmfault_count % 1000 == 0)
                LOG_VMM("GIC VMFault #%llu addr=0x%lx ip=0x%lx\n",
                        (unsigned long long)gic_vmfault_count,
                        (unsigned long)fault_data_addr,
                        (unsigned long)fault_addr);
        }
        (void)vmfault_count;
    } else {
        LOG_VMM("fault label=0x%lx badge=0x%lx\n",
                (unsigned long)label, (unsigned long)badge);
    }
    fault_count++;
    if (fault_count == 1 || fault_count % 1000 == 0)
        LOG_VMM("fault #%llu label=0x%lx\n",
                (unsigned long long)fault_count, (unsigned long)label);

    bool success = fault_handle(vcpu_id, msginfo);
    if (!success)
        LOG_VMM_ERR("Unhandled fault: badge=0x%lx label=0x%lx\n",
                    (unsigned long)badge, (unsigned long)label);
    return seL4_MessageInfo_new(0, 0, 0, 0);
}

/* ── Main loop ────────────────────────────────────────────────────────────── */

#define VMM_EP_CAP    ((seL4_CPtr)1u)
#define VMM_REPLY_CAP ((seL4_CPtr)4u)

void freebsd_vmm_main(seL4_CPtr ep, seL4_CPtr reply_cap)
{
    extern char __sel4_ipc_buffer_obj[];
    seL4_SetIPCBuffer((seL4_IPCBuffer *)__sel4_ipc_buffer_obj);

    init();

    seL4_Word badge;
#ifdef CONFIG_KERNEL_MCS
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge, reply_cap);
#else
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
#endif
    while (1) {
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (label == seL4_Fault_NullFault) {
            freebsd_vmm_notified(badge);
#ifdef CONFIG_KERNEL_MCS
            info = seL4_Recv(ep, &badge, reply_cap);
#else
            info = seL4_Recv(ep, &badge);
#endif
        } else {
            seL4_MessageInfo_t reply = freebsd_vmm_fault(badge, info);
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

__attribute__((section(".text.start"), noreturn))
void _start(void) {
    freebsd_vmm_main(VMM_EP_CAP, VMM_REPLY_CAP);
    __builtin_unreachable();
}

#else /* !ARCH_AARCH64 */

/* Stub for non-AArch64 builds */
__attribute__((section(".text.start"), noreturn))
void _start(void)
{
    sel4_dbg_puts("[freebsd_vmm] AArch64 only\n");
    while (1) {}
    __builtin_unreachable();
}

#endif /* ARCH_AARCH64 */
