#ifndef AOS_PLATFORM_VMM_VIRTIO_NET_H
#define AOS_PLATFORM_VMM_VIRTIO_NET_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <platform/guest_ram.h>

/* libvmm virtio-mmio net at AOS_VIRTIO_NET_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_net_init(uint32_t client_id);
/* Bind once before guest execution. Caller owns the mapped queue region;
 * placement is guest-emulated MMIO, never a host NIC register mapping. */
bool aos_vmm_virtio_net_init_at(uint32_t client_id, uintptr_t guest_base,
                              unsigned virq, void *shared_region);
void aos_vmm_virtio_net_after_fault(void);
void aos_vmm_virtio_net_rx_ready(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_NET_H */
