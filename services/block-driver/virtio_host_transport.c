#include <platform/virtio_host_transport.h>
#include "arch_barrier.h"
#include "virtio_blk.h"

/* Virtio 1.2 sections 4.1.4.3 (PCI common configuration) and 4.2.2
 * (MMIO). Use each register's specified access width, including byte status
 * and halfword queue controls on PCI. Queue addresses are split dwords. */
static uint32_t rd32(volatile uint8_t *p, size_t off)
{
    uint32_t value = *(volatile uint32_t *)(p + off);
    ARCH_MB();
    return value;
}
static uint16_t rd16(volatile uint8_t *p, size_t off)
{
    uint16_t value = *(volatile uint16_t *)(p + off);
    ARCH_MB();
    return value;
}
static void wr32(volatile uint8_t *p, size_t off, uint32_t value)
{
    *(volatile uint32_t *)(p + off) = value;
    ARCH_MB();
}
static void wr16(volatile uint8_t *p, size_t off, uint16_t value)
{
    *(volatile uint16_t *)(p + off) = value;
    ARCH_MB();
}
static void wr64(volatile uint8_t *p, size_t off, uint64_t value)
{
    wr32(p, off, (uint32_t)value);
    wr32(p, off + 4u, (uint32_t)(value >> 32));
}
static bool span(uintptr_t base, size_t size, size_t minimum, size_t alignment)
{
    return base && !(base & (alignment - 1u)) && size >= minimum &&
           size <= UINTPTR_MAX - base;
}

bool aos_virtio_host_mmio(aos_virtio_host_t *t, uintptr_t base,
                          size_t size, uint32_t device_id)
{
    if (!t) return false;
    *t = (aos_virtio_host_t){0};
    if (!span(base, size, 0x100u, 4u)) return false;
    volatile uint8_t *p = (volatile uint8_t *)base;
    if (rd32(p, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC ||
        rd32(p, VIRTIO_MMIO_VERSION) != 2u || !device_id ||
        rd32(p, VIRTIO_MMIO_DEVICE_ID) != device_id) return false;
    t->registers = p;
    t->config = p + VIRTIO_MMIO_CONFIG;
    t->config_size = size - VIRTIO_MMIO_CONFIG;
    t->queue_epoch = 1;
    return true;
}

bool aos_virtio_host_pci(aos_virtio_host_t *t,
                         uintptr_t common, size_t common_size,
                         uintptr_t config, size_t config_size,
                         uintptr_t notify, size_t notify_size,
                         uint32_t notify_multiplier)
{
    if (!t) return false;
    *t = (aos_virtio_host_t){0};
    if (!span(common, common_size, 56u, 4u) ||
        !span(config, config_size, 4u, 4u) ||
        !span(notify, notify_size, 2u, 2u)) return false;
    t->registers = (volatile uint8_t *)common;
    t->config = (volatile uint8_t *)config;
    t->config_size = config_size;
    t->notify = (volatile uint8_t *)notify;
    t->notify_size = notify_size;
    t->notify_multiplier = notify_multiplier;
    t->pci = true;
    t->queue_epoch = 1;
    return true;
}

uint32_t aos_virtio_host_features(aos_virtio_host_t *t, uint32_t word)
{
    wr32(t->registers, t->pci ? 0u : VIRTIO_MMIO_DEVICE_FEATURES_SEL, word);
    return rd32(t->registers, t->pci ? 4u : VIRTIO_MMIO_DEVICE_FEATURES);
}
void aos_virtio_host_set_features(aos_virtio_host_t *t, uint32_t word, uint32_t value)
{
    wr32(t->registers, t->pci ? 8u : VIRTIO_MMIO_DRIVER_FEATURES_SEL, word);
    wr32(t->registers, t->pci ? 12u : VIRTIO_MMIO_DRIVER_FEATURES, value);
}
uint8_t aos_virtio_host_status(aos_virtio_host_t *t)
{
    if (!t->pci) return (uint8_t)rd32(t->registers, VIRTIO_MMIO_STATUS);
    uint8_t status = t->registers[20];
    ARCH_MB();
    return status;
}
void aos_virtio_host_set_status(aos_virtio_host_t *t, uint8_t status)
{
    if (!status && ++t->queue_epoch == 0) ++t->queue_epoch;
    if (!status) t->queue_ready = false;
    if (t->pci) {
        t->registers[20] = status;
        ARCH_MB();
    } else wr32(t->registers, VIRTIO_MMIO_STATUS, status);
}

uint32_t aos_virtio_host_interrupt_status(aos_virtio_host_t *t)
{
    if (!t || !t->registers || t->pci) return 0;
    return rd32(t->registers, VIRTIO_MMIO_INTERRUPT_STATUS);
}
void aos_virtio_host_interrupt_ack(aos_virtio_host_t *t, uint32_t status)
{
    if (!t || !t->registers || t->pci || !status) return;
    wr32(t->registers, VIRTIO_MMIO_INTERRUPT_ACK, status);
}

bool aos_virtio_host_queue_bind(aos_virtio_host_t *t, aos_virtio_host_queue_t *q,
    uint16_t queue, uint16_t count, uint64_t desc, uint64_t avail, uint64_t used)
{
    if (!t || !t->registers || !q || !count ||
        (count & (count - 1u)) || !desc || (desc & 15u) ||
        !avail || (avail & 1u) || !used || (used & 3u)) return false;
    uint32_t notify_offset = 0;
    if (t->pci) {
        if (queue >= rd16(t->registers, 18u)) return false;
        wr16(t->registers, 22u, queue);
        if (rd16(t->registers, 24u) < count || rd16(t->registers, 28u)) return false;
        uint64_t off = (uint64_t)rd16(t->registers, 30u) * t->notify_multiplier;
        if ((off & 1u) || off > UINT32_MAX || off > t->notify_size - 2u) return false;
        notify_offset = (uint32_t)off;
        /* Polling transport: neither config nor queue uses an MSI-X vector. */
        wr16(t->registers, 16u, UINT16_MAX);
        wr16(t->registers, 26u, UINT16_MAX);
        wr16(t->registers, 24u, count);
        wr64(t->registers, 32u, desc);
        wr64(t->registers, 40u, avail);
        wr64(t->registers, 48u, used);
        wr16(t->registers, 28u, 1u);
        if (rd16(t->registers, 28u) != 1u) return false;
    } else {
        wr32(t->registers, VIRTIO_MMIO_QUEUE_SEL, queue);
        if (rd32(t->registers, VIRTIO_MMIO_QUEUE_NUM_MAX) < count ||
            rd32(t->registers, VIRTIO_MMIO_QUEUE_READY)) return false;
        wr32(t->registers, VIRTIO_MMIO_QUEUE_NUM, count);
        wr64(t->registers, VIRTIO_MMIO_QUEUE_DESC_LOW, desc);
        wr64(t->registers, VIRTIO_MMIO_QUEUE_AVAIL_LOW, avail);
        wr64(t->registers, VIRTIO_MMIO_QUEUE_USED_LOW, used);
        wr32(t->registers, VIRTIO_MMIO_QUEUE_READY, 1u);
        if (rd32(t->registers, VIRTIO_MMIO_QUEUE_READY) != 1u) return false;
    }
    *q = (aos_virtio_host_queue_t){.owner=t, .epoch=t->queue_epoch,
        .notify_offset=notify_offset, .index=queue};
    return true;
}
bool aos_virtio_host_queue(aos_virtio_host_t *t, uint16_t queue, uint16_t count,
                           uint64_t desc, uint64_t avail, uint64_t used)
{
    if (!t || t->queue_ready) return false;
    aos_virtio_host_queue_t q;
    if (!aos_virtio_host_queue_bind(t, &q, queue, count, desc, avail, used)) return false;
    t->queue=queue;
    t->notify_offset=q.notify_offset;
    t->queue_ready=true;
    return true;
}
bool aos_virtio_host_queue_notify(aos_virtio_host_t *t,
                                 const aos_virtio_host_queue_t *q)
{
    if (!t || !t->registers || !q || q->owner != t || q->epoch != t->queue_epoch)
        return false;
    if (t->pci) wr16(t->notify, q->notify_offset, q->index);
    else wr32(t->registers, VIRTIO_MMIO_QUEUE_NOTIFY, q->index);
    return true;
}
bool aos_virtio_host_notify(aos_virtio_host_t *t)
{
    if (!t || !t->registers || !t->queue_ready) return false;
    if (t->pci) wr16(t->notify, t->notify_offset, t->queue);
    else wr32(t->registers, VIRTIO_MMIO_QUEUE_NOTIFY, t->queue);
    return true;
}
bool aos_virtio_host_config32(aos_virtio_host_t *t, uint32_t offset, uint32_t *value)
{
    if (!t || !t->registers || !value || (offset & 3u) ||
        offset > t->config_size || t->config_size - offset < 4u) return false;
    *value = rd32(t->config, offset);
    return true;
}
static uint32_t generation(aos_virtio_host_t *t)
{
    if (!t->pci) return rd32(t->registers, VIRTIO_MMIO_CONFIG_GENERATION);
    uint8_t gen = t->registers[21];
    ARCH_MB();
    return gen;
}
bool aos_virtio_host_config64(aos_virtio_host_t *t, uint32_t offset, uint64_t *value)
{
    if (!t || !t->registers || !value || (offset & 3u) ||
        offset > t->config_size || t->config_size - offset < 8u) return false;
    for (unsigned retry = 0; retry < 8u; retry++) {
        uint32_t before = generation(t);
        uint32_t lo = rd32(t->config, offset), hi = rd32(t->config, offset + 4u);
        if (before == generation(t)) {
            *value = ((uint64_t)hi << 32) | lo;
            return true;
        }
    }
    return false;
}
