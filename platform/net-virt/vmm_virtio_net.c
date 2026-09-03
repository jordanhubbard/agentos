/*
 * Guest-facing virtio-net: libvmm device at AOS_VIRTIO_NET_GUEST_IPA,
 * backend = sDDF queues + local aos_net_virt_pump (loopback until nic_drv).
 *
 * QEMU virtio-mmio at 0x0A000000 remains a kill-dated passthrough crutch.
 */

#include <libvmm/libvmm.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/net.h>
#include <libvmm/virtio/gpa.h>
#include <sddf/network/queue.h>
#include <platform/net_layout.h>
#include <platform/net_virt_pump.h>
#include <platform/vmm_virtio_net.h>
#include <platform/guest_ram.h>

_Static_assert(AOS_NET_BUFFER_SIZE == NET_BUFFER_SIZE,
               "platform net buffer size must match sDDF NET_BUFFER_SIZE");
_Static_assert(sizeof(aos_net_buff_desc_t) == sizeof(net_buff_desc_t),
               "aos_net_buff_desc_t must match sDDF net_buff_desc_t");

static struct virtio_net_device g_aos_net;
static aos_net_virt_t           g_aos_virt;
static net_queue_handle_t       g_rx;
static net_queue_handle_t       g_tx;
static int                      g_aos_net_ready;
static int                      g_aos_net_probed;
static int                      g_aos_net_driver_ok;

void aos_vmm_guest_ram_bind(uint64_t gpa_base, uintptr_t hva_base, size_t size)
{
    aos_guest_ram_configure(gpa_base, hva_base, size);
    virtio_gpa_set_translate(aos_gpa_to_hva_configured);
}

void aos_vmm_virtio_net_init(void)
{
    uint8_t *region = (uint8_t *)AOS_NET_SHMEM_VA;
    aos_net_virt_client_t client;
    uint8_t mac[VIRTIO_NET_CONFIG_MAC_SZ];

    aos_net_virt_reset(&g_aos_virt);
    aos_net_client_bind(region, 0u, &client);
    aos_net_client_init_buffers(&client);
    if (aos_net_virt_add_client(&g_aos_virt, &client) != 0) {
        LOG_VMM_ERR("emulated virtio-net: add client failed\n");
        return;
    }

    net_queue_init(&g_rx, (net_queue_t *)client.rx_free,
                   (net_queue_t *)client.rx_active, AOS_NET_CAPACITY);
    net_queue_init(&g_tx, (net_queue_t *)client.tx_free,
                   (net_queue_t *)client.tx_active, AOS_NET_CAPACITY);

    /*
     * libvmm calls vmm_notify(tx_cap) when the guest TX virtq is kicked.
     * tx_cap is 0 (no net_virt PD yet). Keep consumer_signalled set so
     * net_require_signal_active is false and seL4_Signal(0) is skipped.
     * We pump after every VM MMIO fault instead.
     */
    client.tx_active->consumer_signalled = 1u;

    mac[0] = AOS_VIRTIO_NET_MAC0;
    mac[1] = AOS_VIRTIO_NET_MAC1;
    mac[2] = AOS_VIRTIO_NET_MAC2;
    mac[3] = AOS_VIRTIO_NET_MAC3;
    mac[4] = AOS_VIRTIO_NET_MAC4;
    mac[5] = AOS_VIRTIO_NET_MAC5;

    if (!virtio_mmio_net_init(&g_aos_net,
                              AOS_VIRTIO_NET_GUEST_IPA,
                              AOS_VIRTIO_NET_MMIO_SIZE,
                              AOS_VIRTIO_NET_VIRQ,
                              &g_rx, &g_tx,
                              (uintptr_t)client.rx_data,
                              (uintptr_t)client.tx_data,
                              0, 0, mac)) {
        LOG_VMM_ERR("emulated virtio-net: virtio_mmio_net_init failed\n");
        return;
    }

    g_aos_net_ready = 1;
    LOG_VMM("emulated virtio-net IPA 0x%lx IRQ %u (sDDF pump, not QEMU)\n",
            (unsigned long)AOS_VIRTIO_NET_GUEST_IPA,
            (unsigned)AOS_VIRTIO_NET_VIRQ);
}

void aos_vmm_virtio_net_after_fault(void)
{
    uint32_t status;

    if (!g_aos_net_ready) {
        return;
    }

    status = g_aos_net.virtio_device.regs.Status;
    if (!g_aos_net_probed && (status & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
        g_aos_net_probed = 1;
        LOG_VMM("emulated virtio-net: guest probed IPA 0x%lx (status=0x%x)\n",
                (unsigned long)AOS_VIRTIO_NET_GUEST_IPA, (unsigned)status);
    }
    if (!g_aos_net_driver_ok && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        g_aos_net_driver_ok = 1;
        LOG_VMM("emulated virtio-net: guest DRIVER_OK virq %u\n",
                (unsigned)AOS_VIRTIO_NET_VIRQ);
    }

    (void)aos_net_virt_pump(&g_aos_virt);
    (void)virtio_net_handle_rx(&g_aos_net);
    if (g_tx.active) {
        g_tx.active->consumer_signalled = 1u;
    }
}
