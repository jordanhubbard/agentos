/*
 * agentOS FreeBSD VMM — Virtual Machine Monitor
 *
 * Boots FreeBSD 14 AArch64 directly from /boot/kernel/kernel.bin using libvmm
 * on seL4/Microkit.
 *
 * Boot sequence:
 *   1. VMM copies kernel.bin (_guest_kernel_image) to guest phys 0x40000000.
 *   2. VMM copies freebsd-edk2.dtb (_guest_dtb_image) near the top of guest RAM.
 *   3. guest_start(0x40000000, fdt, 0): vCPU PC = FreeBSD Image entry, EL1h.
 *   4. FreeBSD reads /chosen/bootargs, mounts /dev/vtbd0p3, and uses the
 *      QEMU virtio-mmio devices exposed in the FDT.
 *
 * Memory layout (host phys -> guest phys):
 *   guest_ram (384MB) host:0x40000000 -> guest:0x40000000  (main RAM)
 *     [kernel.bin at 0x40000000, FDT at 0x57f00000]
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
#include <libvmm/arch/aarch64/vgic/vgic.h>

/* ── Raw agentOS CNode layout constants ─────────────────────────────────── */
#define AGENTOS_IRQ_CAP_BASE     64u
#define AGENTOS_VMM_TCB_CAP_BASE 266u
#define AGENTOS_VMM_VCPU_CAP_BASE 330u

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
    (seL4_CPtr)(AGENTOS_IRQ_CAP_BASE + 1u);    /* INTID 79, SPI 47 */
static const seL4_CPtr g_virtio_net_irq_cap =
    (seL4_CPtr)(AGENTOS_IRQ_CAP_BASE + 0u);    /* INTID 48, SPI 16 */

/* ── Notification badge bits from system_desc_aarch64.c ─────────────────── */
#define VIRTIO_NET_NTFN_BADGE  0x1u
#define VIRTIO_BLK_NTFN_BADGE  0x2u

/* ── GIC INTID values ────────────────────────────────────────────────────── */
#define VIRTIO_BLK_IRQ  79u    /* QEMU virt bus 31 = 0xa003e00, SPI 47 */
#define VIRTIO_NET_IRQ  48u    /* QEMU virt bus 0  = 0xa000000, SPI 16 */

/* ── Guest image symbols (kernel Image + FDT from package_guest_images.S) ── */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];

/* ── Guest memory map symbols ────────────────────────────────────────────── */
uintptr_t uefi_code_vaddr;   /* VMM virtual address of uefi_code MR */
uintptr_t uefi_data_vaddr;   /* VMM virtual address of uefi_data MR */
uintptr_t guest_ram_vaddr;   /* VMM virtual address of guest_ram MR */

#define FREEBSD_UEFI_CODE_VADDR 0x00000000UL
#define FREEBSD_UEFI_DATA_VADDR 0x04000000UL
#define FREEBSD_GUEST_RAM_VADDR 0x40000000UL
#define FREEBSD_GUEST_RAM_SIZE  0x18000000UL
#define FREEBSD_KERNEL_VADDR    0x40000000UL
#define FREEBSD_FDT_VADDR       0x57f00000UL
#define FREEBSD_UEFI_DATA_SIZE  0x04000000UL
#define FREEBSD_VTIMER_IRQ      27u

static bool guest_started = false;

static void freebsd_copy_to_guest(uintptr_t dst_addr, const void *src, size_t n)
{
    volatile uint8_t *dst = (volatile uint8_t *)dst_addr;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s[i];
    }
}

/* ── IRQ ack callbacks ───────────────────────────────────────────────────── */

static void virtio_blk_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
}

static void virtio_net_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
    seL4_IRQHandler_Ack(g_virtio_net_irq_cap);
}

/* ─── PL011 UART MMIO Emulation ─────────────────────────────────────────── */
#define PL011_BASE       0x9000000UL
#define PL011_SIZE       0x1000UL
#define PL011_DR         0x00u
#define PL011_RSR_ECR    0x04u
#define PL011_FR         0x18u
#define PL011_ILPR       0x20u
#define PL011_IBRD       0x24u
#define PL011_FBRD       0x28u
#define PL011_LCRH       0x2cu
#define PL011_CR         0x30u
#define PL011_IFLS       0x34u
#define PL011_IMSC       0x38u
#define PL011_RIS        0x3cu
#define PL011_MIS        0x40u
#define PL011_ICR        0x44u
#define PL011_DMACR      0x48u
#define PL011_FR_TXFE    (1u << 7)
#define PL011_FR_RXFE    (1u << 4)
#define PL011_CR_UARTEN  (1u << 0)
#define PL011_CR_TXE     (1u << 8)
#define PL011_CR_RXE     (1u << 9)

static uint32_t pl011_rsr_ecr;
static uint32_t pl011_ilpr;
static uint32_t pl011_ibrd;
static uint32_t pl011_fbrd;
static uint32_t pl011_lcrh;
static uint32_t pl011_cr = PL011_CR_TXE | PL011_CR_RXE;
static uint32_t pl011_ifls = 0x12u;
static uint32_t pl011_imsc;
static uint32_t pl011_dmacr;

static void pl011_store32(uint32_t *reg, size_t offset, uint64_t fsr,
                          uint32_t value)
{
    uint32_t mask = (uint32_t)fault_get_data_mask((uint64_t)offset, fsr);
    uint32_t shift = (uint32_t)((offset & 0x3u) * 8u);
    *reg = (*reg & ~mask) | ((value << shift) & mask);
}

static uint32_t pl011_read(size_t offset)
{
    switch (offset) {
    case PL011_DR:
        return 0;
    case PL011_RSR_ECR:
        return pl011_rsr_ecr;
    case PL011_FR:
        return PL011_FR_TXFE | PL011_FR_RXFE;
    case PL011_ILPR:
        return pl011_ilpr;
    case PL011_IBRD:
        return pl011_ibrd;
    case PL011_FBRD:
        return pl011_fbrd;
    case PL011_LCRH:
        return pl011_lcrh;
    case PL011_CR:
        return pl011_cr;
    case PL011_IFLS:
        return pl011_ifls;
    case PL011_IMSC:
        return pl011_imsc;
    case PL011_RIS:
    case PL011_MIS:
        return 0;
    case PL011_DMACR:
        return pl011_dmacr;
    case 0xfe0u:
        return 0x11u; /* UARTPeriphID0 */
    case 0xfe4u:
        return 0x10u; /* UARTPeriphID1 */
    case 0xfe8u:
        return 0x34u; /* UARTPeriphID2: PL011 r1p4 */
    case 0xfecu:
        return 0x00u; /* UARTPeriphID3 */
    case 0xff0u:
        return 0x0du; /* UARTPCellID0 */
    case 0xff4u:
        return 0xf0u; /* UARTPCellID1 */
    case 0xff8u:
        return 0x05u; /* UARTPCellID2 */
    case 0xffcu:
        return 0xb1u; /* UARTPCellID3 */
    default:
        return 0;
    }
}

static void pl011_write(size_t offset, uint64_t fsr, seL4_UserContext *regs)
{
    uint32_t value = (uint32_t)fault_get_data(regs, fsr);

    switch (offset) {
    case PL011_DR:
        _uart_putc((char)(value & 0xffu));
        break;
    case PL011_RSR_ECR:
        pl011_rsr_ecr = 0;
        break;
    case PL011_ILPR:
        pl011_store32(&pl011_ilpr, offset, fsr, value);
        break;
    case PL011_IBRD:
        pl011_store32(&pl011_ibrd, offset, fsr, value);
        break;
    case PL011_FBRD:
        pl011_store32(&pl011_fbrd, offset, fsr, value);
        break;
    case PL011_LCRH:
        pl011_store32(&pl011_lcrh, offset, fsr, value);
        break;
    case PL011_CR:
        pl011_store32(&pl011_cr, offset, fsr, value);
        break;
    case PL011_IFLS:
        pl011_store32(&pl011_ifls, offset, fsr, value);
        break;
    case PL011_IMSC:
        pl011_store32(&pl011_imsc, offset, fsr, value);
        break;
    case PL011_ICR:
        break;
    case PL011_DMACR:
        pl011_store32(&pl011_dmacr, offset, fsr, value);
        break;
    default:
        break;
    }
}

static bool pl011_fault_handler(size_t vcpu_id, size_t offset, size_t fsr,
                                 seL4_UserContext *regs, void *data)
{
    (void)vcpu_id;
    (void)data;
    if (fault_is_read((uint64_t)fsr)) {
        fault_emulate_write(regs, (size_t)(PL011_BASE + offset),
                            (size_t)fsr, (size_t)pl011_read(offset));
        return true;
    }

    pl011_write(offset, (uint64_t)fsr, regs);
    return true;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void init(void)
{
    LOG_VMM("agentOS freebsd_vmm starting\n");

    vmm_register_vcpu(GUEST_BOOT_VCPU_ID,
                      AGENTOS_VMM_VCPU_CAP_BASE + GUEST_BOOT_VCPU_ID,
                      AGENTOS_VMM_TCB_CAP_BASE  + GUEST_BOOT_VCPU_ID);

    size_t kernel_size = (size_t)(_guest_kernel_image_end - _guest_kernel_image);
    if (kernel_size == 0) {
        LOG_VMM_ERR("FreeBSD kernel image not linked (build with GUEST_OS=freebsd)\n");
        return;
    }
    LOG_VMM("  FreeBSD kernel: %zu bytes (%.1f MB)\n",
            kernel_size, (double)kernel_size / (1024.0 * 1024.0));

    if (uefi_code_vaddr == 0) {
        uefi_code_vaddr = FREEBSD_UEFI_CODE_VADDR;
    }
    if (uefi_data_vaddr == 0) {
        uefi_data_vaddr = FREEBSD_UEFI_DATA_VADDR;
    }
    if (guest_ram_vaddr == 0) {
        guest_ram_vaddr = FREEBSD_GUEST_RAM_VADDR;
    }

    uintptr_t kernel_dst = guest_ram_vaddr +
        (FREEBSD_KERNEL_VADDR - FREEBSD_GUEST_RAM_VADDR);
    uintptr_t fdt_dst = guest_ram_vaddr +
        (FREEBSD_FDT_VADDR - FREEBSD_GUEST_RAM_VADDR);
    if ((FREEBSD_KERNEL_VADDR + kernel_size) >= FREEBSD_FDT_VADDR) {
        LOG_VMM_ERR("FreeBSD kernel overlaps FDT load address\n");
        return;
    }

    freebsd_copy_to_guest(kernel_dst, _guest_kernel_image, kernel_size);
    LOG_VMM("  FreeBSD kernel copied to guest phys 0x%lx\n",
            (unsigned long)FREEBSD_KERNEL_VADDR);

    /*
     * Copy the FDT away from the kernel Image.  x0 carries this pointer per the
     * arm64 boot ABI, and the FreeBSD kernel reads /chosen/bootargs from it.
     */
    size_t dtb_size = (size_t)(_guest_dtb_image_end - _guest_dtb_image);
    if (dtb_size && guest_ram_vaddr &&
        (FREEBSD_FDT_VADDR + dtb_size) <=
        (FREEBSD_GUEST_RAM_VADDR + FREEBSD_GUEST_RAM_SIZE)) {
        freebsd_copy_to_guest(fdt_dst, _guest_dtb_image, dtb_size);
        LOG_VMM("  FDT (%zu bytes) copied to guest phys 0x%lx\n",
                dtb_size, (unsigned long)FREEBSD_FDT_VADDR);
    } else {
        LOG_VMM_ERR("FDT not embedded, guest_ram_vaddr unset, or FDT out of RAM\n");
    }

    if (!virq_controller_init()) {
        LOG_VMM_ERR("Failed to initialise vGIC\n");
        return;
    }

    if (!fault_register_vm_exception_handler(PL011_BASE, PL011_SIZE,
                                             pl011_fault_handler, NULL)) {
        LOG_VMM_ERR("Failed to register PL011 UART fault handler\n");
    }

    if (!virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_NET_IRQ, &virtio_net_ack, NULL)) {
        LOG_VMM_ERR("Failed to register virtio-net IRQ %u\n", VIRTIO_NET_IRQ);
        return;
    }
    seL4_IRQHandler_Ack(g_virtio_net_irq_cap);
    LOG_VMM("  VirtIO-net IRQ %u registered\n", VIRTIO_NET_IRQ);

    /* VirtIO-blk: QEMU assigns to highest bus = bus 31 = 0xa003e00, SPI 47 = INTID 79 */
    if (!virq_register(GUEST_BOOT_VCPU_ID, VIRTIO_BLK_IRQ, &virtio_blk_ack, NULL)) {
        LOG_VMM_ERR("Failed to register virtio-blk IRQ %u\n", VIRTIO_BLK_IRQ);
        return;
    }
    seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
    LOG_VMM("  VirtIO-blk IRQ %u registered\n", VIRTIO_BLK_IRQ);

    LOG_VMM("  Starting FreeBSD kernel at guest phys 0x%lx (EL1h)...\n",
            (unsigned long)FREEBSD_KERNEL_VADDR);
    guest_start(FREEBSD_KERNEL_VADDR, FREEBSD_FDT_VADDR, 0UL);
    guest_started = true;
    LOG_VMM("  FreeBSD VMM: kernel running\n");
}

/* ── Notification handler ─────────────────────────────────────────────────── */

static void freebsd_vmm_notified(seL4_Word badge)
{
    static uint64_t ntfn_count = 0;
    ntfn_count++;
    if (ntfn_count <= 10 || ntfn_count % 1000 == 0)
        LOG_VMM("notified #%llu badge=0x%lx\n",
                (unsigned long long)ntfn_count, (unsigned long)badge);

    if (badge & (seL4_Word)VIRTIO_BLK_NTFN_BADGE) {
        bool injected = virq_inject(VIRTIO_BLK_IRQ);
        if (ntfn_count <= 10 || ntfn_count % 1000 == 0 || !injected)
            LOG_VMM("virtio-blk IRQ %u: inject=%s\n",
                    VIRTIO_BLK_IRQ, injected ? "ok" : "deferred");
        if (!injected) {
            LOG_VMM_ERR("virtio-blk IRQ %u dropped\n", VIRTIO_BLK_IRQ);
            seL4_IRQHandler_Ack(g_virtio_blk_irq_cap);
        }
    }

    if (badge & (seL4_Word)VIRTIO_NET_NTFN_BADGE) {
        bool injected = virq_inject(VIRTIO_NET_IRQ);
        if (ntfn_count <= 10 || ntfn_count % 1000 == 0 || !injected)
            LOG_VMM("virtio-net IRQ %u: inject=%s\n",
                    VIRTIO_NET_IRQ, injected ? "ok" : "deferred");
        if (!injected) {
            LOG_VMM_ERR("virtio-net IRQ %u dropped\n", VIRTIO_NET_IRQ);
            seL4_IRQHandler_Ack(g_virtio_net_irq_cap);
        }
    }
}

static bool freebsd_handle_vppi_event(size_t vcpu_id)
{
    static uint64_t vppi_count = 0;
    vppi_count++;

    seL4_Word ppi_irq = seL4_GetMR(seL4_VPPIEvent_IRQ);
    seL4_Word cntv_ctl = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL);
    seL4_Word cntv_cval = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CVAL);

    bool injected = vgic_inject_irq(vcpu_id, (int)ppi_irq);
    if (!injected) {
        vmm_vcpu_arm_ack_vppi(vcpu_id, ppi_irq);
    }

    if (vppi_count <= 4 || (vppi_count % 1000000) == 0 || !injected) {
        LOG_VMM("FreeBSD VPPIEvent #%llu IRQ %lu ctl=0x%lx cval=0x%lx action=%s\n",
                (unsigned long long)vppi_count,
                (unsigned long)ppi_irq,
                (unsigned long)cntv_ctl,
                (unsigned long)cntv_cval,
                injected ? "inject" : "ack-drop");
    }

    return true;
}

/* ── Fault handler ────────────────────────────────────────────────────────── */

static seL4_MessageInfo_t freebsd_vmm_fault(seL4_Word badge,
                                            seL4_MessageInfo_t msginfo)
{
    /* Microkit 2.1 encodes VCPU fault badges as (1ULL<<62)|vcpu_id */
    size_t vcpu_id = badge & ~(1ULL << 62);

    static uint64_t wfi_count = 0;
    static uint64_t other_vcpu_count = 0;
    size_t label = seL4_MessageInfo_get_label(msginfo);
    if (label == seL4_Fault_VCPUFault) {
        uint64_t hsr = seL4_GetMR(seL4_VCPUFault_HSR);
        uint64_t ec = (hsr >> 26) & 0x3f;
        if (ec == 0x01) { /* HSR_WFx = 0x01 */
            wfi_count++;
            if (wfi_count <= 3 || wfi_count % 100000 == 0)
                LOG_VMM("WFI fault #%llu\n", (unsigned long long)wfi_count);
        } else {
            other_vcpu_count++;
            if (other_vcpu_count <= 5)
                LOG_VMM("VCPUFault ec=0x%lx hsr=0x%lx\n",
                        (unsigned long)ec,
                        (unsigned long)seL4_GetMR(seL4_VCPUFault_HSR));
        }
    } else if (label == seL4_Fault_VMFault) {
        static uint64_t vmfault_count = 0;
        vmfault_count++;
        uint64_t fault_addr = seL4_GetMR(seL4_VMFault_IP);
        uint64_t fault_data_addr = seL4_GetMR(seL4_VMFault_Addr);
        uint64_t fault_fsr = seL4_GetMR(seL4_VMFault_FSR);
        if (vmfault_count <= 8) {
            LOG_VMM("VMFault #%llu addr=0x%lx ip=0x%lx fsr=0x%lx\n",
                    (unsigned long long)vmfault_count,
                    (unsigned long)fault_data_addr,
                    (unsigned long)fault_addr,
                    (unsigned long)fault_fsr);
        }
        if (fault_data_addr >= 0x8000000 && fault_data_addr < 0x8020000) {
            /* GIC distributor/CPU interface region */
            static uint64_t gic_vmfault_count = 0;
            gic_vmfault_count++;
            if (gic_vmfault_count <= 8)
                LOG_VMM("GIC VMFault #%llu addr=0x%lx ip=0x%lx\n",
                        (unsigned long long)gic_vmfault_count,
                        (unsigned long)fault_data_addr,
                        (unsigned long)fault_addr);
        }
        (void)vmfault_count;
    } else {
        if (label != seL4_Fault_VPPIEvent &&
            label != seL4_Fault_VGICMaintenance) {
            LOG_VMM("fault label=0x%lx badge=0x%lx\n",
                    (unsigned long)label, (unsigned long)badge);
        }
    }
    bool success = (label == seL4_Fault_VPPIEvent)
        ? freebsd_handle_vppi_event(vcpu_id)
        : fault_handle(vcpu_id, msginfo);
    if (!success)
        LOG_VMM_ERR("Unhandled fault: badge=0x%lx label=0x%lx\n",
                    (unsigned long)badge, (unsigned long)label);
    return seL4_MessageInfo_new(0, 0, 0, 0);
}

/* ── Main loop ────────────────────────────────────────────────────────────── */

#define VMM_EP_CAP    ((seL4_CPtr)7u)
#define VMM_REPLY_CAP ((seL4_CPtr)9u)

void freebsd_vmm_main(seL4_CPtr ep, seL4_CPtr reply_cap)
{
    seL4_SetIPCBuffer((seL4_IPCBuffer *)0x10000000UL);

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
void _start(seL4_CPtr my_ep, seL4_CPtr ns_ep) {
    (void)ns_ep;
    seL4_CPtr ep = (my_ep != seL4_CapNull) ? my_ep : VMM_EP_CAP;
    freebsd_vmm_main(ep, VMM_REPLY_CAP);
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
