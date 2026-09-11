#ifndef AOS_PLATFORM_NET_VIRT_PUMP_H
#define AOS_PLATFORM_NET_VIRT_PUMP_H

#include <platform/net_layout.h>

/*
 * Raw ring ops on the sDDF-compatible queues.  Safe across PDs: fenced, and
 * the head/tail indices are accessed as volatile.  Used by the VMM-side
 * emulated device (through libvmm), by net_virt, and by the host tests.
 */
uint16_t aos_net_queue_length(const aos_net_queue_t *q);
int aos_net_queue_dequeue(aos_net_queue_t *q, uint32_t capacity,
                          aos_net_buff_desc_t *out);
int aos_net_queue_enqueue(aos_net_queue_t *q, uint32_t capacity,
                          aos_net_buff_desc_t buf);

void aos_net_virt_reset(aos_net_virt_t *v);

/*
 * Client-side helpers, header-only so a queue client (the VMM's emulated
 * virtio-net) links no pump code: the pump lives in the net_virt PD.
 */

/* Bind client_index's stride inside region. Does not touch buffers. */
static inline void aos_net_client_bind(uint8_t *region, uint32_t client_index,
                                       aos_net_virt_client_t *out)
{
    uint8_t *base;

    if (!region || !out || client_index >= AOS_NET_MAX_CLIENTS) {
        return;
    }
    base = region + (client_index * AOS_NET_CLIENT_STRIDE);
    out->rx_free   = (aos_net_queue_t *)(base + AOS_NET_RX_FREE_OFF);
    out->rx_active = (aos_net_queue_t *)(base + AOS_NET_RX_ACTIVE_OFF);
    out->tx_free   = (aos_net_queue_t *)(base + AOS_NET_TX_FREE_OFF);
    out->tx_active = (aos_net_queue_t *)(base + AOS_NET_TX_ACTIVE_OFF);
    out->rx_data   = base + AOS_NET_RX_DATA_OFF;
    out->tx_data   = base + AOS_NET_TX_DATA_OFF;
    out->capacity  = AOS_NET_CAPACITY;
}

/* Zero queue pages, fill rx.free and tx.free with buffer offsets.  Runs
 * before the virtualizer binds the client, so plain stores suffice; one
 * fence publishes the result. */
static inline void aos_net_client_init_buffers(aos_net_virt_client_t *c)
{
    uint32_t i;

    if (!c || !c->rx_free || !c->tx_free) {
        return;
    }
    for (i = 0; i < AOS_NET_QUEUE_BYTES; i++) {
        ((volatile uint8_t *)c->rx_free)[i] = 0u;
        ((volatile uint8_t *)c->rx_active)[i] = 0u;
        ((volatile uint8_t *)c->tx_free)[i] = 0u;
        ((volatile uint8_t *)c->tx_active)[i] = 0u;
    }
    for (i = 0; i < c->capacity; i++) {
        aos_net_buff_desc_t buf = {0};

        buf.io_or_offset = (uint64_t)i * (uint64_t)AOS_NET_BUFFER_SIZE;
        c->rx_free->buffers[i] = buf;
        c->tx_free->buffers[i] = buf;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    c->rx_free->tail = (uint16_t)c->capacity;
    c->tx_free->tail = (uint16_t)c->capacity;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

int aos_net_virt_add_client(aos_net_virt_t *v, const aos_net_virt_client_t *c);

/*
 * Move every pending TX active buffer to a destination RX:
 *   1 client  → loopback to self
 *   2+ clients → copy to every other client (hub)
 * Returns packets successfully forwarded.
 */
uint32_t aos_net_virt_pump(aos_net_virt_t *v);

#endif /* AOS_PLATFORM_NET_VIRT_PUMP_H */
