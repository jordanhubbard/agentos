#ifndef AOS_PLATFORM_VMM_VIRTIO_NET_H
#define AOS_PLATFORM_VMM_VIRTIO_NET_H

/* libvmm virtio-mmio net at AOS_VIRTIO_NET_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_net_init(void);
void aos_vmm_virtio_net_after_fault(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_NET_H */
