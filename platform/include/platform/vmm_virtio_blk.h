#ifndef AOS_PLATFORM_VMM_VIRTIO_BLK_H
#define AOS_PLATFORM_VMM_VIRTIO_BLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <platform/blk_host_layout.h>

/*
 * Blocks the VMM on its listen endpoint until one event arrives, answering
 * any guest-control RPC that lands meanwhile.  Used while the VMM stages
 * media before the guest runs: blk_virt runs below the VMM, so the VMM must
 * block, not spin, while it waits for its own block responses.
 */
typedef void (*aos_vmm_blk_wait_fn)(void);

/* libvmm virtio-mmio blk at AOS_VIRTIO_BLK_GUEST_IPA; sDDF queues in the
 * shared block region, serviced by the blk_virt PD. */
void aos_vmm_virtio_blk_init(uint32_t media_id);
bool aos_vmm_virtio_blk_load_iso_file(const char *path,
                                      uintptr_t guest_dest,
                                      size_t guest_capacity,
                                      size_t *loaded_size,
                                      aos_vmm_blk_wait_fn wait);
/* After every guest exit: complete responses, kick blk_virt if needed. */
void aos_vmm_virtio_blk_after_fault(void);
/* On BLK_VIRT_EVENT_RESP_READY from blk_virt. */
void aos_vmm_virtio_blk_resp_ready(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_BLK_H */
