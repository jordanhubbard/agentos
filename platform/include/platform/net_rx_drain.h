/*
 * Bounded host-to-guest network drain policy.
 *
 * A host virtio-net queue owns 32 descriptors.  An RX notification must be
 * able to drain more than one descriptor cycle because notifications may be
 * coalesced while the VMM is runnable.  Flush between batches so the guest
 * virtqueue can recycle the sDDF RX buffers before the next RAW_RECV call.
 */
#ifndef AOS_PLATFORM_NET_RX_DRAIN_H
#define AOS_PLATFORM_NET_RX_DRAIN_H

#include <stdint.h>

#define AOS_NET_RX_DRAIN_BATCHES 4u

typedef uint32_t (*aos_net_rx_receive_batch_fn)(void *ctx, uint32_t limit);
typedef void (*aos_net_rx_flush_batch_fn)(void *ctx);

static inline uint32_t
aos_net_rx_drain(aos_net_rx_receive_batch_fn receive_batch,
                 aos_net_rx_flush_batch_fn flush_batch,
                 void *ctx, uint32_t batch_size)
{
    uint32_t total = 0u;

    if (receive_batch == 0 || flush_batch == 0 || batch_size == 0u) {
        return 0u;
    }
    for (uint32_t batch = 0u; batch < AOS_NET_RX_DRAIN_BATCHES; batch++) {
        uint32_t received;

        /* Recycle buffers left active by an earlier backpressured attempt. */
        flush_batch(ctx);
        received = receive_batch(ctx, batch_size);
        total += received;
        flush_batch(ctx);
        if (received < batch_size) {
            break;
        }
    }
    return total;
}

#endif /* AOS_PLATFORM_NET_RX_DRAIN_H */
