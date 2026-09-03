/*
 * net_virt pump — sDDF-shaped hub with no seL4 dependency.
 *
 * One client: loopback. Several clients: copy each TX to every other RX.
 * nic_drv / a real net_virt PD replace this local pump later; the queue ABI
 * stays.
 */

#include <platform/net_virt_pump.h>

static void aos_bzero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0u) {
        *d++ = 0;
    }
}

static void aos_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

static uint16_t q_len(const aos_net_queue_t *q)
{
    return (uint16_t)(q->tail - q->head);
}

static int q_empty(const aos_net_queue_t *q)
{
    return q_len(q) == 0;
}

static int q_full(const aos_net_queue_t *q, uint32_t capacity)
{
    return q_len(q) == (uint16_t)capacity;
}

static int dequeue(aos_net_queue_t *q, uint32_t capacity, aos_net_buff_desc_t *out)
{
    if (q_empty(q)) {
        return -1;
    }
    *out = q->buffers[q->head % capacity];
    q->head++;
    return 0;
}

static int enqueue(aos_net_queue_t *q, uint32_t capacity, aos_net_buff_desc_t buf)
{
    if (q_full(q, capacity)) {
        return -1;
    }
    q->buffers[q->tail % capacity] = buf;
    q->tail++;
    return 0;
}

void aos_net_virt_reset(aos_net_virt_t *v)
{
    if (!v) {
        return;
    }
    aos_bzero(v, (uint32_t)sizeof(*v));
}

void aos_net_client_bind(uint8_t *region, uint32_t client_index,
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

void aos_net_client_init_buffers(aos_net_virt_client_t *c)
{
    uint32_t i;
    aos_net_buff_desc_t buf;

    if (!c || !c->rx_free || !c->tx_free) {
        return;
    }

    aos_bzero(c->rx_free, AOS_NET_QUEUE_BYTES);
    aos_bzero(c->rx_active, AOS_NET_QUEUE_BYTES);
    aos_bzero(c->tx_free, AOS_NET_QUEUE_BYTES);
    aos_bzero(c->tx_active, AOS_NET_QUEUE_BYTES);

    aos_bzero(&buf, (uint32_t)sizeof(buf));
    for (i = 0; i < c->capacity; i++) {
        buf.io_or_offset = (uint64_t)i * (uint64_t)AOS_NET_BUFFER_SIZE;
        buf.len = 0;
        (void)enqueue(c->rx_free, c->capacity, buf);
        (void)enqueue(c->tx_free, c->capacity, buf);
    }
}

int aos_net_virt_add_client(aos_net_virt_t *v, const aos_net_virt_client_t *c)
{
    if (!v || !c || v->num_clients >= AOS_NET_MAX_CLIENTS) {
        return -1;
    }
    v->clients[v->num_clients] = *c;
    v->num_clients++;
    return 0;
}

static int deliver_one(aos_net_virt_client_t *dst, const uint8_t *src,
                       uint16_t len)
{
    aos_net_buff_desc_t rx;
    uint32_t copy;

    if (dequeue(dst->rx_free, dst->capacity, &rx) != 0) {
        return -1;
    }
    copy = len;
    if (copy > AOS_NET_BUFFER_SIZE) {
        copy = AOS_NET_BUFFER_SIZE;
    }
    aos_copy(dst->rx_data + (uint32_t)rx.io_or_offset, src, copy);
    rx.len = (uint16_t)copy;
    if (enqueue(dst->rx_active, dst->capacity, rx) != 0) {
        rx.len = 0;
        (void)enqueue(dst->rx_free, dst->capacity, rx);
        return -1;
    }
    return 0;
}

uint32_t aos_net_virt_pump(aos_net_virt_t *v)
{
    uint32_t forwarded = 0;
    uint32_t src_i;

    if (!v || v->num_clients == 0u) {
        return 0;
    }

    for (src_i = 0; src_i < v->num_clients; src_i++) {
        aos_net_virt_client_t *src = &v->clients[src_i];

        while (!q_empty(src->tx_active)) {
            aos_net_buff_desc_t tx;
            const uint8_t *payload;
            uint32_t dst_i;
            int delivered = 0;

            if (dequeue(src->tx_active, src->capacity, &tx) != 0) {
                break;
            }
            payload = src->tx_data + (uint32_t)tx.io_or_offset;

            if (v->num_clients == 1u) {
                if (deliver_one(src, payload, tx.len) == 0) {
                    delivered = 1;
                }
            } else {
                for (dst_i = 0; dst_i < v->num_clients; dst_i++) {
                    if (dst_i == src_i) {
                        continue;
                    }
                    if (deliver_one(&v->clients[dst_i], payload, tx.len) == 0) {
                        delivered = 1;
                    }
                }
            }

            tx.len = 0;
            (void)enqueue(src->tx_free, src->capacity, tx);
            if (delivered) {
                forwarded++;
            }
        }
    }

    return forwarded;
}
