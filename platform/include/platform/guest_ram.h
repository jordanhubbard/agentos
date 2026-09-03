#ifndef AOS_PLATFORM_GUEST_RAM_H
#define AOS_PLATFORM_GUEST_RAM_H

/*
 * GPA → HVA for emulated virtio. Guest descriptor addr fields are guest
 * physical (IPA), not host pointers. Identity-mapping guest_ram makes the
 * two coincide; this helper is the explicit translation so the identity
 * map can die once every DMA path uses it.
 *
 * Host-testable: no seL4, no libvmm.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct aos_guest_ram {
    uint64_t  gpa_base; /* guest physical / IPA of RAM */
    uintptr_t hva_base; /* VMM virtual address of the same pages */
    size_t    size;
} aos_guest_ram_t;

/*
 * Translate [gpa, gpa+len) into the VMM address space.
 *
 * Returns NULL if ram is missing, size is 0, the range wraps uint64,
 * or the range is not entirely inside [gpa_base, gpa_base+size).
 * len == 0 succeeds iff gpa is in [gpa_base, gpa_base+size] (one-past-end
 * allowed for a zero-length copy).
 */
void *aos_gpa_to_hva(const aos_guest_ram_t *ram, uint64_t gpa, size_t len);

/* Process-wide window used by emulated virtio in the VMM PD. */
void  aos_guest_ram_configure(uint64_t gpa_base, uintptr_t hva_base, size_t size);
void *aos_gpa_to_hva_configured(uint64_t gpa, size_t len);

#endif /* AOS_PLATFORM_GUEST_RAM_H */
