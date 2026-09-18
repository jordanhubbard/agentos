#ifndef AGENTOS_CC_TRANSPORT_H
#define AGENTOS_CC_TRANSPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Root -> CC startup record. DMA addresses are never CPU pointers.
 * Version 1 retains the ARM MMIO ABI. Version 2 describes separately mapped
 * modern PCI common, notification and console-configuration regions. */
#define CC_VIRTIO_STARTUP_MAGIC   0x43435651u
#define CC_VIRTIO_STARTUP_VERSION 1u
#define CC_VIRTIO_STARTUP_PCI_VERSION 2u
#define CC_VIRTIO_MMIO_VA         0x10002000UL
#define CC_VIRTIO_STARTUP_VA      0x10003000UL
#define CC_VIRTIO_QUEUE_VA        0x10006000UL
#define CC_VIRTIO_TX_BUFFER_VA    0x10007000UL
#define CC_VIRTIO_RX_BUFFER_VA    0x10008000UL
#define CC_VIRTIO_PCI_VA         0x16000000UL
#define CC_VIRTIO_PAGE_BYTES     4096u
#define CC_VIRTIO_PCI_REGIONS     3u
#define CC_VIRTIO_PCI_COMMON      0u
#define CC_VIRTIO_PCI_NOTIFY      1u
#define CC_VIRTIO_PCI_DEVICE      2u

typedef struct __attribute__((packed)) cc_virtio_startup {
    uint32_t magic;
    uint32_t version;
    uint64_t queue_pa;
    uint64_t tx_buffer_pa;
    uint64_t rx_buffer_pa;
} cc_virtio_startup_t;

typedef struct __attribute__((packed)) {
    cc_virtio_startup_t dma;
    uint32_t offset[CC_VIRTIO_PCI_REGIONS];
    uint32_t length[CC_VIRTIO_PCI_REGIONS];
    uint32_t notify_multiplier;
    uint32_t reserved;
} cc_virtio_pci_startup_t;

_Static_assert(sizeof(cc_virtio_startup_t) == 32u, "CC MMIO startup ABI");
_Static_assert(sizeof(cc_virtio_pci_startup_t) == 64u, "CC PCI startup ABI");
_Static_assert(offsetof(cc_virtio_pci_startup_t, offset) == 32u,
               "CC PCI extension follows DMA record");

static inline bool cc_virtio_startup_valid(const cc_virtio_startup_t *s,
                                         uint32_t version)
{
    if (!s || (version != CC_VIRTIO_STARTUP_VERSION &&
               version != CC_VIRTIO_STARTUP_PCI_VERSION) ||
        s->magic != CC_VIRTIO_STARTUP_MAGIC || s->version != version)
        return false;
    const uint64_t pages[] = {s->queue_pa, s->tx_buffer_pa, s->rx_buffer_pa};
    for (unsigned i = 0; i < 3u; i++) {
        if (!pages[i] || (pages[i] & (CC_VIRTIO_PAGE_BYTES - 1u)) ||
            pages[i] > UINT64_MAX - (CC_VIRTIO_PAGE_BYTES - 1u)) return false;
        for (unsigned j = 0; j < i; j++)
            if (pages[i] == pages[j]) return false;
    }
    return true;
}

static inline bool cc_virtio_pci_startup_valid(const cc_virtio_pci_startup_t *s)
{
    if (!s || !cc_virtio_startup_valid(&s->dma, CC_VIRTIO_STARTUP_PCI_VERSION) ||
        s->reserved || (s->notify_multiplier & 1u)) return false;
    const uint32_t minimum[] = {56u, 2u, 12u};
    const uint32_t alignment[] = {4u, 2u, 4u};
    for (unsigned i = 0; i < CC_VIRTIO_PCI_REGIONS; i++) {
        if (s->offset[i] >= CC_VIRTIO_PAGE_BYTES ||
            (s->offset[i] & (alignment[i] - 1u)) ||
            s->length[i] < minimum[i] ||
            s->length[i] > CC_VIRTIO_PAGE_BYTES - s->offset[i]) return false;
    }
    return true;
}

/* queue_notify_off is read from the selected queue's common configuration.
 * Validate its product before forming a pointer into the mapped notify page. */
static inline bool cc_virtio_pci_notify_address(const cc_virtio_pci_startup_t *s,
                                               uint16_t queue_notify_off,
                                               uintptr_t *address)
{
    if (!address) return false;
    *address = 0u;
    if (!cc_virtio_pci_startup_valid(s)) return false;
    uint64_t delta = (uint64_t)queue_notify_off * s->notify_multiplier;
    if (delta > s->length[CC_VIRTIO_PCI_NOTIFY] - 2u) return false;
    *address = CC_VIRTIO_PCI_VA + CC_VIRTIO_PAGE_BYTES * CC_VIRTIO_PCI_NOTIFY +
               s->offset[CC_VIRTIO_PCI_NOTIFY] + (uintptr_t)delta;
    return true;
}
#endif
