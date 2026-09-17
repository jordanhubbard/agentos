#ifndef AOS_VIRTIO_HOST_TRANSPORT_H
#define AOS_VIRTIO_HOST_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Driver-side transport only. These are root-granted device mappings, never
 * guest RAM. PCI discovery must verify the vendor/device, map each capability
 * span and enable memory decoding before binding. DMA authority is separate.
 * No MSI-X, packed queues or notification-data feature is supported here.
 * Bind only a quiescent device; a binding never resets or revokes live DMA.
 * Feature/status operations require a successful binding. One queue is
 * configured per transport instance; multi-queue drivers need a wider API. */
typedef struct {
    volatile uint8_t *registers;
    volatile uint8_t *config;
    volatile uint8_t *notify;
    size_t config_size, notify_size;
    uint32_t notify_multiplier;
    uint32_t notify_offset;
    uint16_t queue;
    bool pci, queue_ready;
} aos_virtio_host_t;

bool aos_virtio_host_mmio(aos_virtio_host_t *t, uintptr_t base,
                          size_t size, uint32_t device_id);
bool aos_virtio_host_pci(aos_virtio_host_t *t,
                         uintptr_t common, size_t common_size,
                         uintptr_t config, size_t config_size,
                         uintptr_t notify, size_t notify_size,
                         uint32_t notify_multiplier);
uint32_t aos_virtio_host_features(aos_virtio_host_t *t, uint32_t word);
void aos_virtio_host_set_features(aos_virtio_host_t *t, uint32_t word, uint32_t value);
uint8_t aos_virtio_host_status(aos_virtio_host_t *t);
void aos_virtio_host_set_status(aos_virtio_host_t *t, uint8_t status);
bool aos_virtio_host_queue(aos_virtio_host_t *t, uint16_t queue, uint16_t count,
                           uint64_t desc, uint64_t avail, uint64_t used);
bool aos_virtio_host_notify(aos_virtio_host_t *t);
bool aos_virtio_host_config32(aos_virtio_host_t *t, uint32_t offset, uint32_t *value);
bool aos_virtio_host_config64(aos_virtio_host_t *t, uint32_t offset, uint64_t *value);

#endif
