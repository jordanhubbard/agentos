#ifndef AOS_VIRTIO_PCI_CAPS_H
#define AOS_VIRTIO_PCI_CAPS_H

#include <stdbool.h>
#include <stdint.h>

#define AOS_PCI_CONFIG_BYTES 256u
#define AOS_VIRTIO_PCI_COMMON 0u
#define AOS_VIRTIO_PCI_NOTIFY 1u
#define AOS_VIRTIO_PCI_DEVICE 2u
#define AOS_VIRTIO_PCI_REGIONS 3u

typedef struct {
    uint64_t paddr;
    uint32_t length;
} aos_virtio_pci_region_t;
typedef struct {
    aos_virtio_pci_region_t region[AOS_VIRTIO_PCI_REGIONS];
    uint32_t notify_multiplier;
} aos_virtio_pci_layout_t;

/* Decode a standard type-0 config snapshot and independently measured BAR
 * sizes. No I/O, allocation or pointer dereference of device-supplied addresses.
 * Accept one modern common/notify/device capability each. Resources must fit
 * their memory BAR; the caller separately grants and maps the returned spans.
 * expected_device is the modern PCI device ID (e.g. 0x1042 for block). */
bool aos_virtio_pci_decode(const uint8_t config[AOS_PCI_CONFIG_BYTES],
                           const uint64_t bar_sizes[6], uint16_t expected_device,
                           aos_virtio_pci_layout_t *layout);

#endif
