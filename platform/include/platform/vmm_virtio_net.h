#ifndef AOS_PLATFORM_VMM_VIRTIO_NET_H
#define AOS_PLATFORM_VMM_VIRTIO_NET_H

#include <stddef.h>
#include <stdint.h>

/* libvmm virtio-mmio net at AOS_VIRTIO_NET_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_net_init(void);
void aos_vmm_virtio_net_after_fault(void);

/* Install the guest RAM window used by emulated virtio GPA→HVA copies. */
void aos_vmm_guest_ram_bind(uint64_t gpa_base, uintptr_t hva_base, size_t size);

#endif /* AOS_PLATFORM_VMM_VIRTIO_NET_H */
