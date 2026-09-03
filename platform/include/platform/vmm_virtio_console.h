#ifndef AOS_PLATFORM_VMM_VIRTIO_CONSOLE_H
#define AOS_PLATFORM_VMM_VIRTIO_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

/* Guest virtio-console backed by sDDF serial queues inside the VMM. */
void aos_vmm_virtio_console_init(void);
void aos_vmm_virtio_console_after_fault(void);

/* CC-PD / serial_virt bridge used by the guest lifecycle contract. */
uint32_t aos_vmm_virtio_console_drain_tx(uint8_t *dst, uint32_t max);
bool aos_vmm_virtio_console_push_rx(uint8_t byte);

#endif /* AOS_PLATFORM_VMM_VIRTIO_CONSOLE_H */
