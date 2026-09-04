/*
 * Guest-facing virtio-net: libvmm device at AOS_VIRTIO_NET_GUEST_IPA,
 * backend = sDDF guest queues bridged through the formal net_pd contract.
 * net_pd alone owns the page-isolated QEMU bus.16 transport and its DMA.
 */

#if defined(AGENTOS_GUEST_UBUNTU) || defined(AGENTOS_GUEST_FREEBSD)
#include <contracts/net-service/interface.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#endif
#include <libvmm/libvmm.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/net.h>
#include <libvmm/virtio/gpa.h>
#include <sddf/network/queue.h>
#include <platform/net_layout.h>
#include <platform/net_host_layout.h>
#include <platform/net_virt_pump.h>
#include <platform/vmm_virtio_net.h>
#include <platform/guest_ram.h>

_Static_assert(AOS_NET_BUFFER_SIZE == NET_BUFFER_SIZE,
               "platform net buffer size must match sDDF NET_BUFFER_SIZE");
_Static_assert(sizeof(aos_net_buff_desc_t) == sizeof(net_buff_desc_t),
               "aos_net_buff_desc_t must match sDDF net_buff_desc_t");
_Static_assert(AOS_NET_GUEST_CLIENTS * AOS_NET_CLIENT_STRIDE <=
               AOS_NET_SHMEM_SIZE / 2u,
               "guest net queues must not overlap net-service slots");

static struct virtio_net_device g_aos_net;
static aos_net_virt_t           g_aos_virt;
static net_queue_handle_t       g_rx;
static net_queue_handle_t       g_tx;
static int                      g_aos_net_ready;
static int                      g_aos_net_probed;
static int                      g_aos_net_driver_ok;
static int                      g_aos_net_pumped;
#if defined(AGENTOS_GUEST_UBUNTU) || defined(AGENTOS_GUEST_FREEBSD)
static uint32_t                 g_net_pd_handle;
static uint32_t                 g_net_pd_slot;
static int                      g_net_pd_ready;
static int                      g_net_pd_tx_marked;
static int                      g_net_pd_rx_marked;

static uint32_t net_rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] |
           ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static void net_wr32(uint8_t *p, uint32_t off, uint32_t value)
{
    p[off] = (uint8_t)value;
    p[off + 1u] = (uint8_t)(value >> 8);
    p[off + 2u] = (uint8_t)(value >> 16);
    p[off + 3u] = (uint8_t)(value >> 24);
}

static int net_pd_call(uint32_t opcode, uint32_t arg0, uint32_t arg1,
                       sel4_msg_t *rep)
{
    sel4_msg_t req = {0};
    req.opcode = opcode;
    req.length = 8u;
    net_wr32(req.data, 0u, arg0);
    net_wr32(req.data, 4u, arg1);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_NET_PD_EP, &req, rep);
    return rep->opcode == SEL4_ERR_OK &&
           net_rd32(rep->data, 0u) == NET_SVC_RAW_OK;
}

static void net_pd_bridge_init(uint32_t client_id)
{
    sel4_msg_t rep = {0};

    if (!net_pd_call(NET_SVC_OP_RAW_OPEN, client_id, 0u, &rep) ||
        rep.length < 18u) {
        LOG_VMM_ERR("emulated virtio-net: net_pd OPEN failed rc=%u\n",
                    (unsigned)rep.opcode);
        return;
    }
    g_net_pd_handle = net_rd32(rep.data, 4u);
    g_net_pd_slot = net_rd32(rep.data, 8u);
    if (g_net_pd_slot < NET_SVC_SLOT_BASE ||
        g_net_pd_slot + NET_SVC_SLOT_SIZE > AGENTOS_NET_SHARED_SIZE) {
        LOG_VMM_ERR("emulated virtio-net: invalid net_pd shmem slot 0x%x\n",
                    (unsigned)g_net_pd_slot);
        return;
    }
    g_net_pd_ready = 1;
    LOG_VMM("emulated virtio-net: backend net_pd contract v%u handle=%u\n",
            (unsigned)NET_SVC_INTERFACE_VERSION,
            (unsigned)g_net_pd_handle);
    rep = (sel4_msg_t){0};
    if (net_pd_call(NET_SVC_OP_RAW_STATUS, g_net_pd_handle, 0u, &rep) &&
        net_rd32(rep.data, 4u) != 0u) {
        LOG_VMM("[net_pd] HOST_READY: virtio-net bus.16\n");
    } else {
        LOG_VMM_ERR("emulated virtio-net: net_pd host transport unavailable\n");
    }
}

static uint32_t net_pd_bridge_tx(void)
{
    uint32_t sent = 0u;
    net_buff_desc_t buffer;

    while (net_dequeue_active(&g_tx, &buffer) == 0) {
        uint32_t len = buffer.len;
        uint8_t *src = (uint8_t *)AOS_NET_SHMEM_VA +
                       AOS_NET_TX_DATA_OFF +
                       (uint32_t)buffer.io_or_offset;
        uint8_t *dst = (uint8_t *)AGENTOS_NET_SHARED_VA + g_net_pd_slot +
                       NET_SVC_HDR_SIZE;
        if (len > NET_SVC_MAX_FRAME_BYTES) {
            len = NET_SVC_MAX_FRAME_BYTES;
        }
        for (uint32_t i = 0u; i < len; i++) {
            dst[i] = src[i];
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        sel4_msg_t rep = {0};
        if (net_pd_call(NET_SVC_OP_RAW_SEND, g_net_pd_handle, len, &rep)) {
            sent++;
            if (!g_net_pd_tx_marked) {
                g_net_pd_tx_marked = 1;
                LOG_VMM("emulated virtio-net: backend TX accepted by net_pd\n");
                LOG_VMM("[net_pd] HOST_TX: QEMU bus.16 completion observed\n");
            }
        } else {
            LOG_VMM_ERR("emulated virtio-net: backend TX failed rc=%u\n",
                        (unsigned)rep.opcode);
        }
        buffer.len = 0u;
        (void)net_enqueue_free(&g_tx, buffer);
    }
    return sent;
}

static uint32_t net_pd_bridge_rx(void)
{
    uint32_t received = 0u;

    for (uint32_t attempt = 0u; attempt < 8u; attempt++) {
        sel4_msg_t rep = {0};
        if (!net_pd_call(NET_SVC_OP_RAW_RECV, g_net_pd_handle,
                         NET_SVC_MAX_FRAME_BYTES, &rep) ||
            rep.length < 12u) {
            break;
        }
        uint32_t len = net_rd32(rep.data, 4u);
        uint32_t off = net_rd32(rep.data, 8u);
        if (len == 0u) {
            break;
        }
        if (len > NET_SVC_MAX_FRAME_BYTES ||
            off + len > AGENTOS_NET_SHARED_SIZE) {
            LOG_VMM_ERR("emulated virtio-net: backend RX bounds invalid\n");
            break;
        }
        net_buff_desc_t buffer;
        if (net_dequeue_free(&g_rx, &buffer) != 0) {
            break;
        }
        uint8_t *src = (uint8_t *)AGENTOS_NET_SHARED_VA + off;
        uint8_t *dst = (uint8_t *)AOS_NET_SHMEM_VA +
                       AOS_NET_RX_DATA_OFF +
                       (uint32_t)buffer.io_or_offset;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (uint32_t i = 0u; i < len; i++) {
            dst[i] = src[i];
        }
        buffer.len = (uint16_t)len;
        if (net_enqueue_active(&g_rx, buffer) != 0) {
            buffer.len = 0u;
            (void)net_enqueue_free(&g_rx, buffer);
            break;
        }
        received++;
        if (!g_net_pd_rx_marked) {
            g_net_pd_rx_marked = 1;
            LOG_VMM("emulated virtio-net: backend RX delivered from net_pd\n");
        }
    }
    return received;
}
#endif

void aos_vmm_virtio_net_rx_ready(void)
{
#if defined(AGENTOS_GUEST_UBUNTU) || defined(AGENTOS_GUEST_FREEBSD)
    if (!g_aos_net_ready || !g_net_pd_ready) {
        return;
    }
    uint32_t received = net_pd_bridge_rx();
    if (received > 0u) {
        LOG_VMM("[net_pd] HOST_RX: QEMU bus.16 frame received\n");
        LOG_VMM("emulated virtio-net: asynchronous net_pd RX event (%u frame(s))\n",
                (unsigned)received);
        (void)virtio_net_handle_rx(&g_aos_net);
    }
#endif
}

void aos_vmm_virtio_net_init(uint32_t client_id)
{
    uint8_t *region = (uint8_t *)AOS_NET_SHMEM_VA;
    aos_net_virt_client_t client;
    uint8_t mac[VIRTIO_NET_CONFIG_MAC_SZ];

    if (client_id >= AOS_NET_GUEST_CLIENTS) {
        LOG_VMM_ERR("emulated virtio-net: invalid client %u\n",
                    (unsigned)client_id);
        return;
    }
    aos_net_virt_reset(&g_aos_virt);
    aos_net_client_bind(region, client_id, &client);
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

#if defined(AGENTOS_GUEST_UBUNTU) || defined(AGENTOS_GUEST_FREEBSD)
    net_pd_bridge_init(client_id);
#endif

    mac[0] = AOS_VIRTIO_NET_MAC0;
    mac[1] = AOS_VIRTIO_NET_MAC1;
    mac[2] = AOS_VIRTIO_NET_MAC2;
    mac[3] = AOS_VIRTIO_NET_MAC3;
    mac[4] = AOS_VIRTIO_NET_MAC4;
    mac[5] = (uint8_t)(AOS_VIRTIO_NET_MAC5 + client_id);

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
    uint32_t n;

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
        LOG_VMM("emulated virtio-net: guest DRIVER_OK virq %u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned)AOS_VIRTIO_NET_VIRQ,
                (unsigned)AOS_VIRTIO_NET_MAC0,
                (unsigned)AOS_VIRTIO_NET_MAC1,
                (unsigned)AOS_VIRTIO_NET_MAC2,
                (unsigned)AOS_VIRTIO_NET_MAC3,
                (unsigned)AOS_VIRTIO_NET_MAC4,
                (unsigned)g_aos_net.config.mac[5]);
    }

#if defined(AGENTOS_GUEST_UBUNTU) || defined(AGENTOS_GUEST_FREEBSD)
    n = 0u;
    if (g_net_pd_ready) {
        n += net_pd_bridge_tx();
        n += net_pd_bridge_rx();
    }
    if (!g_aos_net_pumped && n > 0u) {
        g_aos_net_pumped = 1;
        LOG_VMM("emulated virtio-net: pumped %u frame(s) via host-backed net_pd\n",
                n);
    }
#else
    n = aos_net_virt_pump(&g_aos_virt);
    if (!g_aos_net_pumped && n > 0u) {
        g_aos_net_pumped = 1;
        LOG_VMM("emulated virtio-net: pumped %u frame(s) TX->RX\n", n);
    }
#endif
    (void)virtio_net_handle_rx(&g_aos_net);
    if (g_tx.active) {
        g_tx.active->consumer_signalled = 1u;
    }
}
