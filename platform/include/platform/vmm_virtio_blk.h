#ifndef AOS_PLATFORM_VMM_VIRTIO_BLK_H
#define AOS_PLATFORM_VMM_VIRTIO_BLK_H

/* libvmm virtio-mmio blk at AOS_VIRTIO_BLK_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_blk_init(void);
void aos_vmm_virtio_blk_after_fault(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_BLK_H */
