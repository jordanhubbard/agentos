/*
 * Guest-facing virtio-console backed by sDDF serial byte queues.
 *
 * The physical UART remains owned by serial_pd. CC-PD and future native
 * serial_virt clients drain/fill these queues through the VMM contract.
 */

#include <libvmm/libvmm.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/console.h>
#include <sddf/serial/queue.h>
#include <platform/serial_layout.h>
#include <platform/vmm_virtio_console.h>

static struct virtio_console_device g_aos_console;
static serial_queue_t               g_rx_queue;
static serial_queue_t               g_tx_queue;
static serial_queue_handle_t        g_rx;
static serial_queue_handle_t        g_tx;
static char                         g_rx_data[AOS_SERIAL_RX_CAPACITY];
static char                         g_tx_data[AOS_SERIAL_TX_CAPACITY];
static int                          g_ready;
static int                          g_probed;
static int                          g_driver_ok;
static int                          g_tx_pumped;
static int                          g_rx_pumped;

static void zero_bytes(void *ptr, uint32_t size)
{
    uint8_t *p = (uint8_t *)ptr;
    while (size-- > 0u) {
        *p++ = 0u;
    }
}

void aos_vmm_virtio_console_init(void)
{
    zero_bytes(&g_aos_console, (uint32_t)sizeof(g_aos_console));
    zero_bytes(&g_rx_queue, (uint32_t)sizeof(g_rx_queue));
    zero_bytes(&g_tx_queue, (uint32_t)sizeof(g_tx_queue));
    zero_bytes(g_rx_data, (uint32_t)sizeof(g_rx_data));
    zero_bytes(g_tx_data, (uint32_t)sizeof(g_tx_data));

    serial_queue_init(&g_rx, &g_rx_queue, AOS_SERIAL_RX_CAPACITY, g_rx_data);
    serial_queue_init(&g_tx, &g_tx_queue, AOS_SERIAL_TX_CAPACITY, g_tx_data);

    /*
     * There is no separate serial_virt PD notification in this vertical
     * slice. QueueNotify is the VM fault that drives TX, and CC-PD drains it.
     */
    g_tx_queue.producer_signalled = 1u;
    g_rx_queue.producer_signalled = 1u;

    if (!virtio_mmio_console_init(&g_aos_console,
                                  AOS_VIRTIO_CONSOLE_GUEST_IPA,
                                  AOS_VIRTIO_CONSOLE_MMIO_SIZE,
                                  AOS_VIRTIO_CONSOLE_VIRQ,
                                  &g_rx, &g_tx, 0)) {
        LOG_VMM_ERR("emulated virtio-console: init failed\n");
        return;
    }

    g_ready = 1;
    LOG_VMM("emulated virtio-console IPA 0x%lx IRQ %u (sDDF serial queues)\n",
            (unsigned long)AOS_VIRTIO_CONSOLE_GUEST_IPA,
            (unsigned)AOS_VIRTIO_CONSOLE_VIRQ);
}

void aos_vmm_virtio_console_after_fault(void)
{
    uint32_t status;

    if (!g_ready) {
        return;
    }

    status = g_aos_console.virtio_device.regs.Status;
    if (!g_probed && (status & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
        g_probed = 1;
        LOG_VMM("emulated virtio-console: guest probed IPA 0x%lx (status=0x%x)\n",
                (unsigned long)AOS_VIRTIO_CONSOLE_GUEST_IPA,
                (unsigned)status);
    }
    if (!g_driver_ok && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        g_driver_ok = 1;
        LOG_VMM("emulated virtio-console: guest DRIVER_OK virq %u\n",
                (unsigned)AOS_VIRTIO_CONSOLE_VIRQ);
    }

    (void)virtio_console_handle_rx(&g_aos_console);
}

uint32_t aos_vmm_virtio_console_drain_tx(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0u;
    char byte;

    if (!g_ready || dst == NULL) {
        return 0u;
    }
    while (n < max && serial_dequeue(&g_tx, &byte) == 0) {
        dst[n++] = (uint8_t)byte;
    }
    if (!g_tx_pumped && n > 0u) {
        g_tx_pumped = 1;
        LOG_VMM("emulated virtio-console: pumped %u byte(s) guest->serial_virt\n",
                (unsigned)n);
    }
    return n;
}

bool aos_vmm_virtio_console_push_rx(uint8_t byte)
{
    if (!g_ready || !g_driver_ok || serial_enqueue(&g_rx, (char)byte) != 0) {
        return false;
    }
    if (!virtio_console_handle_rx(&g_aos_console)) {
        return false;
    }
    if (!g_rx_pumped) {
        g_rx_pumped = 1;
        LOG_VMM("emulated virtio-console: pumped input serial_virt->guest\n");
    }
    return true;
}
