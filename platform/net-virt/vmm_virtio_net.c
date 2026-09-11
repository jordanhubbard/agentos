/*
 * Guest-facing virtio-net: libvmm device at AOS_VIRTIO_NET_GUEST_IPA,
 * backend = sDDF guest queues in the shared net frame, serviced by the
 * net_virt PD (platform/net-virt/net_virt.c).  The VMM never moves a frame
 * over IPC: it enqueues/dequeues the shared queues and exchanges
 * notifications with net_virt (contracts/net_virt_contract.h).  net_pd alone
 * owns the page-isolated QEMU bus.16 transport and its DMA.
 */

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
#include <contracts/net_virt_contract.h>
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
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
static int                      g_net_virt_attached;
static uint32_t                 g_net_virt_hw;
static int                      g_tx_kicked;
static uint32_t                 g_rx_events;

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

static void net_virt_attach(uint32_t client_id)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    uint32_t status;

    req.opcode = NET_VIRT_OP_ATTACH;
    req.length = (uint32_t)sizeof(net_virt_attach_req_t);
    net_wr32(req.data, 0u, NET_VIRT_CONTRACT_VERSION);
    net_wr32(req.data, 4u, client_id);
#if defined(AGENTOS_GUEST_SECONDARY)
    net_wr32(req.data, 8u, NET_VIRT_VMM_SLOT_SECONDARY);
#else
    net_wr32(req.data, 8u, NET_VIRT_VMM_SLOT_PRIMARY);
#endif
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_NET_VIRT_EP, &req, &rep);
    status = net_rd32(rep.data, 0u);
    if (rep.opcode != SEL4_ERR_OK || status != NET_VIRT_OK ||
        rep.length < sizeof(net_virt_attach_reply_t)) {
        LOG_VMM_ERR("emulated virtio-net: net_virt ATTACH failed rc=%u status=%u\n",
                    (unsigned)rep.opcode, (unsigned)status);
        return;
    }
    g_net_virt_hw = net_rd32(rep.data, 8u);
    g_net_virt_attached = 1;
    LOG_VMM("emulated virtio-net: attached to net_virt contract v%u client %u hw=%u\n",
            (unsigned)net_rd32(rep.data, 4u), (unsigned)client_id,
            (unsigned)g_net_virt_hw);
}

static void net_virt_kick(void)
{
    seL4_NBSend((seL4_CPtr)PD_CNODE_SLOT_NET_VIRT_EP,
                seL4_MessageInfo_new(NET_VIRT_EVENT_KICK, 0u, 0u, 0u));
}

static void net_mark_pumped(uint32_t n, const char *how)
{
    if (!g_aos_net_pumped && n > 0u) {
        g_aos_net_pumped = 1;
        LOG_VMM("emulated virtio-net: pumped %u frame(s) via net_virt (%s)\n",
                (unsigned)n, how);
    }
}

/*
 * Service the shared queues against net_virt.  Called after every guest
 * MMIO exit and on every RX_READY event:
 *   - push frames net_virt put on rx_active into the guest RX virtq;
 *   - kick net_virt if the guest queued TX and net_virt asked for kicks
 *     (tx_active.consumer_signalled == 0), or if we just recycled RX buffers
 *     while net_virt is backpressured (rx_free.consumer_signalled == 0).
 * NBSend kicks are lossy; the flag stays 0 until net_virt drains, so a lost
 * kick is repeated on the next exit.
 */
static void net_virt_service(void)
{
    uint32_t rx_n;
    int kick = 0;

    if (!g_net_virt_attached) {
        return;
    }

    rx_n = net_queue_length(g_rx.active);
    if (rx_n > 0u) {
        net_mark_pumped(rx_n, "RX delivered to guest");
        g_rx_events += rx_n;
        if (g_rx_events <= rx_n ||
            (g_rx_events & (g_rx_events - 1u)) == 0u) {
            LOG_VMM("emulated virtio-net: RX %u frame(s) from net_virt total=%u\n",
                    (unsigned)rx_n, (unsigned)g_rx_events);
        }
    }
    (void)virtio_net_handle_rx(&g_aos_net);

    if (!net_queue_empty_active(&g_tx)) {
        if (net_require_signal_active(&g_tx)) {
            kick = 1;
            g_tx_kicked = 1;
        }
    } else if (g_tx_kicked) {
        g_tx_kicked = 0;
        net_mark_pumped(1u, "TX consumed");
    }
    if (rx_n > 0u && net_require_signal_free(&g_rx) &&
        !net_queue_empty_free(&g_rx)) {
        kick = 1;
    }
    if (kick) {
        net_virt_kick();
    }
}
#endif

void aos_vmm_virtio_net_rx_ready(void)
{
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    if (!g_aos_net_ready) {
        return;
    }
    net_virt_service();
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

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    /*
     * Buffers are initialised (above) before net_virt binds the queues.
     * tx_cap stays 0: libvmm does not signal on TX; the kick is issued from
     * net_virt_service() under the contract's consumer_signalled rules.
     */
    net_virt_attach(client_id);
#else
    /* No virtualizer in this build: keep the in-process pump silent. */
    client.tx_active->consumer_signalled = 1u;
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
    LOG_VMM("emulated virtio-net IPA 0x%lx IRQ %u (sDDF queues to net_virt, not QEMU)\n",
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
        LOG_VMM("emulated virtio-net: guest DRIVER_OK virq %u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned)AOS_VIRTIO_NET_VIRQ,
                (unsigned)AOS_VIRTIO_NET_MAC0,
                (unsigned)AOS_VIRTIO_NET_MAC1,
                (unsigned)AOS_VIRTIO_NET_MAC2,
                (unsigned)AOS_VIRTIO_NET_MAC3,
                (unsigned)AOS_VIRTIO_NET_MAC4,
                (unsigned)g_aos_net.config.mac[5]);
    }

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    net_virt_service();
#else
    {
        uint32_t n = aos_net_virt_pump(&g_aos_virt);
        if (!g_aos_net_pumped && n > 0u) {
            g_aos_net_pumped = 1;
            LOG_VMM("emulated virtio-net: pumped %u frame(s) TX->RX\n", n);
        }
        (void)virtio_net_handle_rx(&g_aos_net);
        if (g_tx.active) {
            g_tx.active->consumer_signalled = 1u;
        }
    }
#endif
}
