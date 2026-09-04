#ifndef AOS_PLATFORM_VMM_VIRTIO_BLK_H
#define AOS_PLATFORM_VMM_VIRTIO_BLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libvmm virtio-mmio blk at AOS_VIRTIO_BLK_GUEST_IPA, pumped after faults. */
void aos_vmm_virtio_blk_init(void);
bool aos_vmm_virtio_blk_load_casper_initrd(uintptr_t guest_dest,
                                           size_t guest_capacity,
                                           size_t *loaded_size);
void aos_vmm_virtio_blk_after_fault(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_BLK_H */
