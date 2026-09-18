#ifndef AOS_X86_HOST_PCI_H
#define AOS_X86_HOST_PCI_H
#include <platform/virtio_pci_caps.h>

typedef enum {
    AOS_X86_HOST_BLOCK,
    AOS_X86_HOST_NET,
    AOS_X86_HOST_CONSOLE,
} aos_x86_host_device_t;

/* Boot-only discovery of the board's assigned 00:05.0 modern virtio block
 * 00:06.0 network or 00:07.0 console function. No bus scan is performed.
 * BAR sizing runs with decode and bus mastering disabled; command
 * state is restored and the temporary PCI configuration-port cap is deleted.
 * Returns a stage number on failure, zero on success. No DMA is started. */
unsigned aos_x86_host_pci_discover(aos_x86_host_device_t device,
                                  aos_virtio_pci_layout_t *layout);
/* Called only after driver mappings succeed. Enables memory decoding and
 * bus mastering, disables INTx for the polling driver, deletes its port cap. */
bool aos_x86_host_pci_enable(aos_x86_host_device_t device);
#endif
