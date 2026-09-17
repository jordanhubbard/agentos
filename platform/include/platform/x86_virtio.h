#ifndef AOS_X86_VIRTIO_H
#define AOS_X86_VIRTIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <platform/x86_ioapic.h>

/* Guest-only aperture: no host frame may be mapped here. Slot n uses GSI
 * 16+n. Firmware descriptions must advertise only registered slots. */
#define AOS_X86_VIRTIO_BASE UINT64_C(0xf0000000)
#define AOS_X86_VIRTIO_STRIDE 4096u
#define AOS_X86_VIRTIO_SLOTS 8u
#define AOS_X86_VIRTIO_GSI_BASE 16u
_Static_assert(AOS_X86_VIRTIO_GSI_BASE+AOS_X86_VIRTIO_SLOTS <= AOS_X86_IOAPIC_INPUTS,
               "virtio slots require distinct private IOAPIC inputs");

/* One private bus per VMM PD. Bind once, before libvmm device initializers.
 * RAM must have a 4 KiB-aligned base and size. This initial layout admits
 * contiguous guest RAM starting at GPA zero and
 * ending before the device aperture. It does not implement sparse high RAM. */
bool aos_x86_virtio_init(aos_x86_ioapic_t *ioapic, void *ram, size_t ram_size);
bool aos_x86_virtio_contains(uint64_t gpa);
/* Caller validates the fault, instruction and guest VA-to-GPA translation.
 * Transport registers require aligned DWORD accesses. Read-only device config
 * also admits aligned byte/word reads, extracted from the shared DWORD API.
 * Invalid access shape and failed reads leave *value unchanged. */
bool aos_x86_virtio_access(uint64_t gpa, unsigned width, bool write, uint32_t *value);
#endif
