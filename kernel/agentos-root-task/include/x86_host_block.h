#ifndef AOS_X86_HOST_BLOCK_H
#define AOS_X86_HOST_BLOCK_H
#include <platform/virtio_pci_caps.h>

/* Boot-only discovery of the board's assigned 00:05.0 modern virtio block
 * function. BAR sizing runs with decode and bus mastering disabled; command
 * state is restored and the temporary PCI configuration-port cap is deleted.
 * Returns a stage number on failure, zero on success. No DMA is started. */
unsigned aos_x86_host_block_discover(aos_virtio_pci_layout_t *layout);
#endif
