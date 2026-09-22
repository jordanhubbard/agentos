#ifndef AOS_PLATFORM_VIRTIO_NET_RX_ACCOUNTING_H
#define AOS_PLATFORM_VIRTIO_NET_RX_ACCOUNTING_H
#include <libvmm/virtio/net.h>

/* Count descriptor chains actually published to the guest, not packets
 * waiting in the backend queue. last_idx is VMM-owned and wraps at 16 bits. */
static inline uint16_t aos_virtio_net_rx_completed(struct virtio_net_device *dev)
{
    uint16_t before = dev->vqs[VIRTIO_NET_RX_VIRTQ].last_idx;
    (void)virtio_net_handle_rx(dev);
    return (uint16_t)(dev->vqs[VIRTIO_NET_RX_VIRTQ].last_idx - before);
}
#endif
