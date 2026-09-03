/*
 * Host virtio-mmio net emulator. Packet path matches libvmm virtio-net:
 * TX strips AOS_VIRTIO_NET_HDR_LEN, RX prepends a zeroed mrg_rxbuf header,
 * QueueNotify uses the written queue index (Linux virtio-mmio does not
 * write QueueSel first).
 */

#include "virtio_mmio_net_emu.h"

#include <string.h>

static uint32_t offered_lo(void)
{
    return (1u << AOS_VIRTIO_NET_F_MAC) | (1u << AOS_VIRTIO_NET_F_MRG_RXBUF);
}

static uint32_t offered_hi(void)
{
    return 1u; /* VIRTIO_F_VERSION_1 is bit 32 → sel=1 bit 0 */
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

static void used_push(aos_virtq_t *vq, uint32_t desc_head, uint32_t bytes)
{
    aos_virtq_used_elem_t *elem = &vq->used->ring[vq->used->idx % vq->num];
    elem->id = desc_head;
    elem->len = bytes;
    vq->used->idx++;
}

void aos_virtio_mmio_net_init(aos_virtio_mmio_net_t *d,
                              const aos_net_virt_client_t *client,
                              const uint8_t mac[6])
{
    memset(d, 0, sizeof(*d));
    if (client) {
        d->client = *client;
    }
    if (mac) {
        memcpy(d->mac, mac, 6);
    }
}

static int get_features(aos_virtio_mmio_net_t *d, uint32_t *out)
{
    if (d->device_features_sel == 0u) {
        *out = offered_lo();
        return 0;
    }
    if (d->device_features_sel == 1u) {
        *out = offered_hi();
        return 0;
    }
    return -1;
}

static int set_features(aos_virtio_mmio_net_t *d, uint32_t features)
{
    if (d->driver_features_sel == 0u) {
        if ((features & ~offered_lo()) != 0u) {
            return -1;
        }
        d->features_happy = 1;
        return 0;
    }
    if (d->driver_features_sel == 1u) {
        if ((features & ~offered_hi()) != 0u) {
            return -1;
        }
        d->features_happy = 1;
        return 0;
    }
    return -1;
}

static int set_status(aos_virtio_mmio_net_t *d, uint32_t reg)
{
    uint32_t newly;

    d->status &= reg;
    newly = reg ^ d->status;

    if (reg == 0u) {
        d->status = 0;
        d->features_happy = 0;
        d->driver_ok = 0;
        d->vqs[0].ready = 0;
        d->vqs[1].ready = 0;
        d->vqs[0].last_idx = 0;
        d->vqs[1].last_idx = 0;
        return 0;
    }

    if (newly & AOS_VIRTIO_S_ACKNOWLEDGE) {
        if (d->status == 0u) {
            d->status |= AOS_VIRTIO_S_ACKNOWLEDGE;
            d->probed = 1;
        }
    }
    if (newly & AOS_VIRTIO_S_DRIVER) {
        if (d->status & AOS_VIRTIO_S_ACKNOWLEDGE) {
            d->status |= AOS_VIRTIO_S_DRIVER;
        }
    }
    if (newly & AOS_VIRTIO_S_FEATURES_OK) {
        if (d->status & AOS_VIRTIO_S_DRIVER) {
            if (d->features_happy) {
                d->status |= AOS_VIRTIO_S_FEATURES_OK;
            }
        }
    }
    if (newly & AOS_VIRTIO_S_DRIVER_OK) {
        d->status |= AOS_VIRTIO_S_DRIVER_OK;
        d->driver_ok = 1;
    }
    return 0;
}

static aos_virtio_vq_t *selected_vq(aos_virtio_mmio_net_t *d)
{
    if (d->queue_sel >= AOS_VIRTIO_NET_NUM_VQ) {
        return NULL;
    }
    return &d->vqs[d->queue_sel];
}

static uint32_t copy_from_chain(aos_virtq_t *vq, uint16_t desc_head,
                                uint32_t skip, uint8_t *dst, uint32_t dst_cap)
{
    aos_virtq_desc_t *desc = &vq->desc[desc_head];
    uint32_t written = 0;
    uint32_t skip_left = skip;

    for (;;) {
        uint32_t skipping = skip_left < desc->len ? skip_left : desc->len;
        uint32_t payload = desc->len - skipping;
        uint32_t writing = payload;
        const uint8_t *src = (const uint8_t *)(uintptr_t)desc->addr;

        if (writing > dst_cap - written) {
            writing = dst_cap - written;
        }
        if (writing > 0u) {
            memcpy(dst + written, src + skipping, writing);
            written += writing;
        }
        skip_left -= skipping;
        if ((desc->flags & AOS_VIRTQ_DESC_F_NEXT) == 0u) {
            break;
        }
        desc = &vq->desc[desc->next];
        if (written == dst_cap) {
            break;
        }
    }
    return written;
}

static uint32_t copy_to_chain(aos_virtq_t *vq, uint16_t *desc_head,
                              uint32_t *desc_off, const void *buf, uint32_t size)
{
    uint32_t copied = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (copied < size) {
        aos_virtq_desc_t *desc = &vq->desc[*desc_head];
        uint32_t room = desc->len - *desc_off;
        uint32_t n = size - copied;
        uint8_t *dst = (uint8_t *)(uintptr_t)desc->addr;

        if (n > room) {
            n = room;
        }
        memcpy(dst + *desc_off, src + copied, n);
        copied += n;
        *desc_off += n;
        if (*desc_off == desc->len) {
            if ((desc->flags & AOS_VIRTQ_DESC_F_NEXT) == 0u) {
                break;
            }
            *desc_head = desc->next;
            *desc_off = 0;
        }
    }
    return copied;
}

static int handle_tx(aos_virtio_mmio_net_t *d)
{
    aos_virtio_vq_t *vq = &d->vqs[AOS_VIRTIO_NET_TX_VQ];
    aos_virtq_t *virtq = &vq->virtq;
    uint16_t guest_idx;
    uint16_t idx;
    int did = 0;

    if ((d->status & AOS_VIRTIO_S_DRIVER_OK) == 0u ||
        (d->status & AOS_VIRTIO_S_FEATURES_OK) == 0u) {
        return -1;
    }
    if (!vq->ready || virtq->avail == NULL || virtq->desc == NULL || virtq->used == NULL) {
        return -1;
    }

    guest_idx = virtq->avail->idx;
    idx = vq->last_idx;
    for (; idx != guest_idx; idx++) {
        uint16_t desc_head = virtq->avail->ring[idx % virtq->num];
        aos_net_buff_desc_t buf;
        uint32_t written;

        if (q_full(d->client.tx_active, d->client.capacity) ||
            dequeue(d->client.tx_free, d->client.capacity, &buf) != 0) {
            used_push(virtq, desc_head, 0);
            did = 1;
            continue;
        }
        written = copy_from_chain(virtq, desc_head, AOS_VIRTIO_NET_HDR_LEN,
                                  d->client.tx_data + (uint32_t)buf.io_or_offset,
                                  AOS_NET_BUFFER_SIZE);
        buf.len = (uint16_t)written;
        if (enqueue(d->client.tx_active, d->client.capacity, buf) != 0) {
            buf.len = 0;
            (void)enqueue(d->client.tx_free, d->client.capacity, buf);
            used_push(virtq, desc_head, 0);
            did = 1;
            continue;
        }
        used_push(virtq, desc_head, written);
        did = 1;
    }
    vq->last_idx = idx;
    if (did) {
        d->interrupt_status |= 1u;
    }
    return 0;
}

uint32_t aos_virtio_mmio_net_handle_rx(aos_virtio_mmio_net_t *d)
{
    aos_virtio_vq_t *vq = &d->vqs[AOS_VIRTIO_NET_RX_VQ];
    aos_virtq_t *virtq = &vq->virtq;
    uint32_t delivered = 0;
    uint8_t hdr[AOS_VIRTIO_NET_HDR_LEN];

    if ((d->status & AOS_VIRTIO_S_DRIVER_OK) == 0u ||
        (d->status & AOS_VIRTIO_S_FEATURES_OK) == 0u) {
        return 0;
    }
    if (!vq->ready || virtq->avail == NULL) {
        return 0;
    }

    memset(hdr, 0, sizeof(hdr));
    hdr[10] = 1; /* num_buffers little-endian low byte */
    hdr[11] = 0;

    while (!q_empty(d->client.rx_active)) {
        aos_net_buff_desc_t buf;
        uint16_t guest_idx = virtq->avail->idx;
        uint16_t desc_head;
        uint16_t curr;
        uint32_t desc_off = 0;
        uint32_t copied;

        if (vq->last_idx == guest_idx) {
            break;
        }
        if (dequeue(d->client.rx_active, d->client.capacity, &buf) != 0) {
            break;
        }
        desc_head = virtq->avail->ring[vq->last_idx % virtq->num];
        curr = desc_head;
        copied = copy_to_chain(virtq, &curr, &desc_off, hdr, AOS_VIRTIO_NET_HDR_LEN);
        copied += copy_to_chain(virtq, &curr, &desc_off,
                                d->client.rx_data + (uint32_t)buf.io_or_offset, buf.len);
        used_push(virtq, desc_head, copied);
        vq->last_idx++;
        buf.len = 0;
        (void)enqueue(d->client.rx_free, d->client.capacity, buf);
        delivered++;
        d->interrupt_status |= 1u;
    }
    return delivered;
}

uint32_t aos_virtio_mmio_net_after_fault(aos_virtio_mmio_net_t *d, aos_net_virt_t *virt)
{
    if (!d || !virt) {
        return 0;
    }
    (void)aos_net_virt_pump(virt);
    return aos_virtio_mmio_net_handle_rx(d);
}

int aos_virtio_mmio_net_read(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t *out)
{
    aos_virtio_vq_t *vq;

    if (!d || !out) {
        return -1;
    }
    switch (off) {
    case AOS_VIRTIO_REG_MAGIC:
        *out = AOS_VIRTIO_MMIO_MAGIC;
        return 0;
    case AOS_VIRTIO_REG_VERSION:
        *out = AOS_VIRTIO_MMIO_VERSION;
        return 0;
    case AOS_VIRTIO_REG_DEVICE_ID:
        *out = AOS_VIRTIO_NET_DEVICE_ID;
        return 0;
    case AOS_VIRTIO_REG_VENDOR_ID:
        *out = AOS_VIRTIO_MMIO_VENDOR_SEL4;
        return 0;
    case AOS_VIRTIO_REG_DEVICE_FEATURES:
        return get_features(d, out);
    case AOS_VIRTIO_REG_QUEUE_NUM_MAX:
        *out = AOS_VIRTIO_QUEUE_SIZE_MAX;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_READY:
        vq = selected_vq(d);
        *out = (vq && vq->ready) ? 1u : 0u;
        return 0;
    case AOS_VIRTIO_REG_INTERRUPT_STATUS:
        *out = d->interrupt_status;
        return 0;
    case AOS_VIRTIO_REG_STATUS:
        *out = d->status;
        return 0;
    case AOS_VIRTIO_REG_CONFIG:
        *out = (uint32_t)d->mac[0]
             | ((uint32_t)d->mac[1] << 8)
             | ((uint32_t)d->mac[2] << 16)
             | ((uint32_t)d->mac[3] << 24);
        return 0;
    case AOS_VIRTIO_REG_CONFIG + 4u:
        *out = (uint32_t)d->mac[4] | ((uint32_t)d->mac[5] << 8);
        return 0;
    default:
        return -1;
    }
}

int aos_virtio_mmio_net_write(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t val)
{
    aos_virtio_vq_t *vq;
    uintptr_t ptr;

    if (!d) {
        return -1;
    }
    switch (off) {
    case AOS_VIRTIO_REG_DEVICE_FEATURES_SEL:
        d->device_features_sel = val;
        return 0;
    case AOS_VIRTIO_REG_DRIVER_FEATURES:
        return set_features(d, val);
    case AOS_VIRTIO_REG_DRIVER_FEATURES_SEL:
        d->driver_features_sel = val;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_SEL:
        d->queue_sel = val;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_NUM:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        vq->virtq.num = val;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_READY:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        vq->ready = (val == 1u);
        return 0;
    case AOS_VIRTIO_REG_QUEUE_NOTIFY:
        d->queue_notify = val;
        /* Linux writes the queue index here; QueueSel is not updated. */
        if (val == AOS_VIRTIO_NET_TX_VQ) {
            return handle_tx(d);
        }
        return 0;
    case AOS_VIRTIO_REG_INTERRUPT_ACK:
        d->interrupt_status &= ~val;
        return 0;
    case AOS_VIRTIO_REG_STATUS:
        return set_status(d, val);
    case AOS_VIRTIO_REG_QUEUE_DESC_LOW:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.desc;
        ptr = (ptr & 0xffffffff00000000ULL) | (uintptr_t)val;
        vq->virtq.desc = (aos_virtq_desc_t *)ptr;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_DESC_HIGH:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.desc;
        ptr = (ptr & 0xffffffffULL) | ((uintptr_t)val << 32);
        vq->virtq.desc = (aos_virtq_desc_t *)ptr;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_AVAIL_LOW:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.avail;
        ptr = (ptr & 0xffffffff00000000ULL) | (uintptr_t)val;
        vq->virtq.avail = (aos_virtq_avail_t *)ptr;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_AVAIL_HIGH:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.avail;
        ptr = (ptr & 0xffffffffULL) | ((uintptr_t)val << 32);
        vq->virtq.avail = (aos_virtq_avail_t *)ptr;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_USED_LOW:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.used;
        ptr = (ptr & 0xffffffff00000000ULL) | (uintptr_t)val;
        vq->virtq.used = (aos_virtq_used_t *)ptr;
        return 0;
    case AOS_VIRTIO_REG_QUEUE_USED_HIGH:
        vq = selected_vq(d);
        if (!vq) {
            return -1;
        }
        ptr = (uintptr_t)vq->virtq.used;
        ptr = (ptr & 0xffffffffULL) | ((uintptr_t)val << 32);
        vq->virtq.used = (aos_virtq_used_t *)ptr;
        return 0;
    default:
        return -1;
    }
}
