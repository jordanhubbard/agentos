/*
 * agentOS FreeBSD VMM — Virtual Machine Monitor
 *
 * Boots FreeBSD 15 AArch64 directly from /boot/kernel/kernel using libvmm
 * on seL4/Microkit.
 *
 * Boot sequence:
 *   1. VMM copies the FreeBSD kernel (_guest_kernel_image) to guest RAM.
 *   2. VMM copies freebsd-direct.dtb (_guest_dtb_image) near the top of RAM.
 *   3. guest_start(kernel, fdt, 0): vCPU PC = FreeBSD Image entry, x0 = FDT.
 *   4. FreeBSD mounts the 15.0 DVD ISO through agentOS's emulated virtio-blk
 *      endpoint and reaches the emulated PL011/CC-PD console path.
 *
 * Guest RAM is anonymous VMM memory, not identity-mapped host RAM. All
 * emulated VirtIO queue and payload GPAs are translated before dereference.
 * Host bus.31 is owned only by the canonical block-service PD.
 *
 * PSCI: FreeBSD uses SMC-based PSCI (psci { method = "smc"; }).
 *   seL4 intercepts HVC from VCPU EL1 as a seL4 UnknownSyscall, so we use SMC.
 *   libvmm fault.c routes HSR_SMC_64_EXCEPTION to smc_handle() / handle_psci().
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sel4_boot.h"
#include "contracts/guest_contract.h"
#include "contracts/cc_contract.h"
#include "contracts/freebsd_vmm_contract.h"
#include "sel4_ipc.h"

#if defined(ARCH_AARCH64)

#include <libvmm/libvmm.h>
#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <platform/blk_layout.h>
#include <platform/guest_ram.h>
#include <platform/vmm_virtio_net.h>
#include <platform/vmm_virtio_blk.h>
#include <contracts/net-service/interface.h>
#include "serial_log.h"

/* ── Raw agentOS CNode layout constants ─────────────────────────────────── */
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

/* FreeBSD VMM diagnostics are serialized by the generic serial driver. */
static serial_log_t g_vmm_log = {
    .ep = PD_CNODE_SLOT_SERIAL_EP,
};

void microkit_dbg_putc(char c) { serial_log_putc(&g_vmm_log, c); }

void microkit_dbg_puts(const char *s)
{
    serial_log_puts(&g_vmm_log, s);
}

void microkit_dbg_put32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    serial_log_putc(&g_vmm_log, '0');
    serial_log_putc(&g_vmm_log, 'x');
    for (int i = 28; i >= 0; i -= 4)
        serial_log_putc(&g_vmm_log, hex[(v >> i) & 0xfu]);
    serial_log_flush(&g_vmm_log);
}

seL4_IPCBuffer *__sel4_ipc_buffer = NULL;

vmm_vcpu_t g_vmm_vcpus[VMM_MAX_VCPUS];
#if defined(AGENTOS_GUEST_BOTH)
static uint32_t g_guest_state = GUEST_STATE_READY;
#else
static uint32_t g_guest_state = GUEST_STATE_RUNNING;
#endif

/* ── Guest image symbols (kernel + FDT from package_guest_images.S) ─────── */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];

/* ── Guest memory map symbols ────────────────────────────────────────────── */
uintptr_t guest_ram_vaddr;   /* VMM virtual address of guest_ram MR */

#define FREEBSD_GUEST_RAM_VADDR 0x40000000UL
#define FREEBSD_KERNEL_VADDR    0x40000000UL
#if defined(AGENTOS_GUEST_BOTH)
#define FREEBSD_FDT_VADDR       0x4f000000UL
#define FREEBSD_GUEST_RAM_SIZE  0x10000000UL
#else
#define FREEBSD_FDT_VADDR       0x5f000000UL
#define FREEBSD_GUEST_RAM_SIZE  0x20000000UL
#endif
#define FREEBSD_VTIMER_IRQ      27u

#ifndef FREEBSD_IRQ_TRACE
#define FREEBSD_IRQ_TRACE 0
#endif

static bool guest_started = false;
static bool g_freebsd_startable = false;
static bool g_freebsd_runtime_ready = false;

static void freebsd_copy_to_guest(uintptr_t dst_addr, const void *src, size_t n)
{
    volatile uint8_t *dst = (volatile uint8_t *)dst_addr;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s[i];
    }
}

static void uart_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)vcpu_id; (void)irq; (void)cookie;
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
#define PL011_RXIS       (1u << 4)
#define PL011_TXIS       (1u << 5)
#define PL011_RTIS       (1u << 6)
#define FREEBSD_UART_IRQ 33u
#define GUEST_CONSOLE_TX_RING_SIZE 8192u
#define GUEST_CONSOLE_RX_RING_SIZE 1024u
#define GUEST_INPUT_RAW_BYTE_BASE  0x100u

static uint32_t pl011_rsr_ecr;
static uint32_t pl011_ilpr;
static uint32_t pl011_ibrd;
static uint32_t pl011_fbrd;
static uint32_t pl011_lcrh;
static uint32_t pl011_cr = PL011_CR_TXE | PL011_CR_RXE;
static uint32_t pl011_ifls = 0x12u;
static uint32_t pl011_imsc;
static uint32_t pl011_dmacr;
static uint32_t pl011_irq_latch;

static uint8_t console_tx_ring[GUEST_CONSOLE_TX_RING_SIZE];
static uint32_t console_tx_head;
static uint32_t console_tx_tail;
static uint32_t console_tx_count;

static uint8_t console_rx_ring[GUEST_CONSOLE_RX_RING_SIZE];
static uint32_t console_rx_head;
static uint32_t console_rx_tail;
static uint32_t console_rx_count;

static uint32_t vmm_msg_rd32(const uint8_t *src, uint32_t off)
{
    return ((uint32_t)src[off + 0u]) |
           ((uint32_t)src[off + 1u] << 8u) |
           ((uint32_t)src[off + 2u] << 16u) |
           ((uint32_t)src[off + 3u] << 24u);
}

static void vmm_msg_wr32(uint8_t *dst, uint32_t off, uint32_t value)
{
    dst[off + 0u] = (uint8_t)(value & 0xffu);
    dst[off + 1u] = (uint8_t)((value >> 8u) & 0xffu);
    dst[off + 2u] = (uint8_t)((value >> 16u) & 0xffu);
    dst[off + 3u] = (uint8_t)((value >> 24u) & 0xffu);
}

static void console_tx_push(uint8_t byte)
{
    if (console_tx_count == GUEST_CONSOLE_TX_RING_SIZE) {
        console_tx_tail = (console_tx_tail + 1u) % GUEST_CONSOLE_TX_RING_SIZE;
        console_tx_count--;
    }
    console_tx_ring[console_tx_head] = byte;
    console_tx_head = (console_tx_head + 1u) % GUEST_CONSOLE_TX_RING_SIZE;
    console_tx_count++;
}

static uint32_t console_tx_drain(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0u;
    while (n < max && console_tx_count > 0u) {
        dst[n++] = console_tx_ring[console_tx_tail];
        console_tx_tail = (console_tx_tail + 1u) % GUEST_CONSOLE_TX_RING_SIZE;
        console_tx_count--;
    }
    return n;
}

static bool console_rx_push(uint8_t byte)
{
    if (console_rx_count == GUEST_CONSOLE_RX_RING_SIZE) return false;
    console_rx_ring[console_rx_head] = byte;
    console_rx_head = (console_rx_head + 1u) % GUEST_CONSOLE_RX_RING_SIZE;
    console_rx_count++;
    return true;
}

static bool console_rx_pop(uint8_t *byte)
{
    if (console_rx_count == 0u) return false;
    *byte = console_rx_ring[console_rx_tail];
    console_rx_tail = (console_rx_tail + 1u) % GUEST_CONSOLE_RX_RING_SIZE;
    console_rx_count--;
    return true;
}

static uint32_t pl011_pending_irqs(void)
{
    uint32_t pending = pl011_irq_latch;
    if (console_rx_count > 0u) {
        pending |= PL011_RXIS | PL011_RTIS;
    }
    return pending;
}

static void pl011_maybe_inject_irq(void)
{
    uint32_t pending = pl011_pending_irqs();
    uint32_t masked = pending & pl011_imsc;
    if (guest_started && masked != 0u) {
        bool injected = virq_inject(FREEBSD_UART_IRQ);
        if (!injected) {
            LOG_VMM_ERR("PL011 IRQ pending=0x%lx imsc=0x%lx inject failed\n",
                        (unsigned long)pending,
                        (unsigned long)pl011_imsc);
        }
    }
}

#if FREEBSD_IRQ_TRACE
static void freebsd_log_irq_state(const char *where, unsigned count)
{
    if (count <= 8u || (count % 1000u) == 0u) {
        LOG_VMM("%s irq%u pending=%u enabled=%u inflight=%u irq%u pending=%u enabled=%u inflight=%u\n",
                where,
                AOS_VIRTIO_BLK_VIRQ,
                vgic_irq_is_pending(GUEST_BOOT_VCPU_ID, AOS_VIRTIO_BLK_VIRQ) ? 1u : 0u,
                vgic_irq_is_enabled(GUEST_BOOT_VCPU_ID, AOS_VIRTIO_BLK_VIRQ) ? 1u : 0u,
                vgic_irq_is_inflight(GUEST_BOOT_VCPU_ID, AOS_VIRTIO_BLK_VIRQ) ? 1u : 0u,
                FREEBSD_UART_IRQ,
                vgic_irq_is_pending(GUEST_BOOT_VCPU_ID, FREEBSD_UART_IRQ) ? 1u : 0u,
                vgic_irq_is_enabled(GUEST_BOOT_VCPU_ID, FREEBSD_UART_IRQ) ? 1u : 0u,
                vgic_irq_is_inflight(GUEST_BOOT_VCPU_ID, FREEBSD_UART_IRQ) ? 1u : 0u);
    }
}
#endif

static void guest_console_write(uint8_t byte)
{
    /* Guest PL011 output belongs to this guest's virtual TTY. */
    console_tx_push(byte);
}

static bool input_event_to_byte(uint32_t event_type, uint32_t keycode,
                                uint8_t *byte)
{
    if (event_type != 1u) return false; /* CC_INPUT_KEY_DOWN */

    if ((keycode & 0xffffff00u) == GUEST_INPUT_RAW_BYTE_BASE) {
        *byte = (uint8_t)(keycode & 0xffu);
        return true;
    }

    if (keycode >= 0x04u && keycode <= 0x1du) {
        *byte = (uint8_t)('a' + (keycode - 0x04u));
        return true;
    }
    if (keycode >= 0x1eu && keycode <= 0x26u) {
        *byte = (uint8_t)('1' + (keycode - 0x1eu));
        return true;
    }

    switch (keycode) {
    case 0x27u: *byte = '0'; return true;
    case 0x28u: *byte = '\r'; return true;
    case 0x29u: *byte = 0x1bu; return true;
    case 0x2au: *byte = 0x7fu; return true;
    case 0x2bu: *byte = '\t'; return true;
    case 0x2cu: *byte = ' '; return true;
    case 0x2du: *byte = '-'; return true;
    case 0x2eu: *byte = '='; return true;
    case 0x2fu: *byte = '['; return true;
    case 0x30u: *byte = ']'; return true;
    case 0x31u: *byte = '\\'; return true;
    case 0x33u: *byte = ';'; return true;
    case 0x34u: *byte = '\''; return true;
    case 0x35u: *byte = '`'; return true;
    case 0x36u: *byte = ','; return true;
    case 0x37u: *byte = '.'; return true;
    case 0x38u: *byte = '/'; return true;
    default: return false;
    }
}

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
    {
        uint8_t byte = 0u;
        (void)console_rx_pop(&byte);
        pl011_maybe_inject_irq();
        return byte;
    }
    case PL011_RSR_ECR:
        return pl011_rsr_ecr;
    case PL011_FR:
        return PL011_FR_TXFE |
               (console_rx_count == 0u ? PL011_FR_RXFE : 0u);
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
        return pl011_pending_irqs();
    case PL011_MIS:
        return pl011_pending_irqs() & pl011_imsc;
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
        guest_console_write((uint8_t)(value & 0xffu));
        if ((pl011_cr & PL011_CR_TXE) != 0u) {
            pl011_irq_latch |= PL011_TXIS;
        }
        pl011_maybe_inject_irq();
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
        pl011_maybe_inject_irq();
        break;
    case PL011_IFLS:
        pl011_store32(&pl011_ifls, offset, fsr, value);
        break;
    case PL011_IMSC:
        pl011_store32(&pl011_imsc, offset, fsr, value);
        pl011_maybe_inject_irq();
        break;
    case PL011_ICR:
        pl011_irq_latch &= ~(value & (PL011_RXIS | PL011_TXIS | PL011_RTIS));
        pl011_maybe_inject_irq();
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

static bool freebsd_vmm_prepare_runtime(void)
{
    if (g_freebsd_runtime_ready) {
        return true;
    }

    if (!virq_controller_init()) {
        LOG_VMM_ERR("Failed to initialise vGIC\n");
        return false;
    }

    if (!fault_register_vm_exception_handler(PL011_BASE, PL011_SIZE,
                                             pl011_fault_handler, NULL)) {
        LOG_VMM_ERR("Failed to register PL011 UART fault handler\n");
        return false;
    }

    /*
     * Register the guest-only VirtIO endpoint after the vGIC exists.
     * libvmm installs its MMIO fault handler and virtual IRQ here; no host
     * transport or hardware IRQ capability enters this VMM.
     */
    aos_vmm_virtio_net_init(1u);
    aos_vmm_virtio_blk_init(AOS_HOST_BLK_MEDIA_FREEBSD);

    if (!virq_register(GUEST_BOOT_VCPU_ID, FREEBSD_UART_IRQ, &uart_ack, NULL)) {
        LOG_VMM_ERR("Failed to register UART IRQ %u\n", FREEBSD_UART_IRQ);
        return false;
    }
    LOG_VMM("  PL011 UART IRQ %u registered\n", FREEBSD_UART_IRQ);

    g_freebsd_runtime_ready = true;
    return true;
}

static bool freebsd_vmm_start_guest(void)
{
    if (guest_started) {
        return true;
    }
    if (!g_freebsd_startable) {
        return false;
    }
    if (!freebsd_vmm_prepare_runtime()) {
        return false;
    }

    LOG_VMM("  Starting FreeBSD kernel at guest phys 0x%lx with FDT 0x%lx...\n",
            (unsigned long)FREEBSD_KERNEL_VADDR,
            (unsigned long)FREEBSD_FDT_VADDR);
    guest_start(FREEBSD_KERNEL_VADDR, FREEBSD_FDT_VADDR, 0UL);
    guest_started = true;
    g_guest_state = GUEST_STATE_RUNNING;
    LOG_VMM("  FreeBSD VMM: kernel running\n");
    return true;
}

static void freebsd_vmm_suspend_guest_tcb(void)
{
    seL4_UserContext regs = {0};
    seL4_Error err = seL4_TCB_ReadRegisters(
        (seL4_CPtr)(AGENTOS_VMM_TCB_CAP_BASE + GUEST_BOOT_VCPU_ID),
        true,
        0,
        SEL4_USER_CONTEXT_SIZE,
        &regs);
    if (err != seL4_NoError) {
        LOG_VMM_ERR("FreeBSD guest suspend/read-registers failed: %d\n", (int)err);
        seL4_TCB_Suspend((seL4_CPtr)(AGENTOS_VMM_TCB_CAP_BASE + GUEST_BOOT_VCPU_ID));
    }
}

#define FREEBSD_KERNBASE         0xffff000000000000ull
#define FREEBSD_DMAPBASE         0xffffa00000000000ull
#define FREEBSD_ALLPROC_VA       0xffff000001088b00ull
#define FREEBSD_PROC_THREADS_OFF 16u
#define FREEBSD_PROC_PID_OFF     196u
#define FREEBSD_THREAD_PCB_OFF   1152u
#define FREEBSD_PCB_FP_OFF       80u
#define FREEBSD_PCB_LR_OFF       88u
#define FREEBSD_PCB_SP_OFF       96u

static void *freebsd_debug_va(uint64_t va, size_t len)
{
    uint64_t off;
    if (len > FREEBSD_GUEST_RAM_SIZE) {
        return NULL;
    }
    if (va >= FREEBSD_DMAPBASE) {
        off = va - FREEBSD_DMAPBASE;
    } else if (va >= FREEBSD_KERNBASE) {
        off = va - FREEBSD_KERNBASE;
        if (off > FREEBSD_GUEST_RAM_SIZE - len) {
            uint64_t table =
                vmm_vcpu_arm_read_reg(
                    GUEST_BOOT_VCPU_ID, seL4_VCPUReg_TTBR1) &
                0x0000fffffffff000ull;
            for (unsigned level = 0u; level < 4u; level++) {
                unsigned shift = 39u - level * 9u;
                uint64_t index = (va >> shift) & 0x1ffu;
                uint64_t *entry = aos_gpa_to_hva_configured(
                    table + index * sizeof(uint64_t), sizeof(uint64_t));
                if (entry == NULL || ((*entry & 1u) == 0u)) {
                    return NULL;
                }
                if (level < 3u && ((*entry & 2u) == 0u)) {
                    uint64_t block_mask = (1ull << shift) - 1ull;
                    uint64_t gpa =
                        (*entry & 0x0000fffffffff000ull) & ~block_mask;
                    return aos_gpa_to_hva_configured(
                        gpa | (va & block_mask), len);
                }
                table = *entry & 0x0000fffffffff000ull;
            }
            return aos_gpa_to_hva_configured(
                table | (va & 0xfffu), len);
        }
    } else {
        return NULL;
    }
    if (off > FREEBSD_GUEST_RAM_SIZE - len) {
        return NULL;
    }
    return aos_gpa_to_hva_configured(FREEBSD_GUEST_RAM_VADDR + off, len);
}

static uint64_t freebsd_debug_u64(uint64_t va)
{
    uint64_t value = 0u;
    uint8_t *src = freebsd_debug_va(va, sizeof(value));
    if (src != NULL) {
        for (size_t i = 0u; i < sizeof(value); i++) {
            ((uint8_t *)&value)[i] = src[i];
        }
    }
    return value;
}

static uint32_t freebsd_debug_u32(uint64_t va)
{
    uint32_t value = 0u;
    uint8_t *src = freebsd_debug_va(va, sizeof(value));
    if (src != NULL) {
        for (size_t i = 0u; i < sizeof(value); i++) {
            ((uint8_t *)&value)[i] = src[i];
        }
    }
    return value;
}

static void freebsd_dump_init_stack(void)
{
    uint64_t proc = freebsd_debug_u64(FREEBSD_ALLPROC_VA);
    for (unsigned n = 0u; proc != 0u && n < 256u; n++) {
        if (freebsd_debug_u32(proc + FREEBSD_PROC_PID_OFF) == 1u) {
            uint64_t td =
                freebsd_debug_u64(proc + FREEBSD_PROC_THREADS_OFF);
            uint64_t pcb =
                freebsd_debug_u64(td + FREEBSD_THREAD_PCB_OFF);
            uint64_t fp = freebsd_debug_u64(pcb + FREEBSD_PCB_FP_OFF);
            LOG_VMM("FreeBSD pid1 td=0x%lx pcb=0x%lx fp=0x%lx lr=0x%lx sp=0x%lx\n",
                    (unsigned long)td, (unsigned long)pcb,
                    (unsigned long)fp,
                    (unsigned long)freebsd_debug_u64(
                        pcb + FREEBSD_PCB_LR_OFF),
                    (unsigned long)freebsd_debug_u64(
                        pcb + FREEBSD_PCB_SP_OFF));
            for (unsigned frame = 0u;
                 fp != 0u && frame < 24u; frame++) {
                uint64_t next_fp = freebsd_debug_u64(fp);
                uint64_t next_lr = freebsd_debug_u64(fp + 8u);
                LOG_VMM("FreeBSD pid1 frame[%u] fp=0x%lx lr=0x%lx\n",
                        frame, (unsigned long)fp,
                        (unsigned long)next_lr);
                if (next_fp <= fp) {
                    break;
                }
                fp = next_fp;
            }
            return;
        }
        proc = freebsd_debug_u64(proc);
    }
    LOG_VMM_ERR("FreeBSD pid1 not found\n");
}

static seL4_MessageInfo_t freebsd_vmm_rpc(seL4_MessageInfo_t info)
{
    (void)info;
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    _sel4_mrs_to_msg(&req);

    switch (req.opcode) {
    case MSG_GUEST_CREATE: {
        if (req.length >= 4u) {
            uint32_t os_type = vmm_msg_rd32(req.data, 0u);
            if (os_type != 0u && os_type != FREEBSD_VMM_OS_TYPE) {
                rep.opcode = GUEST_ERR_BAD_OS_TYPE;
                break;
            }
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        vmm_msg_wr32(rep.data, 0u, GUEST_OK);
        vmm_msg_wr32(rep.data, 4u, 0u);
        rep.length = 8u;
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_BOOT: {
        if (req.length < 4u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        if (!guest_started && !freebsd_vmm_start_guest()) {
            rep.opcode = GUEST_ERR_NOT_READY;
            break;
        }
        g_guest_state = GUEST_STATE_RUNNING;
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_SUSPEND: {
        if (req.length < 4u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        if (g_guest_state != GUEST_STATE_SUSPENDED) {
            freebsd_vmm_suspend_guest_tcb();
            vmm_vcpu_arm_ack_vppi(GUEST_BOOT_VCPU_ID, FREEBSD_VTIMER_IRQ);
            g_guest_state = GUEST_STATE_SUSPENDED;
        }
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_RESUME: {
        if (req.length < 4u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        if (g_guest_state == GUEST_STATE_SUSPENDED) {
            seL4_TCB_Resume((seL4_CPtr)(AGENTOS_VMM_TCB_CAP_BASE + GUEST_BOOT_VCPU_ID));
        }
        g_guest_state = GUEST_STATE_RUNNING;
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_DESTROY: {
        if (req.length < 4u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state != GUEST_STATE_DEAD) {
            freebsd_vmm_suspend_guest_tcb();
            vmm_vcpu_arm_ack_vppi(GUEST_BOOT_VCPU_ID, FREEBSD_VTIMER_IRQ);
            g_guest_state = GUEST_STATE_DEAD;
        }
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_SEND_INPUT: {
        if (req.length < 28u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        if (g_guest_state != GUEST_STATE_RUNNING) {
            rep.opcode = GUEST_ERR_BAD_STATE;
            break;
        }

        uint8_t byte = 0u;
        uint32_t event_type = vmm_msg_rd32(req.data, 4u);
        uint32_t keycode = vmm_msg_rd32(req.data, 8u);
        if (event_type == CC_INPUT_TEXT) {
            if (keycode > CC_INPUT_TEXT_MAX || req.length < 28u + keycode) {
                rep.opcode = GUEST_ERR_PROTOCOL_VIOLATION;
                break;
            }
            for (uint32_t i = 0u; i < keycode; i++) {
                if (!console_rx_push(req.data[28u + i])) {
                    rep.opcode = GUEST_ERR_DEVICE_UNAVAILABLE;
                    break;
                }
            }
            if (rep.opcode != 0u) break;
            pl011_maybe_inject_irq();
        } else if (input_event_to_byte(event_type, keycode, &byte)) {
            if (byte == 0x1du) {
                freebsd_dump_init_stack();
                rep.opcode = GUEST_OK;
                break;
            }
            if (!console_rx_push(byte)) {
                rep.opcode = GUEST_ERR_DEVICE_UNAVAILABLE;
                break;
            }
            pl011_maybe_inject_irq();
        }
        rep.opcode = GUEST_OK;
        break;
    }
    case MSG_GUEST_CONSOLE_DRAIN: {
        if (req.length < 8u || vmm_msg_rd32(req.data, 0u) != 0u) {
            rep.opcode = GUEST_ERR_BAD_GUEST_ID;
            break;
        }
        if (g_guest_state == GUEST_STATE_DEAD) {
            rep.opcode = GUEST_ERR_DEAD;
            break;
        }
        uint32_t max = vmm_msg_rd32(req.data, 4u);
        if (max > SEL4_MSG_DATA_BYTES) max = SEL4_MSG_DATA_BYTES;
        rep.length = console_tx_drain(rep.data, max);
        rep.opcode = GUEST_OK;
        break;
    }
    default:
        rep.opcode = GUEST_ERR_PROTOCOL_VIOLATION;
        break;
    }

    _sel4_msg_to_mrs(&rep);
    return seL4_MessageInfo_new((seL4_Word)rep.opcode, 0, 0,
                                (seL4_Word)_SEL4_MR_COUNT);
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
        LOG_VMM_ERR("FreeBSD kernel image not linked (run make fetch-guest GUEST_OS=freebsd)\n");
        return;
    }
    LOG_VMM("  FreeBSD kernel: %zu bytes (%.1f MB)\n",
            kernel_size, (double)kernel_size / (1024.0 * 1024.0));

    if (guest_ram_vaddr == 0) {
        guest_ram_vaddr = FREEBSD_GUEST_RAM_VADDR;
    }
    aos_vmm_guest_ram_bind(FREEBSD_GUEST_RAM_VADDR, guest_ram_vaddr,
                           FREEBSD_GUEST_RAM_SIZE);

    uintptr_t kernel_dst = guest_ram_vaddr +
        (FREEBSD_KERNEL_VADDR - FREEBSD_GUEST_RAM_VADDR);
    uintptr_t fdt_dst = guest_ram_vaddr +
        (FREEBSD_FDT_VADDR - FREEBSD_GUEST_RAM_VADDR);
    if ((FREEBSD_KERNEL_VADDR + kernel_size) >= FREEBSD_FDT_VADDR) {
        LOG_VMM_ERR("FreeBSD kernel overlaps FDT load address\n");
        return;
    }

    LOG_VMM("  FreeBSD guest RAM zeroed by seL4 retype\n");

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

    g_freebsd_startable = true;
#if defined(AGENTOS_GUEST_BOTH)
    g_guest_state = GUEST_STATE_READY;
    LOG_VMM("  FreeBSD guest ready; waiting for lifecycle BOOT\n");
#else
    (void)freebsd_vmm_start_guest();
#endif
}

/* ── Notification handler ─────────────────────────────────────────────────── */

static void freebsd_vmm_notified(seL4_Word badge)
{
    /* Emulated devices signal the guest vGIC directly from their VMM fault
     * handlers. This PD intentionally owns no host device IRQ capability. */
    (void)badge;
}

/* ── Fault handler ────────────────────────────────────────────────────────── */

static seL4_MessageInfo_t freebsd_vmm_fault(seL4_Word badge,
                                            seL4_MessageInfo_t msginfo)
{
    seL4_Word fault_mrs[seL4_MsgMaxLength];
    seL4_Word fault_length = seL4_MessageInfo_get_length(msginfo);

    if (fault_length > seL4_MsgMaxLength) {
        fault_length = seL4_MsgMaxLength;
    }
    /* serial_pd diagnostics use this thread's IPC buffer. Keep the guest
     * fault payload intact until libvmm has decoded it. */
    for (seL4_Word i = 0u; i < fault_length; i++) {
        fault_mrs[i] = seL4_GetMR((int)i);
    }

    /* Microkit 2.1 encodes VCPU fault badges as (1ULL<<62)|vcpu_id */
    size_t vcpu_id = badge & ~(1ULL << 62);

    static uint64_t wfi_count = 0;
    static uint64_t other_vcpu_count = 0;
    size_t label = seL4_MessageInfo_get_label(msginfo);
    if (label == seL4_Fault_VCPUFault) {
        uint64_t hsr = fault_mrs[seL4_VCPUFault_HSR];
        uint64_t ec = (hsr >> 26) & 0x3f;
        if (ec == 0x01) { /* HSR_WFx = 0x01 */
            wfi_count++;
            if (wfi_count <= 3u)
                LOG_VMM("WFI fault #%llu\n", (unsigned long long)wfi_count);
            seL4_Word timer_ctl =
                vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL);
            if (!vgic_irq_is_pending(vcpu_id, FREEBSD_VTIMER_IRQ) &&
                !vgic_irq_is_inflight(vcpu_id, FREEBSD_VTIMER_IRQ)) {
                if ((timer_ctl & 0x5u) == 0x5u) {
                    /*
                     * The deadline expired while seL4's physical VPPI was
                     * masked. Reflect the asserted architectural timer level
                     * into the vGIC now that no delivery is outstanding.
                     */
                    (void)virq_inject_vcpu(vcpu_id, FREEBSD_VTIMER_IRQ);
                } else if ((timer_ctl & 0x5u) == 0x1u) {
                    /*
                     * FreeBSD advanced CVAL after EOI. Release a physical
                     * VPPI whose acknowledgement was deferred while the timer
                     * level remained asserted.
                     */
                    vmm_vcpu_arm_ack_vppi(vcpu_id, FREEBSD_VTIMER_IRQ);
                }
            }
#if FREEBSD_IRQ_TRACE
            freebsd_log_irq_state("WFI irq state", (unsigned)wfi_count);
#endif
        } else {
            other_vcpu_count++;
            if (other_vcpu_count <= 5)
                LOG_VMM("VCPUFault ec=0x%lx hsr=0x%lx\n",
                        (unsigned long)ec,
                        (unsigned long)hsr);
        }
    } else if (label == seL4_Fault_VMFault) {
        static uint64_t vmfault_count = 0;
        vmfault_count++;
        uint64_t fault_addr = fault_mrs[seL4_VMFault_IP];
        uint64_t fault_data_addr = fault_mrs[seL4_VMFault_Addr];
        uint64_t fault_fsr = fault_mrs[seL4_VMFault_FSR];
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
    for (seL4_Word i = 0u; i < fault_length; i++) {
        seL4_SetMR((int)i, fault_mrs[i]);
    }
    bool success = fault_handle(vcpu_id, msginfo);
    if (!success)
        LOG_VMM_ERR("Unhandled fault: badge=0x%lx label=0x%lx\n",
                    (unsigned long)badge, (unsigned long)label);
    aos_vmm_virtio_net_after_fault();
    aos_vmm_virtio_blk_after_fault();
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
        if (label == MSG_GUEST_CREATE ||
            label == MSG_GUEST_BOOT ||
            label == MSG_GUEST_SEND_INPUT ||
            label == MSG_GUEST_CONSOLE_DRAIN ||
            label == MSG_GUEST_SUSPEND ||
            label == MSG_GUEST_RESUME ||
            label == MSG_GUEST_DESTROY) {
            seL4_MessageInfo_t reply = freebsd_vmm_rpc(info);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(reply_cap, reply);
            info = seL4_Recv(ep, &badge, reply_cap);
#else
            seL4_Reply(reply);
            info = seL4_Recv(ep, &badge);
#endif
        } else if (label == NET_SVC_EVENT_RX_READY) {
            aos_vmm_virtio_net_rx_ready();
#ifdef CONFIG_KERNEL_MCS
            info = seL4_Recv(ep, &badge, reply_cap);
#else
            info = seL4_Recv(ep, &badge);
#endif
        } else if (label == seL4_Fault_NullFault) {
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
    while (1) {}
    __builtin_unreachable();
}

#endif /* ARCH_AARCH64 */
