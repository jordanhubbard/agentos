#include <libvmm/virtio/gpa.h>
#include <platform/guest_ram.h>

void aos_vmm_guest_ram_bind(uint64_t gpa_base, uintptr_t hva_base, size_t size)
{
    aos_guest_ram_configure(gpa_base, hva_base, size);
    virtio_gpa_set_translate(aos_gpa_to_hva_configured);
}
