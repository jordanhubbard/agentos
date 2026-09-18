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
/* After vCPUs stop and the current callback returns, retire guest ring
 * references and ignore late network wakeups. Idempotent. Shared packet
 * storage remains owned by the virtualizer; this is not a detach/rebind. */
void aos_vmm_virtio_net_quiesce(void);
/* The canonical virtualizer reported an initialized host NIC at attachment. */
bool aos_vmm_virtio_net_host_ready(void);
/* Qualification requires a host NIC, guest DRIVER_OK, and TX/RX activity. */
bool aos_vmm_virtio_net_guest_io_completed(void);
/* Qualification failure detail: status[7:0], pending TX[15:8], TX consumed
 * bit 16, outstanding kick bit 17, observed RX bit 18, pending RX[31:24]. */
uint32_t aos_vmm_virtio_net_diagnostic(void);

#endif /* AOS_PLATFORM_VMM_VIRTIO_NET_H */
