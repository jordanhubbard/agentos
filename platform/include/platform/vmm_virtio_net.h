#ifndef AOS_PLATFORM_VMM_VIRTIO_NET_H
#define AOS_PLATFORM_VMM_VIRTIO_NET_H

#include <stddef.h>
#include <stdint.h>
#include <platform/guest_ram.h>

/* libvmm virtio-mmio net at AOS_VIRTIO_NET_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_net_init(void);
void aos_vmm_virtio_net_after_fault(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_NET_H */
