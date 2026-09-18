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

/* Read host-backed media before guest queue ownership. Units are 4096-byte
 * storage blocks. The destination must hold count blocks; one shared data
 * window is the per-call maximum. Refuses RAM fallback and DRIVER_OK guests.
 * wait must block until notification without consuming the response queue. */
bool aos_vmm_virtio_blk_read_boot(uint64_t block, uint16_t count,
                                  void *destination, size_t capacity,
                                  aos_vmm_blk_wait_fn wait);

/* libvmm virtio-mmio blk at AOS_VIRTIO_BLK_GUEST_IPA; sDDF queues in the
 * shared block region, serviced by the blk_virt PD. */
void aos_vmm_virtio_blk_init(uint32_t media_id);
/* Bind once at an architecture-selected guest MMIO page and interrupt.
 * shared_region is the 4 KiB-aligned root-provisioned layout's virtual base; only
 * this VMM's client stride must be mapped. It does not grant memory authority.
 * After attachment, retries are rejected without resetting live queues. */
bool aos_vmm_virtio_blk_init_at(uint32_t media_id, uintptr_t guest_base,
                               unsigned virq, void *shared_region);
bool aos_vmm_virtio_blk_load_iso_file(const char *path,
                                      uintptr_t guest_dest,
                                      size_t guest_capacity,
                                      size_t *loaded_size,
                                      aos_vmm_blk_wait_fn wait);
/* After every guest exit: complete responses, kick blk_virt if needed. */
void aos_vmm_virtio_blk_after_fault(void);
/* On BLK_VIRT_EVENT_RESP_READY from blk_virt. */
void aos_vmm_virtio_blk_resp_ready(void);
/* True only after a host-backed guest device reached DRIVER_OK and a guest
 * request completed; synchronous preboot reads do not count. */
bool aos_vmm_virtio_blk_guest_io_completed(void);

/* Nonblocking lifecycle drain after vCPUs stop. Stops admission immediately,
 * services available completions, and returns false while accepted work is
 * still pending. Keep RAM mapped and service block notifications until true.
 * Success disables further response callbacks; initialization is required
 * before this backend can admit requests again. */
bool aos_vmm_virtio_blk_quiesce(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_BLK_H */
